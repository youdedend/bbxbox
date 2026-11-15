// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_precompile.h"

#include <string>
#include <vector>

using namespace bbx::d3d12;

namespace {
PlanUnit MakePlanUnit(std::uint64_t hash, std::uint64_t partner = 0) {
    PlanUnit p;
    p.unit.hash = hash;
    p.unit.partner = partner;
    p.unit.kind = ShaderKind::kPixel;
    p.unit.bundle = "allmaterialbnd.shaderbnd.dcx";
    p.unit.entry = "GXFlver_Gbuf_[A].ppo";
    p.key = hash;  // stages use their hash as the cache key
    p.origin = "bundle";
    return p;
}
}  // namespace

TEST(DuplicatesCollapse) {
    std::vector<PlanUnit> units = {MakePlanUnit(1), MakePlanUnit(2), MakePlanUnit(1)};
    Precompiler pre(units, Quarantine{}, {});
    CHECK_EQ(pre.counts().duplicates, 1u);
    CHECK_EQ(pre.counts().total, 2u);
    CHECK_EQ(pre.pending(), 2u);
}

TEST(CachedAndQuarantinedAreSkipped) {
    Quarantine q;
    q.Add(0x30ull);
    std::vector<PlanUnit> units = {MakePlanUnit(0x10), MakePlanUnit(0x20), MakePlanUnit(0x30),
                                   MakePlanUnit(0x40)};
    Precompiler pre(units, q, {0x20});
    Step step = pre.Next();
    CHECK(step.action == StepAction::kCompile);
    CHECK_EQ(step.unit.unit.hash, 0x10ull);
    pre.Complete(true);
    CHECK(pre.Next().action == StepAction::kReuse);
    step = pre.Next();
    CHECK(step.action == StepAction::kSkipQuarantined);
    CHECK_EQ(step.unit.unit.hash, 0x30ull);
    step = pre.Next();
    CHECK(step.action == StepAction::kCompile);
    CHECK_EQ(step.unit.unit.hash, 0x40ull);
    pre.Complete(true);
    CHECK(pre.Next().action == StepAction::kDone);
    CHECK_EQ(pre.counts().reused, 1u);
    CHECK_EQ(pre.counts().quarantined, 1u);
    CHECK_EQ(pre.counts().compiled, 2u);
}

TEST(FailureQuarantinesSoTheNextBootDoesNotRetry) {
    std::vector<PlanUnit> units = {MakePlanUnit(0xaa)};
    Precompiler pre(units, Quarantine{}, {});
    CHECK(pre.Next().action == StepAction::kCompile);
    pre.Complete(false);
    CHECK_EQ(pre.counts().failed, 1u);
    CHECK(pre.quarantine().Contains(0xaaull));

    // The same unit list next boot sees it quarantined.
    Precompiler next(units, pre.quarantine(), {});
    CHECK(next.Next().action == StepAction::kSkipQuarantined);
}

TEST(WaitOnMemoryPressureThenResume) {
    Budget budget;
    // Fill to within 100 MiB of the allocatable budget.
    CHECK(budget.Allocate(Owner::kGuest, 2048ull << 20).ok);
    CHECK(budget.Allocate(Owner::kGpuShadows, 896ull << 20).ok);
    CHECK(budget.Allocate(Owner::kHeaps, 768ull << 20).ok);
    CHECK(budget.Allocate(Owner::kShaderTools, 256ull << 20).ok);
    CHECK(budget.Allocate(Owner::kMedia, 128ull << 20).ok);  // 4,096 of 4,480

    std::vector<PlanUnit> units = {MakePlanUnit(1)};
    Precompiler pre(units, Quarantine{}, {}, &budget);
    const Step waiting = pre.Next();
    CHECK(waiting.action == StepAction::kWait);
    CHECK(waiting.needed_headroom > 0);
    CHECK_EQ(pre.counts().waits, 1u);
    // The cursor did not move: the same unit comes back.
    CHECK_EQ(pre.Next().action, StepAction::kWait);

    // The frame gives memory back; the pass resumes.
    budget.Free(Owner::kMedia, 128ull << 20);
    budget.Free(Owner::kShaderTools, 256ull << 20);
    budget.Free(Owner::kGpuShadows, 128ull << 20);
    CHECK(pre.Next().action == StepAction::kCompile);
    pre.Complete(true, 8ull << 20);
    CHECK_EQ(pre.counts().compiled, 1u);
    CHECK_EQ(budget.used_by(Owner::kShaderTools), 8ull << 20);
}

TEST(CountsDescribeTheRun) {
    std::vector<PlanUnit> units = {MakePlanUnit(1), MakePlanUnit(2), MakePlanUnit(2)};
    Precompiler pre(units, Quarantine{}, {2});
    CHECK(pre.Next().action == StepAction::kCompile);
    pre.Complete(true);
    CHECK(pre.Next().action == StepAction::kReuse);
    CHECK(pre.Next().action == StepAction::kDone);
    const std::string line = pre.counts().Describe();
    CHECK(line.find("2 units") != std::string::npos);
    CHECK(line.find("1 compiled") != std::string::npos);
    CHECK(line.find("1 reused") != std::string::npos);
    CHECK(line.find("1 duplicate") != std::string::npos);
}
