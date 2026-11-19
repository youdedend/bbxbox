// SPDX-License-Identifier: GPL-3.0-or-later
// Controller UX: the mappings that make Bloodborne's PC keyboard/mouse features
// reachable with a pad, and the on-screen keyboard for the Hunter Name.
//
// Pure: it takes a button mask and returns actions, so the chords are tested on
// the build machine instead of remembered. main.cpp wires SDL3's gamepad events
// into `Held`/`Pressed` and applies the actions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bbx::platform {

enum class Pad : std::uint8_t {
    kSouth = 0,
    kEast,
    kWest,
    kNorth,
    kView,      // DS4 touchpad click / Xbox "View"
    kMenu,
    kLeftStick,
    kRightStick,
    kLeftBumper,
    kRightBumper,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kCount,
};

using ButtonMask = std::uint32_t;
[[nodiscard]] constexpr ButtonMask Bit(Pad pad) { return 1u << static_cast<int>(pad); }

enum class PadAction : std::uint8_t {
    kNone = 0,
    kConfirm,
    kCancel,
    kNavigateUp,
    kNavigateDown,
    kNavigateLeft,
    kNavigateRight,
    kOpenPcSettings,   // L3 + R3
    kPersonalEffects,  // View, tapped
    kGestures,         // View + D-pad left
    kMenu,             // Menu
    kRumblePulse,      // feedback for a menu action
};

[[nodiscard]] const char* PadActionName(PadAction action);

struct PadEdges {
    ButtonMask held = 0;     // currently down
    ButtonMask pressed = 0;  // went down since the last poll
    ButtonMask released = 0;
};

// The chord state machine. `View` alone opens Personal Effects, but only after
// it is released without a chord having fired; `View + D-pad left` is a chord and
// must not also open the single-button action. That "pending" state is why this
// is a class and not a lookup table.
class Chords {
public:
    void Reset();
    // Returns at most one action per call, plus the edges that were consumed.
    PadAction Update(const PadEdges& edges);

    // For the launcher's footer: what a player is holding right now.
    [[nodiscard]] bool PendingView() const { return pending_view_; }

private:
    bool pending_view_ = false;
    bool chord_fired_ = false;
    ButtonMask previous_ = 0;
    PadAction deferred_ = PadAction::kNone;
};

// The controller-driven keyboard. 8 columns of ASCII, with space, backspace and
// done; the D-pad moves, A types, B deletes, Menu finishes. The Hunter Name is
// capped at 16 characters by the game's own UI, and the keyboard enforces it
// rather than letting the game truncate mid-UTF-8.
class VirtualKeyboard {
public:
    static constexpr std::size_t kMaxName = 16;

    VirtualKeyboard();
    void Reset(const std::string& initial = {});

    // Row 0..3, column 0..7. Row 4 is the action row.
    [[nodiscard]] const std::string& Row(std::size_t row) const;
    [[nodiscard]] std::size_t RowCount() const { return rows_.size(); }
    [[nodiscard]] std::size_t CursorRow() const { return row_; }
    [[nodiscard]] std::size_t CursorColumn() const { return column_; }

    void Move(int dx, int dy);   // wraps within the grid
    void MoveRow(int dy);        // launcher-style up/down between rows
    [[nodiscard]] bool Accept(); // types the cursor character; false on "done"
    void Backspace();
    void Append(char c);         // a physical keyboard, when one is attached

    [[nodiscard]] const std::string& Text() const { return text_; }
    [[nodiscard]] bool Done() const { return done_; }
    [[nodiscard]] bool Full() const { return text_.size() >= kMaxName; }

private:
    [[nodiscard]] char Cell() const;  // '\0' for an action cell

    std::vector<std::string> rows_;
    std::vector<std::vector<std::uint8_t>> cells_;  // 0 = character, 1..3 = action
    std::size_t row_ = 0;
    std::size_t column_ = 0;
    std::string text_;
    bool done_ = false;
};

// Maps normalized analog stick coordinates [-1.0, 1.0] through a radial
// inner/outer deadzone curve into ScePadData byte coordinates (0..255 with 127
// at rest), preserving diagonal direction.
void ApplyRadialStickDeadzone(double x, double y, float inner, float outer,
                              std::uint8_t* out_x, std::uint8_t* out_y);

}  // namespace bbx::platform
