// SPDX-License-Identifier: GPL-3.0-or-later
// MinGW's winpthreads, for the Store SDK.
//
// bbhost's Windows build is a MinGW one, so its `#if defined(_WIN32)` branches
// are written against winpthreads: `<pthread.h>`, `<sched.h>` and the POSIX
// thread calls. The Store SDK has neither header, and an Xbox title cannot
// take a dependency on MinGW's runtime anyway, so this is the surface the
// upstream sources actually use - thirty-odd symbols, not the whole of POSIX -
// implemented over the App-partition Win32 primitives that are callable inside
// an AppContainer:
//
//   * a mutex is an `SRWLOCK` with an owner thread id and a recursion count
//     (so `PTHREAD_MUTEX_RECURSIVE` behaves as libc's does), a condition
//     variable is a `CONDITION_VARIABLE` woken with
//     `SleepConditionVariableSRW`;
//   * a thread is `CreateThread` with an explicit stack *reservation*
//     (`STACK_SIZE_PARAM_IS_A_RESERVATION`), because the guest creates up to
//     eighty threads and Game Mode's 5,120 MiB commit ceiling is charged for
//     the stack that is *committed*, not reserved;
//   * `pthread_self()` works for threads this shim did not create - `main`,
//     SDL's audio and video threads, the D3D12 worker - by handing back a
//     record made on first use, so `pthread_setname_np(pthread_self(), ...)`
//     in shared code is not a special case.
//
// The header is on the include path only for `BBX_UWP` Windows builds
// (`cmake/bbhost.cmake`), so a Linux build keeps glibc's and a MinGW build
// keeps winpthreads'.
#pragma once

#if !defined(_WIN32)
#error "xbox/winpthreads_shim is the Store SDK's stand-in for winpthreads; a POSIX build has the real thing"
#endif

#include <cstddef>
#include <cstdint>
#include <ctime>

// --- types -----------------------------------------------------------------
// A handle to a thread's record: 8 bytes, so it can live in a guest struct
// (`hle/pthread.cpp` keeps one in `gsync::Thread`, 248 bytes of guest arena).
typedef struct bbx_pthread_rec* pthread_t;

// Mutexes and condition variables are one pointer: zero-initialised storage is
// a valid, uninitialised object, and the first lock or wait initialises it.
// (`pthread_mutex_t m = {};` and a static `pthread_mutex_t` both work, which is
// what every call site in `src/core` and `src/hle` assumes.)
typedef struct bbx_pthread_mutex* pthread_mutex_t;
typedef struct bbx_pthread_cond* pthread_cond_t;

struct pthread_attr_t {
    std::size_t stacksize = 0;
    int detachstate = 0;   // PTHREAD_CREATE_JOINABLE or PTHREAD_CREATE_DETACHED
    int sched_priority = 0;
    int inheritsched = 0;
};

struct pthread_mutexattr_t {
    int type = 0;  // PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE
};

// --- constants -------------------------------------------------------------
// Values match winpthreads' (which matches glibc's) so `hle/pthread.cpp`'s
// translations of the guest's constants stay valid.
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT 0

#define PTHREAD_STACK_MIN 16384

// Not initialisers in the glibc sense (there is nothing to initialise: a null
// pointer is the uninitialised state), but the spelling exists upstream.
#define PTHREAD_MUTEX_INITIALIZER nullptr
#define PTHREAD_COND_INITIALIZER nullptr
#define PTHREAD_MUTEX_INITIALIZER_NP nullptr

extern "C" {

// --- threads ---------------------------------------------------------------
int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg);
// With this shim in the link, the name is set with `SetThreadDescription`
// (kernelbase, callable in the container) as soon as the thread starts, and
// `pthread_getname_np` reads it back either way.
int pthread_create_name_np(pthread_t* thread, const pthread_attr_t* attr,
                           void* (*start_routine)(void*), void* arg, const char* name);
int pthread_join(pthread_t thread, void** retval);
int pthread_detach(pthread_t thread);
[[noreturn]] void pthread_exit(void* retval);
pthread_t pthread_self();
int pthread_equal(pthread_t a, pthread_t b);
int pthread_setname_np(pthread_t thread, const char* name);
int pthread_getname_np(pthread_t thread, char* name, std::size_t len);
// CPU time of a thread, in `clockid_t` terms: only ever compared with
// CLOCK_PROCESS_CPUTIME_ID / CLOCK_THREAD_CPUTIME_ID by `host_clock.cpp`.
typedef int clockid_t;
int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id);

// --- attributes ------------------------------------------------------------
int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setstacksize(pthread_attr_t* attr, std::size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t* attr, std::size_t* stacksize);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate);
int pthread_attr_setschedparam(pthread_attr_t* attr, const struct sched_param* param);
int pthread_attr_getschedparam(const pthread_attr_t* attr, struct sched_param* param);
int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);
int pthread_attr_getstack(const pthread_attr_t* attr, void** addr, std::size_t* size);

// --- mutexes ---------------------------------------------------------------
int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abstime);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutexattr_init(pthread_mutexattr_t* attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr);
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type);
int pthread_mutex_gettype(const pthread_mutex_t* mutex, int* type);

// --- condition variables ---------------------------------------------------
int pthread_cond_init(pthread_cond_t* cond, const void* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);

// --- signals ---------------------------------------------------------------
// There are no POSIX signals in the container. The mask calls exist so shared
// code that blocks SIGSEGV around a guarded read still compiles and runs; the
// guarded read itself is `host_read_safe`, which is `ReadProcessMemory` here.
typedef struct {
    unsigned long long bits[2];
} sigset_t;
#define SIGSEGV 11
#define SIGBUS 7
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset);

}  // extern "C"

// --- the shim's own test ---------------------------------------------------
// Mutex, recursive mutex, condition variable wake and timeout, a joined thread
// with a custom stack reservation, a detached thread, and `pthread_self()` from
// the calling thread. Returns 0, or the step that failed in `why`.
namespace bbx::winpthreads {
int SelfTest(char* why, std::size_t len);
}
