#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <string>
#else
#include <climits>
#include <dlfcn.h>
// bionic has no execinfo.h: Android's unwinder is reached another way, and
// the backtrace below is written by hand there.
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

#ifdef _WIN32

#include <cmath>

// C23's round-half-to-even, which the recompiled vector rounding reaches
// through simde and the UCRT does not have. Whatever the rounding mode: a tie
// goes to the even neighbour, anything else to the nearer one.
extern "C" float roundevenf(float x)
{
    const float nearest = std::round(x);   // ties away from zero
    if (std::fabs(nearest - x) != 0.5f) return nearest;
    return 2.0f * std::round(x * 0.5f);
}

namespace { DWORD g_mainThread = 0; }

void platform::Initialise()
{
    g_mainThread = GetCurrentThreadId();
    timeBeginPeriod(1);
    // A player's build has no console window of its own. Started from a
    // terminal, it writes there, as a console program would.
    if (!GetConsoleWindow() && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

uint32_t platform::ThreadId() { return uint32_t(GetCurrentThreadId()); }
bool platform::IsMainThread() { return GetCurrentThreadId() == g_mainThread; }

void platform::SetThreadName(const char* name)
{
    wchar_t wide[64];
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    if (n <= 0) return;
    // Windows 10 1607 and later; looked up, so older systems just go without.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set = reinterpret_cast<SetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), wide);
}

void platform::LowerThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
void platform::IdleThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE); }

void platform::SetThreadAffinity(ThreadAffinityRole role)
{
    if (role == ThreadAffinityRole::Default) return;
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;
    if (numCores <= 1) return;

    DWORD_PTR mask = 0;
    if (role == ThreadAffinityRole::PrimeCore)
    {
        mask = (DWORD_PTR(1) << (numCores - 1));
    }
    else if (role == ThreadAffinityRole::PerformanceCore)
    {
        for (DWORD i = numCores / 2; i < numCores; ++i)
            mask |= (DWORD_PTR(1) << i);
    }
    else if (role == ThreadAffinityRole::WorkerCore)
    {
        for (DWORD i = 0; i < numCores / 2; ++i)
            mask |= (DWORD_PTR(1) << i);
    }
    if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
}

// No symbols without a PDB: each frame as its offset in the executable, which
// `llvm-symbolizer --obj=pes17.exe` turns back into a name on the build machine.
void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "\n[E] ---- %s (%u frames) ----\n", why, unsigned(n));
    for (USHORT i = 0; i < n; i++)
    {
        const auto at = reinterpret_cast<uintptr_t>(frames[i]);
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frames[i]), &owner);
        if (reinterpret_cast<uintptr_t>(owner) == module)
            std::fprintf(stderr, "[E]   exe+0x%llx\n", (unsigned long long)(at - module));
        else
            std::fprintf(stderr, "[E]   %p\n", frames[i]);
    }
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    DWORD WINAPI RunThread(LPVOID raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return 0;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    auto* start = new ThreadStart{ body, argument, name };
    const HANDLE thread = CreateThread(nullptr, stackBytes, &RunThread, start, 0, nullptr);
    if (!thread) { delete start; return false; }
    CloseHandle(thread);   // detached: nothing joins these
    return true;
}

void* platform::OpenLibrary(const char* path)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n);
    // The library's own folder is searched for what it loads in turn.
    return LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void* platform::LoadedLibrary(const char* name) { return GetModuleHandleA(name); }

void* platform::Symbol(void* library, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

#else

void platform::Initialise() {}
uint32_t platform::ThreadId() { return uint32_t(gettid()); }
bool platform::IsMainThread() { return gettid() == getpid(); }

void platform::SetThreadName(const char* name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
}

void platform::LowerThreadPriority() { setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10); }

void platform::IdleThreadPriority()
{
    sched_param none{};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &none) != 0) LowerThreadPriority();
}

namespace
{
    struct CpuTopology
    {
        uint32_t primeMask = 0;
        uint32_t perfMask = 0;
        uint32_t workerMask = 0;
        bool discovered = false;
    };

    CpuTopology DiscoverCpuTopology()
    {
        CpuTopology topo;
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (cpus <= 1) return topo;

        const int maxCpus = std::min(int(cpus), 32);
        struct CoreFreq { int id; unsigned long freq; };
        std::vector<CoreFreq> freqs;

        for (int i = 0; i < maxCpus; ++i)
        {
            char path[128];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
            FILE* f = std::fopen(path, "r");
            unsigned long freq = 0;
            if (f)
            {
                if (std::fscanf(f, "%lu", &freq) == 1) freqs.push_back({ i, freq });
                std::fclose(f);
            }
        }

        if (freqs.size() == size_t(maxCpus) && !freqs.empty())
        {
            std::sort(freqs.begin(), freqs.end(), [](const CoreFreq& a, const CoreFreq& b) {
                return a.freq > b.freq;
            });

            unsigned long maxFreq = freqs[0].freq;
            for (const auto& c : freqs)
            {
                if (c.freq >= maxFreq * 95 / 100)
                    topo.primeMask |= (1u << c.id);
                else if (c.freq >= maxFreq * 75 / 100)
                    topo.perfMask |= (1u << c.id);
                else
                    topo.workerMask |= (1u << c.id);
            }
            if (!topo.perfMask) topo.perfMask = topo.primeMask;
            if (!topo.workerMask) topo.workerMask = topo.perfMask;
        }
        else
        {
            if (maxCpus >= 8)
            {
                topo.primeMask = (1u << 7);
                topo.perfMask = (1u << 4) | (1u << 5) | (1u << 6);
                topo.workerMask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
            }
            else if (maxCpus >= 4)
            {
                topo.primeMask = (1u << (maxCpus - 1));
                for (int i = maxCpus / 2; i < maxCpus; ++i) topo.perfMask |= (1u << i);
                for (int i = 0; i < maxCpus / 2; ++i) topo.workerMask |= (1u << i);
            }
            else
            {
                topo.primeMask = 1u;
                topo.perfMask = 1u;
                topo.workerMask = 1u;
            }
        }

        topo.discovered = true;
        return topo;
    }
}

static bool IsMaliDevice()
{
    static const bool mali = [] {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("ro.hardware.vulkan", value);
        return std::strcmp(value, "mali") == 0;
    }();
    return mali;
}

void platform::SetThreadAffinity(ThreadAffinityRole role)
{
    if (role == ThreadAffinityRole::Default) return;
    static CpuTopology s_topo = DiscoverCpuTopology();
    if (!s_topo.discovered) return;

    // The ring consumer and the presenter are always taken off the prime core, unless
    // PES17_PINNING=shared: they are created from the main thread, which is pinned there,
    // and inherit it, so the consumer ran on the core the main thread spins on. On the POCO
    // X7 Pro a match load measured the consumer waiting 45 s of 58 in the run queue, and the
    // game at 1-3 fps for the minute and a half after kick-off; moved off, the same macro
    // held 60 fps.
    //
    // PES17_PINNING: "off" pins nothing; "wide" (or its old name "split") gives the guest's
    // threads every core but the prime one. By default the guest's threads
    // share the performance cluster, which on a 1+3+4 part
    // is three cores for some sixty threads: on the POCO X7 Pro (Dimensity 8400, whose
    // "little" cores are 2.1 GHz A725s) the busy ones waited about as long as they ran.
    static const char* const s_pinning = [] {
        const char* value = std::getenv("PES17_PINNING");
        return value ? value : "";
    }();
    if (std::strcmp(s_pinning, "off") == 0 || std::strcmp(s_pinning, "0") == 0) return;
    const bool shared = std::strcmp(s_pinning, "shared") == 0;
    const bool wide = std::strcmp(s_pinning, "split") == 0 || std::strcmp(s_pinning, "wide") == 0;
    // "big": the guest's threads and the ring consumer/presenter share the performance
    // cores with the prime one. On the POCO X7 Pro at a 60 limit the three performance
    // cores ran ~80% busy and the ring consumer waited in their run queue longer than it
    // ran, while the prime core, the main thread's, sat ~55% idle.
    const bool big = std::strcmp(s_pinning, "big") == 0;

    uint32_t targetMask = 0;
    switch (role)
    {
    case ThreadAffinityRole::PrimeCore:
        targetMask = s_topo.primeMask ? s_topo.primeMask : s_topo.perfMask;
        break;
    case ThreadAffinityRole::PerformanceCore:
        targetMask = s_topo.perfMask ? s_topo.perfMask : s_topo.primeMask;
        if (wide && s_topo.workerMask != s_topo.perfMask) targetMask |= s_topo.workerMask;
        if (big) targetMask |= s_topo.primeMask;
        break;
    case ThreadAffinityRole::WorkerCore:
        targetMask = s_topo.workerMask ? s_topo.workerMask : s_topo.perfMask;
        break;
    case ThreadAffinityRole::RenderCore:
        // Only where measured: on the S20 FE (Adreno) pinning these to the performance
        // cluster cost the match its steady 60 fps (43-53 with a wide spread), so there
        // they inherit the main thread's mask as before. PES17_PINNING=render forces it.
        if (shared) return;
        if (std::strcmp(s_pinning, "render") != 0 && !big && !IsMaliDevice()) return;
        targetMask = s_topo.perfMask ? s_topo.perfMask : s_topo.primeMask;
        if (big) targetMask |= s_topo.primeMask;
        break;
    default:
        return;
    }

    if (!targetMask) return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 32; ++i)
    {
        if (targetMask & (1u << i))
            CPU_SET(i, &cpuset);
    }
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

#if defined(__ANDROID__)
// _Unwind_Backtrace is in libgcc/libunwind and is what bionic itself uses.
// dladdr turns an address into a library and a symbol where there is one,
// which is enough to tell "crashed in the driver" from "crashed in the
// recompiled code" -- the two that matter here.
#include <unwind.h>

namespace
{
    struct Frames { void** at; int taken; int room; };

    _Unwind_Reason_Code TakeFrame(_Unwind_Context* context, void* argument)
    {
        auto* frames = static_cast<Frames*>(argument);
        const uintptr_t pc = _Unwind_GetIP(context);
        if (!pc) return _URC_NO_REASON;
        if (frames->taken >= frames->room) return _URC_END_OF_STACK;
        frames->at[frames->taken++] = reinterpret_cast<void*>(pc);
        return _URC_NO_REASON;
    }
}

void platform::PrintBacktrace(const char* why)
{
    // Only a fault is an error; stacks asked for by a probe or the sampler are traces.
    const bool fault = std::strstr(why, "fault") || std::strstr(why, "crash") || std::strstr(why, "abort") ||
                       std::strstr(why, "BugCheck") || std::strstr(why, "caller") || std::strstr(why, "indirect");
    const char* tag = fault ? "[E]" : "[T]";
    void* addresses[64];
    Frames frames{ addresses, 0, 64 };
    _Unwind_Backtrace(&TakeFrame, &frames);
    std::fprintf(stderr, "\n%s ---- %s (%d frames) ----\n", tag, why, frames.taken);
    for (int i = 0; i < frames.taken; i++)
    {
        Dl_info info{};
        if (dladdr(addresses[i], &info) && info.dli_fname)
        {
            const char* file = std::strrchr(info.dli_fname, '/');
            file = file ? file + 1 : info.dli_fname;
            const uintptr_t offset = uintptr_t(addresses[i]) - uintptr_t(info.dli_fbase);
            if (info.dli_sname)
                std::fprintf(stderr, "%s  #%02d %s+0x%zx (%s)\n", tag, i, info.dli_sname,
                             size_t(uintptr_t(addresses[i]) - uintptr_t(info.dli_saddr)), file);
            else
                std::fprintf(stderr, "%s  #%02d %s+0x%zx\n", tag, i, file, size_t(offset));
        }
        else
        {
            std::fprintf(stderr, "%s  #%02d %p\n", tag, i, addresses[i]);
        }
    }
    std::fprintf(stderr, "%s --------------------------------\n", tag);
    std::fflush(stderr);
}
#else
void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}
#endif

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    void* RunThread(void* raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return nullptr;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackBytes)
    {
        // Rounded up to a page, and never below what the platform insists on.
        const size_t page = size_t(sysconf(_SC_PAGESIZE));
        size_t wanted = (stackBytes + page - 1) & ~(page - 1);
#ifdef PTHREAD_STACK_MIN
        if (wanted < size_t(PTHREAD_STACK_MIN)) wanted = size_t(PTHREAD_STACK_MIN);
#endif
        pthread_attr_setstacksize(&attributes, wanted);
    }
    auto* start = new ThreadStart{ body, argument, name };
    pthread_t thread{};
    const int made = pthread_create(&thread, &attributes, &RunThread, start);
    pthread_attr_destroy(&attributes);
    if (made != 0)
    {
        delete start;
        // A smaller stack is worth trying before giving up: a device short of
        // address space would rather have the thread than the headroom.
        pthread_t fallback{};
        auto* retry = new ThreadStart{ body, argument, name };
        if (pthread_create(&fallback, nullptr, &RunThread, retry) != 0)
        {
            delete retry;
            return false;
        }
        pthread_detach(fallback);
    }
    return true;
}

void* platform::OpenLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* platform::LoadedLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }
void* platform::Symbol(void* library, const char* name) { return dlsym(library, name); }

#endif
