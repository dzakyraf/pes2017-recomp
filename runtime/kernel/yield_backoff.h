#pragma once
// A guest that waits by polling -- NtYieldExecution in a loop, or a zero
// KeDelayExecutionThread -- turns sched_yield into a busy loop. On the phone that
// was measured (simpleperf, in a match): the main thread and three worker threads
// spent a quarter of all CPU cycles in the kernel yielding, a core each, which
// is heat the thermal governor answers by clocking the whole chip down.
//
// The first few yields of a burst stay plain yields, so a wait that ends at once
// costs nothing extra. A thread that keeps coming back within a fraction of a
// millisecond is spinning, and gets a sleep of a few microseconds that grows to
// a cap. Measured at 60 fps on a cool phone, a 100 us cap cut CPU cycles by a
// fifth but cost about 6 fps, and 30 us after 20000 yields changed nothing.
//
// So the cap is chosen by what the frame can spare. PES17_YIELD_SLEEP_US (or
// files/yield_tuning) fixes it, 0 being plain yields always. Unset, it is
// kAutoSleepUs while the title runs under an FPS limit below the console's 60
// (the frame has slack, and a spin there only waits out the limiter), or while
// the phone reports itself hot (SetYieldHeat, from the thermal status), when
// the clocks are being pulled down anyway and every spinning core makes it
// worse; plain yields otherwise.
#include "../env.h"
#include "../frame_rate.h"
#include "../log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <sched.h>

namespace kernel
{
    constexpr uint32_t kAutoSleepUs = 100;

    inline std::atomic<bool>& YieldHeat()
    {
        static std::atomic<bool> hot{ false };
        return hot;
    }

    // The thermal listener: hot from THERMAL_STATUS_MODERATE (2) up.
    inline void SetYieldHeat(int thermalStatus)
    {
        YieldHeat().store(thermalStatus >= 2, std::memory_order_relaxed);
    }

    inline void YieldBackoff()
    {
        using Clock = std::chrono::steady_clock;
        thread_local uint32_t spins = 0;
        thread_local Clock::time_point last{};
        // fixed: the cap was given; otherwise it follows the limit and the heat.
        struct Tuning { uint32_t maxSleepUs; uint32_t freeYields; bool fixed; bool limited; };
        static const Tuning tuning = [] {
            Tuning t{ uint32_t(env::Number("PES17_YIELD_SLEEP_US", 0)), uint32_t(env::Number("PES17_YIELD_SPINS", 32)),
                      env::Text("PES17_YIELD_SLEEP_US") != nullptr, false };
#ifdef PES17_ANDROID
            // files/yield_tuning "<sleep us> <free yields>" overrides, for measuring on the phone.
            if (std::FILE* f = std::fopen("/sdcard/Android/data/com.pes2017.recomp/files/yield_tuning", "r"))
            {
                unsigned a = 0, b = 0;
                if (std::fscanf(f, "%u %u", &a, &b) == 2) { t.maxSleepUs = a; t.freeYields = b; t.fixed = true; }
                std::fclose(f);
            }
#endif
            const uint32_t limit = frame_rate::Limit();
            t.limited = limit != 0 && limit < frame_rate::kConsole;
            LOGI("yield: %s", t.fixed ? "sleep cap fixed"
                                              : t.limited ? "sleeping in spins (FPS limit under 60)"
                                                          : "sleeping in spins only while the phone is hot");
            return t;
        }();
        const uint32_t maxSleepUs = tuning.fixed ? tuning.maxSleepUs
                                  : (tuning.limited || YieldHeat().load(std::memory_order_relaxed)) ? kAutoSleepUs : 0;
        const uint32_t kFreeYields = tuning.freeYields;

        if (maxSleepUs == 0) { sched_yield(); return; }
        const auto now = Clock::now();
        // Anything but a tight loop in between: it was not a spin.
        if (now - last > std::chrono::microseconds(500)) spins = 0;
        else if (spins < 0xFFFFFFF0u) ++spins;

        if (spins < kFreeYields)
        {
            sched_yield();
        }
        else
        {
            const uint32_t us = std::min(maxSleepUs, 10u * (spins - kFreeYields + 1));
            timespec wait{ 0, long(us) * 1000L };
            nanosleep(&wait, nullptr);
        }
        last = Clock::now();
    }
}
