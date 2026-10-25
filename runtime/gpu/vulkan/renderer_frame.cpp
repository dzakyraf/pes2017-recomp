// Command buffers and their submission: the frame slots, the end of a frame,
// and the title's occlusion queries, which live and die with the render passes.
#include "renderer_state.h"
#include <algorithm>
#include <map>
#include <vector>
#include "capture.h"
#include "presenter.h"
#include "framegen.h"
#include "texture_cache.h"
#include "../../log.h"
#include "../../pacing_trace.h"
#include "../../report.h"
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif

#ifdef PES17_HAVE_VULKAN

namespace vk::renderer::detail
{
    // Destroying an image the GPU may still use. Waiting for the device is not
    // enough on its own: it covers work already submitted, and the command buffer
    // being recorded -- a render pass open on the very view about to go, or a
    // copy out of it -- has not been. Destroying under it leaves that buffer
    // pointing at freed memory, and submitting it has the GPU write through
    // addresses nothing backs: the page faults every run of this runtime logged
    // until one of them hung the driver and took the machine down. So the open
    // buffer is finished and waited for first, and a fresh one is begun if the
    // caller was in the middle of recording. Commands the recorder thread has
    // not made yet name it too, so it is waited for as well.
    void RetireBeforeDestroy()
    {
        const bool wasRecording = g.recording;
        if (wasRecording) Submit(true);
        vk::record::Drain();
        // The queue is in order, so nothing is outstanding afterwards.
        vkDeviceWaitIdle(g.device);
        for (State::Slot& slot : g.slots) slot.inFlight = false;
        if (wasRecording) BeginFrame();
    }

    bool BeginFrame()
    {
        if (g.recording) return true;

        // Round to the next slot and wait only if the GPU has not finished with
        // it. With three of them and a wait at the present, that is normally
        // already true and this costs nothing.
        g.slot = (g.slot + 1) % kFrameSlots;
        State::Slot& slot = g.slots[g.slot];
        if (!WaitForSlot(slot, "a frame slot's fence")) return false;
        NoteCompleted();
        if (g.timingOn) CollectPassTimings(g.slot);
        g.arenaBase = slot.arenaBase;
        vk::textures::BeginUploads(g.slot);
        g.ended.clear();
        g.segment = 0;
        g.command = slot.buffers[0];
        vk::record::Into(g.command);
        const VkDevice device = g.device;
        const VkCommandPool pool = slot.commands;
        Record([=](VkCommandBuffer) { vkResetCommandPool(device, pool, 0); });
        BeginSegmentBuffer();
        if (g.timingOn)
        {
            const VkQueryPool timestamps = g.passTiming[g.slot].pool;
            g.passTiming[g.slot].count = 0;
            Record([=](VkCommandBuffer command) {
                vkCmdResetQueryPool(command, timestamps, 0, 2 * State::kTimedPasses);
            });
        }
        g.arenaUsed = 0;
        g.vertexCopies.clear();
        g.constantWindows[0].valid = g.constantWindows[1].valid = false;
        g.fetchBlock.valid = g.booleans.valid = g.loops.valid = false;
        g.recording = true;
        return true;
    }

    // Begins g.command. Set 2 is the arena as dwords and is the same for every
    // draw in the frame, so it is bound once a command buffer instead of eight
    // million times.
    void BeginSegmentBuffer()
    {
        const VkPipelineLayout layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());
        const VkDescriptorSet memorySet = g.memorySet;
        Record([=](VkCommandBuffer command) {
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
                LOGW("renderer: a frame's command buffer would not begin");
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    bindings::kArenaSet, 1, &memorySet, 0, nullptr);
        });
        // A fresh command buffer remembers nothing.
        g.bound = State::Bound{};
        g.currentColour = g.currentDepth = VK_NULL_HANDLE;
        g.segmentDraws = false;
    }

    // The command processor is about to report the draws so far done
    // (vk::renderer::BeforeCompletion): the segment's textures are read for
    // the last time, and its draws closed off in a command buffer of their
    // own, so what is read after this reaches only the draws after it. The
    // submission goes on in the next command buffer, and is still one.
    void CloseSegment()
    {
        EndPass();
        if (void* uploads = vk::textures::EndSegment())
            g.ended.push_back(static_cast<VkCommandBuffer>(uploads));
        g.ended.push_back(g.command);
        Record([](VkCommandBuffer command) {
            if (vkEndCommandBuffer(command) != VK_SUCCESS)
                LOGW("renderer: a segment's command buffer would not end");
        });
        State::Slot& slot = g.slots[g.slot];
        if (++g.segment >= slot.buffers.size())
        {
            // The recorder may be making calls into the pool's other buffers:
            // it has to be done with them before the pool is touched here.
            vk::record::Drain();
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool = slot.commands;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = kSegmentBuffers;
            const size_t had = slot.buffers.size();
            slot.buffers.resize(had + kSegmentBuffers);
            if (vkAllocateCommandBuffers(g.device, &allocate, slot.buffers.data() + had) != VK_SUCCESS)
                LOGE("renderer: no command buffers for another segment");
        }
        g.command = slot.buffers[g.segment];
        vk::record::Into(g.command);
        BeginSegmentBuffer();
        g.segments++;
    }

    // The title brackets the draws it wants counted with two EVENT_WRITE_ZPD
    // events, which can be minutes of draws apart in command-buffer terms and
    // can cross any number of render passes. A Vulkan occlusion query has to
    // begin and end inside one render pass instance, so the bracket becomes one
    // query per pass it covers and the counts are added up.
    void OpenGuestQuery()
    {
        if (!g.guestCounting || g.guestQueryOpen) return;
        if (!g.guestQueries || g.guestQueryCount >= kMaxGuestQueries) return;
        const VkQueryPool pool = g.guestQueries;
        const uint32_t query = g.guestQueryFirst + g.guestQueryCount;
        const VkQueryControlFlags flags =
            vk::pipeline::PreciseOcclusionQueries() ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
        Record([=](VkCommandBuffer command) { vkCmdBeginQuery(command, pool, query, flags); });
        g.guestQueryOpen = true;
    }

    void CloseGuestQuery()
    {
        if (!g.guestQueryOpen) return;
        const VkQueryPool pool = g.guestQueries;
        const uint32_t query = g.guestQueryFirst + g.guestQueryCount++;
        Record([=](VkCommandBuffer command) { vkCmdEndQuery(command, pool, query); });
        g.guestQueryOpen = false;
    }

    void EndPass()
    {
        CloseGuestQuery();
        const uint32_t timed = g.timedPass;
        const VkQueryPool timestamps = g.timingOn ? g.passTiming[g.slot].pool : VK_NULL_HANDLE;
        g.timedPass = ~0u;
        if (g.currentColour || g.currentDepth)
            Record([=](VkCommandBuffer command) {
                if (vk::pipeline::LegacyMode())
                    vkCmdEndRenderPass(command);
                else
                    dispatch.endRendering(command);
                if (timestamps && timed != ~0u)
                    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2 * timed + 1);
                // What a render pass's dependency out to the outside was: the
                // copies, clears, passes and sampling after it see its writes.
                VkMemoryBarrier after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                after.dstAccessMask = kColourAccess | kDepthAccess | VK_ACCESS_TRANSFER_READ_BIT |
                                      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(command, kColourStages | kDepthStages,
                                     kColourStages | kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 1, &after, 0, nullptr, 0, nullptr);
            });
        g.currentColour = g.currentDepth = VK_NULL_HANDLE;
    }

    // What a pass was, as one number: width, height, samples and whether it had depth.
    uint32_t PassTimingKey(uint32_t width, uint32_t height, uint32_t samples, bool depth)
    {
        return (std::min(width, 4095u) << 20) | (std::min(height, 2047u) << 9) | ((samples & 7u) << 1) | (depth ? 1u : 0u);
    }

    // The slot's passes of the frame it last held, now finished: their GPU times are
    // added up by kind, and every 600 frames the costliest kinds are logged.
    void CollectPassTimings(uint32_t slotIndex)
    {
        State::PassTiming& timing = g.passTiming[slotIndex];
        static std::map<uint32_t, std::pair<double, uint32_t>> totals;   // key -> ns, passes
        static double frameNs = 0; static uint32_t frames = 0, passes = 0;
        if (timing.count && timing.pool)
        {
            uint64_t ticks[2 * State::kTimedPasses];
            const uint32_t n = std::min(timing.count, State::kTimedPasses);
            if (vkGetQueryPoolResults(g.device, timing.pool, 0, 2 * n, sizeof(uint64_t) * 2 * n, ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                double sum = 0;
                for (uint32_t i = 0; i < n; i++)
                {
                    if (ticks[2 * i + 1] < ticks[2 * i]) continue;
                    const double ns = double(ticks[2 * i + 1] - ticks[2 * i]) * g.timestampPeriod;
                    auto& entry = totals[timing.key[i]];
                    entry.first += ns; entry.second++;
                    sum += ns;
                }
                frameNs += sum; passes += n; frames++;
                // A world frame is twenty-odd passes; a menu or a cutscene is a handful and says
                // nothing about what the match costs.
                if (g.loadScale && n >= 10) g.dynScale.Note(sum / 1e6);
            }
        }
        if (!g.timingReport)
        {
            // Collected for the dynamic scale and not to be logged.
            totals.clear(); frameNs = 0; frames = 0; passes = 0;
        }
        else if (frames >= 600)
        {
            std::vector<std::pair<uint32_t, std::pair<double, uint32_t>>> rows(totals.begin(), totals.end());
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
            LOGW("gpu: %u frames, %.1f passes and %.2f ms of pass time a frame (the passes' own clock, not the frame's)",
                 frames, double(passes) / frames, frameNs / frames / 1e6);
            for (size_t i = 0; i < rows.size() && i < 8; i++)
            {
                const uint32_t key = rows[i].first;
                LOGW("gpu:   %4ux%-4u %ux %s  %.2f ms a frame in %.1f passes (%.3f ms each)",
                     key >> 20, (key >> 9) & 0x7FF, (key >> 1) & 7u, (key & 1) ? "colour+depth" : "colour only",
                     rows[i].second.first / frames / 1e6, double(rows[i].second.second) / frames,
                     rows[i].second.first / rows[i].second.second / 1e6);
            }
            totals.clear(); frameNs = 0; frames = 0; passes = 0;
        }
    }

    // `wait` when the CPU is about to read what the submission produced -- a
    // frame read back, an occlusion query's count -- or free what it uses.
    // Otherwise it is queued and the caller carries straight on, with up to
    // kFrameSlots submissions on the GPU at once. The window thread's copy of a
    // finished frame goes into the same queue after it, and the frame's last
    // barrier orders the two.
    //
    // The submission itself is the recorder thread's, after the commands before
    // it; the serial is the consumer's, counted here.
    bool Submit(bool wait)
    {
        Stopwatch watch(g.submitNanoseconds);
        if (!g.recording) return true;
        EndPass();
        if (g.arenaUsed > g.arenaPeak) g.arenaPeak = g.arenaUsed;
        g.recording = false;

        State::Slot& slot = g.slots[g.slot];
        // The last segment's textures are read for the last time, and its
        // uploads go just ahead of its draws, after the segments before.
        std::vector<VkCommandBuffer> buffers;
        buffers.swap(g.ended);
        if (void* uploads = vk::textures::EndSubmission())
            buffers.push_back(static_cast<VkCommandBuffer>(uploads));
        buffers.push_back(g.command);
        const VkDevice device = g.device;
        const VkQueue queue = g.queue;
        const VkFence fence = slot.fence;
        Record([=, buffers = std::move(buffers)](VkCommandBuffer command) {
            const bool ended = vkEndCommandBuffer(command) == VK_SUCCESS;
            if (!ended) LOGW("renderer: a frame's command buffer would not end");
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = uint32_t(buffers.size());
            submit.pCommandBuffers = buffers.data();
            std::lock_guard lock(vk::pipeline::QueueMutex());
            vkResetFences(device, 1, &fence);
            // Whatever happens the fence is signalled, or whoever waits for
            // this slot waits for ever.
            if (!ended || vk::pipeline::Failed(vkQueueSubmit(queue, 1, &submit, fence),
                                               "a frame's submission"))
            {
                VkSubmitInfo emptySubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                vkQueueSubmit(queue, 1, &emptySubmit, fence);
            }
            else
            {
                // Let the warmup thread know the GPU is being used for rendering
                // so it can throttle pipeline compilation (backpressure).
                NoteRenderSubmit();
            }
        });
        slot.inFlight = true;
        slot.serial = ++g.submissionSerial;
        slot.queuedAt = vk::record::Position();
        g.submits++;
        if (!wait) return true;

        if (!WaitForSlot(slot, "a frame slot's fence")) return false;
        // One queue, submitted in order: this one being done says all of them are.
        for (State::Slot& other : g.slots) other.inFlight = false;
        return true;
    }

    bool WaitForSlot(State::Slot& slot, const char* what)
    {
        if (!slot.inFlight) return true;
        Stopwatch waited(g.fenceNanoseconds);
        stutters::Timed timed(stutters::kGpuWaits);
        vk::record::WaitFor(slot.queuedAt);
        report::Timed spent(report::kGpuWait);
        VkResult res = vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, 2000000000ull);
        if (res == VK_TIMEOUT)
        {
            LOGW("renderer: waiting for %s took >2 seconds; waiting up to 5s...", what);
            res = vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, 5000000000ull);
        }
        if (res == VK_TIMEOUT)
        {
            LOGE("renderer: fence wait timed out for %s; aborting wait to avoid permanent hang", what);
            slot.inFlight = false;
            return false;
        }
        if (vk::pipeline::Failed(res, what))
            return false;
        slot.inFlight = false;
        return true;
    }

    // Asked once a submission rather than whenever it is wanted: a fence's
    // status is a system call, and the shadow wanted it for every draw that
    // touched a page the title keeps rewriting -- 7% of the consumer thread. A
    // value from a little earlier only makes the answer more cautious.
    void NoteCompleted()
    {
        uint64_t oldestRunning = UINT64_MAX;
        for (State::Slot& slot : g.slots)
            if (slot.inFlight && slot.serial < oldestRunning &&
                (!vk::record::Done(slot.queuedAt) ||
                 vkGetFenceStatus(g.device, slot.fence) != VK_SUCCESS))
                oldestRunning = slot.serial;
        g.completedSerial = oldestRunning == UINT64_MAX ? g.submissionSerial : oldestRunning - 1;
    }

    uint64_t CompletedSerial() { return g.completedSerial; }

    // The frame's image is complete in `present`: submit it, move the frame
    // counters on, look at it if asked to, and hand it to the window.
    void FinishFrame(VkImage present)
    {
        // The frame is queued, not waited for: the window thread's blit goes into
        // the same queue behind it. Only a frame something reads back has to be
        // finished first.
        const FrameReadBacks plan = PlanReadBacks();
        if (plan.Any()) RecordReadBack(present);
        if (!Submit(plan.Any())) { Skip("frame submission failed"); return; }
        g.frames++;
        FlipCrowdEvery();
        vk::textures::NewFrame();
#ifdef PES17_ANDROID
        {
            // The frame's length, for the system's performance hint (android::perf): how long
            // since the last frame was finished, which is what it is asked to keep to the target.
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            android::perf::ReportFrameWork(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
            last = now;
        }
#endif

        // The title does not redraw the world every time it presents -- better than
        // half the frames of the training level are the interface over whatever the
        // last world pass left in EDRAM. The world pass is the only one drawn
        // multisampled (2x, 4x on the pass beside it; the interface is 1x) in both
        // executables, so a handful of multisampled draws says it happened. 200
        // draws into any target is the fallback for a device drawing at 1x.
        constexpr uint64_t kWorldDraws = 200;
        constexpr uint64_t kMultisampledDraws = 8;
        bool worldDrawn = false;
        for (auto& [key, target] : g.targets)
        {
            if (!target.depth && (target.colourDrawsThisFrame >= kWorldDraws ||
                                  (key.samples > 0 &&
                                   target.colourDrawsThisFrame >= kMultisampledDraws)))
                worldDrawn = true;
            target.colourDrawsThisFrame = 0;
        }
        // The scale moves after a frame that drew the world, so the next one draws it again into
        // the surfaces made at the new size rather than showing them empty.
        if (g.heatScale)
        {
            g.dynScale.SetThermal(g.thermalStatus.load(std::memory_order_relaxed));
            g.dynScale.Frame();
        }
        if (worldDrawn) ApplySceneScale();
        pacing::Note(pacing::kFrame, worldDrawn ? 1 : 0);
        stutters::Finished(g.frames, worldDrawn);
        report::Frame(worldDrawn);
        NoteFrame(worldDrawn);
        SetCutsceneActive(!worldDrawn);
        if (worldDrawn)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!g.worldStreak++) g.streakFrom = now;
            if (g.worldFrames)
            {
                g.lastWorldAt = now;
                g.worldFrames++;
            }
            else if (g.worldStreak == State::kPlayStreak)
            {
                g.firstWorldAt = g.streakFrom;
                g.lastWorldAt = now;
                g.worldFrames = State::kPlayStreak;
            }
        }
        else g.worldStreak = 0;
        EndFrameDraws(g.frames, worldDrawn);
        // A capture ends and begins between submissions as the GPU gets them,
        // and the window must not copy the frame out before it is submitted:
        // both come after the submission, on the recorder thread.
        const uint64_t frame = g.frames;
        vk::record::Host([=] { vk::capture::FrameBoundary(worldDrawn, frame); });
        g.presentSerial[g.presentIndex] = frame;
        g.presentIndex = (g.presentIndex + 1) % kPresentImages;
        ExamineFrame(worldDrawn, plan);
        const uint32_t width = g.presentWidth, height = g.presentHeight;
        // Before the frame reaches the window, which looks the ball up by the frame's serial.
        if (vk::framegen::WantsBall())
            vk::framegen::NoteBall(frame, g.frameBall.seen, g.frameBall.x, g.frameBall.y, g.frameBall.radius);
        g.frameBall = {};
        vk::record::Host([=] { vk::ShowImage(present, width, height, frame); });
    }
}

using namespace vk::renderer::detail;

namespace
{
    // Whether the GPU has finished the submission a bracket went out in: its
    // slot has moved on to a later one (BeginFrame waited for it first), or
    // the slot's fence says so.
    bool Finished(const State::PendingQuery& query)
    {
        // Still in the command buffer being recorded.
        if (query.serial > g.submissionSerial) return false;
        const State::Slot& slot = g.slots[query.slot];
        if (slot.serial != query.serial || !slot.inFlight) return true;
        // Queued, and not yet handed to the GPU by the recorder thread.
        return vk::record::Done(slot.queuedAt) &&
               vkGetFenceStatus(g.device, slot.fence) == VK_SUCCESS;
    }

    void Read(State::PendingQuery& query)
    {
        stutters::Timed timed(stutters::kQueryReads);
        uint64_t samples[kMaxGuestQueries]{};
        query.read = true;
        query.samples = 0;
        if (vkGetQueryPoolResults(g.device, g.guestQueries, query.first, query.count,
                                  sizeof(uint64_t) * query.count, samples, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return;
        for (uint32_t i = 0; i < query.count; i++) query.samples += samples[i];
        // The title compares the count with numbers of its own pixels.
        const double area = double(g.scale) * g.scale * query.scale * query.scale;
        query.samples = uint64_t(double(query.samples) / area + 0.5);
    }
}

using namespace vk::renderer::detail;

// The title's own occlusion query. Everything drawn between the two calls is
// counted, however many render passes and command buffers that takes.
void vk::renderer::BeginOcclusionQuery()
{
    if (!g.guestQueries) return;
    // Any query still open belongs to a bracket the title abandoned.
    EndPass();
    if (!BeginFrame()) return;
    if (g.guestQueryNext + kMaxGuestQueries > kGuestQueryRing) g.guestQueryNext = 0;
    const uint32_t first = g.guestQueryNext, last = first + kMaxGuestQueries;
    // A bracket still out on this stretch of the ring is counted now, before
    // the reset below reaches the GPU.
    for (State::PendingQuery& query : g.pendingQueries)
        if (!query.read && query.first < last && first < query.first + query.count)
        {
            // Not submitted yet: this recording holds it, and has to go first.
            if (query.serial > g.submissionSerial)
            {
                EndPass();
                if (!Submit(true) || !BeginFrame()) return;
            }
            State::Slot& slot = g.slots[query.slot];
            if (slot.serial == query.serial) WaitForSlot(slot, "an occlusion query's submission");
            Read(query);
        }
    const VkQueryPool pool = g.guestQueries;
    Record([=](VkCommandBuffer command) {
        vkCmdResetQueryPool(command, pool, first, kMaxGuestQueries);
    });
    g.guestQueryFirst = first;
    g.guestQueryCount = 0;
    g.guestCounting = true;
}

// Not submitted here: the count goes out with the rest of the frame and is
// handed over once the GPU has finished it, and the title polls the record
// until then. Submitting at every bracket ended the command buffer mid-frame
// -- the textures re-read, every binding made again in the next one -- for 8%
// of the consumer thread.
bool vk::renderer::EndOcclusionQuery(uint32_t tag, uint64_t& samples)
{
    samples = 0;
    if (!g.guestQueries || !g.guestCounting) return true;
    EndPass();
    g.guestCounting = false;
    const uint32_t counted = g.guestQueryCount;
    g.guestQueryCount = 0;
    if (!counted) return true;
    g.guestQueryNext = g.guestQueryFirst + counted;
    g.pendingQueries.push_back({ g.guestQueryFirst, counted, tag, g.slot, g.submissionSerial + 1,
                                 false, 0, g.currentScale });
    return false;
}

uint64_t vk::renderer::CompletedSubmission() { return CompletedSerial(); }

void vk::renderer::BeforeStreamWrite(uint32_t physical, uint32_t size)
{
    if (!g.device || !g.recording) return;
    if (!vk::textures::BoundInThisSegment(physical, size)) return;
    CloseSegment();
    g.streamWriteSegments++;
}

void vk::renderer::BeforeCompletion()
{
    g.completions++;
    if (!g.device || !g.recording || !g.segmentDraws) return;
    CloseSegment();
}

void vk::renderer::CollectOcclusionQueries(void (*deliver)(uint32_t tag, uint64_t samples))
{
    // In the order the title closed them; a later bracket is not handed over
    // before an earlier one.
    size_t done = 0;
    for (State::PendingQuery& query : g.pendingQueries)
    {
        if (!query.read && !Finished(query)) break;
        if (!query.read) Read(query);
        deliver(query.tag, query.samples);
        done++;
    }
    g.pendingQueries.erase(g.pendingQueries.begin(), g.pendingQueries.begin() + done);
}

#endif  // PES17_HAVE_VULKAN
