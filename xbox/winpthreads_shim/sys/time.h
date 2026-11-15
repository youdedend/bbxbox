// SPDX-License-Identifier: GPL-3.0-or-later
// `<sys/time.h>`, the small part of it a Windows build ever gets from MinGW.
//
// `hle/libc.cpp` includes it for `gettimeofday` and the POSIX time formatting
// calls next to it. The guest's `timeval` is FreeBSD's - two 64-bit fields -
// and the HLE converts, so what is needed here is the host's declaration:
// `struct timeval` plus `gettimeofday` over the same clock
// `core/host_clock_shim.h` reads for CLOCK_REALTIME.
#pragma once

#if !defined(_WIN32)
#error "xbox/winpthreads_shim is the Store SDK's stand-in for winpthreads; a POSIX build has the real thing"
#endif

#include <cstdint>
#include <ctime>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

extern "C" {
int gettimeofday(struct timeval* tv, void* tz);
}
