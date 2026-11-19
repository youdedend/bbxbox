// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/ux.h"

#include <algorithm>
#include <cmath>

namespace bbx::platform {

const char* PadActionName(PadAction action) {
    switch (action) {
    case PadAction::kNone: return "none";
    case PadAction::kConfirm: return "confirm";
    case PadAction::kCancel: return "cancel";
    case PadAction::kNavigateUp: return "up";
    case PadAction::kNavigateDown: return "down";
    case PadAction::kNavigateLeft: return "left";
    case PadAction::kNavigateRight: return "right";
    case PadAction::kOpenPcSettings: return "pc-settings";
    case PadAction::kPersonalEffects: return "personal-effects";
    case PadAction::kGestures: return "gestures";
    case PadAction::kMenu: return "menu";
    case PadAction::kRumblePulse: return "rumble";
    }
    return "none";
}

void Chords::Reset() {
    pending_view_ = false;
    chord_fired_ = false;
    previous_ = 0;
    deferred_ = PadAction::kNone;
}

PadAction Chords::Update(const PadEdges& edges) {
    const ButtonMask held = edges.held;
    const bool view_down = (held & Bit(Pad::kView)) != 0;
    const bool left_down = (held & Bit(Pad::kDpadLeft)) != 0;
    const bool view_pressed = (edges.pressed & Bit(Pad::kView)) != 0;
    const bool left_pressed = (edges.pressed & Bit(Pad::kDpadLeft)) != 0;

    if (view_pressed) pending_view_ = true;

    // Chord 1: View + D-pad left -> Gestures. It must win over the single-button
    // action, and the release of View afterwards must not also fire it.
    if (pending_view_ && view_down && (left_pressed || (left_down && !chord_fired_))) {
        if (!chord_fired_) {
            chord_fired_ = true;
            deferred_ = PadAction::kNone;
            previous_ = held;
            return PadAction::kGestures;
        }
    }

    // Chord 2: L3 + R3 -> the PC settings menu, on the frame both are down.
    const ButtonMask sticks = Bit(Pad::kLeftStick) | Bit(Pad::kRightStick);
    if ((held & sticks) == sticks && (previous_ & sticks) != sticks) {
        previous_ = held;
        return PadAction::kOpenPcSettings;
    }

    // Single-button View: only on release, and only when no chord fired.
    const bool view_released =
        ((edges.released & Bit(Pad::kView)) != 0) || (pending_view_ && !view_down && edges.released != 0);
    if (pending_view_ && !view_down) {
        const bool fired = chord_fired_;
        pending_view_ = false;
        chord_fired_ = false;
        previous_ = held;
        if (!fired && view_released) return PadAction::kPersonalEffects;
        return PadAction::kNone;
    }

    previous_ = held;
    if ((edges.pressed & Bit(Pad::kSouth)) != 0) return PadAction::kConfirm;
    if ((edges.pressed & Bit(Pad::kEast)) != 0) return PadAction::kCancel;
    if ((edges.pressed & Bit(Pad::kMenu)) != 0) return PadAction::kMenu;
    if ((edges.pressed & Bit(Pad::kDpadUp)) != 0) return PadAction::kNavigateUp;
    if ((edges.pressed & Bit(Pad::kDpadDown)) != 0) return PadAction::kNavigateDown;
    if ((edges.pressed & Bit(Pad::kDpadRight)) != 0) return PadAction::kNavigateRight;
    if ((edges.pressed & Bit(Pad::kDpadLeft)) != 0) return PadAction::kNavigateLeft;
    return deferred_;
}

VirtualKeyboard::VirtualKeyboard() { Reset(); }

void VirtualKeyboard::Reset(const std::string& initial) {
    rows_ = {"ABCDEFGH", "abcdefgh", "IJKLMNOP", "ijklmnop",
             "QRSTUVWX", "qrstuvwx", "YZ012345", "yz6789_-",
             " Space  ", " Delete ", " Done   ", " Clear  "};
    cells_.assign(rows_.size(), std::vector<std::uint8_t>(8, 0));
    // The last four rows are actions, not characters.
    for (std::size_t row = 8; row < rows_.size(); ++row) {
        cells_[row].assign(8, static_cast<std::uint8_t>(row == 10 ? 3 : (row == 9 ? 2 : (row == 11 ? 4 : 1))));
        // Only the leftmost cell of an action row is the action; the rest are dead.
        for (std::size_t column = 1; column < 8; ++column) cells_[row][column] = 4;
    }
    text_ = initial;
    if (text_.size() > kMaxName) text_.resize(kMaxName);
    row_ = 0;
    column_ = 0;
    done_ = false;
}

const std::string& VirtualKeyboard::Row(std::size_t row) const {
    static const std::string kEmpty;
    return row < rows_.size() ? rows_[row] : kEmpty;
}

void VirtualKeyboard::Move(int dx, int dy) {
    const int rows = static_cast<int>(rows_.size());
    if (dy != 0) {
        row_ = static_cast<std::size_t>((static_cast<int>(row_) + dy + rows) % rows);
        return;
    }
    if (dx != 0) {
        column_ = static_cast<std::size_t>((static_cast<int>(column_) + dx + 8) % 8);
    }
}

void VirtualKeyboard::MoveRow(int dy) { Move(0, dy); }

char VirtualKeyboard::Cell() const {
    if (row_ >= cells_.size() || column_ >= cells_[row_].size()) return '\0';
    if (cells_[row_][column_] != 0) return '\0';
    const char c = rows_[row_][column_];
    return c == ' ' ? ' ' : c;
}

bool VirtualKeyboard::Accept() {
    if (row_ >= 8) {
        if (cells_[row_][0] == 3) {
            done_ = true;
            return false;
        }
        if (cells_[row_][0] == 2) {
            Backspace();
            return true;
        }
        if (cells_[row_][0] == 1) {
            if (text_.size() < kMaxName) text_.push_back(' ');
            return true;
        }
        if (cells_[row_][0] == 4) {
            text_.clear();  // Clear
            return true;
        }
        return true;
    }
    const char c = Cell();
    if (c == '\0' || text_.size() >= kMaxName) return true;
    text_.push_back(c);
    return true;
}

void VirtualKeyboard::Backspace() {
    if (!text_.empty()) text_.pop_back();
}

void VirtualKeyboard::Append(char c) {
    if (c < 0x20 || c > 0x7e || text_.size() >= kMaxName) return;
    text_.push_back(c);
}

namespace {

std::uint8_t StickAxisToU8(double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    const int scaled = (static_cast<int>(clamped * 32767.0) + 32768) / 257;
    return static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
}

}  // namespace

void ApplyRadialStickDeadzone(double x, double y, float inner, float outer,
                              std::uint8_t* out_x, std::uint8_t* out_y) {
    const double dz_in = static_cast<double>(std::clamp(inner, 0.0f, 0.90f));
    const double dz_out = static_cast<double>(std::clamp(outer, static_cast<float>(dz_in) + 0.05f, 1.0f));
    const double mag = std::hypot(x, y);
    double sx = 0.0;
    double sy = 0.0;
    if (mag > dz_in && dz_out > dz_in) {
        double t = (mag - dz_in) / (dz_out - dz_in);
        if (t > 1.0) t = 1.0;
        const double scale = t / mag;
        sx = std::clamp(x * scale, -1.0, 1.0);
        sy = std::clamp(y * scale, -1.0, 1.0);
    }
    if (out_x != nullptr) *out_x = StickAxisToU8(sx);
    if (out_y != nullptr) *out_y = StickAxisToU8(sy);
}

}  // namespace bbx::platform
