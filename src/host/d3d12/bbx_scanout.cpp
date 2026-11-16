// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_scanout.h"

#include <algorithm>

namespace bbx::d3d12 {
namespace {

// D3D12 wants every row of a placed footprint on a 256-byte pitch.
constexpr std::uint64_t kRowPitchAlign = 256;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t to) {
    return (value + to - 1) / to * to;
}

// The four-byte unorm family the detile shader writes (untile.hlsl: one thread
// per 4-byte element). Anything else is presented linearly or not at all.
FormatInfo FourByteFormat(DxgiFormat format, bool srgb) {
    FormatInfo info;
    info.format = format;
    info.cls = FormatClass::kUnorm;
    info.bytes_per_block = 4;
    info.block_width = 1;
    info.block_height = 1;
    info.srgb = srgb;
    return info;
}

}  // namespace

const char* ScanoutFormatName(ScanoutFormat format) {
    switch (format) {
    case ScanoutFormat::kA8R8G8B8Srgb: return "A8R8G8B8_SRGB";
    case ScanoutFormat::kB8R8G8A8Srgb: return "B8R8G8A8_SRGB";
    case ScanoutFormat::kA8R8G8B8: return "A8R8G8B8";
    case ScanoutFormat::kB8R8G8A8: return "B8R8G8A8";
    case ScanoutFormat::kR8G8B8A8Srgb: return "R8G8B8A8_SRGB";
    case ScanoutFormat::kR8G8B8A8: return "R8G8B8A8";
    case ScanoutFormat::kA2R10G10B10: return "A2R10G10B10";
    case ScanoutFormat::kA2R10G10B10Srgb: return "A2R10G10B10_SRGB";
    case ScanoutFormat::kA2B10G10R10: return "A2B10G10R10";
    case ScanoutFormat::kA2B10G10R10Srgb: return "A2B10G10R10_SRGB";
    }
    return "unknown";
}

const char* ScanoutTilingName(ScanoutTiling tiling) {
    switch (tiling) {
    case ScanoutTiling::kLinear: return "linear";
    case ScanoutTiling::kTile: return "tiled";
    }
    return "unknown";
}

ScanoutState DescribeScanout(const ScanoutDesc& desc) {
    ScanoutState state;
    if (desc.width == 0 || desc.height == 0) {
        state.reason = "a display buffer with no pixels";
        return state;
    }

    // The name is the dword from the top channel down, so the byte order in
    // memory is the mirror of it. A8R8G8B8 -> B,G,R,A = B8G8R8A8; R8G8B8A8 is
    // already in the order DXGI names, which is why the two are different
    // DXGI formats and not a swizzle of one.
    FormatInfo info;
    bool four_byte = true;  // the detile shader's element size
    switch (desc.format) {
    case ScanoutFormat::kA8R8G8B8Srgb:
        state.srgb = true;
        state.format = DxgiFormat::kB8G8R8A8UnormSrgb;
        info = FourByteFormat(state.format, true);
        break;
    case ScanoutFormat::kA8R8G8B8:
        state.format = DxgiFormat::kB8G8R8A8Unorm;
        info = FourByteFormat(state.format, false);
        break;
    case ScanoutFormat::kR8G8B8A8Srgb:
        state.srgb = true;
        state.format = DxgiFormat::kR8G8B8A8UnormSrgb;
        info = FourByteFormat(state.format, true);
        break;
    case ScanoutFormat::kR8G8B8A8:
        state.format = DxgiFormat::kR8G8B8A8Unorm;
        info = FourByteFormat(state.format, false);
        break;
    case ScanoutFormat::kA2B10G10R10:
        // A in bits 30-31, B in 20-29, G in 10-19, R in 0-9: little-endian that
        // is DXGI's R10G10B10A2. The _SRGB spelling beside it has no DXGI
        // format at all (there is no R10G10B10A2_UNORM_SRGB), and the encoding
        // is not a swizzle, so it stops by name below.
        state.format = DxgiFormat::kR10G10B10A2Unorm;
        info = FourByteFormat(state.format, false);
        break;
    case ScanoutFormat::kA2R10G10B10:
        // R in the top ten bits: DXGI's order puts R lowest, and it has no
        // B10G10R10A2 to offer, so there is nothing to make the image in.
        state.reason = std::string("a display buffer in the console's ") +
                       ScanoutFormatName(desc.format) +
                       ": DXGI has no B10G10R10A2, and its R10G10B10A2 is the other order";
        return state;
    case ScanoutFormat::kA2R10G10B10Srgb:
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       ": there is no 10-bit sRGB DXGI format to make the image in";
        return state;
    case ScanoutFormat::kA2B10G10R10Srgb:
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       ": there is no R10G10B10A2_UNORM_SRGB to make the image in";
        return state;
    case ScanoutFormat::kB8R8G8A8:
    case ScanoutFormat::kB8R8G8A8Srgb:
        // B, R, G, A from the top: red and green in each other's places, which
        // is not a DXGI layout.
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       ": red and green share no DXGI layout with it";
        return state;
    default:
        state.reason = std::string("a display buffer in a format the port does not know");
        return state;
    }

    state.row_pitch = desc.pitch != 0 ? desc.pitch : desc.width * info.bytes_per_block;
    if (state.row_pitch < desc.width * info.bytes_per_block) {
        state.reason = "a display buffer whose pitch is shorter than its row";
        return state;
    }
    state.tiled = desc.tiling == ScanoutTiling::kTile;
    // What the untile writes is a linear image with D3D12's row pitch; the
    // guest's own bytes are the tiled buffer, one row pitch of micro tiles deep.
    state.dst_row_pitch =
        static_cast<std::uint32_t>(AlignUp(desc.width * info.bytes_per_block, kRowPitchAlign));
    if (!state.tiled) {
        state.bytes = static_cast<std::uint64_t>(state.row_pitch) * desc.height;
        state.plan = PlanUntile(info, Tiling::kLinear, desc.width, desc.height, 1, 0, 0);
        state.ok = state.plan.valid;
        if (!state.ok) { state.reason = "nothing to copy: " + state.plan.reason; }
        return state;
    }
    if (!four_byte) {
        state.reason = std::string("a tiled display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       ": the detile shader writes 4-byte elements";
        return state;
    }
    // The console tiles a display buffer the way the render target that wrote it
    // was tiled: two-dimensional thin micro tiles. (A 4K title's attributes are
    // what confirms the mode; the plan and the dispatches do not depend on the
    // choice, the permutation does.)
    state.plan = PlanUntile(info, Tiling::kThin2D, desc.width, desc.height, 1, 0, 0);
    if (!state.plan.valid) {
        state.reason = "nothing to detile: " + state.plan.reason;
        return state;
    }
    state.bytes = static_cast<std::uint64_t>(state.row_pitch) * desc.height;
    // The detiled rows are the frame's own rows, so the copy after the dispatch
    // reads the plan's linear image at D3D12's pitch.
    state.plan.dst_row_pitch = state.dst_row_pitch;
    state.ok = true;
    return state;
}

}  // namespace bbx::d3d12
