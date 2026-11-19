// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_descriptors.h"

using namespace bbx::d3d12;

TEST(AllocateAndRetireByFence) {
    DescriptorRing ring(64);
    const auto a = ring.Allocate(8);
    const auto b = ring.Allocate(16);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK_EQ(a->offset, 0u);
    CHECK_EQ(b->offset, 8u);
    CHECK_EQ(ring.free(), 40u);
    CHECK_EQ(ring.high_water(), 24u);

    // Released in frame 3, reusable only once the fence passes 3.
    ring.Release(*a, 3);
    CHECK_EQ(ring.Collect(2), 0u);
    CHECK_EQ(ring.free(), 40u);
    const auto c = ring.Allocate(8);
    CHECK(c.has_value());
    CHECK_EQ(c->offset, 24u);  // the released range is not back yet
    CHECK_EQ(ring.Collect(3), 1u);
    CHECK_EQ(ring.free(), 40u);
    const auto d = ring.Allocate(8);
    CHECK(d.has_value());
    CHECK_EQ(d->offset, 0u);
    CHECK_EQ(ring.free(), 32u);
}

TEST(CoalescesAndReusesTheWholeHeap) {
    DescriptorRing ring(32);
    const auto a = ring.Allocate(8);
    const auto b = ring.Allocate(8);
    const auto c = ring.Allocate(8);
    const auto d = ring.Allocate(8);
    CHECK(!ring.Allocate(1).has_value());
    ring.Release(*a, 1);
    ring.Release(*b, 1);
    ring.Release(*c, 1);
    ring.Release(*d, 1);
    CHECK_EQ(ring.Collect(1), 4u);
    CHECK_EQ(ring.free(), 32u);
    const auto big = ring.Allocate(32);
    CHECK(big.has_value());
    CHECK_EQ(big->offset, 0u);
}

TEST(AlignmentKeepsThePaddingFree) {
    DescriptorRing ring(40);
    const auto a = ring.Allocate(3);
    CHECK(a.has_value());
    const auto aligned = ring.Allocate(4, 8);
    CHECK(aligned.has_value());
    CHECK_EQ(aligned->offset % 8, 0u);
    CHECK_EQ(aligned->offset, 8u);
    // 3..7 is padding and is still allocatable; an aligned request after it
    // must still honour the alignment.
    ring.Release(*aligned, 1);
    ring.Collect(1);
    const auto filler = ring.Allocate(3);
    CHECK(filler.has_value());
    CHECK_EQ(filler->offset, 3u);
    const auto aligned2 = ring.Allocate(4, 8);
    CHECK(aligned2.has_value());
    CHECK_EQ(aligned2->offset % 8, 0u);
}

TEST(PermanentReservationSurvivesResetOnly) {
    DescriptorRing ring(16);
    const auto samplers = ring.ReservePermanent(4);
    CHECK(samplers.has_value());
    CHECK_EQ(samplers->offset, 0u);
    CHECK_EQ(ring.free(), 12u);
    // A reset (device lost / test) clears everything, which is why the backend
    // re-creates the permanent tables at device creation.
    ring.Reset();
    CHECK_EQ(ring.free(), 16u);
    CHECK_EQ(ring.high_water(), 0u);
}

TEST(RejectsImpossibleRequests) {
    DescriptorRing ring(16);
    CHECK(!ring.Allocate(0).has_value());
    CHECK(!ring.Allocate(17).has_value());
    ring.Release(DescriptorSlice{}, 1);  // an empty slice is not an error
    CHECK_EQ(ring.Collect(100), 0u);
}

TEST(OldestPendingFenceTracksRetiredSlicesInOrder) {
    DescriptorRing ring(32);
    CHECK_EQ(ring.OldestPendingFence(), 0ull);
    const auto a = ring.Allocate(8);
    const auto b = ring.Allocate(8);
    const auto c = ring.Allocate(16);
    CHECK(a.has_value() && b.has_value() && c.has_value());

    ring.Release(*b, 10);
    ring.Release(*a, 5);
    ring.Release(*c, 20);
    CHECK_EQ(ring.OldestPendingFence(), 5ull);

    CHECK_EQ(ring.Collect(5), 1u);
    CHECK_EQ(ring.OldestPendingFence(), 10ull);
    CHECK_EQ(ring.free(), 8u);

    CHECK_EQ(ring.Collect(10), 1u);
    CHECK_EQ(ring.OldestPendingFence(), 20ull);
    CHECK_EQ(ring.free(), 16u);
    // a (0..7) and b (8..15) must have coalesced into one contiguous 16-slot run.
    const auto merged = ring.Allocate(16);
    CHECK(merged.has_value());
    CHECK_EQ(merged->offset, 0u);

    CHECK_EQ(ring.Collect(20), 1u);
    CHECK_EQ(ring.OldestPendingFence(), 0ull);
}
