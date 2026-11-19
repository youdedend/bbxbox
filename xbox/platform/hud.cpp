// SPDX-License-Identifier: GPL-3.0-or-later
// The HUD's geometry (see hud.h). Every glyph is five rows of three columns, so
// the table below is the font, spelled out where it can be read and checked.
#include "xbox/platform/hud.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace bbx::platform {
namespace {

// 5 rows x 3 columns per glyph, '.' = empty, '#' = ink. Index 0 is a blank, and
// the table is indexed by the character's offset from kFirstGlyph.
struct Glyph {
    char character;
    const char* rows;  // 5 strings of 3 characters
};

constexpr const char* kBlank = "..."  "..."  "..."  "..."  "...";

// The rows are concatenated (15 characters) so the table stays one column wide.
constexpr Glyph kGlyphs[] = {
    {' ', kBlank},
    {'0', "###" "#.#" "#.#" "#.#" "###"},
    {'1', ".#." "##." ".#." ".#." "###"},
    {'2', "###" "..#" "###" "#.." "###"},
    {'3', "###" "..#" ".##" "..#" "###"},
    {'4', "#.#" "#.#" "###" "..#" "..#"},
    {'5', "###" "#.." "###" "..#" "###"},
    {'6', "###" "#.." "###" "#.#" "###"},
    {'7', "###" "..#" "..#" "..#" "..#"},
    {'8', "###" "#.#" "###" "#.#" "###"},
    {'9', "###" "#.#" "###" "..#" "###"},
    {'A', ".#." "#.#" "###" "#.#" "#.#"},
    {'B', "##." "#.#" "##." "#.#" "##."},
    {'C', "###" "#.." "#.." "#.." "###"},
    {'D', "##." "#.#" "#.#" "#.#" "##."},
    {'E', "###" "#.." "###" "#.." "###"},
    {'F', "###" "#.." "###" "#.." "#.."},
    {'G', "###" "#.." "#.#" "#.#" "###"},
    {'H', "#.#" "#.#" "###" "#.#" "#.#"},
    {'I', "###" ".#." ".#." ".#." "###"},
    {'J', "..#" "..#" "..#" "#.#" "###"},
    {'K', "#.#" "#.#" "##." "#.#" "#.#"},
    {'L', "#.." "#.." "#.." "#.." "###"},
    {'M', "#.#" "###" "###" "#.#" "#.#"},
    {'N', "##." "#.#" "#.#" "#.#" "#.#"},
    {'O', "###" "#.#" "#.#" "#.#" "###"},
    {'P', "###" "#.#" "###" "#.." "#.."},
    {'Q', "###" "#.#" "###" "..#" "..#"},
    {'R', "###" "#.#" "##." "#.#" "#.#"},
    {'S', "###" "#.." "###" "..#" "###"},
    {'T', "###" ".#." ".#." ".#." ".#."},
    {'U', "#.#" "#.#" "#.#" "#.#" "###"},
    {'V', "#.#" "#.#" "#.#" "#.#" ".#."},
    {'W', "#.#" "#.#" "###" "###" "#.#"},
    {'X', "#.#" "#.#" ".#." "#.#" "#.#"},
    {'Y', "#.#" "#.#" ".#." ".#." ".#."},
    {'Z', "###" "..#" ".#." "#.." "###"},
    {'-', "..." "..." "###" "..." "..."},
    {'.', "..." "..." "..." "..." ".#."},
    {':', "..." ".#." "..." ".#." "..."},
    {'/', "..#" "..#" ".#." "#.." "#.."},
    {'%', "#.#" "..#" ".#." "#.." "#.#"},
    {'(', "..#" ".#." ".#." ".#." "..#"},
    {')', "#.." ".#." ".#." ".#." "#.."},
    {'[', ".##" ".#." ".#." ".#." ".##"},
    {']', "##." ".#." ".#." ".#." "##."},
    {'+', "..." ".#." "###" ".#." "..."},
    {'=', "..." "###" "..." "###" "..."},
    {'!', ".#." ".#." ".#." "..." ".#."},
    {'?', "###" "..#" ".#." "..." ".#."},
    {',', "..." "..." "..." ".#." "#.."},
    {'_', "..." "..." "..." "..." "###"},
    {'>', "#.." ".#." "..#" ".#." "#.."},
    {'<', "..#" ".#." "#.." ".#." "..#"},
    {'*', "#.#" ".#." "###" ".#." "#.#"},
    {'|', ".#." ".#." ".#." ".#." ".#."},
    {'\'', ".#." ".#." "..." "..." "..."},
    {'"', "#.#" "#.#" "..." "..." "..."},
    {';', "..." ".#." "..." ".#." "#.."},
    {'&', ".#." "#.#" ".#." "#.#" ".##"},
    {'\\', "#.." "#.." ".#." "..#" "..#"},
};

// 9x13 bitmap of the Bloodborne Hunter's Mark (Caryll Rune of the Hunter).
constexpr const char* kHuntersMarkRows[13] = {
    "....#....",
    "..#####..",
    "....#....",
    "...###...",
    "..#.#.#..",
    ".#..#..#.",
    "#.##.##.#",
    "#..###..#",
    ".#.##.#..",
    "..#.#.#..",
    "...###...",
    "..#.#.#..",
    "....#....",
};

std::uint8_t RowMask(const Glyph& glyph, std::size_t row) {
    if (glyph.rows == nullptr || row >= HudFont::kGlyphHeight) { return 0; }
    std::uint8_t mask = 0;
    for (std::size_t column = 0; column < HudFont::kGlyphWidth; ++column) {
        if (glyph.rows[row * HudFont::kGlyphWidth + column] == '#') {
            mask |= static_cast<std::uint8_t>(1u << (HudFont::kGlyphWidth - 1 - column));
        }
    }
    return mask;
}

const Glyph* Find(char character) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.character == upper) { return &glyph; }
    }
    // A character the font does not have draws as a blank rather than as garbage
    // that would look like a different glyph.
    return &kGlyphs[0];
}

}  // namespace

std::uint8_t HudFont::Rows(char c, std::uint8_t row) {
    return RowMask(*Find(c), row);
}

bool HudFont::Has(char c) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.character == upper) { return true; }
    }
    return false;
}

Hud::Hud(float width, float height) : width_(width), height_(height) {}

void Hud::Resize(float width, float height) {
    width_ = width;
    height_ = height;
}

void Hud::Clear() { quads_.clear(); }

float Hud::TextWidth(std::string_view text, std::uint32_t scale) {
    if (text.empty()) { return 0.0f; }
    const float columns = static_cast<float>(text.size() * HudFont::kColumns - 1);
    return columns * static_cast<float>(scale);
}

float Hud::TextHeight(std::uint32_t scale) {
    return static_cast<float>(HudFont::kGlyphHeight * scale);
}

void Hud::Rect(float x, float y, float width, float height, HudColor colour) {
    if (width <= 0.0f || height <= 0.0f) { return; }
    HudQuad quad{};
    quad.x = x;
    quad.y = y;
    quad.width = width;
    quad.height = height;
    // The white 1x1 texture is the whole quad; a solid panel is the same draw with
    // any UV in 0..1.
    quad.u0 = 0.0f;
    quad.v0 = 0.0f;
    quad.u1 = 1.0f;
    quad.v1 = 1.0f;
    quad.r = colour.r;
    quad.g = colour.g;
    quad.b = colour.b;
    quad.a = colour.a;
    quads_.push_back(quad);
}

void Hud::Panel(float x, float y, float width, float height, HudColor colour) {
    Rect(x, y, width, height, colour);
}

void Hud::GothicPanel(float x, float y, float width, float height, HudColor fill, HudColor border,
                      HudColor accent) {
    if (width <= 0.0f || height <= 0.0f) { return; }
    // 1. Deep blood-obsidian body
    Rect(x, y, width, height, fill);

    // 2. Outer tarnished-brass Victorian frame
    const float t = std::max(2.0f, std::min(width, height) * 0.0045f);
    Rect(x, y, width, t, border);
    Rect(x, y + height - t, width, t, border);
    Rect(x, y, t, height, border);
    Rect(x + width - t, y, t, height, border);

    // 3. Inner blood-crimson filigree inset frame
    const float inset = t * 2.5f;
    if (width > inset * 4.0f && height > inset * 4.0f) {
        const HudColor inner_col{accent.r, accent.g, accent.b, accent.a * 0.65f};
        const float it = std::max(1.0f, t * 0.65f);
        Rect(x + inset, y + inset, width - inset * 2.0f, it, inner_col);
        Rect(x + inset, y + height - inset - it, width - inset * 2.0f, it, inner_col);
        Rect(x + inset, y + inset, it, height - inset * 2.0f, inner_col);
        Rect(x + width - inset - it, y + inset, it, height - inset * 2.0f, inner_col);
    }

    // 4. Four corner tarnished-gold finials
    const float corner = t * 3.2f;
    const HudColor gold = HudColor::TarnishedGold();
    Rect(x - t * 0.5f, y - t * 0.5f, corner, corner, gold);
    Rect(x + width - corner + t * 0.5f, y - t * 0.5f, corner, corner, gold);
    Rect(x - t * 0.5f, y + height - corner + t * 0.5f, corner, corner, gold);
    Rect(x + width - corner + t * 0.5f, y + height - corner + t * 0.5f, corner, corner, gold);
}

void Hud::GothicDivider(float x, float y, float width, std::uint32_t scale, HudColor line_colour,
                        HudColor gem_colour) {
    if (width <= 0.0f) { return; }
    const float px = static_cast<float>(scale == 0 ? 1u : scale);
    const float line_h = std::max(1.5f, px * 0.45f);
    const float mid_x = x + width * 0.5f;
    const float gap = px * 5.0f;
    if (width > gap * 2.5f) {
        Rect(x, y, (width - gap * 2.0f) * 0.5f, line_h, line_colour);
        Rect(mid_x + gap, y, (width - gap * 2.0f) * 0.5f, line_h, line_colour);
    } else {
        Rect(x, y, width, line_h, line_colour);
    }
    // End finials
    Rect(x, y - px * 0.5f, px * 1.2f, line_h + px, line_colour);
    Rect(x + width - px * 1.2f, y - px * 0.5f, px * 1.2f, line_h + px, line_colour);
    // Central Caryll diamond clasp
    Rect(mid_x - px * 2.2f, y - px * 0.55f, px * 4.4f, line_h + px * 1.1f, line_colour);
    Rect(mid_x - px * 1.1f, y - px * 1.35f, px * 2.2f, line_h + px * 2.7f, gem_colour);
}

void Hud::HuntersMark(float cx, float cy, std::uint32_t scale, HudColor rune_colour,
                      HudColor glow_colour) {
    const float px = static_cast<float>(scale == 0 ? 1u : scale) * 0.75f;
    constexpr std::uint32_t kCols = 9;
    constexpr std::uint32_t kRows = 13;
    const float x0 = cx - (static_cast<float>(kCols) * px) * 0.5f;
    const float y0 = cy - (static_cast<float>(kRows) * px) * 0.5f;

    // Subtle blood-crimson halo behind the mark
    Rect(x0 - px * 1.5f, y0 - px, static_cast<float>(kCols + 3) * px,
         static_cast<float>(kRows + 2) * px,
         HudColor{glow_colour.r * 0.45f, glow_colour.g * 0.25f, glow_colour.b * 0.25f, 0.55f});

    for (std::uint32_t r = 0; r < kRows; ++r) {
        const char* row_str = kHuntersMarkRows[r];
        std::uint32_t c = 0;
        while (c < kCols) {
            if (row_str[c] != '#') {
                ++c;
                continue;
            }
            std::uint32_t run = 0;
            while (c + run < kCols && row_str[c + run] == '#') {
                ++run;
            }
            Rect(x0 + static_cast<float>(c) * px, y0 + static_cast<float>(r) * px,
                 static_cast<float>(run) * px, px, rune_colour);
            c += run;
        }
    }
}

void Hud::Text(float x, float y, std::uint32_t scale, std::string_view text, HudColor colour) {
    if (scale == 0) { scale = 1; }
    const float pixel = static_cast<float>(scale);
    float pen = x;
    for (const char character : text) {
        const Glyph* glyph = Find(character);
        for (std::uint32_t row = 0; row < HudFont::kGlyphHeight; ++row) {
            const std::uint8_t mask = RowMask(*glyph, row);
            if (mask == 0) { continue; }
            // Runs of ink in a row become one rect: fewer quads, same picture.
            std::uint32_t column = 0;
            while (column < HudFont::kGlyphWidth) {
                if ((mask & (1u << (HudFont::kGlyphWidth - 1 - column))) == 0) {
                    ++column;
                    continue;
                }
                std::uint32_t run = 0;
                while (column + run < HudFont::kGlyphWidth &&
                       (mask & (1u << (HudFont::kGlyphWidth - 1 - column - run))) != 0) {
                    ++run;
                }
                Rect(pen + static_cast<float>(column) * pixel,
                     y + static_cast<float>(row) * pixel, static_cast<float>(run) * pixel, pixel,
                     colour);
                column += run;
            }
        }
        pen += static_cast<float>(HudFont::kColumns) * pixel;
    }
}

void Hud::Row(float x, float y, float width, std::uint32_t scale, std::string_view label,
              std::string_view value, HudColor value_colour) {
    Text(x, y, scale, label, HudColor::AshSilver());
    const float value_width = TextWidth(value, scale);
    Text(x + width - value_width, y, scale, value, value_colour);
}

void Hud::Status(float x, float y, std::uint32_t scale, bool ok, std::string_view text) {
    const float box = static_cast<float>(HudFont::kGlyphHeight * scale);
    Rect(x, y, box, box, ok ? HudColor::Green() : HudColor::Red());
    Text(x + box + static_cast<float>(scale) * 3.0f, y, scale, text,
         ok ? HudColor::BoneIvory() : HudColor::Amber());
}

}  // namespace bbx::platform
