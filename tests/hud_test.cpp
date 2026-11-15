// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay HUD's geometry: the font's shape, the quads a line of text becomes,
// and the layout the boot report uses. All of it is arithmetic, so all of it is
// checked here rather than on a television.
#include "harness.h"

#include "xbox/platform/hud.h"

#include <cstring>
#include <string>
#include <string_view>

using namespace bbx::platform;

namespace {

int InkRows(char character) {
    int rows = 0;
    for (std::uint8_t row = 0; row < HudFont::kGlyphHeight; ++row) {
        if (HudFont::Rows(character, row) != 0) { ++rows; }
    }
    return rows;
}

}  // namespace

TEST(HudFontHasTheGlyphsTheBootReportNeeds) {
    // Every character the report spells must exist, or a label silently blanks.
    const std::string_view needed =
        "ADAPTER DEVICE FEATURE LEVEL MEMORY UMA SWAPCHAIN SELF TEST PASSED FAILED "
        "0123456789-.:/%()[]+=!?,_<>*";
    for (const char character : needed) {
        CHECK(HudFont::Has(character));
    }
    CHECK(!HudFont::Has('\n'));
    CHECK(!HudFont::Has('@'));
}

TEST(HudFontIsCaseFoldedAndThreeByFive) {
    CHECK(HudFont::Rows('a', 0) == HudFont::Rows('A', 0));
    CHECK(HudFont::Rows('z', 4) == HudFont::Rows('Z', 4));
    // Five rows, three columns: no row mask may set a bit outside the glyph.
    for (const char character : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
        for (std::uint8_t row = 0; row < HudFont::kGlyphHeight; ++row) {
            CHECK(HudFont::Rows(character, row) <= 0b111u);
        }
        CHECK(InkRows(character) >= 2);
    }
    // Out-of-range rows are empty, not a read past the table.
    CHECK_EQ(HudFont::Rows('X', 7), 0u);
}

TEST(HudLettersAreDistinguishable) {
    // A cheap sanity check on the table: 'I' is a bar with serifs, 'L' is a corner
    // with a floor, and neither is a copy of the blank.
    CHECK(HudFont::Rows('I', 0) == 0b111u);
    CHECK(HudFont::Rows('L', 0) == 0b100u);
    CHECK(HudFont::Rows('L', 4) == 0b111u);
    CHECK(HudFont::Rows(' ', 0) == 0u);
    // 'O' and '0' may share a shape; that is why the report labels its numbers.
    CHECK(HudFont::Rows('O', 1) == 0b101u);
}

TEST(HudTextBecomesInkRunsNotPerPixelQuads) {
    Hud hud(1920.0f, 1080.0f);
    hud.Text(10.0f, 20.0f, 4, "I", HudColor::White());
    // 'I' has three full rows of three pixels and two rows of one, all contiguous:
    // five runs, so five quads rather than fifteen pixels.
    CHECK_EQ(hud.Quads().size(), 5u);
    // The first run starts at the pen position and is three pixels of ink wide.
    CHECK(hud.Quads()[0].x == 10.0f);
    CHECK(hud.Quads()[0].y == 20.0f);
    CHECK(hud.Quads()[0].width == 12.0f);
    CHECK(hud.Quads()[0].height == 4.0f);
    // Every quad samples the whole white texture: that is what makes a solid panel
    // and a letter the same draw.
    for (const HudQuad& quad : hud.Quads()) {
        CHECK(quad.u0 == 0.0f && quad.v0 == 0.0f && quad.u1 == 1.0f && quad.v1 == 1.0f);
        CHECK(quad.a > 0.0f);
    }
}

TEST(HudTextAdvancesByFourColumnsPerGlyph) {
    CHECK(Hud::TextWidth("", 4) == 0.0f);
    CHECK(Hud::TextWidth("A", 4) == 12.0f);       // 3 columns of ink
    CHECK(Hud::TextWidth("AB", 4) == 28.0f);      // 4 + 3 = 7 columns
    CHECK(Hud::TextWidth("ABC", 8) == 88.0f);     // 11 columns at 8 pixels
    CHECK(Hud::TextHeight(4) == 20.0f);
    CHECK(Hud::TextHeight(1) == 5.0f);
}

TEST(HudRowRightAlignsItsValue) {
    Hud hud(1280.0f, 720.0f);
    hud.Row(40.0f, 100.0f, 600.0f, 4, "MEMORY", "5120", HudColor::White());
    CHECK(!hud.Quads().empty());
    float min_x = 1e9f;
    float max_right = 0.0f;
    for (const HudQuad& quad : hud.Quads()) {
        if (quad.x < min_x) { min_x = quad.x; }
        if (quad.x + quad.width > max_right) { max_right = quad.x + quad.width; }
    }
    // The label starts where it was asked to.
    CHECK(min_x == 40.0f);
    // The value ends at x + width, to the pixel of its last column of ink.
    const float value_width = Hud::TextWidth("5120", 4);
    CHECK(max_right <= 40.0f + 600.0f);
    CHECK(max_right > 40.0f + 600.0f - value_width - 4.0f);
}

TEST(HudStatusCarriesItsVerdictInColour) {
    Hud ok(1920.0f, 1080.0f);
    ok.Status(10.0f, 10.0f, 4, true, "PASSED");
    CHECK(!ok.Empty());
    CHECK(ok.Quads()[0].g > ok.Quads()[0].r);  // the green box

    Hud failed(1920.0f, 1080.0f);
    failed.Status(10.0f, 10.0f, 4, false, "FAILED");
    CHECK(failed.Quads()[0].r > failed.Quads()[0].g);  // the red box
}

TEST(HudIgnoresDegenerateGeometry) {
    Hud hud(1920.0f, 1080.0f);
    hud.Panel(0.0f, 0.0f, 0.0f, 40.0f, HudColor::Panel());
    hud.Panel(0.0f, 0.0f, 40.0f, -1.0f, HudColor::Panel());
    CHECK(hud.Empty());
    // A zero scale is clamped rather than producing invisible glyphs.
    hud.Text(0.0f, 0.0f, 0, "A", HudColor::White());
    CHECK(!hud.Empty());
    CHECK(hud.Quads()[0].height == 1.0f);
    hud.Clear();
    CHECK(hud.Empty());
}

TEST(HudResizeKeepsItsQuadsAndItsFrame) {
    Hud hud(1280.0f, 720.0f);
    hud.Panel(0.0f, 0.0f, 100.0f, 50.0f, HudColor::Panel());
    hud.Resize(3840.0f, 2160.0f);
    CHECK(hud.Width() == 3840.0f);
    CHECK(hud.Height() == 2160.0f);
    // Resizing does not re-lay-out what was already built: the caller rebuilds a
    // frame per present.
    CHECK_EQ(hud.Quads().size(), 1u);
}
