// MW2's D3D9 is statically linked but not inlined: the engine reaches it
// through ordinary function calls, and a few of them are hooked here.
#include <ppc_recomp_shared.h>
#include "../pacing_trace.h"
#include "../stutters.h"
#include "title.h"
#include "gpu.h"
#include "../kernel/kernel.h"
#include "internal.h"
#include "../diagnostics.h"
#include "../console.h"
#include "../frame_rate.h"
#include "../guest.h"
#include "../log.h"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace
{
    // PES17_FPS_LIMIT (frame_rate.h) takes the title off the display's blank
    // with what the console has for it. D3D's present puts the device's
    // D3DRS_PRESENTINTERVAL in the flip request, and its flip handler retires
    // a frame asked for with D3DPRESENT_INTERVAL_IMMEDIATE when the command
    // processor reaches it instead of queueing it for the blank. The title
    // never sets that state, but a state block put back restores it, so it is
    // set at every present. The title's side is two settings of its own:
    // com_maxfps, its limiter, and r_vsync, which has it take a frame's
    // length from the blanks counted rather than from the clock.
    void Unsynchronise(uint32_t device)
    {
        constexpr uint32_t kPresentInterval = 13596;    // in D3D9's device structure
        constexpr uint32_t kIntervalImmediate = 0x80000000;
        *GuestPtr<be32>(device + kPresentInterval) = kIntervalImmediate;

        static const bool told = [] {
            char limit[32];
            std::snprintf(limit, sizeof limit, "com_maxfps %u", frame_rate::Limit());
            console::RunNow(limit);
            console::RunNow("r_vsync 0");
            if (frame_rate::Limit()) LOGI("d3d: frames are retired off the blank, %u a second at most", frame_rate::Limit());
            else LOGI("d3d: frames are retired off the blank, as many a second as there are");
            return true;
        }();
        (void)told;
    }
}

// D3D9's present. Its frame is retired the way the console retires it: the
// command stream has the command processor raise an interrupt whose handler
// runs D3D's own flip handler, which counts the frame done.
#ifdef T_D3D_Present
GUEST_HOOK(T_D3D_Present)
{
    pacing::Note(pacing::kPresent);
    if (!frame_rate::Console()) Unsynchronise(ctx.r3.u32);
    GUEST_ORIG(T_D3D_Present)(ctx, base);
    pacing::Note(pacing::kPresentEnd);
    gpu::Presented();
    stutters::TitlePresented();
}
#endif

namespace
{
    std::atomic<bool>     g_arenaWaitActive{ false };
    std::atomic<uint32_t> g_arenaWaitNeededPos{ 0 };
    std::atomic<uint32_t> g_arenaWaitNeededSeq{ 0 };
    std::atomic<uint64_t> g_arenaWaitStartTimeUs{ 0 };
}

bool gpu::CheckArenaWaitDeadlock(uint64_t maxWaitUs, uint32_t& outSeq, uint32_t& outPos)
{
    if (!g_arenaWaitActive.load(std::memory_order_acquire)) return false;
    const uint64_t start = g_arenaWaitStartTimeUs.load(std::memory_order_relaxed);
    const uint64_t now = pacing::Microseconds();
    if (now - start >= maxWaitUs)
    {
        outSeq = g_arenaWaitNeededSeq.load(std::memory_order_relaxed);
        outPos = g_arenaWaitNeededPos.load(std::memory_order_relaxed);
        return true;
    }
    return false;
}

// D3D9's wait for arena space, which is where the block the command processor
// reports its progress through is learnt (arena.cpp): the device holds a
// pointer to it, and it holds a sequence counter at +0 and a position at +4.
// The device's other fields are in docs/d3d9-seam.md.
#ifdef T_D3D_ArenaWait
GUEST_HOOK(T_D3D_ArenaWait)
{
    constexpr uint32_t kProgressBlock = 10896;   // in D3D9's device structure
    if (const uint32_t block = *GuestPtr<be32>(ctx.r3.u32 + kProgressBlock))
        gpu::SetArenaProgressBlock(block);

    g_arenaWaitNeededPos.store(ctx.r4.u32, std::memory_order_relaxed);
    g_arenaWaitNeededSeq.store(ctx.r5.u32, std::memory_order_relaxed);
    g_arenaWaitStartTimeUs.store(pacing::Microseconds(), std::memory_order_relaxed);
    g_arenaWaitActive.store(true, std::memory_order_release);

    GUEST_ORIG(T_D3D_ArenaWait)(ctx, base);

    g_arenaWaitActive.store(false, std::memory_order_release);
}
#endif

#if PES17_DIAGNOSTICS && defined(T_D3D_ReplayRecording)
namespace
{
    std::atomic<uint64_t> g_endless{ 0 };
}

// A renderer pass' recording is replayed by T_D3D_ReplayRecording, which walks
// the chunk list at [object+116] -- next at +0, an entry count at +4, entries
// from +8 -- and copies each chunk into the command-buffer arena. The inner
// loop counts up to [chunk+4] with a do-while, so a chunk holding no entries is
// 2^32 of them and one whose count is not a count is as good as forever. The
// thread does not come back to be diagnosed, so the list is checked on the way in.
GUEST_HOOK(T_D3D_ReplayRecording)
{
    const uint32_t object = ctx.r4.u32;
    const uint32_t head = object ? uint32_t(*GuestPtr<be32>(object + 116)) : 0;
    uint32_t nodes = 0, entries = 0;
    bool endless = false;
    // Tortoise and hare: a cycle is as endless as a bad count.
    for (uint32_t slow = head, fast = head; fast && !endless; nodes++)
    {
        const uint32_t count = *GuestPtr<be32>(slow + 4);
        if (!count || count > (1u << 20)) { endless = true; break; }
        entries += count;
        fast = *GuestPtr<be32>(fast);
        if (fast) fast = *GuestPtr<be32>(fast);
        slow = *GuestPtr<be32>(slow);
        if (!slow) break;
        if (slow == fast || nodes > 4096) endless = true;
    }
    if (endless && g_endless++ < 8)
        LOGW("d3d9: a recorded command buffer D3D9 cannot walk to the end of"
             " (object %08X, head %08X, %u chunks, %u entries)", object, head, nodes, entries);
    GUEST_ORIG(T_D3D_ReplayRecording)(ctx, base);
}

void gpu::detail::ReportD3DHooks()
{
    if (uint64_t n = g_endless.load())
        LOGI("d3d9: %llu recordings with no end", (unsigned long long)n);
}
#else
void gpu::detail::ReportD3DHooks() {}
#endif

#ifdef T_D3D_BlockOnFence
// Says what a stalled fence wait is waiting for: the fence, what the GPU has
// retired, and where the CPU reads that, so it can be compared with where the
// command processor writes it.
GUEST_HOOK(T_D3D_BlockOnFence)
{
    const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
    const uint32_t retiredAt = *GuestPtr<be32>(device + 11024);
    const uint32_t issued = *GuestPtr<be32>(device + 11036);
    const uint32_t retired = retiredAt ? uint32_t(*GuestPtr<be32>(retiredAt)) : 0;
    if (issued - fence < issued - retired)
    {
        static std::atomic<int> told{ 0 };
        if (told++ < 8)
            LOGW("d3d: waiting for fence %08X; retired %08X (read at %08X, physical %08X), issued %08X",
                 fence, retired, retiredAt, kernel::ToPhysical(retiredAt), issued);
    }
    const kernel::DelayCounts before = kernel::ThreadDelays();
    const auto from = std::chrono::steady_clock::now();
    GUEST_ORIG(T_D3D_BlockOnFence)(ctx, base);
    const kernel::DelayCounts& after = kernel::ThreadDelays();
    FenceWaits& waits = gpu::detail::g_fenceWaits;
    waits.calls.fetch_add(1, std::memory_order_relaxed);
    waits.nanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - from).count()), std::memory_order_relaxed);
    waits.sleeps.fetch_add(after.sleeps - before.sleeps, std::memory_order_relaxed);
    waits.sleptNs.fetch_add(after.sleptNs - before.sleptNs, std::memory_order_relaxed);
    waits.yields.fetch_add(after.yields - before.yields, std::memory_order_relaxed);
}
#endif
