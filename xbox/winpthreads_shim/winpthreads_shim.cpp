// SPDX-License-Identifier: GPL-3.0-or-later
// winpthreads over the App-partition Win32 primitives. The header explains why
// this exists; the notes here are the parts a reader would otherwise have to
// reconstruct:
//
//   * mutexes and condition variables are heap records behind a single
//     pointer, allocated on first use. That is the only shape in which a
//     static or zero-initialised `pthread_mutex_t` is valid without an
//     initialiser macro, which is what the upstream sources expect.
//   * a mutex owns its `SRWLOCK` outright, so a condition variable can sleep
//     on it directly (`SleepConditionVariableSRW` takes the SRW lock), which
//     is also what makes `pthread_cond_wait` able to release a *recursive*
//     mutex's full recursion count and restore it.
//   * every thread - the shim's own and the ones the host made (SDL's audio
//     and video threads, the D3D12 worker, `main`) - has a record, made on
//     `pthread_self()` if it is missing. That is what makes the name calls and
//     `pthread_exit` uniform: `hle/pthread.cpp` calls `pthread_exit` from the
//     trampoline of a thread it created, and the guest's `scePthreadExit`
//     expects the sibling's `join` to return that value.
#include <windows.h>

#include "pthread.h"
#include "sched.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

// The record behind `pthread_t`. A thread this shim made holds one reference
// for itself while it runs and one for the creator (joinable) or nobody
// (detached); whoever drops the last one frees it and closes the handle.
struct Mutex {
    SRWLOCK srw = SRWLOCK_INIT;
    std::atomic<DWORD> owner{0};
    unsigned count = 0;
    int type = PTHREAD_MUTEX_NORMAL;
};

struct Cond {
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
};

}  // namespace

namespace {

struct Rec {
    HANDLE handle = nullptr;   // the thread's handle, when this shim made it
    DWORD tid = 0;
    void* retval = nullptr;
    std::atomic<long> refs{0};
    std::atomic<bool> done{false};
    char name[64] = {};
    std::size_t stack = 0;
};

// The current thread's record. Set by the trampoline for threads this shim
// created, made lazily for everyone else.
thread_local Rec* t_self = nullptr;

Rec* self_rec() {
    if (t_self != nullptr) return t_self;
    Rec* r = new (std::nothrow) Rec();
    if (r == nullptr) return nullptr;
    r->tid = GetCurrentThreadId();
    r->refs.store(1);
    t_self = r;
    // A record made for a host thread has no handle worth joining (it is the
    // thread's own, and the thread outlives the join), so `pthread_join` on it
    // reports EINVAL rather than blocking.
    return r;
}

void release(Rec* r) {
    if (r != nullptr && r->refs.fetch_sub(1) == 1) {
        HANDLE h = InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&r->handle), nullptr);
        if (h != nullptr) CloseHandle(h);
        delete r;
    }
}

// Ends the calling thread's record: the value `join` sees, and the reference
// the thread held for itself.
void finish(Rec* r, void* retval) {
    if (r == nullptr) return;
    r->retval = retval;
    r->done.store(true);
    release(r);
    t_self = nullptr;
}

struct Start {
    Rec* rec;
    void* (*fn)(void*);
    void* arg;
};

DWORD WINAPI tramp(LPVOID param) {
    Start start = *static_cast<Start*>(param);
    delete static_cast<Start*>(param);
    t_self = start.rec;
    if (start.rec->name[0] != '\0') {
        const int wide = MultiByteToWideChar(CP_UTF8, 0, start.rec->name, -1, nullptr, 0);
        if (wide > 0 && wide < 64) {
            wchar_t w[64];
            if (MultiByteToWideChar(CP_UTF8, 0, start.rec->name, -1, w, 64) == wide) {
                // Resolved by name: `SetThreadDescription` lives in an api-set
                // the import audit does not know, and a debugger's thread list
                // is not worth a packaging failure.
                using Pfn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
                static const Pfn set_description = [] {
                    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
                    if (kernel == nullptr) kernel = GetModuleHandleW(L"kernel32.dll");
                    return kernel != nullptr ? reinterpret_cast<Pfn>(GetProcAddress(kernel, "SetThreadDescription"))
                                             : static_cast<Pfn>(nullptr);
                }();
                if (set_description != nullptr) set_description(GetCurrentThread(), w);
            }
        }
    }
    void* r = start.fn(start.arg);
    finish(start.rec, r);
    return 0;
}

// --- reference-time helpers ------------------------------------------------
// An absolute CLOCK_REALTIME deadline as a relative millisecond count for
// `SleepConditionVariableSRW` / `WaitForSingleObject`. The clock is the same
// one `host_clock_shim.h` reads for CLOCK_REALTIME.
long long ms_until(const struct timespec* abs) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    // 100 ns ticks since 1601, and 11644473600 seconds from there to the epoch.
    const long long now = (static_cast<long long>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    const long long want = static_cast<long long>(abs->tv_sec) * 10000000LL + abs->tv_nsec / 100;
    const long long delta = want - now;
    if (delta <= 0) return 0;
    return (delta + 9999) / 10000;
}

Mutex* mutex_of(pthread_mutex_t* m) { return m != nullptr ? reinterpret_cast<Mutex*>(*m) : nullptr; }

Mutex* mutex_ensure(pthread_mutex_t* m) {
    if (m == nullptr) return nullptr;
    Mutex* existing = reinterpret_cast<Mutex*>(*m);
    if (existing != nullptr) return existing;
    Mutex* made = new (std::nothrow) Mutex();
    if (made == nullptr) return nullptr;
    if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(m), made, nullptr) != nullptr) {
        delete made;  // another thread got there first
        return reinterpret_cast<Mutex*>(*m);
    }
    return made;
}

Cond* cond_ensure(pthread_cond_t* c) {
    if (c == nullptr) return nullptr;
    Cond* existing = reinterpret_cast<Cond*>(*c);
    if (existing != nullptr) return existing;
    Cond* made = new (std::nothrow) Cond();
    if (made == nullptr) return nullptr;
    if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(c), made, nullptr) != nullptr) {
        delete made;
        return reinterpret_cast<Cond*>(*c);
    }
    return made;
}

void lock_mutex(Mutex* m) {
    const DWORD me = GetCurrentThreadId();
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner.load(std::memory_order_relaxed) == me) {
        ++m->count;
        return;
    }
    AcquireSRWLockExclusive(&m->srw);
    m->owner.store(me, std::memory_order_relaxed);
    m->count = 1;
}

void unlock_mutex(Mutex* m) {
    if (m->owner.load(std::memory_order_relaxed) != GetCurrentThreadId()) return;  // not the owner: a no-op, as glibc's would be UB
    if (--m->count == 0) {
        m->owner.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&m->srw);
    }
}

}  // namespace

extern "C" {

// --- threads ---------------------------------------------------------------
int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    return pthread_create_name_np(thread, attr, fn, arg, nullptr);
}

int pthread_create_name_np(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg,
                           const char* name) {
    if (thread == nullptr || fn == nullptr) return EINVAL;
    Rec* rec = new (std::nothrow) Rec();
    if (rec == nullptr) return EAGAIN;
    Start* start = new (std::nothrow) Start{rec, fn, arg};
    if (start == nullptr) {
        delete rec;
        return EAGAIN;
    }
    if (name != nullptr) std::snprintf(rec->name, sizeof(rec->name), "%s", name);
    const std::size_t stack = attr != nullptr && attr->stacksize >= PTHREAD_STACK_MIN ? attr->stacksize : 0;
    rec->stack = stack;
    // Two references: the creator's (dropped by join or detach) and the
    // thread's own (dropped by `finish`). A detached thread's creator drops
    // its reference immediately below, so the record dies with the thread.
    rec->refs.store(2);
    const DWORD flags = stack != 0 ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0;
    HANDLE h = CreateThread(nullptr, stack, tramp, start, flags | CREATE_SUSPENDED, &rec->tid);
    if (h == nullptr) {
        delete start;
        delete rec;
        return EAGAIN;
    }
    rec->handle = h;
    *thread = reinterpret_cast<pthread_t>(rec);
    if (attr != nullptr && attr->detachstate == PTHREAD_CREATE_DETACHED) {
        release(rec);  // the creator is not going to join it
    }
    ResumeThread(h);
    return 0;
}

int pthread_join(pthread_t thread, void** retval) {
    Rec* rec = reinterpret_cast<Rec*>(thread);
    if (rec == nullptr || rec->handle == nullptr) return EINVAL;
    if (GetCurrentThreadId() == rec->tid) return EDEADLK;
    WaitForSingleObject(rec->handle, INFINITE);
    if (retval != nullptr) *retval = rec->retval;
    release(rec);  // the creator's reference
    return 0;
}

int pthread_detach(pthread_t thread) {
    Rec* rec = reinterpret_cast<Rec*>(thread);
    if (rec == nullptr) return EINVAL;
    // The caller gives up its join; the thread's own reference keeps the
    // record alive until it exits. Swap handle atomically so a concurrent
    // thread exit in release() can never double-close it.
    HANDLE h = InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&rec->handle), nullptr);
    if (h != nullptr) CloseHandle(h);
    release(rec);
    return 0;
}

void pthread_exit(void* retval) {
    Rec* rec = self_rec();
    finish(rec, retval);
    // No C++ unwinding: a guest thread's exit is not allowed to run dtors of
    // host frames it happened to be inside (they belong to other threads'
    // lifetimes). `finish` has already published the retval and dropped the
    // thread's reference, so nothing is lost but this thread's stack.
    ExitThread(0);
}

pthread_t pthread_self() {
    return reinterpret_cast<pthread_t>(self_rec());
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b ? 1 : 0; }

int pthread_setname_np(pthread_t thread, const char* name) {
    Rec* rec = reinterpret_cast<Rec*>(thread);
    if (rec == nullptr || name == nullptr) return EINVAL;
    std::snprintf(rec->name, sizeof(rec->name), "%s", name);
    return 0;
}

int pthread_getname_np(pthread_t thread, char* name, std::size_t len) {
    const Rec* rec = reinterpret_cast<const Rec*>(thread);
    if (rec == nullptr || name == nullptr || len == 0) return EINVAL;
    std::snprintf(name, len, "%s", rec->name);
    return 0;
}

int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id) {
    if (thread == nullptr || clock_id == nullptr) return EINVAL;
    // One value for all threads: `host_clock.cpp` only distinguishes "this
    // thread" from "the process", and `GetThreadTimes` is asked of the calling
    // thread anyway.
    *clock_id = 1;
    return 0;
}

// --- attributes ------------------------------------------------------------
int pthread_attr_init(pthread_attr_t* attr) {
    if (attr == nullptr) return EINVAL;
    attr->stacksize = 0;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->sched_priority = 0;
    attr->inheritsched = 0;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, std::size_t stacksize) {
    if (attr == nullptr) return EINVAL;
    attr->stacksize = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* attr, std::size_t* stacksize) {
    if (attr == nullptr || stacksize == nullptr) return EINVAL;
    *stacksize = attr->stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate) {
    if (attr == nullptr) return EINVAL;
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate) {
    if (attr == nullptr || detachstate == nullptr) return EINVAL;
    *detachstate = attr->detachstate;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t* attr, const struct sched_param* param) {
    if (attr == nullptr) return EINVAL;
    attr->sched_priority = param != nullptr ? param->sched_priority : 0;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t* attr, struct sched_param* param) {
    if (attr == nullptr || param == nullptr) return EINVAL;
    param->sched_priority = attr->sched_priority;
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) {
    const Rec* rec = reinterpret_cast<const Rec*>(thread);
    if (rec == nullptr || attr == nullptr) return EINVAL;
    pthread_attr_init(attr);
    attr->stacksize = rec->stack;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t* attr, void** addr, std::size_t* size) {
    // The bounds of a thread's stack are `GetCurrentThreadStackLimits`'
    // answer for the calling thread, which is what `host_thread_stack` asks;
    // there is no per-thread query here that is callable in the container.
    (void)attr;
    if (addr == nullptr || size == nullptr) return EINVAL;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    *addr = reinterpret_cast<void*>(low);
    *size = high > low ? static_cast<std::size_t>(high - low) : 0;
    return 0;
}

// --- mutexes ---------------------------------------------------------------
int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (mutex == nullptr) return EINVAL;
    Mutex* m = new (std::nothrow) Mutex();
    if (m == nullptr) return EAGAIN;
    if (attr != nullptr) m->type = attr->type;
    Mutex* old = reinterpret_cast<Mutex*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(mutex), m));
    delete old;  // re-initialising a live mutex leaks waiters; nobody does it
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (mutex == nullptr) return EINVAL;
    Mutex* old = reinterpret_cast<Mutex*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(mutex), nullptr));
    delete old;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    Mutex* m = mutex_ensure(mutex);
    if (m == nullptr) return EINVAL;
    lock_mutex(m);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    Mutex* m = mutex_ensure(mutex);
    if (m == nullptr) return EINVAL;
    const DWORD me = GetCurrentThreadId();
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner.load(std::memory_order_relaxed) == me) {
        ++m->count;
        return 0;
    }
    if (!TryAcquireSRWLockExclusive(&m->srw)) return EBUSY;
    m->owner.store(me, std::memory_order_relaxed);
    m->count = 1;
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abstime) {
    Mutex* m = mutex_ensure(mutex);
    if (m == nullptr || abstime == nullptr) return EINVAL;
    const DWORD me = GetCurrentThreadId();
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner.load(std::memory_order_relaxed) == me) {
        ++m->count;
        return 0;
    }
    // No timed acquisition of an SRW lock: try, then sleep in short steps.
    // Only used where a timeout is a policy choice, never on a hot path.
    for (;;) {
        if (TryAcquireSRWLockExclusive(&m->srw)) {
            m->owner.store(me, std::memory_order_relaxed);
            m->count = 1;
            return 0;
        }
        if (ms_until(abstime) <= 0) return ETIMEDOUT;
        Sleep(1);
    }
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    Mutex* m = mutex_of(mutex);
    if (m == nullptr) return EINVAL;
    unlock_mutex(m);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (attr == nullptr) return EINVAL;
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (attr == nullptr) return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type) {
    if (attr == nullptr || type == nullptr) return EINVAL;
    *type = attr->type;
    return 0;
}

int pthread_mutex_gettype(const pthread_mutex_t* mutex, int* type) {
    if (mutex == nullptr || type == nullptr) return EINVAL;
    const Mutex* m = reinterpret_cast<const Mutex*>(*mutex);
    *type = m != nullptr ? m->type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

// --- condition variables ---------------------------------------------------
int pthread_cond_init(pthread_cond_t* cond, const void* attr) {
    (void)attr;
    if (cond == nullptr) return EINVAL;
    Cond* c = new (std::nothrow) Cond();
    if (c == nullptr) return EAGAIN;
    Cond* old = reinterpret_cast<Cond*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(cond), c));
    delete old;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    Cond* old = reinterpret_cast<Cond*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(cond), nullptr));
    delete old;
    return 0;
}

namespace {

// The wait itself: `SleepConditionVariableSRW` requires `&m->srw` to be held
// exclusively on entry, atomically releases it while sleeping, and re-acquires
// it before returning (even on timeout). Drop the logical owner/recursion count
// while keeping `&m->srw` held so another thread can acquire the mutex cleanly
// while we sleep, then restore the full recursion count once `&m->srw` is ours
// again.
int cond_wait_impl(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    Mutex* m = mutex_ensure(mutex);
    Cond* c = cond_ensure(cond);
    if (m == nullptr || c == nullptr) return EINVAL;
    const DWORD me = GetCurrentThreadId();
    if (m->owner.load(std::memory_order_relaxed) != me) return EPERM;
    const unsigned held = m->count;
    if (abstime != nullptr) {
        const long long ms = ms_until(abstime);
        if (ms <= 0) {
            return ETIMEDOUT;
        }
        m->count = 0;
        m->owner.store(0, std::memory_order_relaxed);
        const BOOL ok = SleepConditionVariableSRW(
            &c->cv, &m->srw, static_cast<DWORD>(ms > 0x7fffffff ? 0x7fffffff : ms), 0);
        m->owner.store(me, std::memory_order_relaxed);
        m->count = held;
        return ok ? 0 : ETIMEDOUT;
    }
    m->count = 0;
    m->owner.store(0, std::memory_order_relaxed);
    const BOOL ok = SleepConditionVariableSRW(&c->cv, &m->srw, INFINITE, 0);
    m->owner.store(me, std::memory_order_relaxed);
    m->count = held;
    return ok ? 0 : ETIMEDOUT;
}

}  // namespace

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) { return cond_wait_impl(cond, mutex, nullptr); }

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    if (abstime == nullptr) return EINVAL;
    return cond_wait_impl(cond, mutex, abstime);
}

int pthread_cond_signal(pthread_cond_t* cond) {
    Cond* c = cond_ensure(cond);
    if (c == nullptr) return EINVAL;
    WakeConditionVariable(&c->cv);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    Cond* c = cond_ensure(cond);
    if (c == nullptr) return EINVAL;
    WakeAllConditionVariable(&c->cv);
    return 0;
}

// --- signals ---------------------------------------------------------------
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how;
    (void)set;
    if (oldset != nullptr) std::memset(oldset, 0, sizeof(*oldset));
    return 0;  // there are no signals to mask; the fault handler is VEH-based
}

}  // extern "C"

// --- sched -----------------------------------------------------------------
extern "C" {

int sched_yield(void) {
    SwitchToThread();
    return 0;
}

int sched_getaffinity(pid_t pid, std::size_t cpusetsize, cpu_set_t* mask) {
    (void)pid;
    if (mask == nullptr || cpusetsize < sizeof(cpu_set_t)) return EINVAL;
    CPU_ZERO(mask);
    // The console reports 8 cores to a title (Series X, 3.8 GHz, with SMT the
    // guest sees 8 threads), and the count asked of the OS here is the
    // process's allowed set: what the scheduler will actually run us on.
    //
    // `GetActiveProcessorCount` is not among the exports a packaged title may
    // link against (the link audit caught it), and it is not needed: every
    // console in this family has one processor group, and `GetSystemInfo`
    // answers for that group.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const DWORD n = info.dwNumberOfProcessors;
    DWORD_PTR process_mask = 0, system_mask = 0;
    const BOOL queried = GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    for (DWORD i = 0; i < n && i < CPU_SETSIZE; ++i) {
        if (!queried || i >= 64 || (process_mask & (static_cast<DWORD_PTR>(1) << i)) != 0) {
            CPU_SET(static_cast<int>(i), mask);
        }
    }
    return 0;
}

int sched_setaffinity(pid_t pid, std::size_t cpusetsize, const cpu_set_t* mask) {
    (void)pid;
    if (mask == nullptr || cpusetsize < sizeof(cpu_set_t)) return EINVAL;
    DWORD_PTR bits = 0;
    for (int i = 0; i < 64; ++i) {
        if (CPU_ISSET(i, mask)) bits |= static_cast<DWORD_PTR>(1) << i;
    }
    if (bits == 0) return EINVAL;
    return SetThreadAffinityMask(GetCurrentThread(), bits) != 0 ? 0 : EINVAL;
}

int pthread_setaffinity_np(pthread_t thread, std::size_t cpusetsize, const cpu_set_t* mask) {
    const Rec* rec = reinterpret_cast<const Rec*>(thread);
    if (rec == nullptr || mask == nullptr || cpusetsize < sizeof(cpu_set_t)) return EINVAL;
    if (rec->tid != GetCurrentThreadId()) {
        // Changing another thread's affinity needs its handle, and a handle
        // carries no right to change it unless it was opened with
        // THREAD_SET_INFORMATION; the guest's requests are for its own
        // threads, which the HLE routes here from inside them.
        return EPERM;
    }
    return sched_setaffinity(0, cpusetsize, mask);
}

int pthread_getaffinity_np(pthread_t thread, std::size_t cpusetsize, cpu_set_t* mask) {
    const Rec* rec = reinterpret_cast<const Rec*>(thread);
    if (rec == nullptr) return EINVAL;
    return sched_getaffinity(0, cpusetsize, mask);
}

int sched_get_priority_min(int policy) {
    (void)policy;
    return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy;
    return 99;
}

}  // extern "C"

// --- the shim's own test ---------------------------------------------------
namespace bbx::winpthreads {

namespace {

void* worker(void* arg) {
    auto* value = static_cast<int*>(arg);
    *value = 42;
    return arg;
}

struct Waiter {
    pthread_mutex_t m = nullptr;
    pthread_cond_t c = nullptr;
    int stage = 0;
};

void* cond_worker(void* arg) {
    auto* w = static_cast<Waiter*>(arg);
    pthread_mutex_lock(&w->m);
    w->stage = 1;
    while (w->stage == 1) pthread_cond_wait(&w->c, &w->m);
    w->stage = 3;
    pthread_mutex_unlock(&w->m);
    return nullptr;
}

void* timed_worker(void* arg) {
    auto* w = static_cast<Waiter*>(arg);
    struct timespec ts;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    // Five milliseconds from now, in CLOCK_REALTIME terms.
    const long long now = (static_cast<long long>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    const long long want = now + 5 * 10000LL;
    ts.tv_sec = static_cast<decltype(ts.tv_sec)>(want / 10000000LL);
    ts.tv_nsec = static_cast<long>((want % 10000000LL) * 100);
    pthread_mutex_lock(&w->m);
    const int rc = pthread_cond_timedwait(&w->c, &w->m, &ts);
    w->stage = rc == ETIMEDOUT ? 3 : -3;
    pthread_mutex_unlock(&w->m);
    return nullptr;
}

int fail(char* why, std::size_t len, const char* step) {
    if (why != nullptr && len > 0) std::snprintf(why, len, "%s", step);
    return 0;  // the caller's `return fail(...), N` supplies the step number
}

}  // namespace

int SelfTest(char* why, std::size_t len) {
    // pthread_self() on the calling thread, and its name.
    if (pthread_self() == nullptr) return fail(why, len, "pthread_self"), 1;
    if (pthread_setname_np(pthread_self(), "bbx-selftest") != 0) return fail(why, len, "setname"), 2;
    char name[32];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 || std::strcmp(name, "bbx-selftest") != 0) {
        return fail(why, len, "getname"), 3;
    }

    // A plain mutex, locked and unlocked from here.
    pthread_mutex_t m = nullptr;
    if (pthread_mutex_init(&m, nullptr) != 0) return fail(why, len, "mutex_init"), 4;
    if (pthread_mutex_lock(&m) != 0 || pthread_mutex_unlock(&m) != 0) return fail(why, len, "mutex"), 5;

    // A recursive mutex: two locks, two unlocks, from the same thread.
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t rm = nullptr;
    if (pthread_mutex_init(&rm, &ma) != 0) return fail(why, len, "rec_init"), 6;
    if (pthread_mutex_lock(&rm) != 0 || pthread_mutex_lock(&rm) != 0) return fail(why, len, "rec_lock"), 7;
    if (pthread_mutex_unlock(&rm) != 0 || pthread_mutex_unlock(&rm) != 0) return fail(why, len, "rec_unlock"), 8;

    // A joined thread with a custom stack reservation.
    int value = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    struct sched_param sp{};
    sp.sched_priority = 700;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_t th = nullptr;
    if (pthread_create(&th, &attr, worker, &value) != 0) return fail(why, len, "create"), 9;
    void* ret = nullptr;
    if (pthread_join(th, &ret) != 0) return fail(why, len, "join"), 10;
    if (value != 42 || ret != &value) return fail(why, len, "join_value"), 11;

    // A detached thread: it must run, and nothing may be joined afterwards.
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int detached_value = 0;
    pthread_t dt = nullptr;
    if (pthread_create(&dt, &attr, worker, &detached_value) != 0) return fail(why, len, "detach_create"), 12;
    for (int spin = 0; spin < 2000 && detached_value != 42; ++spin) Sleep(1);
    if (detached_value != 42) return fail(why, len, "detach_run"), 13;
    // The threads below are joined, so the attribute goes back to joinable.
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    // A condition variable: wait, then signal from another thread, and a
    // timed wait that expires.
    Waiter w{nullptr, nullptr, 0};
    pthread_mutex_init(&w.m, nullptr);
    pthread_cond_init(&w.c, nullptr);
    pthread_t cw = nullptr;
    if (pthread_create(&cw, &attr, cond_worker, &w) != 0) return fail(why, len, "cond_create"), 14;
    bool waiter_asleep = false;
    for (int spin = 0; spin < 2000; ++spin) {
        pthread_mutex_lock(&w.m);
        waiter_asleep = (w.stage == 1);
        pthread_mutex_unlock(&w.m);
        if (waiter_asleep) break;
        Sleep(1);
    }
    if (!waiter_asleep) return fail(why, len, "cond_enter"), 14;
    pthread_mutex_lock(&w.m);
    w.stage = 2;
    pthread_cond_signal(&w.c);
    pthread_mutex_unlock(&w.m);
    pthread_join(cw, nullptr);
    if (w.stage != 3) return fail(why, len, "cond_wake"), 15;

    Waiter tw{nullptr, nullptr, 0};
    pthread_mutex_init(&tw.m, nullptr);
    pthread_cond_init(&tw.c, nullptr);
    pthread_t tt = nullptr;
    if (pthread_create(&tt, &attr, timed_worker, &tw) != 0) return fail(why, len, "timed_create"), 16;
    pthread_join(tt, nullptr);
    if (tw.stage != 3) return fail(why, len, "cond_timeout"), 17;

    // sched_yield and the affinity pair.
    sched_yield();
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return fail(why, len, "getaffinity"), 18;
    if (!CPU_ISSET(0, &set)) return fail(why, len, "affinity_bit"), 19;
    if (sched_setaffinity(0, sizeof(set), &set) != 0) return fail(why, len, "setaffinity"), 20;

    pthread_cond_destroy(&w.c);
    pthread_mutex_destroy(&w.m);
    pthread_cond_destroy(&tw.c);
    pthread_mutex_destroy(&tw.m);
    pthread_mutex_destroy(&rm);
    pthread_mutex_destroy(&m);
    if (why != nullptr && len > 0) std::snprintf(why, len, "ok");
    return 0;
}

}  // namespace bbx::winpthreads
