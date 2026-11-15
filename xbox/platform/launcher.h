// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's state machine, without WinRT.
//
// The console UI is a list of actions a gamepad moves through; the interesting
// part is which actions exist and when (a base-only dump gets "Continue anyway"
// and loses "Precompile", a running precompile disables "Start game"), plus the
// navigation edges (D-pad up on the first item wraps to the last). That part is
// pure and tested; the drawing lives in main.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xbox/platform/dump.h"

namespace bbx::platform {

enum class Action : std::uint8_t {
    kNone = 0,
    kStartGame,
    kPrecompile,
    kPickDump,
    kPickPatch,
    kTogglePatch,
    kContinueUnpatched,
    kSyncCacheToUsb,
    kSyncCacheFromUsb,
    kMemorySelfTest,
    kQuit,
};

[[nodiscard]] const char* ActionName(Action action);

struct Item {
    Action action = Action::kNone;
    std::string label;
    std::string hint;   // the second line, when the item is selected
    bool enabled = true;
    bool dangerous = false;  // the base-only "continue anyway"
};

struct LauncherModel {
    DumpLayout layout;
    bool cache_current = false;
    bool precompiling = false;
    bool syncing = false;
    bool has_usb = false;
    std::size_t stages = 0;
    std::size_t pipelines = 0;
    std::string cache_stamp;

    std::vector<Item> items;
    std::size_t selected = 0;

    // Rebuilds `items` from the state and keeps the selection on a sensible
    // item (the same action if it still exists, else the first enabled one).
    void Rebuild();
    void Move(int delta);  // wraps; skips disabled items; -1 up, +1 down
    [[nodiscard]] const Item* Current() const;

    // What activating the current item means. kNone for a disabled item.
    [[nodiscard]] Action Activate() const;

    [[nodiscard]] std::string Title() const;
    [[nodiscard]] std::string Footer() const;
};

}  // namespace bbx::platform
