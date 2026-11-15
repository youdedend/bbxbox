// SPDX-License-Identifier: GPL-3.0-or-later
// P0's exit criterion, as a function: prove the memory contract before the game
// starts, and write the numbers where a bug report can quote them.
//
// The contract has four parts, and a partial pass is a failure:
//   1. the AppContainer lets us allocate executable memory (codeGeneration) and
//      call it with the System V ABI bbhost's thunks use;
//   2. W^X holds: a page is writable, becomes executable, and is never both;
//   3. the guest arena lands below 1 TiB and its commitment agrees with the
//      budget's guest owner;
//   4. the app's own memory use at the moment of the check is under the P0
//      allowance (1,100 MiB), so the launcher is not already spending the frame
//      budget.
#pragma once

#include <cstdint>
#include <string>

namespace bbx::platform {

struct SelfTestResult {
    bool code_generation = false;  // dynamic code + sysv_abi call
    bool wx = false;               // never RWX, RX after flush
    bool guest_arena = false;      // allocated below 1 TiB
    bool budget = false;           // commitment agrees with the plan
    bool under_allowance = false;  // P0 startup allowance
    std::uint64_t app_mib = 0;     // AppMemoryUsage at the check
    std::uint64_t limit_mib = 0;   // AppMemoryUsageLimit
    std::uint64_t guest_mib = 0;   // the arena's commitment
    std::string detail;            // one line per step, for the report file

    [[nodiscard]] bool Passed() const {
        return code_generation && wx && guest_arena && budget && under_allowance;
    }
};

// Runs the four checks. `guest_arena_probe` is false when the launcher runs the
// self-test before any guest memory exists, in which case a 64 MiB probe arena is
// allocated and freed instead.
SelfTestResult RunMemorySelfTest(bool guest_arena_probe);

// Writes `memory-selftest.txt` into LocalState and returns the path (empty on
// failure). The file is plain text: a tester can paste it into an issue.
std::string WriteSelfTestReport(const SelfTestResult& result);

// The P0 allowance: the launcher, SDL3, the D3D12 device and the caches loaded
// so far. Anything above this means a subsystem is holding memory it should not.
inline constexpr std::uint64_t kStartupAllowanceMib = 1100;

}  // namespace bbx::platform
