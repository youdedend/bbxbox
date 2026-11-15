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
    Text(x, y, scale, label, HudColor::Grey());
    const float value_width = TextWidth(value, scale);
    Text(x + width - value_width, y, scale, value, value_colour);
}

void Hud::Status(float x, float y, std::uint32_t scale, bool ok, std::string_view text) {
    const float box = static_cast<float>(HudFont::kGlyphHeight * scale);
    Rect(x, y, box, box, ok ? HudColor::Green() : HudColor::Red());
    Text(x + box + static_cast<float>(scale) * 3.0f, y, scale, text,
         ok ? HudColor::White() : HudColor::Amber());
}

}  // namespace bbx::platform
