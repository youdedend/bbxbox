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
    case ScanoutFormat::kA8R8G8B8: return "A8R8G8B8";
    case ScanoutFormat::kA8B8G8R8Srgb: return "A8B8G8R8_SRGB";
    case ScanoutFormat::kA8B8G8R8: return "A8B8G8R8";
    case ScanoutFormat::kA2R10G10B10Srgb: return "A2R10G10B10_SRGB";
    case ScanoutFormat::kA2R10G10B10: return "A2R10G10B10";
    case ScanoutFormat::kA2B10G10R10Srgb: return "A2B10G10R10_SRGB";
    case ScanoutFormat::kA2B10G10R10: return "A2B10G10R10";
    case ScanoutFormat::kA2R10G10B10Bt2020Pq: return "A2R10G10B10_BT2020_PQ";
    case ScanoutFormat::kA16R16G16B16Float: return "A16R16G16B16_FLOAT";
    }
    return "unknown";
}

const char* ScanoutTilingName(ScanoutTiling tiling) {
    switch (tiling) {
    case ScanoutTiling::kTile: return "tiled";
    case ScanoutTiling::kLinear: return "linear";
    }
    return "unknown";
}

bool ScanoutFormatFromWord(std::uint32_t word, ScanoutFormat& out) {
    switch (word) {
    case 0x80000000u: out = ScanoutFormat::kA8R8G8B8Srgb; return true;
    case 0x80060000u: out = ScanoutFormat::kA8R8G8B8; return true;
    case 0x80002200u: out = ScanoutFormat::kA8B8G8R8Srgb; return true;
    case 0x80062200u: out = ScanoutFormat::kA8B8G8R8; return true;
    case 0x88000000u: out = ScanoutFormat::kA2R10G10B10Srgb; return true;
    case 0x88060000u: out = ScanoutFormat::kA2R10G10B10; return true;
    case 0x88002200u: out = ScanoutFormat::kA2B10G10R10Srgb; return true;
    case 0x88062200u: out = ScanoutFormat::kA2B10G10R10; return true;
    case 0x88740000u: out = ScanoutFormat::kA2R10G10B10Bt2020Pq; return true;
    case 0xC1060000u: out = ScanoutFormat::kA16R16G16B16Float; return true;
    default: return false;
    }
}

bool ScanoutTilingFromWord(std::uint32_t word, ScanoutTiling& out) {
    switch (word) {
    case 0: out = ScanoutTiling::kTile; return true;    // SCE_VIDEO_OUT_TILING_MODE_TILE
    case 1: out = ScanoutTiling::kLinear; return true;  // SCE_VIDEO_OUT_TILING_MODE_LINEAR
    default: return false;
    }
}

ScanoutState DescribeScanout(const ScanoutDesc& desc) {
    ScanoutState state;
    if (desc.width == 0 || desc.height == 0) {
        state.reason = "a display buffer with no pixels";
        return state;
    }

    // The name is the dword from the top channel down, so the byte order in
    // memory is the mirror of it. A8R8G8B8 -> B,G,R,A = B8G8R8A8; A8B8G8R8 is
    // the CB's COMP_SWAP_ALT of the same pair, and its bytes are R,G,B,A, which
    // is DXGI's R8G8B8A8. The two are different DXGI formats, not a swizzle of
    // one: the image is made in the guest's own order and no shader swaps it.
    FormatInfo info;
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
    case ScanoutFormat::kA8B8G8R8Srgb:
        state.srgb = true;
        state.format = DxgiFormat::kR8G8B8A8UnormSrgb;
        info = FourByteFormat(state.format, true);
        break;
    case ScanoutFormat::kA8B8G8R8:
        state.format = DxgiFormat::kR8G8B8A8Unorm;
        info = FourByteFormat(state.format, false);
        break;
    case ScanoutFormat::kA2B10G10R10:
        // A in bits 30-31, B in 20-29, G in 10-19, R in 0-9: little-endian that
        // is DXGI's R10G10B10A2.
        state.format = DxgiFormat::kR10G10B10A2Unorm;
        info = FourByteFormat(state.format, false);
        break;
    case ScanoutFormat::kA2R10G10B10:
        // R in the top ten bits: DXGI's order puts R lowest, and it has no
        // B10G10R10A2 to offer, so there is nothing to make the image in.
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
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
    case ScanoutFormat::kA2R10G10B10Bt2020Pq:
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       ": a BT.2020 PQ (HDR10) swapchain is not wired yet, and the SDR image is not the picture";
        return state;
    case ScanoutFormat::kA16R16G16B16Float:
        // Twelve bytes of picture and four of nothing in each pixel's 16: the
        // detile shader writes 4-byte elements, and no display here takes 16F.
        state.reason = std::string("a display buffer in the console's ") + ScanoutFormatName(desc.format) +
                       " (8-byte scanout): a 16-bit float scanout is beyond the detile shader and this swapchain";
        return state;
    default:
        state.reason = "a display buffer in a format the port does not know";
        return state;
    }
    state.bytes_per_pixel = info.bytes_per_block;

    // The pitch is in pixels: a 1920-wide buffer states 1920, not 7680. 0 is
    // "the width", which is what a packed buffer's attribute carries.
    const std::uint32_t pixels_per_row = desc.pitch_pixels != 0 ? desc.pitch_pixels : desc.width;
    state.row_pitch = pixels_per_row * info.bytes_per_block;
    if (state.row_pitch < desc.width * info.bytes_per_block) {
        state.reason = "a display buffer whose pitch is shorter than its row";
        return state;
    }
    state.dst_row_pitch =
        static_cast<std::uint32_t>(AlignUp(desc.width * info.bytes_per_block, kRowPitchAlign));
    state.bytes = static_cast<std::uint64_t>(state.row_pitch) * desc.height;
    state.tiled = desc.tiling == ScanoutTiling::kTile;
    if (!state.tiled) {
        state.plan = PlanUntile(info, Tiling::kLinear, desc.width, desc.height, 1, 0, 0);
        state.ok = state.plan.valid;
        if (!state.ok) { state.reason = "nothing to copy: " + state.plan.reason; }
        return state;
    }
    // The console tiles a display buffer the way the render target that wrote it
    // was tiled: two-dimensional thin micro tiles. (A title's attributes are what
    // confirms the mode; the plan and the dispatches do not depend on the choice,
    // the permutation does.)
    state.plan = PlanUntile(info, Tiling::kThin2D, desc.width, desc.height, 1, 0, 0);
    if (!state.plan.valid) {
        state.reason = "nothing to detile: " + state.plan.reason;
        return state;
    }
    // The detiled rows are the frame's own rows, so the copy after the dispatch
    // reads the plan's linear image at D3D12's pitch.
    state.plan.dst_row_pitch = state.dst_row_pitch;
    state.ok = true;
    return state;
}

}  // namespace bbx::d3d12
