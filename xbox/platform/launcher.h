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
    kToggleMods,
    kToggleModdedEboot,
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

    // The `<dump>\mods` overlay: a directory the player drops loose files into
    // (`msg/`, `font/`, `menu/` - the Arabic localisation patch for CUSA03173
    // v1.09 is exactly that) which resolves ahead of `dvdroot_ps4` without the
    // dump being touched. `mods_present` is whether the directory exists,
    // `mods_enabled` the player's switch; the overlay is on by default when the
    // directory is there, because a player who put files there meant them.
    //
    // A localisation patch usually rewrites `eboot.bin` as well, and that file
    // no longer matches the SHA-256 the port pins for v1.09 - so the second
    // switch exists: with it on, a v1.09 eboot whose digest is not the pinned
    // one is loaded anyway (`BBHOST_ANY_EBOOT=1`'s meaning), and the digest is
    // logged so a bug report says which build was actually run.
    bool mods_present = false;
    bool mods_enabled = true;
    bool modded_eboot_ok = false;

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
