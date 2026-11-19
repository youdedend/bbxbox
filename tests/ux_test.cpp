// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/ux.h"

#include <string>

using namespace bbx::platform;

namespace {

PadEdges Down(ButtonMask held, ButtonMask pressed) { return PadEdges{held, pressed, 0}; }

}  // namespace

TEST(TouchpadClickOpensPersonalEffects) {
    Chords chords;
    CHECK(chords.Update(Down(Bit(Pad::kView), Bit(Pad::kView))) == PadAction::kNone);
    CHECK(chords.PendingView());
    // Released without a chord: the single-button action fires.
    CHECK(chords.Update(PadEdges{0, 0, Bit(Pad::kView)}) == PadAction::kPersonalEffects);
    CHECK(!chords.PendingView());
}

TEST(ViewPlusDpadLeftIsGesturesOnly) {
    Chords chords;
    CHECK(chords.Update(Down(Bit(Pad::kView), Bit(Pad::kView))) == PadAction::kNone);
    // Holding left while View is down fires Gestures once.
    CHECK(chords.Update(Down(Bit(Pad::kView) | Bit(Pad::kDpadLeft), Bit(Pad::kDpadLeft))) ==
          PadAction::kGestures);
    CHECK(chords.Update(Down(Bit(Pad::kView) | Bit(Pad::kDpadLeft), 0)) == PadAction::kNone);
    // Releasing View must not also open Personal Effects.
    CHECK(chords.Update(PadEdges{Bit(Pad::kDpadLeft), 0, Bit(Pad::kView)}) == PadAction::kNone);
}

TEST(StickClickChordOpensPcSettingsOnce) {
    Chords chords;
    const ButtonMask both = Bit(Pad::kLeftStick) | Bit(Pad::kRightStick);
    CHECK(chords.Update(Down(Bit(Pad::kLeftStick), Bit(Pad::kLeftStick))) == PadAction::kNone);
    CHECK(chords.Update(Down(both, Bit(Pad::kRightStick))) == PadAction::kOpenPcSettings);
    // Holding both does not repeat.
    CHECK(chords.Update(Down(both, 0)) == PadAction::kNone);
}

TEST(NavigationAndConfirmPassThrough) {
    Chords chords;
    CHECK(chords.Update(Down(Bit(Pad::kDpadDown), Bit(Pad::kDpadDown))) == PadAction::kNavigateDown);
    CHECK(chords.Update(Down(Bit(Pad::kSouth), Bit(Pad::kSouth))) == PadAction::kConfirm);
    CHECK(chords.Update(Down(Bit(Pad::kEast), Bit(Pad::kEast))) == PadAction::kCancel);
    CHECK(chords.Update(Down(Bit(Pad::kMenu), Bit(Pad::kMenu))) == PadAction::kMenu);
}

TEST(KeyboardTypesAName) {
    VirtualKeyboard keyboard;
    CHECK(keyboard.Text().empty());
    CHECK_EQ(keyboard.RowCount(), static_cast<std::size_t>(12));
    // 'A' is the first cell.
    CHECK(keyboard.Accept());
    CHECK(keyboard.Text() == "A");
    keyboard.Move(1, 0);
    keyboard.Move(1, 0);
    CHECK(keyboard.Accept());
    CHECK(keyboard.Text() == "AC");
    keyboard.Backspace();
    CHECK(keyboard.Text() == "A");
}

TEST(KeyboardSpaceDeleteDoneAndClear) {
    VirtualKeyboard keyboard;
    keyboard.Append('X');
    keyboard.MoveRow(8);  // Space row
    CHECK(keyboard.Accept());
    CHECK(keyboard.Text() == "X ");
    keyboard.MoveRow(1);  // Delete row
    CHECK(keyboard.Accept());
    CHECK(keyboard.Text() == "X");
    keyboard.MoveRow(1);  // Done row
    CHECK(!keyboard.Accept());
    CHECK(keyboard.Done());
    keyboard.MoveRow(1);  // Clear row
    CHECK(keyboard.Accept());
    CHECK(keyboard.Text().empty());
}

TEST(KeyboardEnforcesTheNameLimit) {
    VirtualKeyboard keyboard;
    for (std::size_t i = 0; i < 40; ++i) CHECK(keyboard.Accept());
    CHECK_EQ(keyboard.Text().size(), VirtualKeyboard::kMaxName);
    CHECK(keyboard.Full());
    keyboard.Backspace();
    CHECK(!keyboard.Full());
}

TEST(KeyboardCursorWraps) {
    VirtualKeyboard keyboard;
    keyboard.Move(-1, 0);
    CHECK_EQ(keyboard.CursorColumn(), static_cast<std::size_t>(7));
    keyboard.Move(1, 0);
    CHECK_EQ(keyboard.CursorColumn(), static_cast<std::size_t>(0));
    keyboard.Move(0, -1);
    CHECK_EQ(keyboard.CursorRow(), static_cast<std::size_t>(11));
    keyboard.Move(0, 1);
    CHECK_EQ(keyboard.CursorRow(), static_cast<std::size_t>(0));
}

TEST(KeyboardAcceptsAPhysicalKeyboardToo) {
    VirtualKeyboard keyboard;
    keyboard.Append('H');
    keyboard.Append('i');
    keyboard.Append('\n');  // control characters are ignored
    CHECK(keyboard.Text() == "Hi");
}

TEST(RadialDeadzoneKeepsRestAndSuppressesDrift) {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    ApplyRadialStickDeadzone(0.0, 0.0, 0.10f, 1.0f, &x, &y);
    CHECK_EQ(static_cast<int>(x), 127);
    CHECK_EQ(static_cast<int>(y), 127);

    // Slight stick drift inside the 10% inner deadzone snaps cleanly to rest (127).
    ApplyRadialStickDeadzone(0.06, -0.07, 0.10f, 1.0f, &x, &y);
    CHECK_EQ(static_cast<int>(x), 127);
    CHECK_EQ(static_cast<int>(y), 127);

    // Zero deadzone passes slight deflection through without swallowing it.
    ApplyRadialStickDeadzone(0.08, 0.0, 0.0f, 1.0f, &x, &y);
    CHECK(x > 127);
    CHECK_EQ(static_cast<int>(y), 127);
}

TEST(RadialDeadzoneReachesFullDeflectionAndPreservesDiagonals) {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    ApplyRadialStickDeadzone(1.0, 0.0, 0.15f, 1.0f, &x, &y);
    CHECK_EQ(static_cast<int>(x), 255);
    CHECK_EQ(static_cast<int>(y), 127);

    ApplyRadialStickDeadzone(-1.0, 0.0, 0.15f, 1.0f, &x, &y);
    CHECK_EQ(static_cast<int>(x), 0);
    CHECK_EQ(static_cast<int>(y), 127);

    // 45-degree diagonal stays symmetric after radial rescaling.
    ApplyRadialStickDeadzone(0.5, 0.5, 0.10f, 1.0f, &x, &y);
    CHECK(x > 127);
    CHECK_EQ(static_cast<int>(x), static_cast<int>(y));
}
