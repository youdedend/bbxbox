// SPDX-License-Identifier: GPL-3.0-or-later
// tests/scanout_test.cpp -- the display HLE's scanout description (bbx_scanout).
//
// The two things a wrong answer here costs: a red menu bar drawn blue (the
// format's memory order read as its name), and a sheared frame (a tiled display
// buffer copied as if it were linear).

#include <cstdint>
#include <string>
#include <vector>

#include "host/d3d12/bbx_scanout.h"
#include "harness.h"

using namespace bbx::d3d12;

namespace {

ScanoutDesc Desc(ScanoutFormat format, ScanoutTiling tiling, std::uint32_t width, std::uint32_t height,
                 std::uint32_t pitch = 0) {
    ScanoutDesc desc;
    desc.format = format;
    desc.tiling = tiling;
    desc.width = width;
    desc.height = height;
    desc.pitch = pitch;
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
    // And R8G8B8A8 is already in DXGI's order: a different format, not a swizzle.
    const ScanoutState r8g8b8a8 = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 1920, 1080));
    CHECK(r8g8b8a8.ok);
    CHECK(r8g8b8a8.format == DxgiFormat::kR8G8B8A8Unorm);
    CHECK(r8g8b8a8.format != a8r8g8b8.format);
}

TEST(ScanoutKeepsTheSrgbSpellingThatMatches) {
    const ScanoutState srgb = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8Srgb, ScanoutTiling::kLinear, 1280, 720));
    CHECK(srgb.ok);
    CHECK(srgb.srgb);
    CHECK(srgb.format == DxgiFormat::kB8G8R8A8UnormSrgb);
    const ScanoutState linear = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1280, 720));
    CHECK(linear.ok);
    CHECK(!linear.srgb);
    CHECK(linear.format == DxgiFormat::kB8G8R8A8Unorm);
    const ScanoutState rgba_srgb = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8Srgb, ScanoutTiling::kLinear, 64, 64));
    CHECK(rgba_srgb.ok);
    CHECK(rgba_srgb.format == DxgiFormat::kR8G8B8A8UnormSrgb);
}

TEST(ScanoutRefusesAFormatWithNoDxgiLayoutByName) {
    // A named refusal, not another format: a wrong picture is worse than none,
    // and the name is what a log line needs to say what the title asked for.
    for (const ScanoutFormat format : {ScanoutFormat::kA2R10G10B10, ScanoutFormat::kA2R10G10B10Srgb,
                                       ScanoutFormat::kB8R8G8A8, ScanoutFormat::kB8R8G8A8Srgb,
                                       ScanoutFormat::kA2B10G10R10Srgb}) {
        const ScanoutState state = DescribeScanout(Desc(format, ScanoutTiling::kLinear, 640, 480));
        CHECK(!state.ok);
        CHECK(!state.reason.empty());
        CHECK(state.reason.find(ScanoutFormatName(format)) != std::string::npos);
        CHECK(state.format == DxgiFormat::kUnknown);
    }
    // The one 10-bit form that has a layout: A2B10G10R10 is R10G10B10A2's bits.
    const ScanoutState ten = DescribeScanout(Desc(ScanoutFormat::kA2B10G10R10, ScanoutTiling::kLinear, 640, 480));
    CHECK(ten.ok);
    CHECK(ten.format == DxgiFormat::kR10G10B10A2Unorm);
}

TEST(ScanoutStatesThePitchesTheCopyReadsAndWrites) {
    // 1920 pixels of 4 bytes is 7680, already on D3D12's 256-byte pitch.
    const ScanoutState packed = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kLinear, 1920, 1080));
    CHECK(packed.ok);
    CHECK(packed.row_pitch == 7680);
    CHECK(packed.dst_row_pitch == 7680);
    CHECK(packed.bytes == 7680ull * 1080);
    // A width that is not a multiple of 64 pixels: the guest's rows are tight and
    // the image's are padded to the next 256 bytes.
    const ScanoutState odd = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 100, 50));
    CHECK(odd.ok);
    CHECK(odd.row_pitch == 400);
    CHECK(odd.dst_row_pitch == 512);
    CHECK(odd.bytes == 400ull * 50);
    // The guest's stated pitch is what its rows sit at, even when it is wider.
    const ScanoutState stated = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 100, 50, 768));
    CHECK(stated.ok);
    CHECK(stated.row_pitch == 768);
    CHECK(stated.dst_row_pitch == 512);
    CHECK(stated.bytes == 768ull * 50);
    // A pitch shorter than a row is not a display buffer.
    const ScanoutState bad = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 100, 50, 256));
    CHECK(!bad.ok);
    CHECK(!bad.reason.empty());
    const ScanoutState empty = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 0, 0));
    CHECK(!empty.ok);
}

TEST(ScanoutPlansTheDetileOfATiledBuffer) {
    // A tiled 1920x1080 buffer: 240x135 micro tiles, one group of 64 threads per
    // tile, and the destination rows at the image's own pitch.
    const ScanoutState tiled = DescribeScanout(Desc(ScanoutFormat::kA8R8G8B8, ScanoutTiling::kTile, 1920, 1080));
    CHECK(tiled.ok);
    CHECK(tiled.tiled);
    CHECK(tiled.plan.valid);
    CHECK(tiled.plan.tiling == Tiling::kThin2D);
    CHECK(!tiled.plan.dispatches.empty());
    CHECK(tiled.plan.dst_row_pitch == 7680);
    std::uint64_t tiles = 0;
    for (const UntileDispatch& dispatch : tiled.plan.dispatches) {
        CHECK(dispatch.tiles_x == dispatch.groups_x);
        CHECK(dispatch.groups_x >= 1 && dispatch.groups_x <= 65535);  // D3D12's dispatch limit
        tiles += static_cast<std::uint64_t>(dispatch.tiles_x) * dispatch.tiles_y;
    }
    CHECK(tiles == 240ull * 135u);
    // Every format this maps is 4 bytes an element, which is the only size
    // UntileCs writes; a tiled buffer in any other would stop by name (the
    // branch is `four_byte` in bbx_scanout.cpp, unreachable from this list).
    CHECK(tiled.plan.format == DxgiFormat::kB8G8R8A8Unorm);
    // A linear buffer has nothing to detile: the plan is valid and dispatches
    // nothing, which is what the copy path keys on.
    const ScanoutState linear = DescribeScanout(Desc(ScanoutFormat::kR8G8B8A8, ScanoutTiling::kLinear, 320, 240));
    CHECK(linear.ok);
    CHECK(linear.plan.valid);
    CHECK(linear.plan.dispatches.empty());
}

TEST(ScanoutNamesItsFormatsAsTheConsoleHeaderDoes) {
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA8R8G8B8Srgb)) == "A8R8G8B8_SRGB");
    CHECK(std::string(ScanoutFormatName(ScanoutFormat::kA2B10G10R10)) == "A2B10G10R10");
    CHECK(std::string(ScanoutTilingName(ScanoutTiling::kLinear)) == "linear");
    CHECK(std::string(ScanoutTilingName(ScanoutTiling::kTile)) == "tiled");
}
