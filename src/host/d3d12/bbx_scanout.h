// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's scanout surface: what sceVideoOutSetBufferAttribute states about
// a display buffer's pixels, and what the present pass must do with them before
// it samples the frame.
//
// The format names are the console's, and they are not the memory order.
// "A8R8G8B8" is a dword read from the most significant channel down: alpha in
// bits 24-31, red in 16-23, green in 8-15, blue in 0-7. Little-endian, those
// bytes are B, G, R, A - DXGI's B8G8R8A8, not R8G8B8A8. Reading the name as the
// byte order swaps red and blue, and the first frame says so: every menu bar
// the game draws red comes out blue, and no amount of shader debugging finds it
// because the shader is right and the image was made in the wrong format.
//
// The values are the console's own, and they are a CB format pair in a word:
// bits 0-3 select the channel layout (0x0 = A8R8G8B8's B,G,R,A; 0x2 = the
// COMP_SWAP_ALT order A8B8G8R8, whose bytes are R,G,B,A), bits 16-18 the number
// format (0x6 = UNORM, 0x0 = sRGB), bit 19 the 10-bit/float widening, and bit
// 31 marks a display format at all. A8R8G8B8_UNORM is 0x80060000 and its sRGB
// twin 0x80000000; A8B8G8R8's are 0x80062200 and 0x80002200. They are not
// consecutive numbers, which is why this table is written out rather than
// computed: a format guessed from a bit pattern is a format that is wrong for
// exactly the titles that use the other half of the pair.
//
// Tiling is the other half, and 0 is the tiled mode: sceVideoOutTilingMode has
// SCE_VIDEO_OUT_TILING_MODE_TILE = 0 and _LINEAR = 1, and every render-target
// display buffer the console registers for 1080p names tiling mode 0.
#pragma once

#include <cstdint>
#include <string>

#include "host/d3d12/bbx_format.h"
#include "host/d3d12/bbx_untile.h"

namespace bbx::d3d12 {

// SceVideoOutPixelFormat: the console's words, bit for bit.
enum class ScanoutFormat : std::uint32_t {
    kA8R8G8B8Srgb = 0x80000000u,        // bytes B,G,R,A
    kA8R8G8B8 = 0x80060000u,            // bytes B,G,R,A
    kA8B8G8R8Srgb = 0x80002200u,        // COMP_SWAP_ALT: bytes R,G,B,A
    kA8B8G8R8 = 0x80062200u,            // bytes R,G,B,A
    kA2R10G10B10Srgb = 0x88000000u,     // refused: DXGI has no B10G10R10A2
    kA2R10G10B10 = 0x88060000u,         // refused
    kA2B10G10R10Srgb = 0x88002200u,     // refused: no R10G10B10A2_UNORM_SRGB
    kA2B10G10R10 = 0x88062200u,         // DXGI's R10G10B10A2
    kA2R10G10B10Bt2020Pq = 0x88740000u,  // refused: HDR10 PQ is not a DXGI swapchain format
    kA16R16G16B16Float = 0xC1060000u,   // refused: 8 bytes a pixel, beyond the detile shader
};

// SceVideoOutTilingMode. 0 is the tiled mode.
enum class ScanoutTiling : std::uint32_t {
    kTile = 0,    // GCN micro tiles: the render target's own layout
    kLinear = 1,  // rows of pixels, as the display controller scans them out
};

struct ScanoutDesc {
    ScanoutFormat format = ScanoutFormat::kA8R8G8B8;
    ScanoutTiling tiling = ScanoutTiling::kTile;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // sceVideoOutSetBufferAttribute's pitch, **in pixels**, not bytes; 0 means
    // "the width", which is what a title passes when its buffer is packed.
    std::uint32_t pitch_pixels = 0;
};

// What one display buffer means to the presenter.
struct ScanoutState {
    bool ok = false;
    std::string reason;  // why not, when a buffer cannot be presented
    DxgiFormat format = DxgiFormat::kUnknown;  // the frame image's own format
    bool srgb = false;
    bool tiled = false;
    std::uint32_t bytes_per_pixel = 0;
    std::uint32_t row_pitch = 0;      // bytes per row in the guest's buffer
    std::uint32_t dst_row_pitch = 0;  // bytes per row after the detile (256-aligned)
    std::uint64_t bytes = 0;          // how much of the guest's buffer the copy reads
    UntilePlan plan;                  // the detile dispatches, when tiled
};

// The console's name for a format, spelled as its header spells it.
[[nodiscard]] const char* ScanoutFormatName(ScanoutFormat format);
[[nodiscard]] const char* ScanoutTilingName(ScanoutTiling tiling);

// The guest's own words, as sceVideoOutSetBufferAttribute writes them into the
// attribute block: `SceVideoOutPixelFormat` and `SceVideoOutTilingMode`. Both
// return false for a word this port has no name for.
[[nodiscard]] bool ScanoutFormatFromWord(std::uint32_t word, ScanoutFormat& out);
[[nodiscard]] bool ScanoutTilingFromWord(std::uint32_t word, ScanoutTiling& out);

// Resolves a display buffer's attribute into the image the present pass makes,
// the row pitch of the guest's own bytes, and - when the guest asked for tiling
// - the detile plan that linearises them. A format with no DXGI layout the
// present pass can sample is refused by name rather than presented as another
// format: a wrong picture is worse than a named one.
[[nodiscard]] ScanoutState DescribeScanout(const ScanoutDesc& desc);

}  // namespace bbx::d3d12
