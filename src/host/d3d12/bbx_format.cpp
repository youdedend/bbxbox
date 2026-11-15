// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_format.h"

#include <algorithm>

namespace bbx::d3d12 {
namespace {

constexpr FormatInfo Unknown() { return {}; }

constexpr FormatInfo F(DxgiFormat f, FormatClass c, std::uint8_t bytes, std::uint8_t bw = 1,
                       std::uint8_t bh = 1, bool srgb = false, bool rb_swap = false) {
    const bool compressed = bw != 1 || bh != 1;
    return FormatInfo{f, rb_swap, c, bytes, bw, bh, srgb, compressed};
}

}  // namespace

FormatInfo FromGcnFormat(std::uint32_t dfmt, std::uint32_t nfmt, std::uint32_t comp_swap) {
    // NFMT as bbhost reads it (src/host/textures.cpp image_format).
    const bool snorm = nfmt == 1;
    const bool uint_ = nfmt == 4;
    const bool sint = nfmt == 5;
    const bool flt = nfmt == 7;
    const bool srgb = nfmt == 9;

    switch (dfmt) {
    case 1:  // 8
        if (uint_) return F(DxgiFormat::kR8Uint, FormatClass::kUint, 1);
        if (sint) return F(DxgiFormat::kR8Sint, FormatClass::kSint, 1);
        if (snorm) return F(DxgiFormat::kR8Snorm, FormatClass::kSnorm, 1);
        if (srgb) return Unknown();  // DXGI has no R8 sRGB
        return F(DxgiFormat::kR8Unorm, FormatClass::kUnorm, 1);
    case 2:  // 16
        if (flt) return F(DxgiFormat::kR16Float, FormatClass::kFloat, 2);
        if (uint_) return F(DxgiFormat::kR16Uint, FormatClass::kUint, 2);
        if (sint) return F(DxgiFormat::kR16Sint, FormatClass::kSint, 2);
        if (snorm) return F(DxgiFormat::kR16Snorm, FormatClass::kSnorm, 2);
        return F(DxgiFormat::kR16Unorm, FormatClass::kUnorm, 2);
    case 3:  // 8_8
        if (uint_) return F(DxgiFormat::kR8G8Uint, FormatClass::kUint, 2);
        if (sint) return F(DxgiFormat::kR8G8Sint, FormatClass::kSint, 2);
        if (snorm) return F(DxgiFormat::kR8G8Snorm, FormatClass::kSnorm, 2);
        if (srgb) return Unknown();
        return F(DxgiFormat::kR8G8Unorm, FormatClass::kUnorm, 2);
    case 4:  // 32
        if (flt) return F(DxgiFormat::kR32Float, FormatClass::kFloat, 4);
        if (uint_) return F(DxgiFormat::kR32Uint, FormatClass::kUint, 4);
        if (sint) return F(DxgiFormat::kR32Sint, FormatClass::kSint, 4);
        return Unknown();  // no R32 UNORM/SNORM in DXGI
    case 5:  // 16_16
        if (flt) return F(DxgiFormat::kR16G16Float, FormatClass::kFloat, 4);
        if (uint_) return F(DxgiFormat::kR16G16Uint, FormatClass::kUint, 4);
        if (sint) return F(DxgiFormat::kR16G16Sint, FormatClass::kSint, 4);
        if (snorm) return F(DxgiFormat::kR16G16Snorm, FormatClass::kSnorm, 4);
        return F(DxgiFormat::kR16G16Unorm, FormatClass::kUnorm, 4);
    case 6:  // 10_11_11
    case 7:
        if (flt) return F(DxgiFormat::kR11G11B10Float, FormatClass::kSharedExp, 4);
        return Unknown();
    case 8:  // 2_10_10_10 with R in the high bits: no DXGI layout, swizzled
        if (uint_) return F(DxgiFormat::kR10G10B10A2Uint, FormatClass::kUint, 4, 1, 1, false, true);
        return F(DxgiFormat::kR10G10B10A2Unorm, FormatClass::kUnorm, 4, 1, 1, false, true);
    case 9:  // 2_10_10_10
        if (uint_) return F(DxgiFormat::kR10G10B10A2Uint, FormatClass::kUint, 4);
        return F(DxgiFormat::kR10G10B10A2Unorm, FormatClass::kUnorm, 4, 1, 1, false,
                 comp_swap == 1);
    case 10:  // 8_8_8_8
        if (comp_swap == 1 || comp_swap == 3) {
            return srgb ? F(DxgiFormat::kB8G8R8A8Unorm, FormatClass::kUnorm, 4, 1, 1, true)
                        : F(DxgiFormat::kB8G8R8A8Unorm, FormatClass::kUnorm, 4);
        }
        if (srgb) return F(DxgiFormat::kR8G8B8A8UnormSrgb, FormatClass::kUnorm, 4, 1, 1, true);
        if (uint_) return F(DxgiFormat::kR8G8B8A8Uint, FormatClass::kUint, 4);
        if (sint) return F(DxgiFormat::kR8G8B8A8Sint, FormatClass::kSint, 4);
        if (snorm) return F(DxgiFormat::kR8G8B8A8Snorm, FormatClass::kSnorm, 4);
        return F(DxgiFormat::kR8G8B8A8Unorm, FormatClass::kUnorm, 4);
    case 11:  // 32_32
        if (flt) return F(DxgiFormat::kR32G32Float, FormatClass::kFloat, 8);
        if (uint_) return F(DxgiFormat::kR32G32Uint, FormatClass::kUint, 8);
        if (sint) return F(DxgiFormat::kR32G32Sint, FormatClass::kSint, 8);
        return Unknown();
    case 12:  // 16_16_16_16
        if (flt) return F(DxgiFormat::kR16G16B16A16Float, FormatClass::kFloat, 8);
        if (uint_) return F(DxgiFormat::kR16G16B16A16Uint, FormatClass::kUint, 8);
        if (sint) return F(DxgiFormat::kR16G16B16A16Sint, FormatClass::kSint, 8);
        if (snorm) return F(DxgiFormat::kR16G16B16A16Snorm, FormatClass::kSnorm, 8);
        return F(DxgiFormat::kR16G16B16A16Unorm, FormatClass::kUnorm, 8);
    case 13:  // 32_32_32
        if (flt) return F(DxgiFormat::kR32G32B32Float, FormatClass::kFloat, 12);
        if (uint_) return F(DxgiFormat::kR32G32B32Uint, FormatClass::kUint, 12);
        if (sint) return F(DxgiFormat::kR32G32B32Sint, FormatClass::kSint, 12);
        return Unknown();
    case 14:  // 32_32_32_32
        if (flt) return F(DxgiFormat::kR32G32B32A32Float, FormatClass::kFloat, 16);
        if (uint_) return F(DxgiFormat::kR32G32B32A32Uint, FormatClass::kUint, 16);
        if (sint) return F(DxgiFormat::kR32G32B32A32Sint, FormatClass::kSint, 16);
        return Unknown();  // no 128-bit UNORM in DXGI
    case 16:  // 5_6_5
        return F(DxgiFormat::kB5G6R5Unorm, FormatClass::kUnorm, 2);
    case 35:
        return srgb ? F(DxgiFormat::kBc1UnormSrgb, FormatClass::kUnorm, 8, 4, 4, true)
                    : F(DxgiFormat::kBc1Unorm, FormatClass::kUnorm, 8, 4, 4);
    case 36:
        return srgb ? F(DxgiFormat::kBc2UnormSrgb, FormatClass::kUnorm, 16, 4, 4, true)
                    : F(DxgiFormat::kBc2Unorm, FormatClass::kUnorm, 16, 4, 4);
    case 37:
        return srgb ? F(DxgiFormat::kBc3UnormSrgb, FormatClass::kUnorm, 16, 4, 4, true)
                    : F(DxgiFormat::kBc3Unorm, FormatClass::kUnorm, 16, 4, 4);
    case 38:
        return snorm ? F(DxgiFormat::kBc4Snorm, FormatClass::kSnorm, 8, 4, 4)
                     : F(DxgiFormat::kBc4Unorm, FormatClass::kUnorm, 8, 4, 4);
    case 39:
        return snorm ? F(DxgiFormat::kBc5Snorm, FormatClass::kSnorm, 16, 4, 4)
                     : F(DxgiFormat::kBc5Unorm, FormatClass::kUnorm, 16, 4, 4);
    case 40:
        return snorm ? F(DxgiFormat::kBc6hSf16, FormatClass::kFloat, 16, 4, 4)
                     : F(DxgiFormat::kBc6hUf16, FormatClass::kFloat, 16, 4, 4);
    case 41:
        return srgb ? F(DxgiFormat::kBc7UnormSrgb, FormatClass::kUnorm, 16, 4, 4, true)
                    : F(DxgiFormat::kBc7Unorm, FormatClass::kUnorm, 16, 4, 4);
    default:
        return Unknown();
    }
}

FormatInfo FromDepthFormat(std::uint32_t z_format, bool stencil_enabled) {
    switch (z_format & 3u) {
    case 0:
        return stencil_enabled
                   ? F(DxgiFormat::kD32FloatS8X24Uint, FormatClass::kDepthStencil, 8)
                   : Unknown();
    case 1:
        return stencil_enabled ? F(DxgiFormat::kD24UnormS8Uint, FormatClass::kDepthStencil, 4)
                               : F(DxgiFormat::kD16Unorm, FormatClass::kDepth, 2);
    case 2:
    case 3:
    default:
        return stencil_enabled ? F(DxgiFormat::kD32FloatS8X24Uint, FormatClass::kDepthStencil, 8)
                               : F(DxgiFormat::kD32Float, FormatClass::kDepth, 4);
    }
}

std::uint64_t TextureBytes(FormatInfo info, std::uint32_t width, std::uint32_t height,
                           std::uint32_t slices, std::uint32_t mips) {
    if (!info.valid() || width == 0 || height == 0 || slices == 0) return 0;
    mips = std::max<std::uint32_t>(1, mips);
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint32_t w = std::max<std::uint32_t>(1, width >> mip);
        const std::uint32_t h = std::max<std::uint32_t>(1, height >> mip);
        const std::uint64_t blocks_w = (w + info.block_width - 1) / info.block_width;
        const std::uint64_t blocks_h = (h + info.block_height - 1) / info.block_height;
        const std::uint64_t row_bytes = blocks_w * info.bytes_per_block;
        const std::uint64_t slice_bytes = (row_bytes + 255) & ~std::uint64_t{255};  // D3D12 pitch
        total += slice_bytes * blocks_h * slices;
    }
    return total;
}

}  // namespace bbx::d3d12
