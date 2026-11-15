// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_budget.h"

using namespace bbx::d3d12;

TEST(PlanSumsToTheCeiling) {
    std::uint64_t sum = 0;
    for (const OwnerBudget& owner : Budget::Plan()) sum += owner.cap;
    CHECK_EQ(sum, kGameModeLimit);
    // The documentation's table is the plan; a change here is a doc change.
    CHECK_EQ(Budget::Plan()[static_cast<int>(Owner::kGuest)].cap, 2048ull << 20);
    CHECK_EQ(Budget::Plan()[static_cast<int>(Owner::kReserve)].cap, 640ull << 20);
}

TEST(CapsAreHard) {
    Budget budget;
    CHECK(budget.Allocate(Owner::kHeaps, 700ull << 20).ok);
    const AllocationResult over = budget.Allocate(Owner::kHeaps, 100ull << 20);
    CHECK(!over.ok);
    CHECK(!over.reason.empty());
    // Taking from a different owner still works and does not borrow.
    CHECK(budget.Allocate(Owner::kGuest, 100ull << 20).ok);
    CHECK_EQ(budget.used_by(Owner::kHeaps), 700ull << 20);
    budget.Free(Owner::kHeaps, 700ull << 20);
    CHECK_EQ(budget.used_by(Owner::kHeaps), 0ull);
}

TEST(ReserveIsNotAnOwner) {
    Budget budget;
    CHECK(!budget.Allocate(Owner::kReserve, 1).ok);
}

TEST(ResidencyTiersTriggerAtTheDocumentedFractions) {
    Budget budget;
    CHECK(budget.Level() == ResidencyLevel::kNormal);
    // The tiers are fractions of the allocatable budget (5,120 - 640 reserve =
    // 4,480 MiB): 75% = 3,360, 85% = 3,808, 92% = 4,122.
    CHECK(budget.Allocate(Owner::kGuest, 2048ull << 20).ok);
    CHECK(budget.Allocate(Owner::kGpuShadows, 896ull << 20).ok);
    CHECK(budget.Allocate(Owner::kHeaps, 416ull << 20).ok);  // 3,360 exactly
    CHECK(budget.Level() == ResidencyLevel::kElevated);
    CHECK(budget.Allocate(Owner::kHeaps, 352ull << 20).ok);  // 3,712
    CHECK(budget.Allocate(Owner::kMedia, 96ull << 20).ok);   // 3,808 exactly
    CHECK(budget.Level() == ResidencyLevel::kHigh);
    CHECK(budget.Allocate(Owner::kShaderTools, 256ull << 20).ok);
    CHECK(budget.Allocate(Owner::kIo, 58ull << 20).ok);  // 4,122
    CHECK(budget.Level() == ResidencyLevel::kCritical);

    const PressurePolicy critical = budget.PolicyFor(ResidencyLevel::kCritical);
    CHECK(critical.trim_dxil_cache);
    CHECK(critical.trim_dxgi);
    CHECK(critical.drop_resolution_step);
    CHECK_EQ(critical.table_keep_critical, 64u);
}

TEST(PagerHasHysteresis) {
    Budget budget;
    // Start paging at 4,480 - 384 = 4,096 MiB, stop at 4,480 - 512 = 3,968.
    CHECK(budget.Allocate(Owner::kGuest, 2048ull << 20).ok);
    CHECK(budget.Allocate(Owner::kGpuShadows, 896ull << 20).ok);
    CHECK(budget.Allocate(Owner::kHeaps, 768ull << 20).ok);
    CHECK(budget.Allocate(Owner::kShaderTools, 256ull << 20).ok);   // 3,968
    CHECK(!budget.ShouldPage());
    CHECK(budget.Allocate(Owner::kMedia, 128ull << 20).ok);         // 4,096
    CHECK(budget.ShouldPage());
    // Still inside the band: paging continues.
    budget.Free(Owner::kMedia, 64ull << 20);                        // 4,032
    CHECK(budget.ShouldPage());
    // Below 3,968 it stops.
    budget.Free(Owner::kMedia, 64ull << 20);                        // 3,968
    CHECK(!budget.ShouldPage());
}

TEST(PagingNeverUnderflows) {
    Budget budget;
    budget.NotePagedOut(1ull << 20);
    CHECK_EQ(budget.used_by(Owner::kGuest), 0ull);
    budget.NotePagedIn(1ull << 20);
    CHECK_EQ(budget.used_by(Owner::kGuest), 1ull << 20);
}

TEST(CompileNeedsHeadroom) {
    Budget budget;
    CHECK(budget.CanCompile());
    CHECK(budget.Allocate(Owner::kGuest, 2048ull << 20).ok);
    CHECK(budget.CanCompile());  // ~2.4 GiB of headroom
    CHECK(budget.Allocate(Owner::kGpuShadows, 896ull << 20).ok);
    CHECK(budget.Allocate(Owner::kHeaps, 700ull << 20).ok);  // used 3,644
    CHECK(budget.CanCompile());                              // 836 MiB >= the 768 reserve
    CHECK(!budget.CanCompile(256ull << 20));                 // 256 more needs 1,024
    budget.Free(Owner::kHeaps, 400ull << 20);
    CHECK(budget.CanCompile(256ull << 20));
}

TEST(DescribeNamesEveryOwner) {
    Budget budget;
    CHECK(budget.Allocate(Owner::kGuest, 1024ull << 20).ok);
    const std::string line = budget.Describe();
    CHECK(line.find("Budget: limit 5120 MiB") != std::string::npos);
    CHECK(line.find("guest 1024/2048") != std::string::npos);
    CHECK(line.find("pager idle") != std::string::npos);
}

TEST(ScaledLimitKeepsThePlanAdditive) {
    Budget small(4ull << 30);  // 4 GiB, as a test or a future limit would be
    std::uint64_t total = 0;
    for (Owner owner : {Owner::kGuest, Owner::kGpuShadows, Owner::kHeaps, Owner::kShaderTools,
                        Owner::kMedia, Owner::kHost, Owner::kIo, Owner::kReserve}) {
        total += small.cap(owner);
    }
    CHECK_EQ(total, 4ull << 30);
}
