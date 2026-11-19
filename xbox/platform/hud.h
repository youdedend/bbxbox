// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay HUD's geometry: every pixel it draws is a quad, and every quad is
// a rectangle of the 1x1 white texture. No glyph atlas, no font file, no SDL —
// a 3x5 bitmap font scaled by an integer, which is what a 1080p television two
// metres away can actually read, and what a host test can assert on.
//
// Coordinates are D3D12 viewport pixels with the origin at the top left, in the
// swapchain's space, so the replay can hand the first vertex straight to
// DrawInstanced.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bbx::platform {

struct HudColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    static constexpr HudColor White() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    static constexpr HudColor Grey() { return {0.60f, 0.56f, 0.54f, 1.0f}; }
    static constexpr HudColor Green() { return {0.35f, 0.85f, 0.45f, 1.0f}; }
    static constexpr HudColor Red() { return {0.95f, 0.35f, 0.35f, 1.0f}; }
    static constexpr HudColor Amber() { return {0.98f, 0.78f, 0.30f, 1.0f}; }
    static constexpr HudColor Panel() { return {0.035f, 0.022f, 0.032f, 0.80f}; }

    // Bloodborne Yharnam Gothic & Hunter's Dream palette
    static constexpr HudColor BoneIvory() { return {0.94f, 0.90f, 0.82f, 1.0f}; }
    static constexpr HudColor TarnishedGold() { return {0.86f, 0.68f, 0.36f, 1.0f}; }
    static constexpr HudColor BloodCrimson() { return {0.78f, 0.13f, 0.16f, 1.0f}; }
    static constexpr HudColor PalebloodEmber() { return {0.95f, 0.36f, 0.24f, 1.0f}; }
    static constexpr HudColor AshSilver() { return {0.62f, 0.57f, 0.54f, 1.0f}; }
    static constexpr HudColor ObsidianPanel() { return {0.032f, 0.020f, 0.030f, 0.82f}; }
    static constexpr HudColor GothicBorder() { return {0.58f, 0.43f, 0.24f, 0.90f}; }
    static constexpr HudColor CrimsonSelection() { return {0.34f, 0.05f, 0.07f, 0.84f}; }
};

// One quad: a position, its UV rect (always the whole white texture) and its
// colour. 32 bytes, the layout pipeline.cpp's input layout declares.
struct HudQuad {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

// The 3x5 font: glyphs are five rows of three columns, '.' for empty and '#' for
// ink. Uppercase only, plus the punctuation the boot report needs.
class HudFont {
public:
    static constexpr std::uint32_t kGlyphWidth = 3;
    static constexpr std::uint32_t kGlyphHeight = 5;
    static constexpr std::uint32_t kColumns = 4;  // a glyph plus one column of spacing

    // The glyph's five rows as bit masks (bit 2 is the left column). `0` for a
    // character the font does not have, which draws as a blank.
    [[nodiscard]] static std::uint8_t Rows(char c, std::uint8_t row);
    [[nodiscard]] static bool Has(char c);
};

class Hud {
public:
    Hud(float width, float height);

    // The viewport changed (a TV mode change, or a swapchain resize).
    void Resize(float width, float height);
    [[nodiscard]] float Width() const { return width_; }
    [[nodiscard]] float Height() const { return height_; }

    void Clear();
    [[nodiscard]] bool Empty() const { return quads_.empty(); }
    [[nodiscard]] const std::vector<HudQuad>& Quads() const { return quads_; }

    void Panel(float x, float y, float width, float height, HudColor colour);
    // Victorian gothic double-bordered frame with corner finials and crimson trim.
    void GothicPanel(float x, float y, float width, float height,
                     HudColor fill = HudColor::ObsidianPanel(),
                     HudColor border = HudColor::GothicBorder(),
                     HudColor accent = HudColor::BloodCrimson());
    // Ornamental horizontal divider with a central diamond rune emblem.
    void GothicDivider(float x, float y, float width, std::uint32_t scale,
                       HudColor line_colour = HudColor::GothicBorder(),
                       HudColor gem_colour = HudColor::BloodCrimson());
    // The iconic Bloodborne Hunter's Mark (Caryll Rune of the Hunter) centered at (cx, cy).
    void HuntersMark(float cx, float cy, std::uint32_t scale,
                     HudColor rune_colour = HudColor::TarnishedGold(),
                     HudColor glow_colour = HudColor::BloodCrimson());

    // `scale` is the size of one glyph pixel: 4 gives a 12x20 glyph, readable
    // across a room at 1080p.
    void Text(float x, float y, std::uint32_t scale, std::string_view text, HudColor colour);
    // A label at `x` and its value right-aligned at `x + width`.
    void Row(float x, float y, float width, std::uint32_t scale, std::string_view label,
             std::string_view value, HudColor value_colour);
    // A one-line status: a coloured square, then the text.
    void Status(float x, float y, std::uint32_t scale, bool ok, std::string_view text);

    [[nodiscard]] static float TextWidth(std::string_view text, std::uint32_t scale);
    [[nodiscard]] static float TextHeight(std::uint32_t scale);

private:
    void Rect(float x, float y, float width, float height, HudColor colour);

    float width_ = 1920.0f;
    float height_ = 1080.0f;
    std::vector<HudQuad> quads_;
};

}  // namespace bbx::platform
