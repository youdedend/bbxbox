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
    kToggleFpsCap,
    kCycleResolution,
    kToggleVsync,
    kToggleSharpUpscale,
    kTogglePerformanceMode,
    kCycleShadowRes,
    kTogglePerfHud,
    kToggleSkipLogos,
    kToggleEnhancements,
    kCycleDeadzone,
    kToggleAutoBackup,
    kSyncCacheToUsb,
    kSyncCacheFromUsb,
    kBackupSavesToUsb,
    kRestoreSavesFromUsb,
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
    std::string status_banner;
    std::string hunter_name;

    // Video, gameplay, post-processing, camera & control preferences
    // (persisted in LocalState\bbxbox.ini and editable live in L3+R3 menu).
    int fps_cap = 60;                  // 30 (native PS4 pacing) or 60 (60 FPS++)
    int render_res_mode = 0;           // 0 = 1920x1080, 1 = 1600x900, 2 = 1280x720, 3 = 960x540
    bool vsync = true;                 // DXGI sync interval 1 vs 0 (VRR/FreeSync when off)
    bool sharp_upscale = true;         // HLSL SM 6.4 RCAS sharpening vs plain bilinear
    bool performance_mode = false;     // disables heavy YEBIS post passes (full effect on boot)
    bool ssao = true;                  // live ambient occlusion toggle
    bool anti_alias = true;            // live engine AA toggle
    bool dynamic_shadows = true;       // live dynamic light shadows toggle
    int shadow_resolution = 2;         // 0 Lowest, 1 Low, 2 Default, 3 High (live)
    int lod_bias = 0;                  // -2 finest .. +2 coarsest (live)
    bool perf_hud = false;             // compact corner FPS / RAM / GPU stats overlay
    bool skip_logos = true;            // skip intro company logos at boot
    bool enhancements = true;          // Hunter's Dream mirror, Rebirth Altar, Auto-Replenish
    bool chromatic_aberration = true;  // live graphics_patch CA mask
    bool motion_blur = true;           // live graphics_patch + shader_patch blur toggle
    bool depth_of_field = true;        // live YEBIS depth-of-field toggle
    bool vignette = true;              // live YEBIS vignette toggle
    int fov_percent = 100;             // 100..125% in 5% steps (live LockCamParam)
    int camera_distance_percent = 100; // 90..130% in 10% steps (live LockCamParam)
    int stick_deadzone_percent = 10;   // 0..20% in 5% steps (radial stick deadzone)
    bool auto_backup_saves = true;     // auto-mirror savedata to USB on exit

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
