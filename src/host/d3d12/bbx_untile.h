// SPDX-License-Identifier: GPL-3.0-or-later
// GCN texture tiling: the micro-tile permutation and the detile plan.
//
// The game writes textures in GCN's tiled layouts. The port detiles them on the
// GPU: a compute shader reads each 8x8 micro tile in its swizzled order and
// writes linear texels, then a copy moves the result into the real texture.
//
// This file owns the two things that must be exactly right and are testable on
// a build machine:
//   * the micro-tile permutation (thin: 8x8, thick: 8x8x4), which is a bijection
//     over the tile's elements -- an off-by-one here skews every texture that
//     uses the mode;
//   * the job plan: how many dispatches, with what extents, so the shader has no
//     loop bounds arithmetic of its own.
// The macro-tile (pipe/bank) addressing lives in the shader and mirrors
// bbhost's src/host/textures.cpp; the plan passes it the mode.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "host/d3d12/bbx_format.h"

namespace bbx::d3d12 {

enum class Tiling : std::uint8_t {
    kLinear = 0,
    kThin1D = 1,   // ARRAY_1D_TILED_THIN1
    kThin2D = 2,   // ARRAY_2D_TILED_THIN1
    kThick2D = 3,  // ARRAY_2D_TILED_THICK
};

[[nodiscard]] const char* TilingName(Tiling tiling);

struct MicroTile {
    std::uint32_t width = 8;
    std::uint32_t height = 8;
    std::uint32_t depth = 1;  // 1 thin, 4 thick
    std::uint32_t elements() const { return width * height * depth; }
};

[[nodiscard]] MicroTile MicroTileFor(Tiling tiling);

// Element index inside a micro tile, exactly as GCN's swizzle orders them
// (bbhost's thin_index/thick_index). Returns `elements()` for an out-of-range
// coordinate rather than reading out of bounds.
[[nodiscard]] std::uint32_t MicroTileIndex(Tiling tiling, std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z = 0);

// The clean element coordinate (linear within the tile) for a swizzled index.
// The inverse of MicroTileIndex; the test asserts the round trip.
struct TileCoord {
    std::uint32_t x = 0, y = 0, z = 0;
};
[[nodiscard]] TileCoord MicroTileCoord(Tiling tiling, std::uint32_t index);

// One dispatch of the detile shader.
struct UntileDispatch {
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::uint32_t tiles_z = 1;
    std::uint32_t groups_x = 0;  // 64 threads per group, one thread per element
    std::uint32_t groups_y = 0;
    std::uint32_t groups_z = 1;
    std::uint64_t src_offset = 0;
    std::uint64_t dst_offset = 0;
};

struct UntilePlan {
    bool valid = false;
    std::string reason;
    Tiling tiling = Tiling::kLinear;
    DxgiFormat format = DxgiFormat::kUnknown;
    std::uint32_t width = 0, height = 0, depth = 1;
    std::uint32_t src_row_pitch = 0;  // bytes, for the tiled source's outer stride
    // Bytes per row of the linear destination: D3D12's 256-byte pitch for a
    // texture copy, and the stride UntileCs writes a row at.
    std::uint32_t dst_row_pitch = 0;
    std::vector<UntileDispatch> dispatches;

    [[nodiscard]] std::uint32_t total_groups() const;
};

// Plans the detile of one mip/slice run. `is_3d` uses the thick mode's z
// dimension; linear sources return a single copy-shaped plan with no compute.
[[nodiscard]] UntilePlan PlanUntile(FormatInfo format, Tiling tiling, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t depth,
                                    std::uint64_t src_offset, std::uint64_t dst_offset);

// True when a texture's extent and tiling mean it can be copied linearly with
// no detile at all (the census's fast path).
[[nodiscard]] bool CanCopyLinear(Tiling tiling, std::uint32_t width, std::uint32_t height);

}  // namespace bbx::d3d12
