// Making a phone hold 60 frames a second, and not run out of memory doing it.
//
// Three things matter on a handset and on nothing else:
//
//   * which cores the hot threads run on. A big.LITTLE scheduler will happily
//     leave the guest's main thread on a 1.8 GHz little core while four
//     performance cores idle, and the frame time doubles. The threads that
//     must not be late say so.
//   * how much memory the renderer is allowed to hold. The desktop default --
//     half the device's memory, up to 2 GB -- is most of a 4 GB phone, and the
//     kernel kills the process rather than the cache evicting.
//   * what to give back when Android asks. onTrimMemory is the only warning
//     before the low-memory killer, and a texture cache that answers it lives.
#ifdef PES17_ANDROID

#include "android.h"
#include "../env.h"
#include "../frame_rate.h"
#include "../log.h"
#include "../gpu/vulkan/texture_cache.h"
#include "../gpu/vulkan/pipeline.h"

#include <dlfcn.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::once_flag g_cpusOnce;
    cpu_set_t g_bigCores;
    cpu_set_t g_allCores;
    uint32_t g_bigCoreCount = 0;
    uint32_t g_coreCount = 0;

    uint64_t ReadNumber(const char* path)
    {
        FILE* file = std::fopen(path, "r");
        if (!file) return 0;
        unsigned long long value = 0;
        if (std::fscanf(file, "%llu", &value) != 1) value = 0;
        std::fclose(file);
        return uint64_t(value);
    }

    // The cores that clock highest. Within 15% of the fastest counts as big,
    // which puts a 2+2+4 cluster's prime and performance cores together and
    // leaves the little ones out.
    void FindCores()
    {
        CPU_ZERO(&g_bigCores);
        CPU_ZERO(&g_allCores);
        const long configured = sysconf(_SC_NPROCESSORS_CONF);
        g_coreCount = uint32_t(std::clamp<long>(configured, 1, CPU_SETSIZE));

        std::vector<uint64_t> maximum(g_coreCount, 0);
        uint64_t fastest = 0;
        for (uint32_t cpu = 0; cpu < g_coreCount; cpu++)
        {
            CPU_SET(int(cpu), &g_allCores);
            char path[128];
            std::snprintf(path, sizeof path,
                          "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
            maximum[cpu] = ReadNumber(path);
            fastest = std::max(fastest, maximum[cpu]);
        }
        if (!fastest)
        {
            // No cpufreq to read (an emulator, a locked-down kernel): every
            // core is as good as the next.
            g_bigCores = g_allCores;
            g_bigCoreCount = g_coreCount;
            return;
        }
        // Within ~30% captures both prime and performance/mid cores on tri-cluster
        // SoCs (such as Google Tensor 2+2+4 or Snapdragon 1+3+4), while excluding
        // low-frequency efficiency cores (Cortex-A55/A510).
        const uint64_t threshold = fastest - fastest / 3;
        for (uint32_t cpu = 0; cpu < g_coreCount; cpu++)
            if (maximum[cpu] >= threshold) { CPU_SET(int(cpu), &g_bigCores); g_bigCoreCount++; }

        // If we found at least 2 fast cores, keep them; otherwise (e.g. single-core
        // or uniform cluster) use all cores.
        if (g_bigCoreCount < 2)
        {
            g_bigCores = g_allCores;
            g_bigCoreCount = g_coreCount;
        }
        LOGI("perf: %u cores, %u of them fast (%llu kHz)", g_coreCount, g_bigCoreCount,
             (unsigned long long)fastest);
    }

    uint64_t MemInfoKB(const char* key)
    {
        FILE* file = std::fopen("/proc/meminfo", "r");
        if (!file) return 0;
        char line[256];
        const size_t length = std::strlen(key);
        uint64_t value = 0;
        while (std::fgets(line, sizeof line, file))
        {
            if (std::strncmp(line, key, length) != 0) continue;
            const char* at = line + length;
            while (*at == ':' || *at == ' ' || *at == '\t') at++;
            value = std::strtoull(at, nullptr, 10);
            break;
        }
        std::fclose(file);
        return value;
    }

    uint64_t g_fullTextureBudget = 0;

    // ---- the performance hint session (APerformanceHint, API 33 in the NDK; looked up
    // by name so a phone with Android 8 to 12 loads this library all the same) ----------
    struct Hint
    {
        using GetManager = void* (*)();
        using Create = void* (*)(void*, const int32_t*, size_t, int64_t);
        using Report = int (*)(void*, int64_t);
        using Close = void (*)(void*);
        using SetThreads = int (*)(void*, const int32_t*, size_t);
        // Android 16 (API 36): a session made from a config can say it times a graphics
        // pipeline's critical path, which the system may boost harder than a plain one.
        // These are the signatures Android 16 shipped (android16-release); AOSP main has
        // since made the setters return int and createSessionUsingConfig return the session.
        using ConfigCreate = void* (*)();
        using ConfigRelease = void (*)(void*);
        using ConfigSetTids = void (*)(void*, const int32_t*, size_t);
        using ConfigSetTarget = void (*)(void*, int64_t);
        using ConfigSetPipeline = void (*)(void*, bool);
        using CreateFromConfig = int (*)(void*, void*, void**);
        using MaxPipelineThreads = int (*)(void*);

        std::mutex lock;
        std::once_flag once;
        bool usable = false;
        bool renderThreads = true;
        GetManager getManager = nullptr;
        Create create = nullptr;
        Report report = nullptr;
        Close close = nullptr;
        SetThreads setThreads = nullptr;
        ConfigCreate configCreate = nullptr;
        ConfigRelease configRelease = nullptr;
        ConfigSetTids configSetTids = nullptr;
        ConfigSetTarget configSetTarget = nullptr;
        ConfigSetPipeline configSetPipeline = nullptr;
        CreateFromConfig createFromConfig = nullptr;
        size_t pipelineLimit = 0;   // 0: not a graphics pipeline session
        void* manager = nullptr;
        void* session = nullptr;
        std::vector<int32_t> threads;
        int64_t targetNanoseconds = 16'666'667;
    } g_hint;

    void LoadHint()
    {
        if (const char* off = std::getenv("PES17_HINT"); off && off[0] == '0') return;
        void* library = dlopen("libandroid.so", RTLD_NOW);
        if (!library) return;
        g_hint.getManager = reinterpret_cast<Hint::GetManager>(dlsym(library, "APerformanceHint_getManager"));
        g_hint.create = reinterpret_cast<Hint::Create>(dlsym(library, "APerformanceHint_createSession"));
        g_hint.report = reinterpret_cast<Hint::Report>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
        g_hint.close = reinterpret_cast<Hint::Close>(dlsym(library, "APerformanceHint_closeSession"));
        g_hint.setThreads = reinterpret_cast<Hint::SetThreads>(dlsym(library, "APerformanceHint_setThreads"));
        // PES17_HINT_THREADS=0 keeps the session to the guest's main thread and the frame
        // thread, as before the ring consumer, presenter and recorder were added to it.
        if (const char* off = std::getenv("PES17_HINT_THREADS"); off && off[0] == '0') g_hint.renderThreads = false;
        if (!g_hint.getManager || !g_hint.create || !g_hint.report) return;
        g_hint.manager = g_hint.getManager();
        if (!g_hint.manager) return;
        // Where the system has the API (Android 16) the session is a graphics pipeline one,
        // with as many threads as it allows such a session. On the POCO X7 Pro (Dimensity
        // 8400, 60 fps limit) it won three interleaved pairs of match runs, mean fps 55.5 /
        // 58.2 / 56.4 against 54.0 / 52.8 / 52.2. PES17_HINT_PIPELINE=0 makes a plain session.
        if (const char* off = std::getenv("PES17_HINT_PIPELINE"); !off || off[0] != '0')
        {
            g_hint.configCreate = reinterpret_cast<Hint::ConfigCreate>(dlsym(library, "ASessionCreationConfig_create"));
            g_hint.configRelease = reinterpret_cast<Hint::ConfigRelease>(dlsym(library, "ASessionCreationConfig_release"));
            g_hint.configSetTids = reinterpret_cast<Hint::ConfigSetTids>(dlsym(library, "ASessionCreationConfig_setTids"));
            g_hint.configSetTarget = reinterpret_cast<Hint::ConfigSetTarget>(dlsym(library, "ASessionCreationConfig_setTargetWorkDurationNanos"));
            g_hint.configSetPipeline = reinterpret_cast<Hint::ConfigSetPipeline>(dlsym(library, "ASessionCreationConfig_setGraphicsPipeline"));
            g_hint.createFromConfig = reinterpret_cast<Hint::CreateFromConfig>(dlsym(library, "APerformanceHint_createSessionUsingConfig"));
            const auto maxThreads = reinterpret_cast<Hint::MaxPipelineThreads>(dlsym(library, "APerformanceHint_getMaxGraphicsPipelineThreadsCount"));
            if (g_hint.configCreate && g_hint.configRelease && g_hint.configSetTids && g_hint.configSetTarget &&
                g_hint.configSetPipeline && g_hint.createFromConfig && maxThreads)
            {
                const int limit = maxThreads(g_hint.manager);
                g_hint.pipelineLimit = limit > 0 ? size_t(limit) : 0;
            }
            LOGI("perf: graphics pipeline hint sessions %s (up to %zu threads)",
                 g_hint.pipelineLimit ? "available" : "not available", g_hint.pipelineLimit);
        }
        const uint32_t limit = frame_rate::Limit();
        g_hint.targetNanoseconds = int64_t(1'000'000'000.0 / double(limit ? std::min(limit, 240u) : 60u));
        g_hint.usable = true;
    }

    // Android 14 changes a session's threads in place (setThreads), which keeps what the
    // system has learnt of it; before that each new thread makes the session over. Called
    // with the lock held. A system that refuses the first session (a Samsung on Android 13
    // did, with the main thread's id) is not asked again; one that refuses a larger set
    // keeps the session it had, without the new thread.
    void* CreatePipelineSession()
    {
        void* const config = g_hint.configCreate();
        if (!config) return nullptr;
        void* made = nullptr;
        g_hint.configSetTids(config, g_hint.threads.data(), g_hint.threads.size());
        g_hint.configSetTarget(config, g_hint.targetNanoseconds);
        g_hint.configSetPipeline(config, true);
        const int result = g_hint.createFromConfig(g_hint.manager, config, &made);
        if (result != 0) { made = nullptr; LOGW("perf: no graphics pipeline session (error %d)", result); }
        g_hint.configRelease(config);
        return made;
    }

    void RebuildHintSession()
    {
        if (g_hint.pipelineLimit && g_hint.threads.size() > g_hint.pipelineLimit)
        {
            g_hint.threads.pop_back();   // a pipeline session has a thread limit
            return;
        }
        if (g_hint.session && g_hint.setThreads &&
            g_hint.setThreads(g_hint.session, g_hint.threads.data(), g_hint.threads.size()) == 0)
        {
            LOGI("perf: performance hint session now has %zu threads", g_hint.threads.size());
            return;
        }
        void* made = g_hint.pipelineLimit ? CreatePipelineSession() : nullptr;
        const bool pipeline = made != nullptr;
        if (!made)
            made = g_hint.create(g_hint.manager, g_hint.threads.data(), g_hint.threads.size(),
                                 g_hint.targetNanoseconds);
        LOGI("perf: performance hint session %s for %zu threads, %.1f ms a frame%s",
             made ? "made" : "refused", g_hint.threads.size(), double(g_hint.targetNanoseconds) / 1e6,
             pipeline ? " (graphics pipeline)" : "");
        if (!made)
        {
            if (g_hint.session) g_hint.threads.pop_back();
            else g_hint.usable = false;
            return;
        }
        if (g_hint.session && g_hint.close) g_hint.close(g_hint.session);
        g_hint.session = made;
    }
}

void android::perf::AddHintThread(bool render)
{
    std::call_once(g_hint.once, LoadHint);
    if (!g_hint.usable || (render && !g_hint.renderThreads)) return;
    const int32_t tid = int32_t(syscall(SYS_gettid));
    std::lock_guard lock(g_hint.lock);
    if (std::find(g_hint.threads.begin(), g_hint.threads.end(), tid) != g_hint.threads.end()) return;
    g_hint.threads.push_back(tid);
    RebuildHintSession();
}

void android::perf::ReportFrameWork(int64_t actualNanoseconds)
{
    std::call_once(g_hint.once, LoadHint);
    if (!g_hint.usable) return;
    // The thread that finishes frames is the other one that must not be late.
    static thread_local bool joined = false;
    if (!joined) { joined = true; AddHintThread(); }
    // A loading stall is not a frame: past four targets it is reported as four, so one
    // long wait does not pull the system's estimate of the work far off.
    actualNanoseconds = std::min(actualNanoseconds, g_hint.targetNanoseconds * 4);
    std::lock_guard lock(g_hint.lock);
    if (g_hint.session) g_hint.report(g_hint.session, actualNanoseconds);
}

void android::perf::PinToBigCores()
{
    std::call_once(g_cpusOnce, FindCores);
    if (g_bigCoreCount == g_coreCount) return;   // nothing to gain
    if (sched_setaffinity(0, sizeof(cpu_set_t), &g_bigCores) != 0)
        LOGW("perf: this thread could not be pinned to the fast cores");
}

void android::perf::PinToAllCores()
{
    std::call_once(g_cpusOnce, FindCores);
    sched_setaffinity(0, sizeof(cpu_set_t), &g_allCores);
}

void android::perf::RaiseThreadPriority()
{
    // -8 is what Android calls URGENT_DISPLAY: above everything the app does
    // and below the audio pipeline. An app is allowed this for its own
    // threads; where it is not, the failure is harmless.
    const int tid = int(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, id_t(tid), -8) != 0)
        setpriority(PRIO_PROCESS, id_t(tid), -4);
    AddHintThread();
}

uint32_t android::perf::TotalMemoryMB() { return uint32_t(MemInfoKB("MemTotal") / 1024); }

uint32_t android::perf::AvailableMemoryMB()
{
    const uint64_t available = MemInfoKB("MemAvailable");
    return uint32_t((available ? available : MemInfoKB("MemFree")) / 1024);
}

// The instant-boot shader budget, sized for this device. A weak phone keeps
// the conservative default (15 pipelines / 250 ms: sub-2-second boot); a
// strong one compiles more up front and finishes warming sooner. Overridable
// for testing: PES17_PREWARM_MAX / PES17_PREWARM_MS, either one alone leaving the
// other at its tier.
void android::perf::PrewarmBudget(uint32_t& maxSync, uint32_t& deadlineMs)
{
    std::call_once(g_cpusOnce, FindCores);
    const uint32_t totalMB = TotalMemoryMB();
    if (totalMB >= 8000 && g_bigCoreCount >= 4) { maxSync = 40; deadlineMs = 600; }
    else if (totalMB >= 6000)                   { maxSync = 25; deadlineMs = 400; }
    else                                        { maxSync = 15; deadlineMs = 250; }
    const uint64_t overrideMax = env::Number("PES17_PREWARM_MAX");
    const uint64_t overrideMs = env::Number("PES17_PREWARM_MS");
    if (overrideMax) maxSync = uint32_t(std::min<uint64_t>(overrideMax, 5000));
    if (overrideMs) deadlineMs = uint32_t(std::min<uint64_t>(overrideMs, 60000));
    LOGI("perf: prewarm budget %u pipelines / %u ms (%u MB RAM, %u fast cores)",
         maxSync, deadlineMs, totalMB, g_bigCoreCount);
}

// The defaults the renderer reads out of the environment, sized for this
// device. Set with overwrite = 0, so anything the player chose in the app
// wins.
void android::perf::ApplyMemoryDefaults()
{
    const uint32_t totalMB = TotalMemoryMB();
    const Paths& paths = GetPaths();

    // files/env_override: NAME=value lines set as switches before the run, for A/B runs
    // without a rebuild or the app's settings (PES17_ADRENO_BC=hw, for one).
    if (!paths.external.empty())
    {
        if (std::FILE* f = std::fopen((paths.external + "/env_override").c_str(), "r"))
        {
            char line[256];
            while (std::fgets(line, sizeof line, f))
            {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                const size_t eq = text.find('=');
                if (eq == std::string::npos || eq == 0 || text.compare(0, 6, "PES17_") != 0) continue;
                setenv(text.substr(0, eq).c_str(), text.c_str() + eq + 1, 1);
                LOGI("perf: env_override %s", text.c_str());
            }
            std::fclose(f);
        }
    }

    // The guest's own 512 MB bank, its 440 MB heap and the image are not
    // negotiable; what is left over is what the renderer may cache. A sixth of
    // the device's memory keeps a 4 GB phone alive with room for the system.
    uint32_t textureMB = std::clamp(totalMB / 6, 128u, 1024u);
    if (totalMB >= 12000) textureMB = 1536;
    char text[32];
    std::snprintf(text, sizeof text, "%u", textureMB);
    setenv("PES17_TEXTURE_BUDGET_MB", text, 0);

    // The 3D scene at three quarters of 720p each way (960x540), stretched back
    // by the present: the match is GPU-bound on a Snapdragon 865 (98% busy at
    // its top clock), and this is 44% fewer pixels for every full-frame pass.
    setenv("PES17_SCENE_SCALE", "0.75", 0);

    // The upload arena is committed up front, so it is 512 MB of resident
    // memory on a device that does not have it to spare.
    const uint32_t arenaMB = totalMB >= 8000 ? 384u : totalMB >= 6000 ? 256u : 192u;
    std::snprintf(text, sizeof text, "%u", arenaMB);
    setenv("PES17_ARENA_MB", text, 0);

    // The arena has a floor -- a frame that runs out of it draws nothing from
    // the point it ran out -- but the shadow behind it does not: it is 512 MB
    // in the same buffer, and it is a cache, so without it a draw copies its
    // vertex data again (slower, and nothing else). On a six-gigabyte phone
    // that half-gigabyte is most of what the killer would otherwise count, and
    // it is asked for before the first frame. A variable already set wins, so
    // a device with room to spare keeps it.
    if (totalMB <= 6000) setenv("PES17_NO_SHADOW", "1", 0);

    // Multisampling is the single most expensive thing a mobile GPU can be
    // asked for, and the title asks for 2x and 4x by default.
    setenv("PES17_NO_MSAA", "1", 0);

    if (!paths.external.empty())
    {
        const std::string shaders = paths.external + "/shader_cache.bin";
        setenv("PES17_SHADER_CACHE", shaders.c_str(), 0);
    }
    else if (!paths.files.empty())
    {
        const std::string shaders = paths.files + "/shader_cache.bin";
        setenv("PES17_SHADER_CACHE", shaders.c_str(), 0);
    }
    if (!paths.files.empty())
    {
        const std::string pipeline = paths.files + "/pipeline.cache";
        setenv("PES17_PIPELINE_CACHE", pipeline.c_str(), 0);
    }

    const char* activeBudget = std::getenv("PES17_TEXTURE_BUDGET_MB");
    const uint32_t loggedTextureMB = activeBudget ? uint32_t(std::strtoul(activeBudget, nullptr, 10)) : textureMB;
    LOGI("perf: %u MB of memory, %u MB free; textures %u MB (budget), arena %u MB", totalMB,
         AvailableMemoryMB(), loggedTextureMB, arenaMB);
}

// ComponentCallbacks2: 5 RUNNING_MODERATE, 10 RUNNING_LOW, 15 RUNNING_CRITICAL,
// 20 UI_HIDDEN, 40 BACKGROUND, 60 MODERATE, 80 COMPLETE.
// Back to what the device was judged able to hold. Called when a run
// resumes: a budget cut under pressure has to be given back, or the cut is
// permanent and every texture is uploaded again on every draw.
void android::perf::RestoreMemory()
{
    if (!g_fullTextureBudget) return;
    if (vk::textures::Budget() >= g_fullTextureBudget) return;
    vk::textures::SetBudget(g_fullTextureBudget);
    LOGI("perf: the texture budget is back to %llu MB",
         (unsigned long long)(g_fullTextureBudget >> 20));
}

void android::perf::TrimMemory(int level)
{
    vk::pipeline::SaveCache();
    const uint64_t budget = vk::textures::Budget();
    if (budget && !g_fullTextureBudget) g_fullTextureBudget = budget;

    // TRIM_MEMORY_UI_HIDDEN is 20 and says the window went away, not that
    // memory is short -- and it arrives every time the player switches away
    // for a moment. Read as pressure, it quartered the texture cache on a
    // phone with four gigabytes free, and nothing put it back: the run came
    // out of the pause uploading every texture it drew, for ever. The levels
    // that mean pressure while running are 5, 10 and 15; the ones above are
    // about where the process sits in the list of things to kill.
    if (level == 20)
    {
        LOGI("perf: the window is hidden (level 20); that is not memory pressure,"
             " the texture budget is left alone");
        return;
    }

    if (level >= 15)
    {
        // The system is about to start killing processes. A quarter of the
        // budget is enough for what is on screen; the rest is uploaded again
        // when it is next drawn.
        if (g_fullTextureBudget) vk::textures::SetBudget(std::max<uint64_t>(g_fullTextureBudget / 4, 64ull << 20));
        LOGW("perf: the system asked for memory back (level %d); the texture budget is now %llu MB",
             level, (unsigned long long)(vk::textures::Budget() >> 20));
    }
    else if (level >= 10)
    {
        if (g_fullTextureBudget) vk::textures::SetBudget(std::max<uint64_t>(g_fullTextureBudget / 2, 96ull << 20));
        LOGI("perf: memory is short (level %d); the texture budget is now %llu MB", level,
             (unsigned long long)(vk::textures::Budget() >> 20));
    }
    else if (g_fullTextureBudget)
    {
        vk::textures::SetBudget(g_fullTextureBudget);
    }

    // Bionic keeps freed pages around rather than returning them to the
    // kernel; this hands them back. The option is bionic's own, so it is
    // asked for only where it exists.
#if defined(M_PURGE)
    mallopt(M_PURGE, 0);
#endif
}

#endif  // PES17_ANDROID
