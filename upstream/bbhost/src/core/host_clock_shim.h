// SPDX-License-Identifier: GPL-3.0-or-later
// `clock_gettime` for a Windows build without winpthreads.
//
// Upstream's `hle/kernel.cpp` includes MinGW's `<pthread_time.h>` to get
// `clock_gettime` and the POSIX clock ids, because its Windows build is a
// MinGW one. clang-cl against the Windows Store SDK has no such header, and
// the guest asks for `CLOCK_PROCESS_CPUTIME_ID` and `CLOCK_THREAD_CPUTIME_ID`
// by name: without them `sceKernelClockGettime` would have to be stubbed, and
// a game that measures its own frame budget with it would see zero.
//
// So this is the same contract over the Win32 clocks, which is what
// winpthreads implements too:
//
//   CLOCK_REALTIME           GetSystemTimePreciseAsFileTime (UTC since 1970)
//   CLOCK_MONOTONIC          QueryPerformanceCounter (system-wide, suspend-excluded)
//   CLOCK_PROCESS_CPUTIME_ID GetProcessTimes
//   CLOCK_THREAD_CPUTIME_ID  GetThreadTimes
//
// Values are `struct timespec`, 100 ns resolution where the clock has it; the
// call is a handful of instructions and allocates nothing, because the HLE
// calls it on the guest's frame path.
#pragma once

#if defined(_WIN32) && !defined(__MINGW32__)

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <ctime>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

namespace bbx::host_clock {

inline void SplitTicks(std::uint64_t ticks_100ns, struct timespec* ts) {
    ts->tv_sec = static_cast<long>(ticks_100ns / 10000000ull);
    ts->tv_nsec = static_cast<long>((ticks_100ns % 10000000ull) * 100ull);
}

// 100 ns units from a FILETIME (which is 100 ns since 1601).
inline std::uint64_t FromFileTime(const FILETIME& ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

inline int ClockGettime(int clock_id, struct timespec* ts) {
    if (ts == nullptr) return EINVAL;
    switch (clock_id) {
        case CLOCK_REALTIME: {
            FILETIME ft{};
            ::GetSystemTimePreciseAsFileTime(&ft);
            // 1601 -> 1970: 11644473600 s = 116444736000000000 of 100 ns.
            const std::uint64_t ticks = FromFileTime(ft);
            if (ticks < 116444736000000000ull) {
                ts->tv_sec = 0;
                ts->tv_nsec = 0;
                return 0;
            }
            SplitTicks(ticks - 116444736000000000ull, ts);
            return 0;
        }
        case CLOCK_MONOTONIC: {
            static const std::uint64_t freq = [] {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                return static_cast<std::uint64_t>(f.QuadPart == 0 ? 1 : f.QuadPart);
            }();
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            const std::uint64_t ticks = static_cast<std::uint64_t>(now.QuadPart);
            // ns = ticks * 1e9 / freq, without a 128-bit divide.
            ts->tv_sec = static_cast<long>(ticks / freq);
            ts->tv_nsec = static_cast<long>(((ticks % freq) * 1000000000ull) / freq);
            return 0;
        }
        case CLOCK_PROCESS_CPUTIME_ID: {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                return EINVAL;
            }
            SplitTicks(FromFileTime(kernel) + FromFileTime(user), ts);
            return 0;
        }
        case CLOCK_THREAD_CPUTIME_ID: {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user)) {
                return EINVAL;
            }
            SplitTicks(FromFileTime(kernel) + FromFileTime(user), ts);
            return 0;
        }
        default:
            return EINVAL;
    }
}

}  // namespace bbx::host_clock

// The unqualified name the HLE calls, at global scope as the POSIX header
// would have declared it.
inline int clock_gettime(int clock_id, struct timespec* ts) {
    return bbx::host_clock::ClockGettime(clock_id, ts);
}

// `clockid_t` is used to pass a converted guest clock id through; POSIX has it
// in <sys/types.h>, MSVC does not.
using clockid_t = int;

#endif  // _WIN32 && !__MINGW32__
