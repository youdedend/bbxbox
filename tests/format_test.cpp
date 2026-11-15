// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_format.h"

using namespace bbx::d3d12;

TEST(FormatRgba8) {
    const FormatInfo unorm = FromGcnFormat(10, 0);
    CHECK(unorm.valid());
    CHECK(unorm.format == DxgiFormat::kR8G8B8A8Unorm);
    CHECK_EQ(unorm.bytes_per_block, 4);
    CHECK(!unorm.rb_swap);

    const FormatInfo srgb = FromGcnFormat(10, 9);
    CHECK(srgb.format == DxgiFormat::kR8G8B8A8UnormSrgb);
    CHECK(srgb.srgb);

    const FormatInfo uint_ = FromGcnFormat(10, 4);
    CHECK(uint_.format == DxgiFormat::kR8G8B8A8Uint);
    CHECK(uint_.cls == FormatClass::kUint);

    const FormatInfo bgra = FromGcnFormat(10, 0, /*comp_swap=*/1);
    CHECK(bgra.format == DxgiFormat::kB8G8R8A8Unorm);
}

TEST(FormatCompressed) {
    const FormatInfo bc1 = FromGcnFormat(35, 0);
    CHECK(bc1.format == DxgiFormat::kBc1Unorm);
    CHECK(bc1.compressed);
    CHECK_EQ(bc1.bytes_per_block, 8);
    CHECK_EQ(bc1.block_width, 4);

    const FormatInfo bc7srgb = FromGcnFormat(41, 9);
    CHECK(bc7srgb.format == DxgiFormat::kBc7UnormSrgb);
    CHECK(bc7srgb.srgb);

    const FormatInfo bc6sf = FromGcnFormat(40, 1);
    CHECK(bc6sf.format == DxgiFormat::kBc6hSf16);
}

TEST(FormatRejectsWhatDxgiCannotExpress) {
    // No 32-bit UNORM, no 128-bit UNORM, no R32G32B32 UNORM in DXGI: those must
    // be rejected rather than silently becoming something plausible.
    CHECK(!FromGcnFormat(4, 0).valid());
    CHECK(!FromGcnFormat(14, 0).valid());
    CHECK(!FromGcnFormat(13, 0).valid());
    CHECK(!FromGcnFormat(1, 9).valid());
    CHECK(!FromGcnFormat(99, 0).valid());
}

TEST(FormatSharedExponentAndPacked) {
    CHECK(FromGcnFormat(6, 7).format == DxgiFormat::kR11G11B10Float);
    CHECK(FromGcnFormat(9, 0).format == DxgiFormat::kR10G10B10A2Unorm);
    CHECK(FromGcnFormat(9, 0, 1).rb_swap);  // comp_swap 1 on 2_10_10_10
    CHECK(FromGcnFormat(16, 0).format == DxgiFormat::kB5G6R5Unorm);
}

TEST(DepthFormats) {
    CHECK(FromDepthFormat(1, false).format == DxgiFormat::kD16Unorm);
    CHECK(FromDepthFormat(1, true).format == DxgiFormat::kD24UnormS8Uint);
    CHECK(FromDepthFormat(2, false).format == DxgiFormat::kD32Float);
    CHECK(FromDepthFormat(3, true).format == DxgiFormat::kD32FloatS8X24Uint);
    CHECK(FromDepthFormat(0, false).format == DxgiFormat::kUnknown);
    // z_info's format field is two bits; anything above is the same as 3.
    CHECK(FromDepthFormat(7, false).format == DxgiFormat::kD32Float);
}

TEST(TextureSizeRespectsBlocksAndPitch) {
    const FormatInfo bc3 = FromGcnFormat(37, 0);
    // 1024x1024 BC3: 256 blocks of 16 bytes per row, 256 rows = 1 MiB, one mip.
    CHECK_EQ(TextureBytes(bc3, 1024, 1024, 1, 1), 1'048'576ull);
    // A 65x65 BC3 texture rounds up to 17x17 blocks (272 bytes/row, aligned to
    // 256 -> 512).
    const std::uint64_t odd = TextureBytes(bc3, 65, 65, 1, 1);
    CHECK_EQ(odd, static_cast<std::uint64_t>(512) * 17);
    // Four slices, two mips: the second mip is a quarter of the first.
    const FormatInfo rgba = FromGcnFormat(10, 0);
    // Row pitch is aligned to 256 bytes, so the 32x32 mip (128-byte rows) is
    // padded to 256.
    CHECK_EQ(TextureBytes(rgba, 64, 64, 4, 2), 4ull * (64 * 256 + 32 * 256));
    // Degenerate input is zero, not an underflow.
    CHECK_EQ(TextureBytes(bc3, 0, 0, 1, 1), 0ull);
    CHECK_EQ(TextureBytes(FormatInfo{}, 16, 16, 1, 1), 0ull);
}
