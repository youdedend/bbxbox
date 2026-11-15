// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_precompile.h"

#include <algorithm>
#include <cstdio>

namespace bbx::d3d12 {
namespace {
// Kept in step with bbx_budget.cpp's compile reserve: the policy here asks the
// budget whether it can take `kNeededNow`, and reports the shortfall so a log
// says why a pass paused.
constexpr std::uint64_t kNeededNow = 64ull << 20;
constexpr std::uint64_t kReserve = 768ull << 20;
}  // namespace

const char* StepActionName(StepAction action) {
    switch (action) {
    case StepAction::kCompile: return "compile";
    case StepAction::kReuse: return "reuse";
    case StepAction::kSkipQuarantined: return "quarantined";
    case StepAction::kWait: return "wait";
    case StepAction::kDone: return "done";
    }
    return "?";
}

std::string PrecompileCounts::Describe() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "precompile: %zu units (%zu duplicate), %zu compiled, %zu reused, "
                  "%zu quarantined, %zu failed, %zu waits",
                  total, duplicates, compiled, reused, quarantined, failed, waits);
    return buf;
}

Precompiler::Precompiler(std::vector<PlanUnit> units, Quarantine quarantine,
                         std::set<std::uint64_t> cached, Budget* budget)
    : quarantine_(std::move(quarantine)), cached_(std::move(cached)), budget_(budget) {
    units_.reserve(units.size());
    std::set<std::uint64_t> seen;
    for (PlanUnit& u : units) {
        if (u.key == 0) u.key = u.unit.hash;  // stages default to their hash
        if (!seen.insert(u.key).second) {
            ++counts_.duplicates;
            continue;
        }
        units_.push_back(std::move(u));
    }
    counts_.total = units_.size();
}

Step Precompiler::Next() {
    Step step;
    if (cursor_ >= units_.size()) {
        step.action = StepAction::kDone;
        return step;
    }
    const PlanUnit& unit = units_[cursor_];
    step.index = cursor_;
    step.unit = unit;

    if (quarantine_.Contains(unit.unit)) {
        step.action = StepAction::kSkipQuarantined;
        ++counts_.quarantined;
        ++cursor_;
        waiting_ = false;
        return step;
    }
    if (cached_.count(unit.key) != 0) {
        step.action = StepAction::kReuse;
        ++counts_.reused;
        ++cursor_;
        waiting_ = false;
        return step;
    }
    if (budget_ != nullptr && !budget_->CanCompile(kNeededNow)) {
        step.action = StepAction::kWait;
        step.needed_headroom = kReserve + kNeededNow;
        ++counts_.waits;
        waiting_ = true;
        return step;  // cursor stays: the same unit is retried after the frame
    }
    step.action = StepAction::kCompile;
    waiting_ = false;
    return step;
}

void Precompiler::Complete(bool ok, std::uint64_t bytes_used) {
    if (cursor_ >= units_.size()) return;
    const PlanUnit& unit = units_[cursor_];
    if (ok) {
        ++counts_.compiled;
        cached_.insert(unit.key);
    } else {
        ++counts_.failed;
        // A shader or PSO that failed is quarantined: retrying it every boot is
        // how a port ends up with a startup that takes a minute and never gets
        // further.
        quarantine_.Add(unit.unit);
    }
    if (bytes_used != 0 && budget_ != nullptr) {
        (void)budget_->Allocate(Owner::kShaderTools, bytes_used);
    }
    ++cursor_;
    waiting_ = false;
}

}  // namespace bbx::d3d12
