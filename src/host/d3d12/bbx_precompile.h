// SPDX-License-Identifier: GPL-3.0-or-later
// The precompile pass: what to compile, in what order, and when to stop.
//
// The work itself (DXC, PSO creation) lives in dxil.cpp/pipeline.cpp, because
// it needs the console. This is the policy, which is where the failures were:
// skip what is quarantined, reuse what the cache already has with this build's
// stamp, pause instead of pushing the process into the ceiling, and count
// everything so the report can explain a slow or missing warm start.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "host/d3d12/bbx_budget.h"
#include "host/d3d12/bbx_quarantine.h"

namespace bbx::d3d12 {

struct PlanUnit {
    Unit unit;
    std::uint64_t key = 0;   // the cache key (interface or PSO hash)
    std::string origin;      // "bundle", "manifest", "eboot"
};

enum class StepAction : std::uint8_t {
    kCompile = 0,
    kReuse,            // already in the cache with the current stamp
    kSkipQuarantined,  // a previous run crashed on it
    kWait,             // memory pressure: call again after the frame
    kDone,
};

[[nodiscard]] const char* StepActionName(StepAction action);

struct Step {
    StepAction action = StepAction::kDone;
    std::size_t index = 0;  // into the unit list
    PlanUnit unit;
    // For kWait: how much headroom the caller has to find before the next
    // attempt can succeed.
    std::uint64_t needed_headroom = 0;
};

struct PrecompileCounts {
    std::size_t total = 0;
    std::size_t duplicates = 0;
    std::size_t compiled = 0;
    std::size_t failed = 0;
    std::size_t reused = 0;
    std::size_t quarantined = 0;
    std::size_t waits = 0;

    [[nodiscard]] std::string Describe() const;
};

class Precompiler {
public:
    // `units` is not modified; duplicates (same key) are collapsed. `cached`
    // holds the keys the cache has with the current stamp. `budget` may be
    // null (tests, or a pass that should not check memory).
    Precompiler(std::vector<PlanUnit> units, Quarantine quarantine,
                std::set<std::uint64_t> cached, Budget* budget = nullptr);

    // The next thing to do. Call `Complete` exactly once after kCompile/kWait
    // (a kReuse/kSkipQuarantined/kDone step advances by itself on the next
    // call).
    [[nodiscard]] Step Next();

    // `ok == false` counts a failure and quarantines the unit, so a shader DXC
    // rejects is not retried every boot.
    void Complete(bool ok, std::uint64_t bytes_used = 0);

    [[nodiscard]] const PrecompileCounts& counts() const { return counts_; }
    [[nodiscard]] const Quarantine& quarantine() const { return quarantine_; }
    [[nodiscard]] bool done() const { return cursor_ >= units_.size(); }
    [[nodiscard]] std::size_t pending() const {
        return units_.size() > cursor_ ? units_.size() - cursor_ : 0;
    }

private:
    std::vector<PlanUnit> units_;
    Quarantine quarantine_;
    std::set<std::uint64_t> cached_;
    Budget* budget_ = nullptr;
    std::size_t cursor_ = 0;
    bool waiting_ = false;
    PrecompileCounts counts_;
};

}  // namespace bbx::d3d12
