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
    static constexpr HudColor Grey() { return {0.55f, 0.58f, 0.62f, 1.0f}; }
    static constexpr HudColor Green() { return {0.35f, 0.85f, 0.45f, 1.0f}; }
    static constexpr HudColor Red() { return {0.95f, 0.35f, 0.35f, 1.0f}; }
    static constexpr HudColor Amber() { return {0.98f, 0.78f, 0.30f, 1.0f}; }
    static constexpr HudColor Panel() { return {0.03f, 0.04f, 0.06f, 0.72f}; }
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
