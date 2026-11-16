// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_untile.h"

#include <algorithm>

#include "host/d3d12/bbx_common.h"

namespace bbx::d3d12 {
namespace {
constexpr std::uint32_t kMaxGroups = 4096;  // per dimension, kept well under 65535
std::uint32_t CeilDiv(std::uint32_t a, std::uint32_t b) { return b ? (a + b - 1) / b : 0; }
}  // namespace

const char* TilingName(Tiling tiling) {
    switch (tiling) {
    case Tiling::kLinear: return "linear";
    case Tiling::kThin1D: return "1d-thin";
    case Tiling::kThin2D: return "2d-thin";
    case Tiling::kThick2D: return "2d-thick";
    }
    return "?";
}

MicroTile MicroTileFor(Tiling tiling) {
    switch (tiling) {
    case Tiling::kLinear: return MicroTile{1, 1, 1};
    case Tiling::kThin1D:
    case Tiling::kThin2D: return MicroTile{8, 8, 1};
    case Tiling::kThick2D: return MicroTile{8, 8, 4};
    }
    return MicroTile{1, 1, 1};
}

std::uint32_t MicroTileIndex(Tiling tiling, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const MicroTile tile = MicroTileFor(tiling);
    if (x >= tile.width || y >= tile.height || z >= tile.depth) return tile.elements();
    switch (tiling) {
    case Tiling::kLinear:
        return 0;
    case Tiling::kThin1D:
    case Tiling::kThin2D:
        // x0 y0 x1 y1 x2 y2
        return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) |
               ((y & 4u) << 3);
    case Tiling::kThick2D:
        // x0 y0 z0 x1 y1 z1 x2 y2
        return (x & 1u) | ((y & 1u) << 1) | ((z & 1u) << 2) | ((x & 2u) << 2) | ((y & 2u) << 3) |
               ((z & 2u) << 4) | ((x & 4u) << 4) | ((y & 4u) << 5);
    }
    return tile.elements();
}

TileCoord MicroTileCoord(Tiling tiling, std::uint32_t index) {
    TileCoord c;
    if (index >= MicroTileFor(tiling).elements()) return c;
    switch (tiling) {
    case Tiling::kLinear:
        break;
    case Tiling::kThin1D:
    case Tiling::kThin2D:
        c.x = (index & 1u) | ((index >> 2) & 1u) << 1 | ((index >> 4) & 1u) << 2;
        c.y = ((index >> 1) & 1u) | ((index >> 3) & 1u) << 1 | ((index >> 5) & 1u) << 2;
        break;
    case Tiling::kThick2D:
        c.x = (index & 1u) | ((index >> 3) & 1u) << 1 | ((index >> 6) & 1u) << 2;
        c.y = ((index >> 1) & 1u) | ((index >> 4) & 1u) << 1 | ((index >> 7) & 1u) << 2;
        c.z = ((index >> 2) & 1u) | ((index >> 5) & 1u) << 1;
        break;
    }
    return c;
}

std::uint32_t UntilePlan::total_groups() const {
    std::uint32_t total = 0;
    for (const UntileDispatch& d : dispatches) total += d.groups_x * d.groups_y * d.groups_z;
    return total;
}

UntilePlan PlanUntile(FormatInfo format, Tiling tiling, std::uint32_t width, std::uint32_t height,
                      std::uint32_t depth, std::uint64_t src_offset, std::uint64_t dst_offset) {
    UntilePlan plan;
    plan.tiling = tiling;
    plan.format = format.format;
    plan.width = width;
    plan.height = height;
    plan.depth = std::max(1u, depth);
    if (!format.valid()) {
        plan.reason = "unknown format";
        return plan;
    }
    if (width == 0 || height == 0) {
        plan.reason = "empty extent";
        return plan;
    }
    // The destination's rows are the texture copy's rows: D3D12's pitch, which
    // is also the stride UntileCs stores a row at.
    plan.dst_row_pitch =
        static_cast<std::uint32_t>(AlignUp(static_cast<std::uint64_t>(width) * format.bytes_per_block, 256));
    if (tiling == Tiling::kLinear) {
        // Nothing to detile: the copy queue does it, and a plan with no
        // dispatch means exactly that.
        plan.valid = true;
        plan.reason = "linear";
        return plan;
    }
    // For block-compressed formats the tiling applies to the block grid, so the
    // extents are the block extents the caller passed in.
    const MicroTile tile = MicroTileFor(tiling);
    const std::uint32_t tiles_x = CeilDiv(width, tile.width);
    const std::uint32_t tiles_y = CeilDiv(height, tile.height);
    // One z-tile of the thick mode covers `tile.depth` linear slices; the thin
    // modes tile one slice at a time.
    const std::uint32_t z_per_tile = tiling == Tiling::kThick2D ? tile.depth : 1;
    const std::uint32_t z_tiles = CeilDiv(plan.depth, z_per_tile);
    if (tiles_x == 0 || tiles_y == 0 || z_tiles == 0) {
        plan.reason = "no tiles";
        return plan;
    }

    const std::uint64_t element_bytes = format.bytes_per_block;
    const std::uint64_t bytes_per_z_tile =
        static_cast<std::uint64_t>(tiles_x) * tiles_y * tile.elements() * element_bytes;
    const std::uint32_t step_x = std::min(tiles_x, kMaxGroups);
    const std::uint32_t step_y = std::min(tiles_y, kMaxGroups);
    const std::uint32_t step_z = std::min(z_tiles, kMaxGroups);
    plan.src_row_pitch = static_cast<std::uint32_t>(
        AlignUp(static_cast<std::uint64_t>(tiles_x) * tile.width * element_bytes, 256));

    for (std::uint32_t tz = 0; tz < z_tiles; tz += step_z) {
        const std::uint32_t z_count = std::min(z_tiles - tz, step_z);
        for (std::uint32_t ty = 0; ty < tiles_y; ty += step_y) {
            const std::uint32_t y_count = std::min(tiles_y - ty, step_y);
            for (std::uint32_t tx = 0; tx < tiles_x; tx += step_x) {
                const std::uint32_t x_count = std::min(tiles_x - tx, step_x);
                UntileDispatch d;
                d.tiles_x = x_count;
                d.tiles_y = y_count;
                d.tiles_z = z_count;
                // 64 threads per group: one thread per element of a micro tile.
                d.groups_x = x_count;
                d.groups_y = y_count;
                d.groups_z = z_count;
                d.src_offset = src_offset + static_cast<std::uint64_t>(tz) * bytes_per_z_tile +
                               static_cast<std::uint64_t>(ty * tiles_x + tx) * tile.elements() *
                                   element_bytes;
                d.dst_offset = dst_offset +
                               static_cast<std::uint64_t>(tz) * z_per_tile * width * height *
                                   element_bytes;
                plan.dispatches.push_back(d);
            }
        }
    }
    plan.valid = true;
    return plan;
}

bool CanCopyLinear(Tiling tiling, std::uint32_t width, std::uint32_t height) {
    (void)width;
    (void)height;
    return tiling == Tiling::kLinear;
}

}  // namespace bbx::d3d12
