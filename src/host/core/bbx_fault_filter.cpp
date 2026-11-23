// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/core/bbx_fault_filter.h"

namespace bbx::core {
namespace {

// The codes that are part of normal operation, not faults of this process.
constexpr std::uint32_t kDbgPrint = 0x40010006u;       // OutputDebugString
constexpr std::uint32_t kDbgPrintContinuation = 0x4001000au;
constexpr std::uint32_t kBreakpoint = 0x80000003u;
constexpr std::uint32_t kSingleStep = 0x80000004u;
constexpr std::uint32_t kCxxException = 0xe06d7363u;

}  // namespace

const char* FaultActionName(FaultAction action) {
    switch (action) {
    case FaultAction::kPass: return "pass";
    case FaultAction::kLog: return "log";
    case FaultAction::kSuppressed: return "suppressed";
    }
    return "?";
}

bool FaultCodeIsNoise(std::uint32_t code) {
    return code == kDbgPrint || code == kDbgPrintContinuation || code == kBreakpoint || code == kSingleStep ||
           code == kCxxException;
}

FaultAction DecideFault(std::uint32_t code, std::uint32_t count, bool reentered) {
    if (reentered) return FaultAction::kPass;      // rule 2 first: never re-enter
    if (FaultCodeIsNoise(code)) return FaultAction::kPass;  // rule 1
    if (count >= FaultGate::kMaxLogged) return FaultAction::kSuppressed;  // rule 3
    return FaultAction::kLog;
}

bool FaultGate::Enter() {
    long expected = 0;
    return guard_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire);
}

void FaultGate::Leave() { guard_.store(0, std::memory_order_release); }

FaultAction FaultGate::EnterAndAdmit(std::uint32_t code) {
    if (!Enter()) return FaultAction::kPass;
    const FaultAction action = DecideFault(code, count_.load(std::memory_order_relaxed), false);
    if (action == FaultAction::kLog) {
        count_.fetch_add(1, std::memory_order_relaxed);
        return action;
    }
    if (action == FaultAction::kSuppressed) {
        if (!suppressed_.exchange(true, std::memory_order_relaxed)) {
            return FaultAction::kSuppressed;
        }
    }
    Leave();
    return FaultAction::kPass;
}

FaultAction FaultGate::Admit(std::uint32_t code) {
    const FaultAction action = DecideFault(code, count_.load(std::memory_order_relaxed),
                                           guard_.load(std::memory_order_relaxed) != 0);
    if (action == FaultAction::kLog) count_.fetch_add(1, std::memory_order_relaxed);
    if (action == FaultAction::kSuppressed) {
        // One line for the whole storm: the first fault past the cap says how many
        // were dropped, and the rest are passed on in silence.
        return suppressed_.exchange(true, std::memory_order_relaxed) ? FaultAction::kPass
                                                                     : FaultAction::kSuppressed;
    }
    return action;
}

}  // namespace bbx::core
