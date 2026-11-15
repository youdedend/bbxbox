// SPDX-License-Identifier: GPL-3.0-or-later
// `<sched.h>` for the Store SDK, the sibling of `pthread.h` here.
//
// Upstream uses three things from it on Windows: `sched_yield()`, the
// affinity get/set pair (thread pinning for the guest's core allocation), and
// `struct sched_param`. There is no scheduler to configure in the container -
// the console's scheduler is the console's - so the param calls accept and
// ignore a priority, and the affinity ones are real: `SetThreadAffinityMask`
// and `SetThreadIdealProcessor` are App-partition calls.
#pragma once

#if !defined(_WIN32)
#error "xbox/winpthreads_shim is the Store SDK's stand-in for winpthreads; a POSIX build has the real thing"
#endif

#include <cstddef>
#include "pthread.h"

typedef int pid_t;

// 1,024 bits, the size glibc's is, so `CPU_SET(i, &set)` for any cpu a console
// reports fits.
typedef struct {
    unsigned long long bits[16];
} cpu_set_t;

#define CPU_SETSIZE 1024
#define CPU_ZERO(set) \
    do { for (int bbx_i = 0; bbx_i < 16; ++bbx_i) (set)->bits[bbx_i] = 0; } while (0)
#define CPU_SET(cpu, set) \
    do { if ((cpu) >= 0 && (cpu) < CPU_SETSIZE) (set)->bits[(cpu) / 64] |= 1ull << ((cpu) % 64); } while (0)
#define CPU_CLR(cpu, set) \
    do { if ((cpu) >= 0 && (cpu) < CPU_SETSIZE) (set)->bits[(cpu) / 64] &= ~(1ull << ((cpu) % 64)); } while (0)
#define CPU_ISSET(cpu, set) \
    ((cpu) >= 0 && (cpu) < CPU_SETSIZE && (((set)->bits[(cpu) / 64] >> ((cpu) % 64)) & 1ull) != 0)

extern "C" {

struct sched_param {
    int sched_priority;
};

int sched_yield(void);
int sched_getaffinity(pid_t pid, std::size_t cpusetsize, cpu_set_t* mask);
int sched_setaffinity(pid_t pid, std::size_t cpusetsize, const cpu_set_t* mask);
int pthread_setaffinity_np(pthread_t thread, std::size_t cpusetsize, const cpu_set_t* mask);
int pthread_getaffinity_np(pthread_t thread, std::size_t cpusetsize, cpu_set_t* mask);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

}
