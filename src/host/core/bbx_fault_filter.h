// SPDX-License-Identifier: GPL-3.0-or-later
// Which exceptions a first-chance handler may log, decided without Windows.
//
// The first console run at 954a97c died of the handler rather than of the fault:
// DBG_PRINTEXCEPTION_C (0x40010006) is what OutputDebugString raises, the handler
// logged it, logging raised another, and 3.2 MB of nesting later the stack was
// gone (0xC00000FD). Three rules come out of that, and all three live here so the
// host tests can hold them:
//
//   1. the codes that are *not* faults of this process - the debug print, the
//      breakpoint and the C++ exception - are passed on untouched, never logged;
//   2. a fault that arrives while the handler is already running is passed on
//      too: the handler's own writes must not be able to re-enter it;
//   3. the handler logs a bounded number of faults and then one line saying how
//      many it stopped writing, so a storm costs a line instead of the process.
#pragma once

#include <atomic>
#include <cstdint>

namespace bbx::core {

enum class FaultAction : std::uint8_t {
    kPass = 0,   // EXCEPTION_CONTINUE_SEARCH: not ours to log
    kLog,        // log it (bounded) and pass it on
    kSuppressed, // past the cap: pass it on, and a line was written once already
};

[[nodiscard]] const char* FaultActionName(FaultAction action);

// `count` is how many faults have already been logged, `reentered` says the
// handler is already on this thread's stack.
[[nodiscard]] FaultAction DecideFault(std::uint32_t code, std::uint32_t count, bool reentered);

// The codes a first-chance handler must never log: an OutputDebugString print, a
// debug print's continuation, a breakpoint, a single-step, and a C++ throw.
[[nodiscard]] bool FaultCodeIsNoise(std::uint32_t code);

// The handler's state: one interlocked guard and one bounded counter, shared by
// every thread. The console's handler holds an instance; the tests hold one each.
class FaultGate {
public:
    // True when this call owns the handler: it sets the guard, and a second call
    // on the same or another thread while the guard is held gets false.
    [[nodiscard]] bool Enter();
    void Leave();
    // Atomically acquires the re-entry guard and evaluates `code`. When the
    // returned action is kPass the guard is already released (or was held by an
    // outer invocation); when it is kLog or kSuppressed the caller owns the
    // guard and must call Leave() after writing.
    [[nodiscard]] FaultAction EnterAndAdmit(std::uint32_t code);
    // The action for `code`, counting the fault when it is one to log.
    [[nodiscard]] FaultAction Admit(std::uint32_t code);
    [[nodiscard]] std::uint32_t logged() const { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool reentered() const { return guard_.load(std::memory_order_relaxed) != 0; }

    static constexpr std::uint32_t kMaxLogged = 32;  // then one suppression line

private:
    std::atomic<long> guard_{0};
    std::atomic<std::uint32_t> count_{0};
    std::atomic<bool> suppressed_{false};
};

}  // namespace bbx::core
