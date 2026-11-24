#include "core/portable.h"
#include "core/thunk.h"
#include "log.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#if defined(BBHOST_HAVE_SDL3) && !defined(BBX_UWP)
#include <SDL3/SDL.h>
#endif

#if defined(BBX_UWP)
// The container's calls are `*FromApp`, and there is no `<timeapi.h>` (winmm
// is a desktop partition DLL) and no `_mkdir` on a path that arrives from a
// `FutureAccessList` token: the file system is the port's host-fs seam.
#include "host/window.h"
#include "xbox/platform/host_fs.h"
#include <malloc.h>
#include <windows.h>
#elif defined(_WIN32)
#include <direct.h>
#include <malloc.h>
#include <windows.h>
#include <timeapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

bool host_mkdir(const char* path) {
#if defined(BBX_UWP)
    return bbx::hostfs::MkDir(path, false);
#elif defined(_WIN32)
    return _mkdir(path) == 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

bool host_list_dir(const char* path, std::vector<std::string>* names) {
    names->clear();
#if defined(BBX_UWP)
    std::vector<bbx::hostfs::DirEntry> entries;
    if (!bbx::hostfs::ListDir(path, &entries)) return false;
    for (const auto& e : entries) names->push_back(e.name);
    return true;
#elif defined(_WIN32)
    std::string pattern = std::string(path) + "\\*";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (std::strcmp(fd.cFileName, ".") != 0 && std::strcmp(fd.cFileName, "..") != 0) names->push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR* d = ::opendir(path);
    if (!d) return false;
    while (dirent* e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names->push_back(e->d_name);
    }
    ::closedir(d);
    return true;
#endif
}

std::uint64_t host_process_cpu_ms() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    const auto to64 = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return (to64(k) + to64(u)) / 10000;  // 100 ns units
#else
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000 +
           static_cast<std::uint64_t>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
#endif
}

std::uint64_t host_thread_cpu_ns() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    const auto to64 = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return (to64(k) + to64(u)) * 100;  // 100 ns units
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

std::uintptr_t host_thread_cpu_clock() {
#if defined(_WIN32)
    // A handle of its own, kept for the process's life (a thread watched so lives as long).
    return reinterpret_cast<std::uintptr_t>(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId()));
#else
    clockid_t id{};
    if (pthread_getcpuclockid(pthread_self(), &id) != 0) return 0;
    return static_cast<std::uintptr_t>(static_cast<std::uint32_t>(id)) | (std::uintptr_t{1} << 40);  // never 0
#endif
}

std::uint64_t host_thread_cpu_ns_of(std::uintptr_t clock) {
    if (!clock) return 0;
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(reinterpret_cast<HANDLE>(clock), &c, &e, &k, &u)) return 0;
    const auto to64 = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return (to64(k) + to64(u)) * 100;  // 100 ns units
#else
    timespec ts{};
    if (clock_gettime(static_cast<clockid_t>(static_cast<std::uint32_t>(clock)), &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

#if !defined(_WIN32)
namespace {
// Set while this thread copies in host_read_safe: a fault in the copy comes
// back there (host_read_safe_recover, from the SIGSEGV handler).
thread_local sigjmp_buf* t_safe_read = nullptr;
}  // namespace

void host_read_safe_recover() {
    if (sigjmp_buf* jb = t_safe_read) {
        t_safe_read = nullptr;
        siglongjmp(*jb, 1);
    }
}
#endif

bool host_read_safe(const void* addr, void* out, std::size_t len) {
    if (len == 0) return true;
    if (!addr || !out) return false;
#if defined(BBX_UWP)
    // `ReadProcessMemory` on our own process is a desktop convenience the
    // container does not offer - and the question it answers is "is this range
    // committed and readable". `VirtualQuery` is App-partition (there is no
    // `FromApp` spelling of it; `xbox/platform/memory_selftest.cpp` uses the
    // same call to prove the dmem window's state).
    const auto base = reinterpret_cast<std::uintptr_t>(addr);
    const auto end = base + len;
    if (end < base) return false;
    std::uintptr_t at = base;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    std::memcpy(out, addr, len);
    return true;
#elif defined(_WIN32)
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, len, &got) && got == len;
#else
    // A plain copy under a jump buffer: a page that is not mapped faults, and
    // the SIGSEGV handler brings the thread back here. It was
    // process_vm_readv(getpid(), ...) - two system calls a read, and the GX
    // resource registry reads a holder's fields one at a time: ~0.6 ms of the
    // main loop in a frame that creates many resources.
    sigjmp_buf jb;
    if (sigsetjmp(jb, 0) != 0) {
        // The handler ran with its signal blocked, and a jump out of it that
        // saved no mask leaves it so: the next fault would kill the process.
        sigset_t faults;
        sigemptyset(&faults);
        sigaddset(&faults, SIGSEGV);
        sigaddset(&faults, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &faults, nullptr);
        return false;
    }
    t_safe_read = &jb;
    std::atomic_signal_fence(std::memory_order_seq_cst);  // the copy stays between the two stores
    std::memcpy(out, addr, len);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    t_safe_read = nullptr;
    return true;
#endif
}

bool host_thread_stack(std::uint64_t* lo, std::uint64_t* hi) {
#if defined(_WIN32)
    // While inside an HLE call, NT_TIB points at t_host_stack; return the
    // saved guest stack bounds when active so callers checking guest pointers
    // see the real guest stack.
    if (hle_host_guest_stack_limits(lo, hi)) return true;
#endif
    // Asked once a thread: the bounds do not move, and pthread_getattr_np
    // reads /proc/self/maps for the main thread.
    static thread_local std::uint64_t l = 0, h = 0;
    if (h == 0) {
        h = 1;  // asked and refused, unless the call below says otherwise
#if defined(_WIN32)
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        if (low && high > low) {
            l = static_cast<std::uint64_t>(low);
            h = static_cast<std::uint64_t>(high);
        }
#else
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* addr = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0 && addr && size) {
                l = reinterpret_cast<std::uint64_t>(addr);
                h = l + size;
            }
            pthread_attr_destroy(&attr);
        }
#endif
    }
    if (h <= 1) return false;
    *lo = l;
    *hi = h;
    return true;
}

std::uint32_t host_thread_id() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentThreadId());
#else
    return static_cast<std::uint32_t>(syscall(SYS_gettid));
#endif
}

#if defined(_WIN32)
namespace {
struct NamedThread {
    DWORD tid;
    HANDLE h;
    char name[32];
};
std::mutex g_named_mu;
std::vector<NamedThread> g_named;  // under g_named_mu

void register_named_thread(const char* name) {
    const DWORD tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> lk(g_named_mu);
    for (NamedThread& t : g_named) {
        if (t.tid == tid) {
            std::snprintf(t.name, sizeof(t.name), "%s", name);
            return;
        }
    }
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) return;
    NamedThread t{tid, h, {}};
    std::snprintf(t.name, sizeof(t.name), "%s", name);
    g_named.push_back(t);
}
}  // namespace
#endif

void host_thread_fit_name(const char* name, char (&out)[16]) {
    // Linux keeps 15 characters and refuses a longer name outright, so the
    // game's "Core.Res.PostProcessor_0" stayed "bbhost" in /proc, the frame
    // statistics and the sampler. Leading dotted parts go first
    // ("PostProcessor_0"), then the tail.
    const char* fit = name ? name : "";
    while (std::strlen(fit) > 15) {
        const char* dot = std::strchr(fit, '.');
        if (!dot || !dot[1]) break;
        fit = dot + 1;
    }
    std::snprintf(out, sizeof(out), "%s", fit);
}

std::vector<HostThreadTime> host_thread_times() {
    std::vector<HostThreadTime> out;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_named_mu);
    out.reserve(g_named.size());
    for (std::size_t i = 0; i < g_named.size();) {
        NamedThread& t = g_named[i];
        DWORD code = 0;
        FILETIME created, exited, kernel, user;
        if (!GetExitCodeThread(t.h, &code) || code != STILL_ACTIVE || !GetThreadTimes(t.h, &created, &exited, &kernel, &user)) {
            CloseHandle(t.h);
            g_named[i] = g_named.back();
            g_named.pop_back();
            continue;
        }
        const auto ft = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        HostThreadTime r{static_cast<std::uint32_t>(t.tid), ft(kernel) + ft(user), {}};
        std::memcpy(r.name, t.name, sizeof(r.name));
        out.push_back(r);
        ++i;
    }
#endif
    return out;
}

void host_thread_background() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
#endif
}

unsigned host_cpu_threads() {
#if defined(_WIN32)
    // (A process whose threads span processor groups gets no mask: then the system's count.)
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
        return static_cast<unsigned>(__builtin_popcountll(static_cast<unsigned long long>(process)));
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0) return static_cast<unsigned>(CPU_COUNT(&set));
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

namespace {
thread_local char t_thread_name[32];  // host_thread_name's
}  // namespace

void host_thread_set_name(const char* name) {
    if (!name || !name[0]) return;
    std::snprintf(t_thread_name, sizeof(t_thread_name), "%s", name);
#if defined(_WIN32)
    pthread_setname_np(pthread_self(), name);
#else
    char comm[16];
    host_thread_fit_name(name, comm);
    pthread_setname_np(pthread_self(), comm);
#endif
#if defined(_WIN32)
    register_named_thread(name);
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetDesc set_desc = [] {
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        if (!k) k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<SetDesc>(GetProcAddress(k, "SetThreadDescription")) : nullptr;
    }();
    if (!set_desc) {
        static bool said = false;
        if (!said) {
            said = true;
            host_log("thread names: SetThreadDescription not available; threads are unnamed to the sampler");
        }
        return;
    }
    wchar_t w[64];
    int i = 0;
    for (; i < 63 && name[i]; ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    w[i] = 0;
    set_desc(GetCurrentThread(), w);
#endif
}

bool host_thread_name(char* out, std::size_t n) {
    if (!out || !n) return false;
    if (t_thread_name[0]) {
        std::snprintf(out, n, "%s", t_thread_name);
        return true;
    }
    out[0] = 0;
#if defined(_WIN32)
    return false;
#else
    return pthread_getname_np(pthread_self(), out, n) == 0 && out[0];
#endif
}

namespace {
std::atomic<bool> g_headless{false};
}  // namespace

void host_set_headless(bool headless) { g_headless.store(headless); }

bool host_desktop_max_size(int* w, int* h) {
#if defined(BBX_UWP)
    return !g_headless.load() && host_window_desktop_size(w, h);
#elif defined(_WIN32)
    // EnumDisplaySettings gives each display's mode in physical pixels,
    // whatever the process's DPI awareness (the monitor rectangles would be
    // scaled for a process that is not aware).
    int bw = 0, bh = 0;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                bw = std::max(bw, static_cast<int>(dm.dmPelsWidth));
                bh = std::max(bh, static_cast<int>(dm.dmPelsHeight));
            }
        }
        dd = DISPLAY_DEVICEW{};
        dd.cb = sizeof(dd);
    }
    if (bw <= 0 || bh <= 0) return false;
    *w = bw;
    *h = bh;
    return true;
#elif defined(BBHOST_HAVE_SDL3)
    // The displays SDL's video driver lists (X11 or Wayland), each desktop
    // mode in physical pixels. The video subsystem is brought up for the
    // question alone - this is asked before the window shell starts - and
    // let go. Not for a headless run: it opens no window, and a test box may
    // have no display server.
    if (g_headless.load()) return false;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    int count = 0, bw = 0, bh = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i) {
            const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i]);
            if (!m) continue;
            const float d = m->pixel_density > 0.0f ? m->pixel_density : 1.0f;
            bw = std::max(bw, static_cast<int>(static_cast<float>(m->w) * d + 0.5f));
            bh = std::max(bh, static_cast<int>(static_cast<float>(m->h) * d + 0.5f));
        }
        SDL_free(ids);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (bw <= 0 || bh <= 0) return false;
    *w = bw;
    *h = bh;
    return true;
#else
    (void)w;
    (void)h;
    return false;
#endif
}

bool host_desktop_primary_size(int* w, int* h) {
#if defined(BBX_UWP)
    return !g_headless.load() && host_window_desktop_size(w, h);
#elif defined(_WIN32)
    // The primary display's mode in physical pixels (EnumDisplaySettings with
    // no device name: the primary one, whatever the DPI awareness).
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (g_headless.load() || !EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) || dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0)
        return false;
    *w = static_cast<int>(dm.dmPelsWidth);
    *h = static_cast<int>(dm.dmPelsHeight);
    return true;
#elif defined(BBHOST_HAVE_SDL3)
    // As host_desktop_max_size, the primary display alone.
    if (g_headless.load()) return false;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    int bw = 0, bh = 0;
    if (const SDL_DisplayID id = SDL_GetPrimaryDisplay()) {
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id)) {
            const float d = m->pixel_density > 0.0f ? m->pixel_density : 1.0f;
            bw = static_cast<int>(static_cast<float>(m->w) * d + 0.5f);
            bh = static_cast<int>(static_cast<float>(m->h) * d + 0.5f);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (bw <= 0 || bh <= 0) return false;
    *w = bw;
    *h = bh;
    return true;
#else
    (void)w;
    (void)h;
    return false;
#endif
}

float host_test_refresh_hz() {
    static const float hz = [] {
        const char* e = std::getenv("BBHOST_TEST_REFRESH_HZ");
        if (!e || !*e) return 0.0f;
        const float v = static_cast<float>(std::atof(e));
        if (!(v >= 20.0f && v <= 1000.0f)) {
            host_log("display: BBHOST_TEST_REFRESH_HZ=%s is not a rate from 20 to 1000 Hz; ignored", e);
            return 0.0f;
        }
        host_log("display: every refresh rate asked reads %.3f Hz (BBHOST_TEST_REFRESH_HZ, a test switch)", static_cast<double>(v));
        return v;
    }();
    return hz;
}

float host_desktop_refresh_hz() {
    if (const float t = host_test_refresh_hz(); t > 0.0f) return t;
#if defined(BBX_UWP)
    return g_headless.load() ? 0.0f : host_window_refresh_hz();
#elif defined(BBHOST_HAVE_SDL3)
    // The primary display, where SDL makes a window it is given no position
    // for (host_window_start), and its desktop mode's rate, which SDL keeps as
    // the display's ratio - Windows' DEVMODE rounds 59.94 down to 59. The
    // video subsystem is brought up for the question alone and let go, as
    // above; not for a headless run.
    if (g_headless.load()) return 0.0f;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 0.0f;
    float hz = 0.0f;
    if (const SDL_DisplayID id = SDL_GetPrimaryDisplay()) {
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id)) hz = m->refresh_rate;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return hz;
#elif defined(_WIN32)
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (g_headless.load() || !EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 0.0f;
    return static_cast<float>(dm.dmDisplayFrequency);
#else
    return 0.0f;
#endif
}

void host_sleep_us(std::uint64_t usec) {
#if defined(_WIN32)
    struct ThreadTimer {
        HANDLE h = nullptr;
        ThreadTimer() {
            static const bool period = [] {
#if !defined(BBX_UWP)
                timeBeginPeriod(1);  // for Sleep and the waits that fall back to the tick
#endif
                return true;
            }();
            (void)period;
            h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        ~ThreadTimer() {
            if (h) CloseHandle(h);
        }
    };
    static thread_local ThreadTimer timer;
    if (usec == 0) {
        Sleep(0);
        return;
    }
    if (timer.h) {
        constexpr std::uint64_t kMaxUsec = static_cast<std::uint64_t>(LLONG_MAX) / 10ull;
        const std::uint64_t clamped = usec > kMaxUsec ? kMaxUsec : usec;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(clamped * 10ull);  // 100 ns units, relative
        if (SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.h, INFINITE);
            return;
        }
    }
    const std::uint64_t ms = (usec + 999ull) / 1000ull;
    Sleep(ms > 0xffffffffull ? 0xffffffffu : static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(usec));
#endif
}

void host_sleep_until(std::chrono::steady_clock::time_point t) {
    const auto now = std::chrono::steady_clock::now();
    if (t <= now) return;
    host_sleep_us(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t - now).count()));
}

const void* host_memmem(const void* hay, std::size_t hay_len, const void* needle, std::size_t needle_len) {
    if (!needle_len) return hay;
    if (!hay || hay_len < needle_len) return nullptr;
    const auto* h = static_cast<const unsigned char*>(hay);
    const auto* n = static_cast<const unsigned char*>(needle);
    for (std::size_t i = 0; i + needle_len <= hay_len; ++i) {
        if (h[i] == n[0] && std::memcmp(h + i, n, needle_len) == 0) return h + i;
    }
    return nullptr;
}

#if defined(BBX_UWP)
namespace {

constexpr std::uint64_t kLowMin = 0x0800000000ull;  // 32 GiB: clear of ELF, dmem (4..12 GiB), and fallback
constexpr std::uint64_t kLowMax = 0x7FFFFFFFFFull;  // 512 GiB - 1: clear of gsync (0xE000000000) and GPU alias (0xFF00000000)
constexpr std::size_t kLowGran = 65536;
constexpr std::uint64_t kLowMagic = 0x4242584c4f574d31ull;

struct LowBlock {
    std::uint64_t magic;
    std::size_t bin;        // 0..10 for slab bins (64..65536? 64..32768), 11 for large direct mapping
    std::size_t req_size;   // user-visible capacity in bytes
    std::size_t map_bytes;  // non-zero for large direct mappings
    LowBlock* next;
    void* raw_base;         // actual base of LowBlock (handles alignments > 64)
    std::uint64_t pad[2];
};
static_assert(sizeof(LowBlock) == 64, "LowBlock must keep 64-byte payload alignment");

std::mutex g_low_mu;
std::uint64_t g_low_cursor = kLowMin;
LowBlock* g_low_bins[10] = {};

struct LowStackEntry {
    void* base;
    std::size_t size;
};
LowStackEntry g_low_stack_pool[128] = {};
std::size_t g_low_stack_count = 0;

std::size_t align_up_sz(std::size_t n, std::size_t a) { return (n + a - 1) & ~(a - 1); }

void* low_alloc_pages_locked(std::size_t bytes) {
    bytes = align_up_sz(bytes ? bytes : kLowGran, kLowGran);
    // Prefer VirtualAlloc2FromApp with explicit [kLowMin, kLowMax] address requirements.
    MEM_ADDRESS_REQUIREMENTS req{};
    req.LowestStartingAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kLowMin));
    req.HighestEndingAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kLowMax));
    MEM_EXTENDED_PARAMETER ext{};
    ext.Type = MemExtendedParameterAddressRequirements;
    ext.Pointer = &req;
    void* p = VirtualAlloc2FromApp(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE, &ext, 1);
    if (p != nullptr) return p;
    // Fallback probe walk (identical strategy to Test-project/src/runtime_memory.c).
    while (g_low_cursor + bytes <= kLowMax) {
        p = VirtualAllocFromApp(reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_low_cursor)), bytes,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_low_cursor += bytes + kLowGran;
        if (p != nullptr) return p;
    }
    // Wrap scan once if cursor reached the top.
    g_low_cursor = kLowMin;
    while (g_low_cursor + bytes <= kLowMax) {
        p = VirtualAllocFromApp(reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_low_cursor)), bytes,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_low_cursor += bytes + kLowGran;
        if (p != nullptr) return p;
    }
    return nullptr;
}

void* low_heap_alloc(std::size_t align, std::size_t n) {
    if (n == 0) n = 1;
    if (align < 16) align = 16;
    if ((align & (align - 1)) != 0) {
        std::size_t p2 = 16;
        while (p2 < align && p2 < (std::size_t{1} << 20)) p2 <<= 1;
        align = p2;
    }
    const std::size_t extra = align > 64 ? (align - 64) : 0;
    const std::size_t need = n + extra;
    std::size_t bin = 0;
    std::size_t cap = 64;
    while (bin < 10 && cap < need) {
        cap <<= 1;
        ++bin;
    }
    LowBlock* blk = nullptr;
    if (bin >= 10) {
        const std::size_t total = align_up_sz(sizeof(LowBlock) + need, kLowGran);
        std::lock_guard<std::mutex> lk(g_low_mu);
        blk = static_cast<LowBlock*>(low_alloc_pages_locked(total));
        if (!blk) return nullptr;
        blk->magic = kLowMagic;
        blk->bin = 10;
        blk->req_size = total - sizeof(LowBlock);
        blk->map_bytes = total;
        blk->next = nullptr;
        blk->raw_base = blk;
    } else {
        std::lock_guard<std::mutex> lk(g_low_mu);
        if (!g_low_bins[bin]) {
            constexpr std::size_t kSlab = 65536;
            auto* slab = static_cast<std::uint8_t*>(low_alloc_pages_locked(kSlab));
            if (slab) {
                const std::size_t stride = sizeof(LowBlock) + cap;
                for (std::size_t off = 0; off + stride <= kSlab; off += stride) {
                    auto* b = reinterpret_cast<LowBlock*>(slab + off);
                    b->magic = kLowMagic;
                    b->bin = bin;
                    b->req_size = cap;
                    b->map_bytes = 0;
                    b->next = g_low_bins[bin];
                    b->raw_base = b;
                    g_low_bins[bin] = b;
                }
            }
        }
        blk = g_low_bins[bin];
        if (blk) {
            g_low_bins[bin] = blk->next;
            blk->next = nullptr;
            blk->raw_base = blk;
        }
    }
    if (!blk) return nullptr;
    if (extra == 0) {
        return reinterpret_cast<void*>(blk + 1);
    }
    const std::uintptr_t raw_payload = reinterpret_cast<std::uintptr_t>(blk + 1);
    const std::uintptr_t aligned_payload = (raw_payload + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    if (aligned_payload == raw_payload) {
        return reinterpret_cast<void*>(aligned_payload);
    }
    // Store an alias header immediately preceding the aligned payload pointing back to blk.
    auto* alias = reinterpret_cast<LowBlock*>(aligned_payload) - 1;
    alias->magic = kLowMagic;
    alias->bin = blk->bin;
    alias->req_size = (reinterpret_cast<std::uintptr_t>(blk + 1) + blk->req_size) - aligned_payload;
    alias->map_bytes = blk->map_bytes;
    alias->next = nullptr;
    alias->raw_base = blk;
    return reinterpret_cast<void*>(aligned_payload);
}

void low_heap_free(void* p) {
    if (!p) return;
    auto* hdr = reinterpret_cast<LowBlock*>(p) - 1;
    if (hdr->magic != kLowMagic || hdr->raw_base == nullptr) {
        // Fallback if a pointer somehow came from CRT.
        _aligned_free(p);
        return;
    }
    auto* blk = static_cast<LowBlock*>(hdr->raw_base);
    if (blk->bin >= 10) {
        VirtualFree(blk, 0, MEM_RELEASE);
        return;
    }
    std::lock_guard<std::mutex> lk(g_low_mu);
    blk->next = g_low_bins[blk->bin];
    g_low_bins[blk->bin] = blk;
}

std::size_t low_heap_size(void* p) {
    if (!p) return 0;
    auto* hdr = reinterpret_cast<LowBlock*>(p) - 1;
    if (hdr->magic != kLowMagic) return 0;
    return hdr->req_size;
}

extern "C" void* bbx_switch_rsp_and_call(void* (*fn)(void*), void* arg, void* new_rsp);
asm(R"(
    .att_syntax prefix
    .text
    .globl bbx_switch_rsp_and_call
bbx_switch_rsp_and_call:
    push %rbp
    push %rbx
    mov %rsp, %rbx
    and $-16, %r8
    sub $32, %r8
    mov %r8, %rsp
    xor %ebp, %ebp
    mov %rcx, %rax
    mov %rdx, %rcx
    call *%rax
    mov %rbx, %rsp
    pop %rbx
    pop %rbp
    ret
)");

}  // namespace

void* host_low_map(std::size_t size) {
    std::lock_guard<std::mutex> lk(g_low_mu);
    return low_alloc_pages_locked(size);
}

void host_low_free(void* p, std::size_t) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

void* host_low_stack_alloc(std::size_t size) {
    const std::size_t bytes = align_up_sz(size ? size : (2u << 20), kLowGran);
    std::lock_guard<std::mutex> lk(g_low_mu);
    for (std::size_t i = 0; i < g_low_stack_count; ++i) {
        if (g_low_stack_pool[i].size == bytes) {
            void* base = g_low_stack_pool[i].base;
            g_low_stack_pool[i] = g_low_stack_pool[--g_low_stack_count];
            return base;
        }
    }
    return low_alloc_pages_locked(bytes);
}

void host_low_stack_free(void* p, std::size_t size) {
    if (!p) return;
    const std::size_t bytes = align_up_sz(size ? size : (2u << 20), kLowGran);
    std::lock_guard<std::mutex> lk(g_low_mu);
    if (g_low_stack_count < sizeof(g_low_stack_pool) / sizeof(g_low_stack_pool[0])) {
        g_low_stack_pool[g_low_stack_count++] = LowStackEntry{p, bytes};
        return;
    }
    VirtualFree(p, 0, MEM_RELEASE);
}

void* host_call_on_low_stack(void* (*fn)(void*), void* arg, void* stack_base, std::size_t stack_size) {
    if (!stack_base || stack_size < 65536) return fn(arg);
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    PVOID old_base = tib->StackBase;
    PVOID old_limit = tib->StackLimit;
    auto* top = static_cast<std::uint8_t*>(stack_base) + stack_size;
    tib->StackBase = top;
    tib->StackLimit = stack_base;
    void* ret = bbx_switch_rsp_and_call(fn, arg, top - 64);
    tib->StackBase = old_base;
    tib->StackLimit = old_limit;
    return ret;
}

void* host_guest_malloc(std::size_t n) { return low_heap_alloc(16, n); }

void* host_guest_calloc(std::size_t n, std::size_t size) {
    if (n != 0 && size > (static_cast<std::size_t>(-1) / n)) return nullptr;
    const std::size_t total = n * size;
    void* p = low_heap_alloc(16, total);
    if (p && total) std::memset(p, 0, total);
    return p;
}

void* host_guest_realloc(void* p, std::size_t n) {
    if (!p) return low_heap_alloc(16, n);
    if (n == 0) {
        low_heap_free(p);
        return nullptr;
    }
    const std::size_t old_sz = low_heap_size(p);
    if (old_sz >= n) return p;
    void* fresh = low_heap_alloc(16, n);
    if (!fresh) return nullptr;
    if (old_sz > 0) std::memcpy(fresh, p, old_sz < n ? old_sz : n);
    low_heap_free(p);
    return fresh;
}

void* host_guest_memalign(std::size_t align, std::size_t n) {
    const std::size_t size = n ? n : 1;
    void* p = low_heap_alloc(align < 16 ? 16 : align, size);
    if (p) std::memset(p, 0, size);
    return p;
}

void host_guest_free(void* p) { low_heap_free(p); }

#else  // !BBX_UWP

void* host_low_map(std::size_t size) {
    return host_page_alloc(size ? size : 65536, false);
}

void host_low_free(void* p, std::size_t size) {
    if (p) host_page_free(p, size);
}

void* host_low_stack_alloc(std::size_t size) { return host_low_map(size); }
void host_low_stack_free(void* p, std::size_t size) { host_low_free(p, size); }
void* host_call_on_low_stack(void* (*fn)(void*), void* arg, void*, std::size_t) { return fn(arg); }

void* host_guest_malloc(std::size_t n) {
#if defined(_WIN32)
    return _aligned_malloc(n ? n : 1, 16);
#else
    return std::malloc(n);
#endif
}

void* host_guest_calloc(std::size_t n, std::size_t size) {
#if defined(_WIN32)
    const std::size_t total = n * size;
    void* p = _aligned_malloc(total ? total : 1, 16);
    if (p) std::memset(p, 0, total);
    return p;
#else
    return std::calloc(n, size);
#endif
}

void* host_guest_realloc(void* p, std::size_t n) {
#if defined(_WIN32)
    return _aligned_realloc(p, n ? n : 1, 16);
#else
    return std::realloc(p, n);
#endif
}

void* host_guest_memalign(std::size_t align, std::size_t n) {
    const std::size_t size = n ? n : 1;
#if defined(_WIN32)
    void* p = _aligned_malloc(size, align < 16 ? 16 : align);
#else
    void* p = nullptr;
    if (posix_memalign(&p, align < 16 ? 16 : align, size) != 0) return nullptr;
#endif
    if (p) std::memset(p, 0, size);
    return p;
}

void host_guest_free(void* p) {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

#endif  // BBX_UWP

void* host_page_alloc(std::size_t size, bool exec) {
#if defined(BBX_UWP)
    // Never RWX: the container answers `ERROR_INVALID_PARAMETER` for it even
    // with the `codeGeneration` capability. `exec` only says the caller will
    // hand the pages to `host_page_make_executable` once it has written them.
    (void)exec;
    return VirtualAllocFromApp(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif defined(_WIN32)
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
    return p;
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void host_page_free(void* p, std::size_t size) {
    if (!p) return;
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

bool host_page_make_writable(void* p, std::size_t size) {
    if (!p || !size) return false;
#if defined(BBX_UWP)
    DWORD old = 0;
    return VirtualProtectFromApp(p, size, PAGE_READWRITE, &old) != FALSE;
#elif defined(_WIN32)
    DWORD old = 0;
    return VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old) != FALSE;
#else
    return mprotect(p, size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

bool host_page_make_executable(void* p, std::size_t size) {
    if (!p || !size) return false;
#if defined(BBX_UWP)
    DWORD old = 0;
    const bool ok = VirtualProtectFromApp(p, size, PAGE_EXECUTE_READ, &old) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return ok;
#elif defined(_WIN32)
    DWORD old = 0;
    const bool ok = VirtualProtect(p, size, PAGE_EXECUTE_READ, &old) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return ok;
#else
    const bool ok = mprotect(p, size, PROT_READ | PROT_EXEC) == 0;
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + size);
    return ok;
#endif
}

void host_page_guard(void* p, std::size_t size) {
#if defined(BBX_UWP)
    DWORD old = 0;
    VirtualProtectFromApp(p, size, PAGE_NOACCESS, &old);
#elif defined(_WIN32)
    DWORD old = 0;
    VirtualProtect(p, size, PAGE_NOACCESS, &old);
#else
    mprotect(p, size, PROT_NONE);
#endif
}

const void* host_map_file(const char* path, std::size_t* size) {
#if defined(BBX_UWP)
    // No `CreateFileA`, no `CreateFileMappingA`, no `MapViewOfFile`: the path
    // may be one the player granted through a `FutureAccessList` token, which
    // only the host-fs seam can open. The file is read into a private
    // read-only allocation instead of being mapped - a dump's tables are read
    // once and kept, and the allocation is exactly its size.
    bbx::hostfs::FileStat st{};
    if (!bbx::hostfs::Stat(path, &st) || st.is_dir || st.size == 0) return nullptr;
    const int f = bbx::hostfs::Open(path, bbx::hostfs::kRead);
    if (f < 0) return nullptr;
    void* p = VirtualAllocFromApp(nullptr, static_cast<SIZE_T>(st.size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr) {
        bbx::hostfs::Close(f);
        return nullptr;
    }
    std::size_t got = 0;
    while (got < st.size) {
        const std::int64_t n = bbx::hostfs::Read(f, static_cast<std::uint8_t*>(p) + got, st.size - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    bbx::hostfs::Close(f);
    DWORD old = 0;
    if (got != st.size || !VirtualProtectFromApp(p, static_cast<SIZE_T>(st.size), PAGE_READONLY, &old)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    *size = got;
    return p;
#elif defined(_WIN32)
    // The sharing fopen's "rb" gives, so a file another program has open still maps.
    const HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER n{};
    const HANDLE m = GetFileSizeEx(f, &n) && n.QuadPart > 0 ? CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(f);
    if (!m) return nullptr;
    const void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);  // the view keeps the mapping
    if (p) *size = static_cast<std::size_t>(n.QuadPart);
    return p;
#else
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    void* p = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0) p = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    *size = static_cast<std::size_t>(st.st_size);
    return p;
#endif
}

#if defined(_WIN32)
extern "C" IMAGE_DOS_HEADER __ImageBase;
std::uintptr_t host_image_base() { return reinterpret_cast<std::uintptr_t>(&__ImageBase); }
#else
extern "C" char __executable_start;
std::uintptr_t host_image_base() { return reinterpret_cast<std::uintptr_t>(&__executable_start); }
#endif
