// What the command stream holds, counted: draws and primitives, presents and
// draws per second, a frame's surfaces and resolves in order -- and the texture
// dumps, which are taken where the draws are. Diagnostics only.
#include <ppc_recomp_shared.h>
#include "internal.h"
#include "gpu.h"
#include "texture.h"
#include "../diagnostics.h"
#include "../guest.h"
#include "../log.h"
#include "../kernel/kernel.h"

#if PES17_DIAGNOSTICS
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace
{
    using namespace gpu::detail;

    constexpr int kSecondsTracked = 400;

    struct State
    {
        Counter draws, drawsIndexed, drawsAuto, drawIndices;
        Counter primitiveCount[64];

        // Draws are counted beside presents because a run that presents 60 times
        // a second over a menu and one that presents 60 times a second over a
        // level are the same number and not the same measurement.
        std::atomic<uint64_t> presentsBySecond[kSecondsTracked]{};
        std::atomic<uint64_t> drawsBySecond[kSecondsTracked]{};
        uint64_t drawsThisFrame = 0;
        std::atomic<uint64_t> presents{ 0 };
        std::atomic<int64_t> lastPresentMs{ -1 };

        // The render target set-up the frame trace last printed.
        uint32_t lastSurface[4]{};
    };
    State g;

    int SecondNow()
    {
        const double seconds = Seconds();
        return seconds >= 0 && seconds < kSecondsTracked ? int(seconds) : -1;
    }

    // PES17_TRACE_FRAME=<n>: n frames' surfaces and resolves, in order, from
    // PES17_TRACE_DRAWS_AFTER presents on: what shape the frame graph has.
    bool TracingFrame()
    {
        static const uint64_t frames = diag::Number("PES17_TRACE_FRAME");
        static const uint64_t after = diag::Number("PES17_TRACE_DRAWS_AFTER");
        const uint64_t presents = g.presents.load(std::memory_order_relaxed);
        return frames && presents >= after && presents < after + frames;
    }

    void ScissorExtent(const gpu::RegisterFile& r, uint32_t& width, uint32_t& height)
    {
        const gpu::ScissorCorner topLeft{ r[gpu::PA_SC_WINDOW_SCISSOR_TL] };
        const gpu::ScissorCorner bottomRight{ r[gpu::PA_SC_WINDOW_SCISSOR_BR] };
        width  = bottomRight.X() > topLeft.X() ? bottomRight.X() - topLeft.X() : 0;
        height = bottomRight.Y() > topLeft.Y() ? bottomRight.Y() - topLeft.Y() : 0;
    }

    void TraceSurface(const gpu::RegisterFile& r, gpu::EdramMode mode)
    {
        const uint32_t key[4] = { r[gpu::RB_SURFACE_INFO], r[gpu::RB_COLOR_INFO],
                                  r[gpu::RB_DEPTH_INFO], uint32_t(mode) };
        if (std::equal(key, key + 4, g.lastSurface)) return;
        std::copy(key, key + 4, g.lastSurface);
        const gpu::SurfaceInfo surface{ key[0] };
        const gpu::ColorInfo colour{ key[1] };
        const gpu::DepthInfo depth{ key[2] };
        uint32_t width = 0, height = 0;
        ScissorExtent(r, width, height);
        const char* colourName = gpu::ColorTargetFormatName(colour.Format());
        LOGK("surface: pitch %u, %ux MSAA, colour %s at tile %u, depth %s at tile %u,"
             " %s, scissor %ux%u", surface.Pitch(), 1u << surface.MsaaSamples(),
             colourName ? colourName : "unknown", colour.BaseTile(),
             gpu::DepthTargetFormatName(depth.Format()), depth.BaseTile(),
             gpu::EdramModeName(mode), width, height);
    }

    // A draw in copy mode is a resolve: the rectangle says which part of EDRAM to
    // copy out, RB_COPY_* where it lands.
    void TraceResolve(const gpu::RegisterFile& r)
    {
        const gpu::CopyControl control{ r[gpu::RB_COPY_CONTROL] };
        const gpu::CopyDestInfo info{ r[gpu::RB_COPY_DEST_INFO] };
        const gpu::CopyDestPitch pitch{ r[gpu::RB_COPY_DEST_PITCH] };
        uint32_t width = 0, height = 0;
        ScissorExtent(r, width, height);
        const uint32_t destination = r[gpu::RB_COPY_DEST_BASE];
        const uint32_t command = control.Command();
        LOGK("resolve: %s -> %08X (%08X), %ux%u of a %ux%u surface, format %u,"
             " endian %u, %s%s%s", control.FromDepth() ? "depth" : "colour", destination,
             kernel::FromPhysical(destination), width, height, pitch.Pitch(), pitch.Height(),
             info.Format(), info.Endian(),
             command == 0 ? "raw" : (command == 1 ? "converting" : "other"),
             control.ClearsColor() ? ", clears colour" : "",
             control.ClearsDepth() ? ", clears depth" : "");
    }
}

void gpu::detail::CensusDraw(const RegisterFile& r, uint32_t primitive, uint32_t indexCount,
                             bool indexed, EdramMode mode)
{
    g.draws++;
    g.drawsThisFrame++;
    g.primitiveCount[primitive & 63]++;
    g.drawIndices += indexCount;
    (indexed ? g.drawsIndexed : g.drawsAuto)++;
    if (TracingFrame())
    {
        if (mode == EdramMode::Copy) TraceResolve(r); else TraceSurface(r, mode);
    }
}

void gpu::detail::CensusSwap()
{
    const int second = SecondNow();
    if (second >= 0) g.drawsBySecond[second].fetch_add(g.drawsThisFrame, std::memory_order_relaxed);
    g.drawsThisFrame = 0;
}

void gpu::detail::CensusPresent()
{
    g.presents.fetch_add(1, std::memory_order_relaxed);
    g.lastPresentMs.store(int64_t(Seconds() * 1000.0), std::memory_order_relaxed);
    const int second = SecondNow();
    if (second >= 0) g.presentsBySecond[second].fetch_add(1, std::memory_order_relaxed);
}

void gpu::detail::ReportCensus()
{
    LOGI("gpu: last present at %lld ms", (long long)g.lastPresentMs.load());
    int last = 0;
    for (int i = 0; i < kSecondsTracked; i++)
        if (g.presentsBySecond[i].load()) last = i;
    // Twenty seconds to a line, labelled, so a rate that changes partway through
    // a run can be read off against the wall clock that drove it.
    for (int from = 0; from <= last; from += 20)
    {
        char line[512]; int n = 0;
        for (int i = from; i < from + 20 && i <= last; i++)
            n += std::snprintf(line + n, sizeof line - n, "%3llu ",
                               (unsigned long long)g.presentsBySecond[i].load());
        LOGI("gpu: presents per second %3ds: %s", from, line);
    }
    for (int from = 0; from <= last; from += 20)
    {
        char line[512]; int n = 0;
        for (int i = from; i < from + 20 && i <= last; i++)
            n += std::snprintf(line + n, sizeof line - n, "%4llu ",
                               (unsigned long long)(g.drawsBySecond[i].load() / 1000));
        LOGI("gpu: thousands of draws per second %3ds: %s", from, line);
    }

    const uint64_t frames = std::max<uint64_t>(1, gpu::FrameCount());
    LOGI("gpu: %llu draws (%.1f per frame): %llu indexed, %llu auto, %llu indices total",
         (unsigned long long)g.draws.load(), double(g.draws.load()) / double(frames),
         (unsigned long long)g.drawsIndexed.load(), (unsigned long long)g.drawsAuto.load(),
         (unsigned long long)g.drawIndices.load());
    if (g.draws)
    {
        std::string line;
        for (uint32_t i = 0; i < 64; i++)
            if (const uint64_t v = g.primitiveCount[i].load())
            {
                char one[64];
                const char* name = gpu::PrimitiveName(i);
                if (name) std::snprintf(one, sizeof one, "%s %llu; ", name, (unsigned long long)v);
                else std::snprintf(one, sizeof one, "type %u (unknown) %llu; ", i, (unsigned long long)v);
                line += one;
            }
        LOGI("gpu: primitives: %s", line.c_str());
    }
}
#endif
