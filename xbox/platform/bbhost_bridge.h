// SPDX-License-Identifier: GPL-3.0-or-later
// What the launcher asks of the guest's half of the port.
//
// Two things cross this line. The controller, which is Windows.Gaming.Input's on
// both sides (the launcher's menus and the guest read the same Gamepads vector,
// so nothing has to be handed over); and the boot, because the launcher's Start
// is the moment the guest is loaded, patched and given its thread. Everything
// else in `platform/bbhost_bridge.cpp` answers bbhost's own headers and is never
// named here.
#pragma once

#include <cstdint>
#include <string>

namespace bbx::bridge {

// Where the dump is, in the terms the HLE's file system wants: the folder with
// eboot.bin, the update overlay when the player is running a patched build, the
// writable temporary directory, and the mods overlay.
struct GuestRequest {
    std::string app0;
    std::string data;
    std::string tmp;
    std::string eboot;
    std::string mods;
    // The launcher's "Allow modded v1.09 eboot" switch: without it an eboot whose
    // sha256 is not 1.09's is refused before anything is patched.
    bool modded_eboot_ok = false;
    // Preferred Hunter name from LocalState\hunter-name.txt; pre-fills the
    // character creation IME and offline online-ID when non-empty.
    std::string hunter_name;
};

// Loads, patches and starts the guest on its own thread. True when the thread
// is running (the launcher then keeps drawing while the game boots); false when
// the eboot could not be used, with the reason in the log and LastStartError().
bool StartGuest(const GuestRequest& req);
[[nodiscard]] std::string LastStartError();

// True between StartGuest's success and the guest's `_start` returning.
bool Running();

// Frames the guest has asked to present (host_present), for the launcher's HUD.
std::uint64_t Flips();

// Suppresses guest pad reads while an overlay (L3+R3 Quick Menu or Virtual
// Keyboard) owns the controller, so menu presses never leak into Bloodborne.
void SetGuestInputSuppressed(bool suppressed);

// Immediately zeroes Xbox controller vibration motors (on overlay/suspend/exit).
void StopPadRumble();

// In-game IME text entry bridge (`host_text_entry_begin` / `poll` / `end`).
struct TextEntryPrompt {
    bool active = false;
    std::string label;
    std::string initial;
    unsigned max_chars = 16;
    bool hidden = false;
};
[[nodiscard]] TextEntryPrompt PollTextEntryPrompt();
void CompleteTextEntry(const std::string& utf8_text, bool accepted);

// Console gameplay, graphics, camera, and controller settings pushed to the
// live HostSettingsStore (`engine/frame_rate.cpp`, `engine/graphics_patch.cpp`,
// `engine/camera.cpp`, `host/shader_patch.cpp`, `engine/patch_manifest.cpp`).
struct ConsoleSettings {
    int fps_cap = 60;
    int render_width = 1920;
    int render_height = 1080;
    bool vsync = true;
    bool skip_logos = true;
    bool enhancements = true;
    bool performance_mode = false;
    bool ssao = true;
    bool anti_alias = true;
    bool dynamic_shadows = true;
    int shadow_resolution = 2;  // 0 Lowest, 1 Low, 2 Default, 3 High
    int lod_bias = 0;           // -2 finest .. +2 coarsest
    bool chromatic_aberration = true;
    bool motion_blur = true;
    bool depth_of_field = true;
    bool vignette = true;
    float fov_scale = 1.0f;
    float camera_distance_scale = 1.0f;
    float stick_deadzone = 0.10f;
};
void ApplyConsoleSettings(const ConsoleSettings& settings);
[[nodiscard]] ConsoleSettings GetConsoleSettings();

// Frame pacing & display settings shared between launcher, L3+R3 quick menu,
// and guest HLE (`frame_rate_*`, `host_window_*`).
void SetTargetFps(int fps);
[[nodiscard]] int TargetFps();
void SetVsyncEnabled(bool on);
[[nodiscard]] bool VsyncEnabled();
void SetDisplayMetrics(int width, int height, float refresh_hz);
[[nodiscard]] bool QuitRequested();

// Live rolling FPS / frame-time snapshot for the toggleable Performance HUD.
struct PerfSnapshot {
    float fps = 0.0f;
    float frame_ms = 0.0f;
    std::uint64_t flips = 0;
    std::uint64_t game_frames = 0;
};
[[nodiscard]] PerfSnapshot SnapshotPerf();

// Active toast notification set by `host_message_show` / `host_text_show`, or
// empty when expired.
[[nodiscard]] std::string ActiveToastMessage();

// The baked console shaders (<dump>/bbx_shaders.cache), once StartGuest has
// loaded them: one line for the HUD, and a look-up by interface hash for the
// pipeline that needs the program. False when the cache did not validate, with
// the reason in the line.
std::string ShaderCacheLine();
bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size);

}  // namespace bbx::bridge
