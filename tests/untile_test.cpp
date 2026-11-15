// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_untile.h"

#include <set>

using namespace bbx::d3d12;

TEST(MicroTileIsABijection) {
    for (const Tiling tiling : {Tiling::kThin1D, Tiling::kThin2D, Tiling::kThick2D}) {
        const MicroTile tile = MicroTileFor(tiling);
        std::set<std::uint32_t> seen;
        for (std::uint32_t z = 0; z < tile.depth; ++z) {
            for (std::uint32_t y = 0; y < tile.height; ++y) {
                for (std::uint32_t x = 0; x < tile.width; ++x) {
                    const std::uint32_t index = MicroTileIndex(tiling, x, y, z);
                    CHECK(index < tile.elements());
                    CHECK(seen.insert(index).second);  // every element once
                    // And the inverse agrees.
                    const TileCoord c = MicroTileCoord(tiling, index);
                    CHECK_EQ(c.x, x);
                    CHECK_EQ(c.y, y);
                    CHECK_EQ(c.z, z);
                }
            }
        }
        CHECK_EQ(seen.size(), tile.elements());
    }
}

TEST(MicroTileIndexKnownValues) {
    // The first six indices of the thin swizzle are x0 y0 x1 y1 x2 y2.
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 0, 0), 0u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 1, 0), 1u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 0, 1), 2u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 2, 0), 4u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 0, 2), 8u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 4, 0), 16u);
    // Thick interleaves z: x0 y0 z0 x1 y1 z1 x2 y2.
    CHECK_EQ(MicroTileIndex(Tiling::kThick2D, 0, 0, 1), 4u);
    CHECK_EQ(MicroTileIndex(Tiling::kThick2D, 2, 0, 0), 8u);
    // Out of range is the element count, never an in-range index.
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 8, 0), 64u);
    CHECK_EQ(MicroTileIndex(Tiling::kThin2D, 0, 0, 1), 64u);
}

TEST(LinearPlanNeedsNoDispatch) {
    const FormatInfo rgba = FromGcnFormat(10, 0);
    const UntilePlan plan = PlanUntile(rgba, Tiling::kLinear, 256, 256, 1, 0, 0);
    CHECK(plan.valid);
    CHECK(plan.dispatches.empty());
    CHECK_EQ(plan.total_groups(), 0u);
    CHECK(CanCopyLinear(Tiling::kLinear, 256, 256));
    CHECK(!CanCopyLinear(Tiling::kThin2D, 256, 256));
}

TEST(ThinPlanCoversEveryTile) {
    const FormatInfo rgba = FromGcnFormat(10, 0);
    // 256x256 = 32x32 tiles, one dispatch, one group per tile.
    const UntilePlan plan = PlanUntile(rgba, Tiling::kThin2D, 256, 256, 1, 0x1000, 0x2000);
    CHECK(plan.valid);
    CHECK_EQ(plan.dispatches.size(), 1u);
    CHECK_EQ(plan.dispatches[0].tiles_x, 32u);
    CHECK_EQ(plan.dispatches[0].tiles_y, 32u);
    CHECK_EQ(plan.dispatches[0].groups_x, 32u);
    CHECK_EQ(plan.dispatches[0].groups_z, 1u);
    CHECK_EQ(plan.dispatches[0].src_offset, 0x1000ull);
    CHECK_EQ(plan.dispatches[0].dst_offset, 0x2000ull);
    CHECK_EQ(plan.total_groups(), 1024u);
}

TEST(PlanSplitsHugeExtents) {
    const FormatInfo rgba = FromGcnFormat(10, 0);
    // 64,000 pixels wide = 8,000 tiles: two x chunks (4,096 + 3,904).
    const UntilePlan plan = PlanUntile(rgba, Tiling::kThin2D, 64'000, 8, 1, 0, 0);
    CHECK(plan.valid);
    CHECK_EQ(plan.dispatches.size(), 2u);
    CHECK_EQ(plan.dispatches[0].tiles_x, 4096u);
    CHECK_EQ(plan.dispatches[1].tiles_x, 8000u - 4096u);
    CHECK_EQ(plan.total_groups(), 8000u);
}

TEST(PlanHandlesMipsSlicesAndOddSizes) {
    const FormatInfo bc3 = FromGcnFormat(37, 0);
    // 65x65 BC3: 17x17 blocks -> 3x3 tiles of 4x4 blocks; nothing may be lost
    // to rounding down.
    const UntilePlan plan = PlanUntile(bc3, Tiling::kThin2D, 17, 17, 1, 0, 0);
    CHECK(plan.valid);
    CHECK_EQ(plan.dispatches[0].tiles_x, 3u);
    CHECK_EQ(plan.dispatches[0].tiles_y, 3u);
    // A volume texture uses the thick mode: one z-tile covers four slices.
    const FormatInfo rgba = FromGcnFormat(10, 0);
    const UntilePlan volume = PlanUntile(rgba, Tiling::kThick2D, 64, 64, 16, 0, 0);
    CHECK(volume.valid);
    CHECK_EQ(volume.dispatches.size(), 1u);
    CHECK_EQ(volume.dispatches[0].tiles_z, 4u);
    CHECK_EQ(volume.dispatches[0].groups_z, 4u);
}

TEST(PlanRejectsNonsense) {
    const FormatInfo unknown;
    CHECK(!PlanUntile(unknown, Tiling::kThin2D, 16, 16, 1, 0, 0).valid);
    const FormatInfo rgba = FromGcnFormat(10, 0);
    CHECK(!PlanUntile(rgba, Tiling::kThin2D, 0, 16, 1, 0, 0).valid);
    const UntilePlan plan = PlanUntile(rgba, Tiling::kThin2D, 0, 16, 1, 0, 0);
    CHECK(!plan.reason.empty());
}
