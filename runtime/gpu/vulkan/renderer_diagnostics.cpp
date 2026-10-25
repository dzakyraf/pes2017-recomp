// The switches that look at what the renderer drew -- frame and target dumps,
// the flash recorder, the traces -- and the report.
#include <cmath>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include "renderer_state.h"
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif
#include "../../diagnostics.h"
#include "capture.h"
#include "display_table.h"
#include "texture_cache.h"
#include "../../player.h"
#include "../../log.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#ifdef PES17_HAVE_VULKAN

namespace vk::renderer::detail
{
    // Which target each pass writes, and under what state, is what tells a depth
    // prepass from the world from the interface.
    uint64_t TracePasses()
    {
        static const uint64_t n = diag::Number("PES17_TRACE_PASSES");
        return n;
    }

    ShaderList::ShaderList(const char* variable)
    {
        for (const char* at = diag::Text(variable); at && *at; )
        {
            char* end = nullptr;
            const uint64_t hash = std::strtoull(at, &end, 16);
            if (end == at) break;
            hashes.push_back(hash);
            at = (*end == ',') ? end + 1 : end;
        }
    }

    const ShaderList& OnlyShaders()  { static const ShaderList l("PES17_ONLY_SHADER"); return l; }
    const ShaderList& SkipShaders()  { static const ShaderList l("PES17_SKIP_SHADER"); return l; }
    const ShaderList& TracedShaders() { static const ShaderList l("PES17_TRACE_SHADER_VERTS"); return l; }

    // The frame the traces start at. The draws before the menu are start-up,
    // which is not what a frame looks like.
    uint64_t TraceAfter()
    {
        static const uint64_t n = diag::Number("PES17_TRACE_RENDER_AFTER");
        return n;
    }

    // Where a trace should fire: standing where PES17_WALK_TO asked, or past the
    // frame PES17_TRACE_RENDER_AFTER names. Either is enough: the walk does not
    // always arrive.
    bool DiagnoseHere()
    {
        return (player::Walking() && player::Arrived()) ||
               (TraceAfter() && g.frames >= TraceAfter());
    }

    // The window shows the same image; this is how it is looked at when there
    // is no window, and the only way to compare two runs.
    const char* FrameDumpDirectory()
    {
        static const char* dir = env::Text("PES17_DUMP_FRAMES");
        return dir;
    }

    // Frame numbers are not comparable between runs: the same second of the
    // level lands on a different frame every time, and a dump taken at a fixed
    // frame is a dump of whatever happened to be on screen. A dump taken at a
    // fixed second of the same scripted run is the same moment.
    bool FrameDumpDue()
    {
        static const double after = env::Real("PES17_DUMP_FRAMES_AFTER_SECONDS");
        if (after <= 0.0) return true;
        static const std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start).count() >= after;
    }

    uint64_t FrameDumpEvery()
    {
        static const uint64_t n = std::max<uint64_t>(env::Number("PES17_DUMP_FRAME_EVERY", 600), 1);
        return n;
    }

    // How an image in `layout` is written and used, for the barriers either
    // side of a copy out of it.
    struct LayoutUse { VkAccessFlags writes, accesses; VkPipelineStageFlags stages; };
    LayoutUse UseOf(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return { VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return { VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };
        // A resolve: its copy's write was made visible to the shaders already.
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return { 0, VK_ACCESS_SHADER_READ_BIT, kShaderStages };
        // The frame's own copies and the display table's pass left it here.
        default:
            return { VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
        }
    }

    // RGBA pixels as a PNM in PES17_DUMP_FRAMES, saying how much of it is not black.
    void WriteDump(const char* name, const uint8_t* rgba, uint32_t width, uint32_t height)
    {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%s.pnm", FrameDumpDirectory(), name);
        std::FILE* file = std::fopen(path, "wb");
        if (!file) return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        const size_t pixels = size_t(width) * height;
        size_t lit = 0;
        for (size_t i = 0; i < pixels; i++)
        {
            std::fwrite(rgba + i * 4, 1, 3, file);
            lit += (rgba[i * 4] || rgba[i * 4 + 1] || rgba[i * 4 + 2]);
        }
        std::fclose(file);
        LOGI("renderer: wrote %s (%zu%% of pixels not black)", path, pixels ? lit * 100 / pixels : 0);
    }

    // Depth spends its whole range near one, so a linear ramp is a white page.
    // The picture is what the image holds, stretched over the range it uses.
    std::vector<uint8_t> DepthAsGrey(const uint8_t* texels, uint32_t width, uint32_t height,
                                     VkFormat format, const char* name)
    {
        const size_t pixels = size_t(width) * height;
        const bool packed = format == VK_FORMAT_D24_UNORM_S8_UINT ||
                            format == VK_FORMAT_X8_D24_UNORM_PACK32;
        std::vector<float> depth(pixels);
        float lo = 1e30f, hi = -1e30f;
        for (size_t i = 0; i < pixels; i++)
        {
            if (packed)
            {
                uint32_t bits;
                std::memcpy(&bits, texels + i * 4, 4);
                depth[i] = float(bits & 0xFFFFFF) / float(0xFFFFFF);
            }
            else std::memcpy(&depth[i], texels + i * 4, 4);
            lo = std::min(lo, depth[i]);
            hi = std::max(hi, depth[i]);
        }
        LOGI("renderer: %s depth spans %.6f..%.6f", name, lo, hi);
        const float span = hi > lo ? hi - lo : 1.0f;
        std::vector<uint8_t> grey(pixels * 4, 255);
        for (size_t i = 0; i < pixels; i++)
        {
            const uint8_t v = uint8_t(std::clamp((depth[i] - lo) / span, 0.0f, 1.0f) * 255.0f);
            grey[i * 4] = grey[i * 4 + 1] = grey[i * 4 + 2] = v;
        }
        return grey;
    }

    // Copies an image out and writes it, waiting for the copy. For the target
    // dumps, which are rare; the frame itself is copied inside its own command
    // buffer (RecordReadBack). `layout` is what the image is in and is left
    // in. A depth resolve is a depth/stencil image, and the colour aspect a
    // plain dump asks for does not exist on one: `aspect` says which to read
    // and `sourceFormat` how to turn what comes back into something to look at.
    void DumpImage(VkImage image, VkImageLayout layout, uint32_t width, uint32_t height,
                   const char* name, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                   VkFormat sourceFormat = VK_FORMAT_UNDEFINED)
    {
        const uint32_t bytes = width * height * 4;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &info, nullptr, &buffer) != VK_SUCCESS) return;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, buffer, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(g.device, &allocate, nullptr, &memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(g.device, buffer, nullptr);
            return;
        }
        vkBindBufferMemory(g.device, buffer, memory, 0);

        // Submitted from here, so after what the recorder thread still holds.
        vk::record::Drain();
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g.setup, &begin);
        // After whatever wrote the image in the layout it is in, even when that
        // is already TRANSFER_SRC, and back afterwards for what uses it there.
        const LayoutUse use = UseOf(layout);
        Barrier(g.setup, image, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                use.writes, VK_ACCESS_TRANSFER_READ_BIT, use.stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { aspect, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(g.setup, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &region);
        if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            Barrier(g.setup, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                    0, use.accesses, VK_PIPELINE_STAGE_TRANSFER_BIT, use.stages);
        vkEndCommandBuffer(g.setup);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g.setup;
        {
            std::lock_guard lock(vk::pipeline::QueueMutex());
            vkResetFences(g.device, 1, &g.setupFence);
            vkQueueSubmit(g.queue, 1, &submit, g.setupFence);
        }
        vk::pipeline::Failed(vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, UINT64_MAX),
                             "the set-up fence");

        void* mapped = nullptr;
        if (vkMapMemory(g.device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS)
        {
            const uint8_t* texels = static_cast<const uint8_t*>(mapped);
            if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
                WriteDump(name, DepthAsGrey(texels, width, height, sourceFormat, name).data(),
                          width, height);
            else
                WriteDump(name, texels, width, height);
            vkUnmapMemory(g.device, memory);
        }
        vkDestroyBuffer(g.device, buffer, nullptr);
        vkFreeMemory(g.device, memory, nullptr);
    }

    bool MakeReadBack()
    {
        if (g.readBack) return true;
        const VkDeviceSize bytes = VkDeviceSize(g.presentWidth) * g.presentHeight * 4;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &info, nullptr, &g.readBack) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, g.readBack, &needs);
        // Cached if the device has it: the host reads every byte of it.
        const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = FindMemory(needs.memoryTypeBits, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == UINT32_MAX) type = FindMemory(needs.memoryTypeBits, visible);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX ||
            vkAllocateMemory(g.device, &allocate, nullptr, &g.readBackMemory) != VK_SUCCESS ||
            vkBindBufferMemory(g.device, g.readBack, g.readBackMemory, 0) != VK_SUCCESS ||
            vkMapMemory(g.device, g.readBackMemory, 0, bytes, 0, &mapped) != VK_SUCCESS)
        {
            LOGW("renderer: no buffer to read frames back into");
            return false;
        }
        g.readBackMapped = static_cast<const uint8_t*>(mapped);
        return true;
    }

    // The rectangle x0,y0 to x1,y1 of an RGBA image `width` wide.
    void WritePnm(const char* dir, const char* name, const std::vector<uint8_t>& rgba,
                  uint32_t width, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%s.pnm", dir, name);
        std::FILE* file = std::fopen(path, "wb");
        if (!file) return;
        std::fprintf(file, "P6\n%u %u\n255\n", x1 - x0, y1 - y0);
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = x0; x < x1; x++)
                std::fwrite(&rgba[(size_t(y) * width + x) * 4], 1, 3, file);
        std::fclose(file);
    }

    // The flash recorder, for flashes a person sees: the last
    // PES17_FLASH_FRAMES presented frames are kept, and F7 writes them all to
    // PES17_FLASH_DIR/mark_NN, named by both frame counters so each can be put
    // beside the texture lines of the same frame. The key comes a few hundred
    // milliseconds after the flash, which is why it is the frames *before* it.
    //
    // Keeping them reads every frame back and waits for it, which changes the
    // timing a race depends on; PES17_FLASH_FRAMES=0 keeps only the marks.
    struct FlashRecorder
    {
        struct Kept
        {
            std::vector<uint8_t> pixels;
            uint64_t frame = 0, textureFrame = 0;
            char stamp[24]{};
        };
        std::vector<Kept> ring;
        size_t next = 0;
        uint32_t width = 0, height = 0;
        uint32_t marks = 0;
    };
    FlashRecorder g_flashes;
    std::atomic<uint32_t> g_flashesSeen{ 0 };

    uint32_t FlashFramesKept()
    {
        static const uint32_t keep = uint32_t(diag::Number("PES17_FLASH_FRAMES"));
        return keep;
    }

    const char* FlashDirectory()
    {
        static const char* dir = diag::Text("PES17_FLASH_DIR") ?: "diagnosis/flashes";
        return dir;
    }

    void KeepFlashFrame(std::vector<uint8_t>&& rgba, uint32_t width, uint32_t height,
                        uint64_t frame)
    {
        FlashRecorder& f = g_flashes;
        if (rgba.size() < size_t(width) * height * 4) return;
        if (width != f.width || height != f.height)
        {
            f.ring.clear();
            f.next = 0;
            f.width = width;
            f.height = height;
        }
        if (f.ring.size() < FlashFramesKept()) f.ring.emplace_back();
        FlashRecorder::Kept& kept = f.ring[f.next];
        f.next = (f.next + 1) % FlashFramesKept();
        kept.pixels = std::move(rgba);
        kept.frame = frame;
        kept.textureFrame = vk::textures::FrameNumber();
        std::snprintf(kept.stamp, sizeof kept.stamp, "%s", pes17log::Stamp());
    }


    // The draw list kept beside the flash frames (see DrawRecord). Only while a
    // flash hunt keeps frames: every draw costs a record and a few hashes.
    struct DrawFrame
    {
        uint64_t frame = 0;
        bool world = false;
        std::vector<DrawRecord> draws;
        std::vector<DrawTexture> textures;
        std::vector<uint64_t> lateRestages;   // re-staged after the frame's draws were recorded
    };
    struct DrawLog
    {
        DrawFrame open;
        std::deque<DrawFrame> done;
        uint64_t lastVertexData = 0;
        uint32_t lastVertexAddress = 0;
    };
    DrawLog g_drawLog;

    bool RecordingDraws()
    {
        static const bool on = FlashFramesKept() > 0 &&
                               (diag::Flag("PES17_FLASH_HUNT") || diag::Flag("PES17_FLASH_DRAWS"));
        return on;
    }

    // Eight bytes at a time; a fingerprint, not a checksum anyone relies on.
    uint64_t Mix(uint64_t hash, const void* data, size_t size)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        hash ^= size * 0x9E3779B97F4A7C15ull;
        size_t at = 0;
        for (; at + 8 <= size; at += 8)
        {
            uint64_t word;
            std::memcpy(&word, bytes + at, 8);
            hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
            hash ^= hash >> 32;
        }
        for (; at < size; at++) hash = (hash ^ bytes[at]) * 0x100000001B3ull;
        return hash;
    }

    // A long window by its ends and its length: enough to tell another buffer
    // or a rewritten one, without hashing twenty megabytes a frame.
    uint64_t Sample(uint64_t hash, const uint8_t* bytes, size_t size)
    {
        constexpr size_t kEnd = 256;
        if (size <= 2 * kEnd) return Mix(hash, bytes, size);
        hash = Mix(hash, bytes, kEnd);
        hash = Mix(hash, bytes + size / 2 - kEnd / 2, kEnd);
        return Mix(hash, bytes + size - kEnd, kEnd) ^ size;
    }

    uint64_t WindowHash(const gpu::RegisterFile& r, uint32_t windowRegister, uint32_t needed)
    {
        const gpu::ConstantWindow window{ r[windowRegister] };
        const uint32_t base = std::min(window.Base(), 256u);
        const uint32_t first = gpu::kConstantBaseAlu + base * 4;
        return Mix(base, &r.values[first], size_t(std::min(needed, 512u - base)) * 16);
    }

    void WriteDrawFrame(const std::string& dir, const DrawFrame& f)
    {
        char name[640];
        std::snprintf(name, sizeof name, "%s/draws_r%06llu.txt", dir.c_str(),
                      (unsigned long long)f.frame);
        FILE* out = std::fopen(name, "w");
        if (!out) return;
        std::fprintf(out, "frame %llu world %d draws %zu\n", (unsigned long long)f.frame,
                     f.world ? 1 : 0, f.draws.size());
        std::fprintf(out, "late-restaged");
        for (uint64_t id : f.lateRestages) std::fprintf(out, " %llx", (unsigned long long)id);
        std::fprintf(out, "\n");
        for (size_t i = 0; i < f.draws.size(); i++)
        {
            const DrawRecord& d = f.draws[i];
            std::fprintf(out,
                         "%zu vs=%016llx ps=%016llx tgt=%u/%u/%u/%u prim=%u n=%u va=%08x ia=%08x"
                         " dc=%08x bl=%08x cm=%08x mc=%08x cc=%08x"
                         " vc=%016llx pc=%016llx fc=%016llx vd=%016llx ix=%016llx",
                         i, (unsigned long long)d.vertexShader, (unsigned long long)d.pixelShader,
                         d.colourTile, d.pitch, d.samples, d.depthTile, d.primitive, d.count,
                         d.vertexAddress, d.indexAddress,
                         d.depthControl, d.blend, d.colourMask, d.modeCntl, d.colourControl,
                         (unsigned long long)d.vertexConstants, (unsigned long long)d.pixelConstants,
                         (unsigned long long)d.fetchConstants, (unsigned long long)d.vertexData,
                         (unsigned long long)d.indices);
            for (uint32_t t = 0; t < d.textureCount; t++)
            {
                const DrawTexture& x = f.textures[d.textureFirst + t];
                std::fprintf(out, " t%u=%c:%08x:%u:%ux%u:%llx:v%u", x.slot,
                             x.resolved ? 'R' : 'T', x.address, x.format, x.width, x.height,
                             (unsigned long long)x.id, x.version);
            }
            if (d.skipped) std::fprintf(out, " SKIPPED=\"%s\"", d.skipped);
            std::fprintf(out, "\n");
        }
        std::fclose(out);
    }

    // On the render thread, the frame after the key.
    void WriteFlashes()
    {
        const uint32_t seen = g_flashesSeen.load(std::memory_order_relaxed);
        FlashRecorder& f = g_flashes;
        if (seen == f.marks) return;
        f.marks = seen;
        player::Where here{};
        const bool located = player::Locate(here);
        LOGW("FLASH MARK %u: now at renderer frame %llu, texture frame %llu%s", seen,
             (unsigned long long)g.frames, (unsigned long long)vk::textures::FrameNumber(),
             located ? [&] { static char b[96];
                             std::snprintf(b, sizeof b, ", standing at %.1f %.1f %.1f yaw %.1f",
                                           here.x, here.y, here.z, here.yaw);
                             return b; }()
                     : "");
        if (f.ring.empty()) return;
        // Oldest first, written off this thread: a second of frames is a few
        // hundred megabytes and the game should not stop for it.
        std::vector<FlashRecorder::Kept> frames;
        frames.reserve(f.ring.size());
        for (size_t i = 0; i < f.ring.size(); i++)
            frames.push_back(std::move(f.ring[(f.next + i) % f.ring.size()]));
        f.ring.clear();
        f.next = 0;
        char dir[512];
        std::snprintf(dir, sizeof dir, "%s/mark_%02u", FlashDirectory(), seen);
        std::error_code ignored;
        std::filesystem::create_directories(dir, ignored);
        LOGW("FLASH MARK %u: writing %zu frames, renderer %llu..%llu, texture %llu..%llu, to %s",
             seen, frames.size(), (unsigned long long)frames.front().frame,
             (unsigned long long)frames.back().frame,
             (unsigned long long)frames.front().textureFrame,
             (unsigned long long)frames.back().textureFrame, dir);
        // The draw lists of the same frames, taken now: the ring moves on.
        std::vector<DrawFrame> draws;
        for (DrawFrame& d : g_drawLog.done)
            if (d.frame >= frames.front().frame && d.frame <= frames.back().frame)
                draws.push_back(std::move(d));
        g_drawLog.done.clear();
        std::thread([frames = std::move(frames), draws = std::move(draws), path = std::string(dir),
                     width = f.width, height = f.height] {
            for (const DrawFrame& d : draws) WriteDrawFrame(path, d);
            for (const FlashRecorder::Kept& kept : frames)
            {
                char name[96];
                std::snprintf(name, sizeof name, "r%06llu_t%06llu",
                              (unsigned long long)kept.frame,
                              (unsigned long long)kept.textureFrame);
                WritePnm(path.c_str(), name, kept.pixels, width, 0, 0, width, height);
            }
        }).detach();
    }

    // Kept as text because the report is written after the images are gone.
    void SummariseTargets()
    {
        g.targetSummary.clear();
        for (const auto& [key, target] : g.targets)
        {
            if (!target.depth && !target.draws && !target.presented) continue;
            char line[224];
            std::snprintf(line, sizeof line,
                          target.depth
                            ? "%ux%u DEPTH at tile %u, pitch %u, format %u, %ux MSAA drawn %ux:"
                              " %llu draws in %llu of %llu frames, presented %llu times"
                            : "%ux%u colour at tile %u, pitch %u, format %u, %ux MSAA drawn %ux:"
                              " %llu draws in %llu of %llu frames, presented %llu times",
                          target.width, target.height, key.baseTile, key.pitch, target.guestFormat,
                          1u << key.samples, unsigned(target.samples), (unsigned long long)target.draws,
                          (unsigned long long)target.framesWithColour,
                          (unsigned long long)g.frames,
                          (unsigned long long)target.presented);
            g.targetSummary.push_back(line);
        }
    }

    // PES17_DUMP_TARGETS: every colour target, depth target and resolve destination
    // beside the frame. A piece of world that does not appear was either never
    // submitted or rejected, and only the targets tell the two apart.
    void DumpTargets()
    {
        char name[96];
        for (const auto& [key, target] : g.targets)
        {
            if (target.depth || !target.draws) continue;
            // A multisampled target is dumped as its twin: the last rectangle
            // resolved out of it, which for the world is the whole frame.
            if (target.samples > 1 && !target.singleReady) continue;
            std::snprintf(name, sizeof name, "frame_%06llu_tile%u_fmt%u_%ux%u_%ux",
                          (unsigned long long)g.frames, key.baseTile, target.guestFormat,
                          target.width, target.height, 1u << key.samples);
            DumpImage(target.samples > 1 ? target.singleImage : target.image,
                      target.samples > 1 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      HostOf(target.width, target.scale), HostOf(std::min(target.height, 1024u), target.scale), name);
        }

        // A multisampled depth image cannot be copied out, so it is resolved into
        // its twin first -- and that resolve is a render pass, so it has to be
        // recorded and submitted before the copy reads what it wrote.
        struct Shot { VkImage image; uint32_t tile, samples; Target* target; };
        std::vector<Shot> shots;
        for (auto& [key, target] : g.targets)
        {
            if (!target.depth || !target.draws) continue;
            if (!BeginFrame()) break;
            const uint32_t tall = std::min(target.height, 1024u);
            VkImage image = BeginReadingDepth(target, { 0, 0 }, target.width, tall);
            if (image) shots.push_back({ image, key.baseTile, key.samples, &target });
        }
        if (!shots.empty()) Submit(true);
        for (const Shot& shot : shots)
        {
            const uint32_t tall = std::min(shot.target->height, 1024u);
            std::snprintf(name, sizeof name, "frame_%06llu_depth_tile%u_%ux%u_%ux",
                          (unsigned long long)g.frames, shot.tile,
                          shot.target->width, tall, 1u << shot.samples);
            DumpImage(shot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      HostOf(shot.target->width, shot.target->scale), HostOf(tall, shot.target->scale), name,
                      VK_IMAGE_ASPECT_DEPTH_BIT, g.depthFormat);
        }
        for (const Shot& shot : shots)
            if (BeginFrame()) EndReadingDepth(*shot.target);

        for (const auto& [address, ring] : g.resolvedTo)
        {
            const Resolved& resolved = ring.copies[ring.newest];
            if (!resolved.image) continue;
            std::snprintf(name, sizeof name, "frame_%06llu_resolve_%08X%s",
                          (unsigned long long)g.frames, address,
                          resolved.depth ? "_depth" : "");
            DumpImage(resolved.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      HostOf(resolved.width, resolved.scale), HostOf(resolved.height, resolved.scale), name,
                      resolved.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                      resolved.format);
        }
    }

    // ---- the black-frame probe (see NoteSwapSource) ----
    namespace
    {
        struct BlackProbe
        {
            uint32_t ringSlot = 0, presentSlot = 0;
            int64_t age = 0;
            uint64_t frames = 0, black = 0, loggedBlack = 0;
            uint64_t blackByRing[kFrameSlots]{}, framesByRing[kFrameSlots]{};
            uint64_t blackByPresent[kPresentImages]{};
            uint64_t blackByAge[4]{}, framesByAge[4]{};   // age -1 (never), 0, 1, 2 or more
            uint64_t blackByMod3[3]{};
        } g_probe;

        bool BlackProbeOn()
        {
#ifdef PES17_ANDROID
            static std::atomic<uint32_t> asked{ 0 };
            static std::atomic<bool> on{ false };
            if (asked++ % 30 == 0)
                on = access((android::GetPaths().external + "/black_probe").c_str(), F_OK) == 0;
            return on;
#else
            static const bool on = env::Flag("PES17_BLACK_PROBE");
            return on;
#endif
        }

        uint32_t AgeBucket(int64_t age) { return age < 0 ? 0 : age == 0 ? 1 : age == 1 ? 2 : 3; }
    }

    void NoteSwapSource(uint32_t ringSlot, int64_t age, uint32_t presentSlot)
    {
        g_probe.ringSlot = ringSlot; g_probe.age = age; g_probe.presentSlot = presentSlot;
    }

    // `frame` is the finished present image, RGBA. Black when a spread of samples has nothing above 6.
    void CountBlackFrame(const std::vector<uint8_t>& frame)
    {
        bool black = true;
        const size_t pixels = frame.size() / 4;
        for (size_t i = 0; i < pixels && black; i += 211)
            if (frame[i * 4] > 6 || frame[i * 4 + 1] > 6 || frame[i * 4 + 2] > 6) black = false;
        BlackProbe& p = g_probe;
        p.frames++;
        p.framesByRing[p.ringSlot % kFrameSlots]++;
        p.framesByAge[AgeBucket(p.age)]++;
        if (black)
        {
            p.black++;
            p.blackByRing[p.ringSlot % kFrameSlots]++;
            p.blackByAge[AgeBucket(p.age)]++;
            p.blackByPresent[p.presentSlot % kPresentImages]++;
            p.blackByMod3[g.frames % 3]++;
            if (p.loggedBlack++ < 60)
                LOGW("probe: black frame %llu (swap %llu): resolve ring slot %u, source last resolved %lld frames ago,"
                     " present image %u, frame mod 3 = %llu", (unsigned long long)p.black,
                     (unsigned long long)g.frames, p.ringSlot, (long long)p.age, p.presentSlot,
                     (unsigned long long)(g.frames % 3));
        }
        if (p.frames % 300 == 0)
            LOGI("probe: %llu of the last %llu frames black; by ring slot %llu/%llu/%llu of %llu/%llu/%llu; by source age"
                 " (never/0/1/2+) %llu/%llu/%llu/%llu of %llu/%llu/%llu/%llu; by present image %llu/%llu/%llu/%llu;"
                 " by frame mod 3 %llu/%llu/%llu",
                 (unsigned long long)p.black, (unsigned long long)p.frames,
                 (unsigned long long)p.blackByRing[0], (unsigned long long)p.blackByRing[1], (unsigned long long)p.blackByRing[2],
                 (unsigned long long)p.framesByRing[0], (unsigned long long)p.framesByRing[1], (unsigned long long)p.framesByRing[2],
                 (unsigned long long)p.blackByAge[0], (unsigned long long)p.blackByAge[1], (unsigned long long)p.blackByAge[2],
                 (unsigned long long)p.blackByAge[3], (unsigned long long)p.framesByAge[0], (unsigned long long)p.framesByAge[1],
                 (unsigned long long)p.framesByAge[2], (unsigned long long)p.framesByAge[3],
                 (unsigned long long)p.blackByPresent[0], (unsigned long long)p.blackByPresent[1],
                 (unsigned long long)p.blackByPresent[2], (unsigned long long)p.blackByPresent[3],
                 (unsigned long long)p.blackByMod3[0], (unsigned long long)p.blackByMod3[1], (unsigned long long)p.blackByMod3[2]);
    }

    FrameReadBacks PlanReadBacks()
    {
        // The frame and target dumps (PES17_DUMP_FRAMES, PES17_DUMP_TARGETS) are in
        // every build, read with env:: -- a null check a frame until the switch is set.
        FrameReadBacks plan;
        plan.dump = FrameDumpDirectory() && ((g.frames + 1) % FrameDumpEvery()) == 0 &&
                    FrameDumpDue();
        plan.keep = FlashFramesKept() > 0;
        plan.probe = BlackProbeOn();
        return plan;
    }

    void RecordReadBack(VkImage present)
    {
        if (!MakeReadBack()) return;
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { g.presentWidth, g.presentHeight, 1 };
        VkBufferMemoryBarrier toHost{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = g.readBack;
        toHost.size = VK_WHOLE_SIZE;
        Record([=](VkCommandBuffer command) {
            vkCmdCopyImageToBuffer(command, present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   toHost.buffer, 1, &region);
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);
        });
    }

    void ExamineFrame(bool worldDrawn, const FrameReadBacks& plan)
    {
        if (TracePasses() && DiagnoseHere()) g.passFrames++;
        std::vector<uint8_t> frame;
        if (plan.Any() && g.readBackMapped)
            frame.assign(g.readBackMapped,
                         g.readBackMapped + size_t(g.presentWidth) * g.presentHeight * 4);

        if (plan.probe && !frame.empty()) CountBlackFrame(frame);

        static const bool onlyWorld = env::Flag("PES17_DUMP_WORLD_FRAMES");
        static const bool targetsToo = env::Flag("PES17_DUMP_TARGETS");
        if (plan.dump && !frame.empty() && (!onlyWorld || worldDrawn))
        {
            char name[64];
            std::snprintf(name, sizeof name, "frame_%06llu", (unsigned long long)g.frames);
            WriteDump(name, frame.data(), g.presentWidth, g.presentHeight);
            if (targetsToo) DumpTargets();
        }
        if (plan.keep && !frame.empty())
            KeepFlashFrame(std::move(frame), g.presentWidth, g.presentHeight, g.frames);
        // PES17_FLASH_MARK_AT=<seconds>: a mark nobody pressed, to test the
        // recorder in a run with no one watching.
        static const double markAt = diag::Real("PES17_FLASH_MARK_AT", 0.0);
        static const auto started = std::chrono::steady_clock::now();
        static bool marked = false;
        if (markAt > 0 && !marked &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= markAt)
        {
            marked = true;
            vk::renderer::FlashSeen("PES17_FLASH_MARK_AT");
        }
        WriteFlashes();
    }

    // The bytes of a whole colour image, any texel size (DumpImage assumes 4).
    std::vector<uint8_t> ReadColourImage(VkImage image, VkImageLayout layout, uint32_t width,
                                         uint32_t height, uint32_t texelBytes,
                                         VkImageAspectFlags aspect)
    {
        std::vector<uint8_t> out;
        const VkDeviceSize bytes = VkDeviceSize(width) * height * texelBytes;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &info, nullptr, &buffer) != VK_SUCCESS) return out;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, buffer, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(g.device, &allocate, nullptr, &memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(g.device, buffer, nullptr);
            return out;
        }
        vkBindBufferMemory(g.device, buffer, memory, 0);
        vk::record::Drain();
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g.setup, &begin);
        const LayoutUse use = UseOf(layout);
        Barrier(g.setup, image, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                use.writes, VK_ACCESS_TRANSFER_READ_BIT, use.stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { aspect, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(g.setup, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        Barrier(g.setup, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                0, use.accesses, VK_PIPELINE_STAGE_TRANSFER_BIT, use.stages);
        vkEndCommandBuffer(g.setup);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g.setup;
        {
            std::lock_guard lock(vk::pipeline::QueueMutex());
            vkResetFences(g.device, 1, &g.setupFence);
            vkQueueSubmit(g.queue, 1, &submit, g.setupFence);
        }
        vk::pipeline::Failed(vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, UINT64_MAX), "the set-up fence");
        void* mapped = nullptr;
        if (vkMapMemory(g.device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS)
        {
            out.assign(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + bytes);
            vkUnmapMemory(g.device, memory);
        }
        vkDestroyBuffer(g.device, buffer, nullptr);
        vkFreeMemory(g.device, memory, nullptr);
        return out;
    }

    // `touch files/dump_frame`, at the end of the dumped frame: five texels of
    // every colour target and resolve copy (centre and the quarter points), as
    // floats -- whether the HDR scene, its copies or the exposure hold what a
    // screenshot of the composite cannot tell apart.
    // The copy each resolve destination held when the dumped frame ended: the
    // samples are read a frame later, once that frame has gone to the GPU, and
    // by then the ring has moved on.
    std::unordered_map<uint32_t, uint32_t> g_sampleSlots;
    void SnapshotForSamples()
    {
        g_sampleSlots.clear();
        for (const auto& [address, ring] : g.resolvedTo) g_sampleSlots[address] = ring.newest;
    }

    void LogTargetSamples()
    {
        const auto half = [](uint16_t h) {
            const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
            float v = exp == 0 ? std::ldexp(float(man), -24)
                    : exp == 31 ? (man ? NAN : INFINITY)
                    : std::ldexp(float(man | 1024), int(exp) - 25);
            return sign ? -v : v;
        };
        const auto describe = [&](const char* what, VkImage image, VkImageLayout layout, VkFormat format,
                                  uint32_t width, uint32_t height, float scale) {
            uint32_t texel = 0;
            switch (format)
            {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: texel = 4; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: texel = 8; break;
            default: LOGW("DUMP   samples %s: vk format %d not read", what, int(format)); return;
            }
            const uint32_t w = HostOf(width, scale), h = HostOf(height, scale);
            const std::vector<uint8_t> bytes = ReadColourImage(image, layout, w, h, texel);
            if (bytes.empty()) return;
            std::string line;
            const uint32_t points[5][2] = { { w / 2, h / 2 }, { w / 4, h / 4 }, { 3 * w / 4, h / 4 },
                                            { w / 4, 3 * h / 4 }, { 0, 0 } };
            for (const auto& p : points)
            {
                const uint8_t* t = &bytes[(size_t(p[1]) * w + p[0]) * texel];
                char item[96];
                if (format == VK_FORMAT_R8G8B8A8_UNORM)
                    std::snprintf(item, sizeof item, " [%u %u %u %u]", t[0], t[1], t[2], t[3]);
                else if (format == VK_FORMAT_R16G16B16A16_SFLOAT)
                {
                    uint16_t c[4]; std::memcpy(c, t, 8);
                    std::snprintf(item, sizeof item, " [%.3g %.3g %.3g %.3g]", half(c[0]), half(c[1]), half(c[2]), half(c[3]));
                }
                else if (format == VK_FORMAT_R32_SFLOAT)
                {
                    float f; std::memcpy(&f, t, 4);
                    std::snprintf(item, sizeof item, " [%.7g]", f);
                }
                else
                {
                    uint32_t v; std::memcpy(&v, t, 4);
                    std::snprintf(item, sizeof item, " [%08X]", v);
                }
                line += item;
            }
            LOGW("DUMP   samples %s %ux%u vk fmt %d:%s", what, width, height, int(format), line.c_str());
            // The whole image as a picture, for the big ones (files/frame_images/).
            if (uint64_t(w) * h >= 32768)
            {
#ifdef PES17_ANDROID
                const std::string dir = android::GetPaths().external + "/frame_images";
                mkdir(dir.c_str(), 0775);
                std::string file = what;
                for (char& c : file) if (c == ' ') c = '_';
                if (std::FILE* out = std::fopen((dir + "/" + file + ".pnm").c_str(), "wb"))
                {
                    std::fprintf(out, "P6\n%u %u\n255\n", w, h);
                    std::vector<uint8_t> row(size_t(w) * 3);
                    // The alpha channel as a picture of its own (<name>_a.pgm): the PNM above drops it, and
                    // the crowd's atlas keeps what is a sprite and what is not in it.
                    std::vector<uint8_t> alpha(format == VK_FORMAT_R8G8B8A8_UNORM ? size_t(w) * h : 0);
                    for (uint32_t y = 0; y < h; y++)
                    {
                        for (uint32_t x = 0; x < w; x++)
                        {
                            const uint8_t* t = &bytes[(size_t(y) * w + x) * texel];
                            if (!alpha.empty()) alpha[size_t(y) * w + x] = t[3];
                            for (int c = 0; c < 3; c++)
                            {
                                float v = 0;
                                if (format == VK_FORMAT_R8G8B8A8_UNORM) v = t[c] / 255.0f;
                                else if (format == VK_FORMAT_R16G16B16A16_SFLOAT)
                                { uint16_t hv; std::memcpy(&hv, t + c * 2, 2); v = half(hv); }
                                else if (format == VK_FORMAT_R32_SFLOAT) std::memcpy(&v, t, 4);
                                row[x * 3 + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                            }
                        }
                        std::fwrite(row.data(), 1, row.size(), out);
                    }
                    std::fclose(out);
                    if (!alpha.empty())
                        if (std::FILE* a = std::fopen((dir + "/" + file + "_a.pgm").c_str(), "wb"))
                        {
                            std::fprintf(a, "P5\n%u %u\n255\n", w, h);
                            std::fwrite(alpha.data(), 1, alpha.size(), a);
                            std::fclose(a);
                        }
                }
#endif
            }
        };
        char name[96];
        for (const auto& [key, target] : g.targets)
        {
            if (!target.depth || !target.draws || target.samples > 1) continue;
            const uint32_t w = HostOf(target.width, target.scale), h = HostOf(std::min(target.height, 1024u), target.scale);
            const std::vector<uint8_t> bytes = ReadColourImage(target.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                               w, h, 4, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (bytes.empty()) continue;
            std::string line;
            const uint32_t points[5][2] = { { w / 2, h / 2 }, { w / 4, h / 4 }, { 3 * w / 4, h / 4 },
                                            { w / 4, 3 * h / 4 }, { 0, 0 } };
            for (const auto& p : points)
            {
                uint32_t v; std::memcpy(&v, &bytes[(size_t(p[1]) * w + p[0]) * 4], 4);
                char item[48];
                if (target.format == VK_FORMAT_D24_UNORM_S8_UINT)
                    std::snprintf(item, sizeof item, " [%.5f]", (v & 0xFFFFFF) / 16777215.0);
                else { float f; std::memcpy(&f, &v, 4); std::snprintf(item, sizeof item, " [%.5f]", f); }
                line += item;
            }
            LOGW("DUMP   samples depth tile %u pitch %u x%u %ux%u vk fmt %d:%s", key.baseTile, key.pitch,
                 key.samples, target.width, target.height, int(target.format), line.c_str());
        }
        for (const auto& [key, target] : g.targets)
        {
            if (target.depth || !target.draws || target.samples > 1) continue;
            std::snprintf(name, sizeof name, "target tile %u pitch %u fmt %u", key.baseTile, key.pitch, target.guestFormat);
            describe(name, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, target.format,
                     target.width, std::min(target.height, 1024u), target.scale);
        }
        for (const auto& [address, ring] : g.resolvedTo)
        {
            const auto slot = g_sampleSlots.find(address);
            const uint32_t newest = slot != g_sampleSlots.end() ? slot->second : ring.newest;
            for (const Resolved* r : { &ring.copies[newest], &ring.exact[newest] })
            {
                if (!r->image || r->depth) continue;
                std::snprintf(name, sizeof name, "resolve %08X%s", address, r == &ring.exact[newest] ? " corner" : "");
                describe(name, r->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r->format, r->width, r->height, r->scale);
            }
        }
    }
}


namespace vk::renderer::detail
{
    DrawRecord* BeginDrawRecord(const gpu::RegisterFile& r, uint64_t vertexShader,
                                uint64_t pixelShader, uint32_t primitive, uint32_t count,
                                uint32_t indexAddress)
    {
        if (!RecordingDraws()) return nullptr;
        DrawRecord& d = g_drawLog.open.draws.emplace_back();
        d.vertexShader = vertexShader;
        d.pixelShader = pixelShader;
        d.primitive = primitive;
        d.count = count;
        d.indexAddress = indexAddress;
        const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
        d.colourTile = gpu::ColorInfo{ r[gpu::RB_COLOR_INFO] }.BaseTile();
        d.depthTile = gpu::DepthInfo{ r[gpu::RB_DEPTH_INFO] }.BaseTile();
        d.pitch = surface.Pitch();
        d.samples = surface.MsaaSamples();
        d.depthControl = r[gpu::RB_DEPTHCONTROL];
        d.blend = r[gpu::RB_BLENDCONTROL0];
        d.colourMask = r[gpu::RB_COLOR_MASK];
        d.modeCntl = r[gpu::PA_SU_SC_MODE_CNTL];
        d.colourControl = r[gpu::RB_COLORCONTROL];
        d.fetchConstants = Mix(0, &r.values[gpu::kConstantBaseFetch], 32 * 6 * 4);
        d.textureFirst = uint32_t(g_drawLog.open.textures.size());
        return &d;
    }

    void NoteDrawTexture(uint32_t slot, const gpu::TextureFetch& fetch, uint64_t id, bool resolved)
    {
        if (!g.drawRecord) return;
        g_drawLog.open.textures.push_back({ slot, fetch.BaseAddress(), fetch.Format(),
                                            fetch.Width(), fetch.Height(), id,
                                            resolved ? 0u : vk::textures::Version(id), resolved });
        g.drawRecord->textureCount++;
    }

    void NoteVertexBuffer(uint32_t physical)
    {
        if (g.drawRecord && !g.drawRecord->vertexAddress) g.drawRecord->vertexAddress = physical;
    }

    void NoteVertexWindow(const uint8_t* bytes, size_t size)
    {
        if (!g.drawRecord) return;
        if (!bytes)
        {
            g.drawRecord->vertexData = g_drawLog.lastVertexData;
            g.drawRecord->vertexAddress = g_drawLog.lastVertexAddress;
            return;
        }
        g.drawRecord->vertexData = Sample(g.drawRecord->vertexData, bytes, size);
        g_drawLog.lastVertexData = g.drawRecord->vertexData;
        g_drawLog.lastVertexAddress = g.drawRecord->vertexAddress;
    }

    void NoteConstantWindows(const gpu::RegisterFile& r, uint32_t vertexNeeded, uint32_t pixelNeeded)
    {
        if (!g.drawRecord) return;
        g.drawRecord->vertexConstants = WindowHash(r, gpu::SQ_VS_CONST, vertexNeeded);
        g.drawRecord->pixelConstants = WindowHash(r, gpu::SQ_PS_CONST, pixelNeeded);
    }

    void NoteIndices(const uint8_t* bytes, size_t size)
    {
        if (g.drawRecord) g.drawRecord->indices = Sample(0, bytes, size);
    }

    void EndFrameDraws(uint64_t frame, bool worldDrawn)
    {
        // The frame's late re-stages are taken every frame, kept or not.
        if (!RecordingDraws())
        {
            std::vector<uint64_t> unkept;
            vk::textures::TakeLateRestages(unkept);
            return;
        }
        DrawFrame& f = g_drawLog.open;
        f.frame = frame;
        f.world = worldDrawn;
        vk::textures::TakeLateRestages(f.lateRestages);
        g_drawLog.done.push_back(std::move(f));
        g_drawLog.open = DrawFrame{};
        while (g_drawLog.done.size() > FlashFramesKept() + 4) g_drawLog.done.pop_front();
    }
}

using namespace vk::renderer::detail;

// From the window thread on F7, or the input poll on the pad's Y. Logged here,
// at the moment of the press, so the time on the line is the time the person
// reacted. Any thread: a count and a line.
void vk::renderer::FlashSeen(const char* how)
{
    const uint32_t n = g_flashesSeen.fetch_add(1, std::memory_order_relaxed) + 1;
    LOGW("FLASH MARK %u: %s pressed", n, how);
}

void vk::renderer::Report()
{
    if constexpr (!diag::kOn) return;
    vk::capture::Report();
    vk::display::Report();
    if (!g.draws && !g.frames) return;
    if (g.device) { g.pipelineCount = g.pipelines.size(); g.shaderCount = g.shaders.size(); }
    LOGI("renderer: %llu frames, %llu of %llu draws recorded, %llu resolves (%llu of depth),"
         " %zu pipelines over %zu shaders",
         (unsigned long long)g.frames, (unsigned long long)g.drawsRecorded,
         (unsigned long long)g.draws, (unsigned long long)g.resolves,
         (unsigned long long)g.depthResolves, g.pipelineCount, g.shaderCount);
    if (g.drawNanoseconds || g.submitNanoseconds)
        LOGI("renderer: %llu ms in Draw (%llu ms of it binding textures), %llu ms in Resolve,"
             " %llu ms in Submit, %llu ms waiting for the queue",
             (unsigned long long)(g.drawNanoseconds / 1000000),
             (unsigned long long)(g.textureNanoseconds / 1000000),
             (unsigned long long)(g.resolveNanoseconds / 1000000),
             (unsigned long long)(g.submitNanoseconds / 1000000),
             (unsigned long long)(g.fenceNanoseconds / 1000000));
    if (g.constantsNanoseconds || g.recordNanoseconds)
        LOGI("renderer: of that, %llu ms on targets and passes, %llu ms on indices,"
             " %llu ms assembling constants, %llu ms on pipeline and texture state"
             " (%llu ms of it choosing pipelines), %llu ms recording commands",
             (unsigned long long)(g.setupNanoseconds / 1000000),
             (unsigned long long)(g.indexNanoseconds / 1000000),
             (unsigned long long)(g.constantsNanoseconds / 1000000),
             (unsigned long long)(g.stateNanoseconds / 1000000),
             (unsigned long long)(g.pipelineNanoseconds / 1000000),
             (unsigned long long)(g.recordNanoseconds / 1000000));
    stutters::Report();
    ReportPreparing();
    if (g.worldFrames > 1)
    {
        const double seconds =
            std::chrono::duration<double>(g.lastWorldAt - g.firstWorldAt).count();
        LOGI("renderer: %llu world frames in the %.1f s from the start of play to the last:"
             " %.1f a second in play", (unsigned long long)g.worldFrames, seconds,
             seconds > 0 ? double(g.worldFrames - 1) / seconds : 0.0);
    }
    if (g.submits)
        LOGI("renderer: %llu submissions, %llu us of waiting each, in %llu segments more:"
             " %llu at the %llu points the command processor reported work done, %llu"
             " because the image pool was about to copy over a texture the segment samples",
             (unsigned long long)g.submits,
             (unsigned long long)(g.fenceNanoseconds / g.submits / 1000),
             (unsigned long long)g.segments,
             (unsigned long long)(g.segments - g.streamWriteSegments),
             (unsigned long long)g.completions, (unsigned long long)g.streamWriteSegments);
    vk::record::Report();
    if (g.prewarmWanted)
        LOGI("renderer: %u of %u pipelines were ready before the first draw (%llu us)",
             g.prewarmed, g.prewarmWanted, (unsigned long long)g.prewarmMicroseconds);
    LOGI("renderer: %llu ms translating shaders, %llu ms compiling pipelines"
         " (%llu us each)",
         (unsigned long long)(g.translateMicroseconds / 1000),
         (unsigned long long)(g.compileMicroseconds / 1000),
         (unsigned long long)(g.pipelineCount ? g.compileMicroseconds / g.pipelineCount : 0));
    for (const std::string& line : g.targetSummary) LOGI("renderer: %s", line.c_str());
    if (g.resolveImagesMade)
        LOGI("renderer: %llu resolve destination images made (each after the first costs"
             " a device wait)", (unsigned long long)g.resolveImagesMade);
    if (g.frames)
    {
        // Per frame, because the arena is per frame: what matters is whether a
        // frame's copies fit, and which of the three does not.
        LOGI("renderer: frame arena peak %u KB of %u KB, per frame:",
             g.arenaPeak >> 10, g.arenaBytes >> 10);
        for (uint32_t kind = 0; kind < kArenaKinds; kind++)
            LOGI("           %-12s %6llu KB copied, %6llu KB reused",
                 kArenaNames[kind],
                 (unsigned long long)(g.arenaWritten[kind] / g.frames >> 10),
                 (unsigned long long)(g.arenaReused[kind] / g.frames >> 10));
    }
    if (g.vertsMatched)
        LOGI("renderer: %llu draws ran the shader PES17_TRACE_SHADER_VERTS named,"
             " %llu of them where the dump was asked for",
             (unsigned long long)g.vertsMatched, (unsigned long long)g.vertsTraced);
    if (g.arenaOverflows)
        LOGI("renderer: the frame arena overflowed %llu times",
             (unsigned long long)g.arenaOverflows);
    std::vector<std::pair<uint64_t, const char*>> skipped;
    for (const auto& [why, count] : g.skipped) skipped.push_back({ count, why });
    std::sort(skipped.rbegin(), skipped.rend());
    for (const auto& [count, why] : skipped)
        LOGI("renderer:   %8llu draws skipped: %s", (unsigned long long)count, why);
}

#endif  // PES17_HAVE_VULKAN
