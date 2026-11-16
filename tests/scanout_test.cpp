// SPDX-License-Identifier: GPL-3.0-or-later
// tests/scanout_test.cpp -- the display HLE's scanout description (bbx_scanout).
//
// The three things a wrong answer here costs: a red menu bar drawn blue (the
// format's memory order read as its name), a frame copied row by row out of a
// buffer that is not rows at all (tiling mode 0 is the tiled one), and rows read
// at the wrong stride (the pitch is in pixels, not bytes).

#include <cstdint>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_scanout.h"

using namespace bbx::d3d12;

namespace {

ScanoutDesc Desc(ScanoutFormat format, ScanoutTiling tiling, std::uint32_t width, std::uint32_t height,
                 std::uint32_t pitch_pixels = 0) {
    ScanoutDesc desc;
    desc.format = format;
    desc.tiling = tiling;
    desc.width = width;
    desc.height = height;
    desc.pitch_pixels = pitch_pixels;
    return desc;
}

}  // namespace

TEST(ScanoutReadsTheConsoleNameFromTheTopChannelDown) {
    // "A8R8G8B8" is a dword with alpha in bits 24-31 and blue in bits 0-7, so its
    // bytes in memory are B, G, R, A: DXGI's B8G8R8A8. Reading the name as the
    // byte order is the swap that turns a red bar blue.
    const ScanoutState a8r8g8b8 = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1920, 1080));
    CHECK(a8r8g8b8.ok);
    CHECK(a8r8g8b8.format == DxgiFormat::kB8G8R8A8Unorm);
    CHECK(!a8r8g8b8.srgb);
    CHECK(!a8r8g8b8.tiled);
    CHECK(a8r8g8b8.bytes_per_pixel == 4);
    // A8B8G8R8 is the CB's COMP_SWAP_ALT of the same pair: bytes R, G, B, A,
    // which is DXGI's R8G8B8A8 - a different format, not a swizzle of one.
    const ScanoutState a8b8g8r8 = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 1920, 1080));
    CHECK(a8b8g8r8.ok);
    CHECK(a8b8g8r8.format == DxgiFormat::kR8G8B8A8Unorm);
    CHECK(a8b8g8r8.format != a8r8g8b8.format);
}

TEST(ScanoutCarriesTheConsolesRealFormatWords) {
    // The words are the console's bitfields, not a sequence: 0x00000000 is the
    // sRGB number format and 0x00060000 the UNORM one, 0x00002200 is
    // COMP_SWAP_ALT, bit 19 widens the channels to 10 bits, 0x740000 is BT.2020
    // PQ and bit 31 says display format at all.
    struct Pair {
        std::uint32_t word;
        ScanoutFormat format;
    };
    const Pair words[] = {
        {0x80000000u, ScanoutFormat::kA8R8G8B8Srgb},        {0x80060000u, ScanoutFormat::kA8R8G8B8},
        {0x80002200u, ScanoutFormat::kA8B8G8R8Srgb},        {0x80062200u, ScanoutFormat::kA8B8G8R8},
        {0x88000000u, ScanoutFormat::kA2R10G10B10Srgb},     {0x88060000u, ScanoutFormat::kA2R10G10B10},
        {0x88002200u, ScanoutFormat::kA2B10G10R10Srgb},     {0x88062200u, ScanoutFormat::kA2B10G10R10},
        {0x88740000u, ScanoutFormat::kA2R10G10B10Bt2020Pq}, {0xC1060000u, ScanoutFormat::kA16R16G16B16Float},
    };
    for (const Pair& pair : words) {
        ScanoutFormat decoded = ScanoutFormat::kA8R8G8B8;
        CHECK(ScanoutFormatFromWord(pair.word, decoded));
        CHECK(decoded == pair.format);
        // And the enum's own value is the word the guest passes.
        CHECK(static_cast<std::uint32_t>(pair.format) == pair.word);
    }
    ScanoutFormat unknown = ScanoutFormat::kA8R8G8B8;
    CHECK(!ScanoutFormatFromWord(0x80060001u, unknown));  // one bit off: no such format
    CHECK(!ScanoutFormatFromWord(0u, unknown));
}

TEST(ScanoutCarriesTheSrgbSpellingThatMatches) {
    const ScanoutState srgb = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8Srgb, ScanoutTiling::kLinear, 1280, 720));
    CHECK(srgb.ok);
    CHECK(srgb.srgb);
    CHECK(srgb.format == DxgiFormat::kB8G8R8A8UnormSrgb);
    const ScanoutState linear = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1280, 720));
    CHECK(linear.ok);
    CHECK(!linear.srgb);
    CHECK(linear.format == DxgiFormat::kB8G8R8A8Unorm);
    const ScanoutState swap_srgb = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8Srgb, ScanoutTiling::kLinear, 64, 64));
    CHECK(swap_srgb.ok);
    CHECK(swap_srgb.format == DxgiFormat::kR8G8B8A8UnormSrgb);
}

TEST(ScanoutTilingModeZeroIsTheTiledOne) {
    // SCE_VIDEO_OUT_TILING_MODE_TILE is 0 and _LINEAR is 1, which is the
    // opposite of what a reader who assumes "0 is none" writes.
    ScanoutTiling tiling = ScanoutTiling::kLinear;
    CHECK(ScanoutTilingFromWord(0, tiling));
    CHECK(tiling == ScanoutTiling::kTile);
    CHECK(ScanoutTilingFromWord(1, tiling));
    CHECK(tiling == ScanoutTiling::kLinear);
    CHECK(!ScanoutTilingFromWord(2, tiling));
    CHECK(static_cast<std::uint32_t>(ScanoutTiling::kTile) == 0);
    CHECK(static_cast<std::uint32_t>(ScanoutTiling::kLinear) == 1);
    CHECK(std::string(ScanoutTilingName(ScanoutTiling::kTile)) == "tiled");
    CHECK(std::string(ScanoutTilingName(ScanoutTiling::kLinear)) == "linear");
    // The default a description starts at is the console's own default use: a
    // render target's buffer, tiled.
    CHECK(ScanoutDesc{}.tiling == ScanoutTiling::kTile);
}

TEST(ScanoutRefusesAFormatWithNoDxgiLayoutByName) {
    // A named refusal, not another format: a wrong picture is worse than none,
    // and the name is what a log line needs to say what the title asked for.
    for (const ScanoutFormat format :
         {ScanoutFormat::kA2R10G10B10, ScanoutFormat::kA2R10G10B10Srgb, ScanoutFormat::kA2B10G10R10Srgb,
          ScanoutFormat::kA2R10G10B10Bt2020Pq, ScanoutFormat::kA16R16G16B16Float}) {
        const ScanoutState state = DescribeScanout(Desc(format, ScanoutTiling::kLinear, 640, 480));
        CHECK(!state.ok);
        CHECK(!state.reason.empty());
        CHECK(state.reason.find(ScanoutFormatName(format)) != std::string::npos);
        CHECK(state.format == DxgiFormat::kUnknown);
    }
    // The 16-bit float one says what makes it different: eight bytes a pixel.
    const ScanoutState wide = DescribeScanout(Desc(ScanoutFormat::kA16R16G16B16Float, ScanoutTiling::kLinear, 64, 64));
    CHECK(wide.reason.find("8-byte scanout") != std::string::npos);
    // The one 10-bit form that has a layout: A2B10G10R10 is R10G10B10A2's bits.
    const ScanoutState ten = DescribeScanout(Desc(ScanoutFormat::kA2B10G10R10, ScanoutTiling::kLinear, 640, 480));
    CHECK(ten.ok);
    CHECK(ten.format == DxgiFormat::kR10G10B10A2Unorm);
    CHECK(ten.bytes_per_pixel == 4);
}

TEST(ScanoutReadsThePitchInPixels) {
    // sceVideoOutSetBufferAttribute's pitch is in pixels: 1920 states 1920 for a
    // 1920-wide buffer, and the bytes are four times that. A reader who takes it
    // for bytes copies a quarter of every row and then walks off the end.
    const ScanoutState packed = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1920, 1080));
    CHECK(packed.ok);
    CHECK(packed.row_pitch == 7680);
    CHECK(packed.dst_row_pitch == 7680);  // already on D3D12's 256-byte pitch
    CHECK(packed.bytes == 7680ull * 1080);
    // Stated explicitly, same answer.
    const ScanoutState stated = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1920, 1080, 1920));
    CHECK(stated.ok);
    CHECK(stated.row_pitch == 7680);
    // A buffer padded wider than its picture: 1024 pixels of picture in a 1088
    // pixel row, 512 bytes of padding past the width's 4096.
    const ScanoutState padded = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 1024, 64, 1088));
    CHECK(padded.ok);
    CHECK(padded.row_pitch == 1088 * 4);
    CHECK(padded.dst_row_pitch == 4096);
    CHECK(padded.bytes == 1088ull * 4 * 64);
    // A width that is not a multiple of 64 pixels: the guest's rows are tight and
    // the image's are padded to the next 256 bytes.
    const ScanoutState odd = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 100, 50));
    CHECK(odd.ok);
    CHECK(odd.row_pitch == 400);
    CHECK(odd.dst_row_pitch == 512);
    // A pitch narrower than the picture is not a display buffer.
    const ScanoutState bad = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 100, 50, 64));
    CHECK(!bad.ok);
    CHECK(!bad.reason.empty());
    const ScanoutState empty = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 0, 0));
    CHECK(!empty.ok);
}

TEST(ScanoutPlansTheDetileOfATiledBuffer) {
    // A tiled 1920x1080 buffer - what the console registers for a 1080p render
    // target - is 240x135 micro tiles, one group of 64 threads each, and the
    // destination rows are the image's own pitch.
    const ScanoutState tiled = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kTile, 1920, 1080));
    CHECK(tiled.ok);
    CHECK(tiled.tiled);
    CHECK(tiled.plan.valid);
    CHECK(tiled.plan.tiling == Tiling::kThin2D);
    CHECK(!tiled.plan.dispatches.empty());
    CHECK(tiled.plan.dst_row_pitch == 7680);
    // The tiled buffer is read whole: its rows are pitches of micro tiles.
    CHECK(tiled.bytes == 7680ull * 1080);
    std::uint64_t tiles = 0;
    for (const UntileDispatch& dispatch : tiled.plan.dispatches) {
        CHECK(dispatch.tiles_x == dispatch.groups_x);
        CHECK(dispatch.groups_x >= 1 && dispatch.groups_x <= 65535);  // D3D12's dispatch limit
        tiles += static_cast<std::uint64_t>(dispatch.tiles_x) * dispatch.tiles_y;
    }
    CHECK(tiles == 240ull * 135u);
    CHECK(tiled.plan.format == DxgiFormat::kB8G8R8A8Unorm);
    // A linear buffer has nothing to detile: the plan is valid and dispatches
    // nothing, which is what the copy path keys on.
    const ScanoutState linear = DescribeScanout(Desc(ScanoutFormat::kA8B8G8R8, ScanoutTiling::kLinear, 320, 240));
    CHECK(linear.ok);
    CHECK(linear.plan.valid);
    CHECK(linear.plan.dispatches.empty());
}

TEST(ScanoutNamesItsFormatsAsTheConsoleHeaderDoes) {
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA8R8G8B8Srgb)) == "A8R8G8B8_SRGB");
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA8B8G8R8)) == "A8B8G8R8");
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA2B10G10R10)) == "A2B10G10R10");
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA2R10G10B10Bt2020Pq)) == "A2R10G10B10_BT2020_PQ");
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA16R16G16B16Float)) == "A16R16G16B16_FLOAT");
}
