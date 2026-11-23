// SPDX-License-Identifier: GPL-3.0-or-later
// The console's side of the guest/host seam.
//
// bbhost is a desktop program: `main.cpp` opens an SDL window, a Vulkan
// presenter and a pad, then hands the game a host API it calls into. This port
// has none of that. The window belongs to the launcher (a CoreWindow), the
// presenter is `d3d12_session`, and the pad arrives through WinRT
// (Windows.Gaming.Input, in xbox/platform/main.cpp). The
// archives are the same code, so they still call the API declared in their own
// headers - `host/window.h`, `host/options.h`, `host/gpu.h` and the rest - and
// something has to answer.
//
// That something is this file. It is the only translation unit in the app that
// includes the host headers, so it is also the only one that can be wrong about
// them without the launcher noticing: the defaults below are chosen so that a
// guest that asks about a display, a recording, a plugin page or a translucency
// report gets the answer of a machine that has none, and keeps running.
//
// The exceptions are the three that a running game cannot do without, and they
// are implemented for real further down:
//   * host_pad_state / host_pad_present - the player's controller;
//   * host_present - the flip: the guest's display buffer, described by the
//     attribute its registration stated (format, tiling, pitch), handed to
//     `d3d12_session`, which untiles and uploads it before the present pass.
//     The description is read here, on the guest's thread, and it is arithmetic:
//     nothing in it waits for the GPU.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// No <SDL3/SDL.h>: SDL cannot initialise under UWP, so this target has no SDL
// input stack at all. The pad is Windows.Gaming.Input's, read below, and it is
// the same pad the launcher's menus read.
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "core/config.h"
#include "engine/graphics_patch.h"
#include "engine/frame_rate.h"
#include "engine/live_resolution.h"
#include "host/framegen.h"
#include "host/translation_cache.h"
#include "host/options.h"
#include "host/window.h"
#include "host/gpu.h"
#include "host/ingame_menu.h"
#include "host/upscaler.h"
#include "engine/menu_pointer.h"
#include "engine/option_menu.h"
#include "engine/patch_manifest.h"
#include "host/plugin_panel.h"
#include "host/plugins.h"
#include "engine/render_scale.h"
#include "host/sampler.h"

#include "xbox/platform/log.h"
#include "xbox/platform/host_fs.h"
#include "xbox/platform/hostfs_extra.h"
#include "xbox/platform/storage.h"
#include "xbox/platform/ux.h"
#include "src/host/d3d12/bbx_scanout.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "xbox/platform/guest_frames.h"
#include "xbox/platform/bbhost_bridge.h"

// The guest runtime. `main.cpp` is not compiled on this target - it opens a
// desktop window and a Vulkan presenter - so the boot it performs is here,
// step for step and in the same order (upstream/bbhost/src/main.cpp:1366-1442).
#include "core/elf.h"
#include "core/imports.h"
#include "core/legacy_cpu.h"
#include "core/portable.h"
#include "engine/addr.h"
#include "engine/debug_menu.h"
#include "guest_abi.h"
#include "hle/fs.h"
#include "hle/hle.h"
#include "engine/option_rows.h"
#include "hle/modules.h"
#include "host/frame_stats.h"
#include "host/gxvk_lists.h"
#include "host/perf_meter.h"
#include "host/settings.h"
#include "host/settings_store.h"
#include "src/host/core/bbx_self.h"
#include "upstream/bbhost/src/log.h"


namespace {

// The player's controller is the console's own: Windows.Gaming.Input hands the
// launcher and the game the same Gamepads vector, so the pad the launcher draws
// its menus with and the pad the guest reads can never be two different
// controllers. There is no SetPad on this target and no SDL handle to keep.
std::atomic<std::uint64_t> g_flips{0};
std::atomic<std::uint64_t> g_game_frames{0};
std::atomic<bool> g_input_suppressed{false};
std::atomic<int> g_target_fps{60};
std::atomic<bool> g_vsync_enabled{true};
std::atomic<int> g_display_w{1920};
std::atomic<int> g_display_h{1080};
std::atomic<int> g_render_w{1920};
std::atomic<int> g_render_h{1080};
std::atomic<float> g_display_hz{60.0f};
std::atomic<bool> g_quit_requested{false};
std::atomic<std::uint64_t> g_last_present_ns{0};

HostSettingsStore g_settings_store;
std::mutex g_console_settings_mu;
bbx::bridge::ConsoleSettings g_console_settings{};

HostSettings BuildHostSettings(const bbx::bridge::ConsoleSettings& cfg) {
    HostSettings hs;
    hs.fullscreen = true;
    hs.vsync = cfg.vsync;
    hs.frame_cap = cfg.fps_cap >= 60 ? 60 : 30;
    hs.res_width = std::clamp(cfg.render_width, 1280, 1920);
    hs.res_height = std::clamp(cfg.render_height, 720, 1080);
    hs.fov_scale = std::clamp(cfg.fov_scale, 1.0f, 1.25f);
    hs.camera_distance_scale = std::clamp(cfg.camera_distance_scale, 0.9f, 1.3f);
    hs.button_prompts = 2;  // Always display controller prompts on Xbox.
    const float dz = std::clamp(cfg.stick_deadzone, 0.0f, 0.25f);
    hs.stick_inner[0] = dz;
    hs.stick_inner[1] = dz;
    hs.stick_outer[0] = 1.0f;
    hs.stick_outer[1] = 1.0f;
    hs.performance_mode = cfg.performance_mode;
    hs.ssao = cfg.ssao;
    hs.anti_alias = cfg.anti_alias;
    hs.dynamic_shadows = cfg.dynamic_shadows;
    hs.shadow_resolution = std::clamp(cfg.shadow_resolution, 0, 3);
    hs.lod_bias = std::clamp(cfg.lod_bias, -2, 2);
    hs.chromatic_aberration = cfg.chromatic_aberration;
    hs.motion_blur = cfg.motion_blur;
    hs.depth_of_field = cfg.depth_of_field;
    hs.vignette = cfg.vignette;
    hs.skip_logos = cfg.skip_logos;
    hs.change_appearance = cfg.enhancements;
    hs.rebirth = cfg.enhancements;
    hs.five_players = false;
    hs.skill_issue = false;
    hs.full_phantom_hp = true;
    hs.auto_replenish = cfg.enhancements;
    return hs;
}

std::mutex g_perf_mu;
std::chrono::steady_clock::time_point g_last_frame_tp{};
bool g_has_frame_tp = false;
float g_ema_frame_ms = 33.33f;

void RecordFrameSample() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_perf_mu);
    if (g_has_frame_tp) {
        const float dt_ms = std::chrono::duration<float, std::milli>(now - g_last_frame_tp).count();
        if (dt_ms > 0.25f && dt_ms < 2000.0f) {
            g_ema_frame_ms = g_ema_frame_ms * 0.85f + dt_ms * 0.15f;
        }
    }
    g_last_frame_tp = now;
    g_has_frame_tp = true;
}

std::mutex g_text_entry_mu;
bool g_text_entry_open = false;
int g_text_entry_state = 0;  // 0 editing, 1 accepted, 2 cancelled
bbx::bridge::TextEntryPrompt g_text_entry_prompt;
std::string g_text_entry_result;

std::mutex g_toast_mu;
std::string g_toast_text;
std::chrono::steady_clock::time_point g_toast_until{};

void SetToastMessage(const char* text, float seconds) {
    if (text == nullptr || text[0] == '\0' || seconds <= 0.0f) return;
    const float clamped = seconds > 15.0f ? 15.0f : seconds;
    std::lock_guard<std::mutex> lock(g_toast_mu);
    g_toast_text = text;
    g_toast_until = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<long long>(clamped * 1000.0f));
}

void ApplyWinrtRumble(std::uint8_t small_mag, std::uint8_t large_mag) {
    try {
        const auto pads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
        if (pads.Size() == 0) return;
        const winrt::Windows::Gaming::Input::Gamepad pad = pads.GetAt(0);
        if (!pad) return;
        winrt::Windows::Gaming::Input::GamepadVibration vib{};
        vib.LeftMotor = static_cast<double>(large_mag) / 255.0;
        vib.RightMotor = static_cast<double>(small_mag) / 255.0;
        vib.LeftTrigger = 0.0;
        vib.RightTrigger = 0.0;
        pad.Vibration(vib);
    } catch (...) {
    }
}

}  // namespace

namespace bbx::bridge {

std::uint64_t Flips() { return g_flips.load(std::memory_order_relaxed); }

void SetGuestInputSuppressed(bool suppressed) {
    g_input_suppressed.store(suppressed, std::memory_order_relaxed);
    if (suppressed) ApplyWinrtRumble(0, 0);
}

void StopPadRumble() { ApplyWinrtRumble(0, 0); }

TextEntryPrompt PollTextEntryPrompt() {
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    if (!g_text_entry_open || g_text_entry_state != 0) return TextEntryPrompt{};
    return g_text_entry_prompt;
}

void CompleteTextEntry(const std::string& utf8_text, bool accepted) {
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    if (!g_text_entry_open) return;
    g_text_entry_result = utf8_text;
    g_text_entry_state = accepted ? 1 : 2;
    g_text_entry_prompt.active = false;
}

ConsoleSettings GetConsoleSettings() {
    std::lock_guard<std::mutex> lock(g_console_settings_mu);
    return g_console_settings;
}

void ApplyConsoleSettings(const ConsoleSettings& settings) {
    ConsoleSettings clean = settings;
    clean.fps_cap = clean.fps_cap >= 60 ? 60 : 30;
    clean.render_width = std::clamp(clean.render_width, 960, 1920);
    clean.render_height = std::clamp(clean.render_height, 540, 1080);
    clean.shadow_resolution = std::clamp(clean.shadow_resolution, 0, 3);
    clean.lod_bias = std::clamp(clean.lod_bias, -2, 2);
    clean.fov_scale = std::clamp(clean.fov_scale, 1.0f, 1.25f);
    clean.camera_distance_scale = std::clamp(clean.camera_distance_scale, 0.9f, 1.3f);
    clean.stick_deadzone = std::clamp(clean.stick_deadzone, 0.0f, 0.25f);
    ConsoleSettings prev{};
    {
        std::lock_guard<std::mutex> lock(g_console_settings_mu);
        prev = g_console_settings;
        g_console_settings = clean;
    }
    g_target_fps.store(clean.fps_cap, std::memory_order_relaxed);
    g_vsync_enabled.store(clean.vsync, std::memory_order_relaxed);
    g_render_w.store(clean.render_width, std::memory_order_relaxed);
    g_render_h.store(clean.render_height, std::memory_order_relaxed);
    g_settings_store.publish(BuildHostSettings(clean));
#if defined(BBX_GUEST_BOOT)
    hle_video_set_fps_cap(clean.fps_cap);
    if (Running()) {
        (void)frame_rate_request(clean.fps_cap);
        if (clean.render_width != prev.render_width || clean.render_height != prev.render_height) {
            (void)live_resolution_request(static_cast<unsigned>(clean.render_width),
                                          static_cast<unsigned>(clean.render_height), false);
        }
        if (clean.shadow_resolution != prev.shadow_resolution ||
            clean.performance_mode != prev.performance_mode) {
            live_shadows_request();
        }
    }
#else
    (void)prev;
#endif
}

void SetTargetFps(int fps) {
    ConsoleSettings cfg = GetConsoleSettings();
    cfg.fps_cap = fps >= 60 ? 60 : 30;
    ApplyConsoleSettings(cfg);
}

int TargetFps() { return g_target_fps.load(std::memory_order_relaxed); }

void SetVsyncEnabled(bool on) {
    ConsoleSettings cfg = GetConsoleSettings();
    cfg.vsync = on;
    ApplyConsoleSettings(cfg);
}

bool VsyncEnabled() { return g_vsync_enabled.load(std::memory_order_relaxed); }

void SetDisplayMetrics(int width, int height, float refresh_hz) {
    if (width > 0) g_display_w.store(width, std::memory_order_relaxed);
    if (height > 0) g_display_h.store(height, std::memory_order_relaxed);
    if (refresh_hz >= 24.0f && refresh_hz <= 240.0f) {
        g_display_hz.store(refresh_hz, std::memory_order_relaxed);
    }
}

bool QuitRequested() { return g_quit_requested.load(std::memory_order_relaxed); }

PerfSnapshot SnapshotPerf() {
    PerfSnapshot snap;
    snap.flips = g_flips.load(std::memory_order_relaxed);
    snap.game_frames = g_game_frames.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_perf_mu);
    if (g_has_frame_tp && g_ema_frame_ms > 0.1f) {
        snap.frame_ms = g_ema_frame_ms;
        snap.fps = 1000.0f / g_ema_frame_ms;
    }
    return snap;
}

std::string ActiveToastMessage() {
    std::lock_guard<std::mutex> lock(g_toast_mu);
    if (g_toast_text.empty()) return {};
    if (std::chrono::steady_clock::now() >= g_toast_until) {
        g_toast_text.clear();
        return {};
    }
    return g_toast_text;
}

}  // namespace bbx::bridge

// --- the controller --------------------------------------------------------
// ScePadButtonDataOffset bits, as the HLE reports them - the same word
// host/window.cpp builds from the desktop build's pad. The PS4's Cross is the
// button at the bottom of the diamond, which is Windows.Gaming.Input's `A` on an
// Xbox pad, so A goes to Cross.
namespace {
constexpr std::uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10,
                        kRight = 0x20, kDown = 0x40, kLeft = 0x80, kL2 = 0x100, kR2 = 0x200,
                        kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
                        kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;
// DS4 touchpad coordinate space (1920x942): Bloodborne opens Gestures when
// clicking the left half (x = 480) and Personal Effects on the right half
// (x = 1440). On an Xbox controller:
//   * View alone           -> TouchPad Right (Personal Effects)
//   * View + D-pad Left    -> TouchPad Left  (Gestures, suppressing D-pad Left)
constexpr std::uint16_t kPadTouchLeftX = 480;
constexpr std::uint16_t kPadTouchRightX = 1440;
constexpr std::uint16_t kPadTouchMidY = 471;

// A trigger is 0..1 on the pad and 0..255 here; `host/window.cpp` divides by 128
// and treats 8000 as held, so the same scale is kept.
std::uint8_t TriggerFromFloat(double value) {
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    const int scaled = static_cast<int>(value * 255.0 + 0.5);
    return static_cast<std::uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
}
}  // namespace

PadState host_pad_state() {
    PadState s;
    try {
        using winrt::Windows::Gaming::Input::GameControllerSwitchPosition;
        using winrt::Windows::Gaming::Input::Gamepad;
        using winrt::Windows::Gaming::Input::GamepadButtons;
        using winrt::Windows::Gaming::Input::GamepadReading;
        using winrt::Windows::Gaming::Input::RawGameController;

        Gamepad pad{nullptr};
        RawGameController raw_pad{nullptr};
        const auto pads = Gamepad::Gamepads();
        if (pads.Size() > 0) {
            pad = pads.GetAt(0);
        } else {
            const auto raws = RawGameController::RawGameControllers();
            if (raws.Size() > 0) {
                raw_pad = raws.GetAt(0);
                if (raw_pad) {
                    pad = Gamepad::FromGameController(raw_pad);
                }
            }
        }
        if (!pad && !raw_pad) return s;
        s.connected = true;

        // While the in-game L3+R3 Quick Menu, Virtual Keyboard IME, or Xbox Guide
        // owns focus, report a connected idle pad so input never leaks into Bloodborne.
        const bool suppressed = g_input_suppressed.load(std::memory_order_relaxed) ||
                                host_text_entry_open();
        if (!suppressed) {
            const auto bit = [&s](bool down, std::uint32_t value) {
                if (down) s.buttons |= value;
            };
            const HostSettings& hs = host_settings_ref();

            if (pad) {
                const GamepadReading reading = pad.GetCurrentReading();
                const auto buttons = reading.Buttons;
                const auto held = [buttons](GamepadButtons flag) {
                    return (buttons & flag) == flag;
                };
                const bool view_down = held(GamepadButtons::View);
                const bool dpad_left = held(GamepadButtons::DPadLeft);
                const bool gestures_chord = view_down && dpad_left;

                bit(held(GamepadButtons::A), kCross);
                bit(held(GamepadButtons::B), kCircle);
                bit(held(GamepadButtons::X), kSquare);
                bit(held(GamepadButtons::Y), kTriangle);
                bit(held(GamepadButtons::LeftShoulder), kL1);
                bit(held(GamepadButtons::RightShoulder), kR1);
                bit(held(GamepadButtons::Menu), kOptions);
                bit(held(GamepadButtons::LeftThumbstick), kL3);
                bit(held(GamepadButtons::RightThumbstick), kR3);
                bit(held(GamepadButtons::DPadUp), kUp);
                bit(held(GamepadButtons::DPadDown), kDown);
                bit(dpad_left && !gestures_chord, kLeft);
                bit(held(GamepadButtons::DPadRight), kRight);

                if (view_down) {
                    s.buttons |= kTouchPad;
                    s.touch_count = 1;
                    s.touch[0].x = gestures_chord ? kPadTouchLeftX : kPadTouchRightX;
                    s.touch[0].y = kPadTouchMidY;
                    s.touch[0].id = gestures_chord ? 201 : 202;
                    s.touch[0].down = true;
                }

                // Windows.Gaming.Input reports +1.0 for stick-up, while ScePadData uses
                // 0 for up and 255 for down; negate both Y axes to match PS4 convention
                // and apply the player's configurable radial deadzone curve.
                bbx::platform::ApplyRadialStickDeadzone(
                    reading.LeftThumbstickX, -reading.LeftThumbstickY,
                    hs.stick_inner[0], hs.stick_outer[0], &s.lx, &s.ly);
                bbx::platform::ApplyRadialStickDeadzone(
                    reading.RightThumbstickX, -reading.RightThumbstickY,
                    hs.stick_inner[1], hs.stick_outer[1], &s.rx, &s.ry);
                s.l2 = TriggerFromFloat(reading.LeftTrigger);
                s.r2 = TriggerFromFloat(reading.RightTrigger);
                if (s.l2 > 62) s.buttons |= kL2;  // 8000 / 128, as the desktop build reads it
                if (s.r2 > 62) s.buttons |= kR2;
            } else if (raw_pad) {
                // Fallback for third-party USB controllers / fightsticks that expose
                // RawGameController without standard Gamepad mapping.
                const std::uint32_t btn_count = static_cast<std::uint32_t>(
                    std::clamp(raw_pad.ButtonCount(), 0, 32));
                const std::uint32_t sw_count = static_cast<std::uint32_t>(
                    std::clamp(raw_pad.SwitchCount(), 0, 8));
                const std::uint32_t ax_count = static_cast<std::uint32_t>(
                    std::clamp(raw_pad.AxisCount(), 0, 16));
                bool btns[32]{};
                GameControllerSwitchPosition sws[8]{};
                double axs[16]{};
                (void)raw_pad.GetCurrentReading(
                    winrt::array_view<bool>(btns, btns + btn_count),
                    winrt::array_view<GameControllerSwitchPosition>(sws, sws + sw_count),
                    winrt::array_view<double>(axs, axs + ax_count));

                const auto b = [&btns, btn_count](std::uint32_t idx) {
                    return idx < btn_count && btns[idx];
                };
                bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
                if (sw_count > 0) {
                    switch (sws[0]) {
                    case GameControllerSwitchPosition::Up: dpad_up = true; break;
                    case GameControllerSwitchPosition::UpRight: dpad_up = true; dpad_right = true; break;
                    case GameControllerSwitchPosition::Right: dpad_right = true; break;
                    case GameControllerSwitchPosition::DownRight: dpad_down = true; dpad_right = true; break;
                    case GameControllerSwitchPosition::Down: dpad_down = true; break;
                    case GameControllerSwitchPosition::DownLeft: dpad_down = true; dpad_left = true; break;
                    case GameControllerSwitchPosition::Left: dpad_left = true; break;
                    case GameControllerSwitchPosition::UpLeft: dpad_up = true; dpad_left = true; break;
                    default: break;
                    }
                }
                const bool view_down = b(6);
                const bool gestures_chord = view_down && dpad_left;

                bit(b(0), kCross);
                bit(b(1), kCircle);
                bit(b(2), kSquare);
                bit(b(3), kTriangle);
                bit(b(4), kL1);
                bit(b(5), kR1);
                bit(b(7), kOptions);
                bit(b(8), kL3);
                bit(b(9), kR3);
                bit(dpad_up, kUp);
                bit(dpad_down, kDown);
                bit(dpad_left && !gestures_chord, kLeft);
                bit(dpad_right, kRight);

                if (view_down) {
                    s.buttons |= kTouchPad;
                    s.touch_count = 1;
                    s.touch[0].x = gestures_chord ? kPadTouchLeftX : kPadTouchRightX;
                    s.touch[0].y = kPadTouchMidY;
                    s.touch[0].id = gestures_chord ? 201 : 202;
                    s.touch[0].down = true;
                }
                if (ax_count >= 2) {
                    bbx::platform::ApplyRadialStickDeadzone(
                        axs[0] * 2.0 - 1.0, axs[1] * 2.0 - 1.0,
                        hs.stick_inner[0], hs.stick_outer[0], &s.lx, &s.ly);
                }
                if (ax_count >= 4) {
                    bbx::platform::ApplyRadialStickDeadzone(
                        axs[2] * 2.0 - 1.0, axs[3] * 2.0 - 1.0,
                        hs.stick_inner[1], hs.stick_outer[1], &s.rx, &s.ry);
                }
                if (ax_count >= 6) {
                    s.l2 = TriggerFromFloat(axs[4]);
                    s.r2 = TriggerFromFloat(axs[5]);
                } else {
                    if (b(10)) s.l2 = 255;
                    if (b(11)) s.r2 = 255;
                }
                if (s.l2 > 62) s.buttons |= kL2;
                if (s.r2 > 62) s.buttons |= kR2;
            }
        }

        // A GamepadReading carries no timestamp of its own; the pad reports
        // microseconds and the guest only compares successive reads, so the
        // launch-relative clock is the honest thing to hand it.
        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        ::QueryPerformanceFrequency(&frequency);
        ::QueryPerformanceCounter(&counter);
        if (frequency.QuadPart > 0 && counter.QuadPart >= 0) {
            const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
            const std::uint64_t freq = static_cast<std::uint64_t>(frequency.QuadPart);
            s.timestamp = (ticks / freq) * 1000000ull + ((ticks % freq) * 1000000ull) / freq;
        }
    } catch (...) {
        // A pad that is unplugged between Gamepads() and GetCurrentReading()
        // throws; the guest reads "no controller" for this frame and retries.
        s.connected = false;
    }
    return s;
}

bool host_pad_present() {
    try {
        if (winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size() != 0) return true;
        return winrt::Windows::Gaming::Input::RawGameController::RawGameControllers().Size() != 0;
    } catch (...) {
        return false;
    }
}

// The guest arena, published for the flip (StartGuest sets it once the image is
// loaded; the guest is the only writer of what it points at).
std::atomic<std::uint8_t*> g_guest_base{nullptr};
std::atomic<std::uint64_t> g_guest_slide{0};
std::atomic<std::uint64_t> g_guest_size{0};

// The guest's scanout range, as this process can read it: Bloodborne's display
// buffers live in mapped DirectMemory (e.g. 0x14D408000), not inside the ELF
// image (0x400000..0x5ED4000). Check mapped kernel memory first, then the ELF
// arena as a fallback.
bool GuestScanoutPixels(std::uint64_t va, std::uint64_t bytes, const std::uint8_t** pixels) {
    if (pixels != nullptr) *pixels = nullptr;
    if (va == 0 || bytes == 0) return false;
    if (hle_kernel_va_mapped(va, static_cast<std::size_t>(bytes))) {
        if (pixels != nullptr) {
            *pixels = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(va));
        }
        return true;
    }
    const std::uint8_t* const base = g_guest_base.load(std::memory_order_acquire);
    const std::uint64_t slide = g_guest_slide.load(std::memory_order_relaxed);
    const std::uint64_t size = g_guest_size.load(std::memory_order_relaxed);
    if (base == nullptr || va < slide || va - slide >= size || bytes > size - (va - slide)) { return false; }
    if (pixels != nullptr) *pixels = base + (va - slide);
    return true;
}

// --- the flip --------------------------------------------------------------
// Called by the guest's video HLE once per frame, on the guest's thread. What it
// does is bounded: a bounds check, a memcpy into the session's pending frame and
// a counter - nothing that waits for the GPU or for the presenter, because the
// guest is between two EOP/label waits here and stalling it stalls the game.
void host_present(int buffer_index, std::uint64_t display_va, unsigned display_w, unsigned display_h,
                  const PresentFlip& flip) {
    (void)buffer_index; (void)flip;
    const std::uint64_t flips = g_flips.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    g_last_present_ns.store(static_cast<std::uint64_t>(now_ns > 0 ? now_ns : 0),
                            std::memory_order_relaxed);
    RecordFrameSample();
    if ((flips % 60) == 1) {
        bbx::platform::LogCrumb("gpu:flip");
    }
    if (display_va == 0 || display_w == 0 || display_h == 0) { return; }
    // The attribute the buffer was registered with. A flip of an address no port
    // registered is not a display buffer, and presenting it "as RGBA8" would be
    // three guesses about bytes the console described exactly - so it is dropped
    // and said once, not per frame.
    std::uint32_t format = 0, tiling = 0, pitch_pixels = 0;
    if (!hle_video_display_attribute(display_va, &format, &tiling, &pitch_pixels)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "video: a flip of %p, which no sceVideoOutRegisterBuffers registered; "
                                      "the frame is dropped (the attribute is what describes the pixels)",
                                      reinterpret_cast<void*>(static_cast<std::uintptr_t>(display_va)));
        }
        return;
    }
    bbx::d3d12::ScanoutFormat scanout_format = bbx::d3d12::ScanoutFormat::kA8R8G8B8Srgb;
    bbx::d3d12::ScanoutTiling scanout_tiling = bbx::d3d12::ScanoutTiling::kTile;
    if (!bbx::d3d12::ScanoutFormatFromWord(format, scanout_format) ||
        !bbx::d3d12::ScanoutTilingFromWord(tiling, scanout_tiling)) {
        static std::atomic<std::uint32_t> said_bits{0xffffffffu};
        if (said_bits.exchange(format << 1 | (tiling & 1u)) != (format << 1 | (tiling & 1u))) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "video: a display buffer with pixel format 0x%08x and tiling mode %u, which "
                                      "this port has no name for; the frame is dropped",
                                      format, tiling);
        }
        return;
    }
    bbx::d3d12::ScanoutDesc desc;
    desc.format = scanout_format;
    desc.tiling = scanout_tiling;
    desc.width = display_w;
    desc.height = display_h;
    desc.pitch_pixels = pitch_pixels;
    // What the copy will read, in the guest's own layout: a tiled buffer is the
    // same number of bytes, in micro tiles rather than rows.
    const bbx::d3d12::ScanoutState state = bbx::d3d12::DescribeScanout(desc);
    if (!state.ok) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn, "video: no frame presented: %s", state.reason.c_str());
        }
        return;
    }
    // Resolve CPU-readable pixels if mapped, but always hand display_va to
    // SubmitGuestFrame so zero-copy direct GPU scanout can present rendered
    // targets directly without touching CPU memory.
    const std::uint8_t* pixels = nullptr;
    (void)GuestScanoutPixels(display_va, state.bytes, &pixels);
    bbx::platform::GuestFrame frame;
    frame.display_va = display_va;
    frame.pixels = pixels;
    frame.width = display_w;
    frame.height = display_h;
    frame.format = format;
    frame.tiling = tiling;
    frame.pitch = pitch_pixels;
    frame.bytes = state.bytes;
    bbx::platform::SubmitGuestFrame(frame);
}

// The launcher owns the window: there is nothing here to pump, nothing to
// close, and it is always in front while it is running.
bool host_window_pump() { return true; }
void host_window_stop() {}
bool host_window_active() { return true; }

// --- the seam's quiet half ------------------------------------------------
// Everything below answers "this machine has none of that": no options page, no
// plugin panel, no capture, no overlay, no translation cache, no second window.
// Each returns the value its own declaration calls neutral, so a guest that asks
// about one is answered instead of stopped at load. The declarations are copied
// from bbhost's headers and the bodies are the quiet answer; when a slice of the
// port needs one of them for real, it is implemented here and the entry is gone.

// engine/frame_rate.h, engine/graphics_patch.h, and engine/patch_manifest.h
// are compiled directly into bbhost_engine so 60 FPS++, post-processing live
// hooks, intro skip, and gameplay enhancements run natively on Xbox.

// host/framegen.h & host/upscaler.h (Vulkan temporal upscaling/framegen are off
// on the D3D12 console path; D3D12 FSR RCAS handles crisp TV upscaling).
float framegen_shown_per_frame() { return 1.0f; }
int upscaler_method() { return 0; }

// engine/menu_pointer.h
void menu_pointer_click() {  }
void menu_pointer_install(ElfImage* image) {  }
std::uint32_t menu_pointer_take_press() { return {}; }
std::uint64_t menu_pointer_tick() { return {}; }
void menu_pointer_wheel() {  }
std::uint32_t menu_region_confirm_button() { return {}; }
void menu_sound_post(int id, int priority) {  }

// engine/option_menu.h
bool option_menu_click_decides(void* list, int index) { return {}; }
void option_menu_confirm_ask(std::uint32_t message) {  }
void option_menu_confirm_close() {  }
bool option_menu_confirm_possible() { return false; }
int option_menu_confirm_take() { return 0; }
void option_menu_install(ElfImage* image) {  }
void option_menu_list_update(void* comp, bool focused) {  }
void option_menu_poll() {  }

// engine/render_scale.h
bool render_scale_applied() { return {}; }
int render_scale_blur_shift(int shift) { return {}; }
void render_scale_flip(std::uint64_t flip) {  }
void render_scale_gx_init(std::uint32_t display_w, std::uint32_t display_h, std::uint64_t flip) {  }
void render_scale_install(ElfImage* image) {  }
bool render_scale_on(RenderScale* out) { return {}; }
bool render_scale_plan(RenderScale* out) { return {}; }
bool render_scale_retarget(std::uint32_t output_w, std::uint32_t output_h, std::uint32_t display_w, std::uint32_t display_h, std::uint64_t flip, RenderScale* before, RenderScale* after) { return {}; }
std::vector<patch_file::Site> render_scale_sites(const RenderScale& rs) { return {}; }
void render_scale_size(int quality, std::uint32_t output_w, std::uint32_t output_h, std::uint32_t* render_w, std::uint32_t* render_h) {  }

// host/gpu.h
//
// Only what the host layer does not itself define: `src/host/d3d12/host_gpu_d3d12.cpp`
// answers the rest of this header on the console (the timeline fence, the guest's
// slabs, the dispatch), and a second definition of anything it answers is a
// duplicate symbol at link, not a fallback. Four were removed here when the
// bridge landed: host_gpu_read_guest, host_gpu_mem_write_serial/_ordered and
// host_gpu_dispatch_user_reads.
void host_gpu_compare_cs_user_data(const GxStageRecords& st, const std::uint32_t* reg, std::uint32_t read_mask, CsUserCompare& out) {  }
std::uint64_t host_gpu_completed_submits() { return host_gpu_submissions_completed(); }
std::uint64_t host_gpu_completed_submits_seen() { return host_gpu_submissions_completed(); }
std::size_t host_gpu_copy_backs_captured(const std::uint64_t* dst, const std::uint64_t* src, const std::uint64_t* bytes, const std::uint32_t* at, std::uint32_t piece, std::size_t n) {
    (void)at; (void)piece;
    return host_gpu_copy_backs(dst, src, bytes, n);
}
bool host_gpu_copy_image_region(const GpuImageCopy& c) { return {}; }
void host_gpu_depth_clear(std::uint64_t base, std::uint32_t word, std::uint32_t stencil) {
    (void)stencil;
    (void)host_gpu_clear_target(base, word, 4);
}
bool host_gpu_device_lost() { return {}; }
void host_gpu_explain_value(std::uint64_t va, std::uint64_t value) {  }
bool host_gpu_fill(std::uint64_t va, std::size_t bytes, const float rgba[4], std::uint32_t depth_clear, std::uint32_t stencil_clear) {
    (void)depth_clear; (void)stencil_clear;
    std::uint32_t packed = 0;
    if (rgba != nullptr) {
        const auto to_u8 = [](float v) -> std::uint32_t {
            const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<std::uint32_t>(c * 255.0f + 0.5f) & 0xFFu;
        };
        packed = to_u8(rgba[0]) | (to_u8(rgba[1]) << 8) | (to_u8(rgba[2]) << 16) | (to_u8(rgba[3]) << 24);
    }
    const bool cleared = host_gpu_clear_target(va, packed, bytes);
    const bool filled = host_gpu_mem_fill(va, packed, bytes);
    return cleared || filled;
}
std::string host_gpu_fill_stats() { return {}; }
void host_gpu_hang_report() {  }
void host_gpu_lists_arrival(std::uint64_t id) {  }
std::string host_gpu_lists_times_report(int part, std::uint64_t parked_ticks, std::uint64_t parks) { return {}; }
void host_gpu_pipeline_time_us(std::uint64_t out[5]) {  }
void host_gpu_pipeline_work(std::uint64_t out[14]) {  }
void host_gpu_recorder_paced(std::uint64_t wait_ns, float cap_fps, bool loading) {  }
void host_gpu_refill_selftest() {  }
std::uint64_t host_gpu_ring_place(const void* data, std::size_t bytes, std::size_t align) { return {}; }
bool host_gpu_segment_flags_needed() { return {}; }
void host_gpu_segment_follows(bool follows) {  }
const std::uint64_t* host_gpu_segment_ids(std::uint64_t id, std::uint32_t* count, std::uint64_t* word) { return {}; }
void host_gpu_segment_refused(std::uint64_t id, std::uint64_t word) {  }
std::string host_gpu_segment_report(bool spliced_part) { return {}; }
void host_gpu_segment_skipped(std::uint64_t ns, bool lost) {  }
void host_gpu_splice_compare(std::uint64_t id) {  }
void host_gpu_splice_compare_begin(std::uint64_t id) {  }
std::string host_gpu_splice_report(bool window) { return {}; }
bool host_gpu_splice_segment(std::uint64_t id, std::uint64_t word) { return {}; }
void host_gpu_splice_unfed(std::uint32_t n) {  }
void host_gpu_stall_test(unsigned ms) {  }
std::uint64_t host_gpu_surface_replacements() { return {}; }
std::uint64_t host_gpu_texture_hash_us() { return {}; }
std::uint64_t host_gpu_texture_hashes() { return {}; }
void host_gpu_texture_retire(std::uint64_t base, std::size_t bytes, std::uint64_t after_submitted, std::uint64_t gen) {
    (void)after_submitted; (void)gen;
    if (base != 0 && bytes != 0) host_gpu_forget_region(base, base + bytes);
}
void host_gpu_texture_time_us(std::uint64_t out[5]) {  }
std::uint64_t host_gpu_texture_uploads() { return {}; }
bool host_gpu_upload_region(std::uint64_t base, const std::uint32_t* tsharp, std::uint32_t mip, std::uint32_t layer, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, const void* data, std::size_t bytes, std::uint32_t row_bytes) {
    (void)tsharp; (void)layer;
    if (mip == 0 && x == 0 && y == 0 && w != 0 && h != 0 && base != 0 && data != nullptr && bytes != 0 && row_bytes != 0) {
        return host_gpu_mem_write(base, data, bytes);
    }
    return false;
}
std::uint32_t host_gpu_vendor_id() { return {}; }
const char* host_gpu_vs_why_name(int why) { return {}; }
void host_gpu_wait_work_submitted(std::uint64_t need) { (void)host_gpu_wait_completed(need, 1000000000ull); }
void host_gpu_watch_display(std::uint64_t display_va) {  }
const char* host_present_step(double& seconds) { return {}; }

// host/ingame_menu.h
bool ingame_menu_event(const SDL_Event& e, float pixel_ratio) { return {}; }
bool ingame_menu_init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, std::uint32_t family, VkQueue queue, VkFormat format, std::uint32_t image_count) { return {}; }
void ingame_menu_pump() {  }
void ingame_menu_record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {  }
bool ingame_menu_record_in_pass(VkCommandBuffer cmd, VkExtent2D extent) { return {}; }
void ingame_menu_shutdown() {  }

// host/options.h
void host_account_action(int which, const std::string& name, const std::string& code) {  }
bool host_account_busy() { return {}; }
std::string host_account_detail() { return {}; }
std::string host_account_outcome() { return {}; }
std::string host_account_signed_in() { return {}; }
bool host_account_take_save() { return {}; }
int host_deck_default_profile() { return {}; }
int host_deck_profile() { return {}; }
HostEffectTimingRow host_effect_timing_row() { return {}; }
void host_frame_generation_warning(int cap_index, bool custom, int custom_fps) {  }
bool host_frame_rate_control(int* cap_index, bool* custom, int* custom_fps, float width) { return {}; }
bool host_frame_rate_fit(int* cap_index, bool* custom, int* custom_fps) { return {}; }
int host_frame_rate_warning_count(int cap_index, bool custom, int custom_fps) { return {}; }
void host_opt_choose_effect_timing(int index) {  }
int host_opt_default_frame_cap() { return 60; }
bool host_opt_default_get(const char* key) { (void)key; return {}; }
int host_opt_default_index(const char* key) { (void)key; return {}; }
int host_opt_default_resolution_index() { return {}; }
int host_opt_frame_cap() { return g_target_fps.load(std::memory_order_relaxed); }
bool host_opt_frame_cap_custom() { return {}; }
int host_opt_frame_cap_custom_fps() { return {}; }
int host_opt_frame_rate() { return g_target_fps.load(std::memory_order_relaxed); }
bool host_opt_get(const char* key) { (void)key; return {}; }
int host_opt_index(const char* key) { (void)key; return {}; }
void host_opt_menu_colour(float* hue_degrees, float* vibrance) { (void)hue_degrees; (void)vibrance; }
float host_opt_menu_text_scale() { return {}; }
int host_opt_motion_rebuilt_bits() { return {}; }
bool host_opt_motion_wanted() { return {}; }
// Return the player's chosen internal guest render resolution (1080p/900p/720p)
// capped at 1920x1080 even on 4K TVs so Bloodborne's render-target heap stays
// inside the 5,120 MiB Xbox Dev Mode memory cap while FSR RCAS upscales crisply.
bool host_opt_output_size(int* w, int* h) {
    if (w != nullptr) *w = g_render_w.load(std::memory_order_relaxed);
    if (h != nullptr) *h = g_render_h.load(std::memory_order_relaxed);
    return true;
}
void host_opt_republish() {  }
bool host_opt_resolution(int* w, int* h) {
    if (w != nullptr) *w = g_render_w.load(std::memory_order_relaxed);
    if (h != nullptr) *h = g_render_h.load(std::memory_order_relaxed);
    return true;
}
int host_opt_resolution_index() { return {}; }
const char* const* host_opt_resolution_labels(int* n) { (void)n; return {}; }
const char* host_opt_resolution_note() { return {}; }
bool host_opt_saved(const char* key) { (void)key; return {}; }
std::uint64_t host_opt_serial() { return g_settings_store.now().serial; }
void host_opt_set(const char* key, bool on) { (void)key; (void)on; }
void host_opt_set_frame_cap(int fps) { bbx::bridge::SetTargetFps(fps); }
void host_opt_set_frame_cap_custom(bool on, int fps) {  }
void host_opt_set_index(const char* key, int index) {  }
void host_opt_set_resolution_index(int i) {  }
bool host_opt_upscaler_chosen() { return {}; }
void host_options_apply() {  }
void host_options_close() {  }
void host_options_draw_page(int page) {  }
void host_options_draw_setup_page(int page) {  }
void host_options_frame(float display_w, float display_h) {  }
void host_options_load() {  }
const char* host_options_page_blurb(int page) { return {}; }
int host_options_page_count() { return {}; }
const char* host_options_page_name(int page) { return {}; }
void host_options_post(std::function<void()> fn) {  }
void host_options_save_now() {  }
int host_options_selftest(const std::string& dir) { return {}; }
void host_options_set_open(bool open) {  }
void host_set_deck_profile(int profile) {  }
bool host_steam_deck() { return {}; }

// host/plugin_panel.h
std::uint32_t plugin_panel_dialog_buttons() { return {}; }
void plugin_panel_draw(float scale, bool pad) {  }
bool plugin_panel_exclusive() { return {}; }
void plugin_panel_hide(const std::string& name) {  }
bool plugin_panel_key(int scancode) { return {}; }
std::vector<PluginMenuEntry> plugin_panel_menus(const std::string& name) { return {}; }
bool plugin_panel_native_centred() { return {}; }
bool plugin_panel_native_confirm(int row) { return {}; }
void plugin_panel_native_failed(const char* why) {  }
void plugin_panel_native_frame() {  }
void plugin_panel_native_gone(bool closed, const char* by) {  }
bool plugin_panel_native_key(int scancode) { return {}; }
void plugin_panel_native_list() {  }
bool plugin_panel_native_menus_any() { return {}; }
bool plugin_panel_native_on_subpage() { return {}; }
bool plugin_panel_native_page(plugin_dialog::Page& out) { return {}; }
int plugin_panel_native_press(int row, std::string* question) { return {}; }
bool plugin_panel_open(const std::string& name, const std::string& page) { return {}; }
bool plugin_panel_overlay_shown() { return {}; }
void plugin_panel_preload() {  }
void plugin_panel_preview_tick() {  }
bool plugin_panel_rows(const std::string& name, const std::string& page, const std::vector<PluginOptionDesc>& rows) { return {}; }
void plugin_panel_settle() {  }
bool plugin_panel_show(const std::string& name, const PluginPanelShow& how) { return {}; }
bool plugin_panel_shown() { return {}; }
void plugin_panel_state(const std::string& name, PluginPanelState& out) {  }
int plugin_panel_take_sound(int* priority) { return {}; }
bool plugin_panel_value(const std::string& name, const std::string& page, const std::string& key, const std::string& value) { return {}; }
void plugin_panel_world(std::uint32_t flags, std::int64_t hp) {  }

// host/plugins.h
int plugins_aa_claim() { return {}; }
std::string plugins_aa_owner() { return {}; }
std::string plugins_aa_owner_scanned() { return {}; }
std::string plugins_adopts() { return {}; }
std::vector<PluginEntry> plugins_catalog() { return {}; }
std::string plugins_clash(const std::vector<PluginEntry>& list, const std::string& name) { return {}; }
std::vector<std::string> plugins_dirs() { return {}; }
void plugins_enter_world(const std::string& host_rules) {  }
void plugins_event_flag(unsigned id, bool value) {  }
void plugins_frame() {  }
void plugins_image(ElfImage* image) {  }
bool plugins_live_switch(const PluginEntry& p) { return {}; }
void plugins_load() {  }
void plugins_menu_option(const std::string& name, const std::string& key, const std::string& value) {  }
std::string plugins_option_value(const PluginEntry& p, const PluginOptionDesc& o) { return {}; }
std::string plugins_ruleset() { return {}; }
void plugins_run_action(const std::string& name, const std::string& action) {  }
std::vector<PluginEntry> plugins_scan() { return {}; }
void plugins_set_enabled(const std::string& name, bool on) {  }
void plugins_set_option(const std::string& name, const std::string& key, const std::string& value) {  }

// host/sampler.h
void sampler_note_exit(const char* step) {  }
void sampler_note_main_thread() {  }
void sampler_note_pump() {  }
void sampler_recover() {  }
void sampler_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }
void sampler_watchdog_start(std::uint64_t guest_lo, std::uint64_t guest_hi) {  }

// host/translation_cache.h // namespace gpu
namespace gpu {
void hlsl_counts(std::uint64_t& taken, std::uint64_t& compiled, std::uint64_t& kept, std::string& reasons) {  }
void hlsl_lane(int lane) {  }
int hlsl_lane_now() { return {}; }
bool hlsl_lift_of(const std::vector<std::uint32_t>& module, std::vector<std::uint32_t>& lift) { return {}; }
int hlsl_mode() { return {}; }
void hlsl_name_next(std::string name, std::uint64_t salt) {  }
void hlsl_note_drawn(std::uint64_t stage_key) {  }
void hlsl_note_lift_again(int stages) {  }
void hlsl_note_override_refused(const std::vector<std::uint32_t>& module) {  }
void hlsl_note_refused(const char* what, int stages, bool rebuilt) {  }
bool hlsl_on() { return {}; }
void hlsl_set_busy_probe(bool (*busy)()) {  }
void hlsl_set_folders(const std::string& bbhost_dir) {  }
void hlsl_set_inline(bool on) {  }
void hlsl_set_name_lookup(std::string (*lookup)(const std::string& name)) {  }
void hlsl_set_overrides(bool on, const std::string& why_off) {  }
void hlsl_set_setting(bool hlsl) {  }
void hlsl_tag_stage(std::uint64_t stage_key) {  }
bool hlsl_took_override() { return {}; }
gcn::LiftResult lift_cached(bool vertex, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference) { return {}; }
gcn::LiftResult lift_compute_cached(const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference) { return {}; }
gcn::LiftResult lift_gs_cached(TranslationUse use, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference, const gcn::GsLift& gs) { return {}; }
gcn::LiftResult lift_tess_cached(TranslationUse use, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference, const gcn::TessLift& tess) { return {}; }
gcn::TranslateResult translate_cached(const gcn::Program& prog, const gcn::TranslateOptions& o, TranslationUse use) { return {}; }
void translation_cache_bounded(bool on) {  }
std::uint64_t translation_cache_bounded_left_out() { return {}; }
bool translation_cache_cold() { return {}; }
void translation_cache_load(const std::string& path) {  }
void translation_cache_report() {  }
void translation_cache_save(const std::string& path, bool force) {  }
void translation_cache_save_async(const std::string& path) {  }
std::string translation_cache_window() { return {}; }
const char* translator_fingerprint() { return {}; }
bool velo_cloth_vs(const std::string& name) { return {}; }
}  // namespace gpu

// host/window.h
float host_display_refresh_hz() { return g_display_hz.load(std::memory_order_relaxed); }
void host_fps_frame_start() { g_game_frames.fetch_add(1, std::memory_order_relaxed); }
InputDevice host_input_device() { return InputDevice::Pad; }
std::uint64_t host_input_serial() { return 1; }
bool host_key_strong() { return {}; }
void host_message_show(const char* text, float seconds) { SetToastMessage(text, seconds); }
void host_mouse_describe(char* out, std::size_t n) { (void)out; (void)n; }
bool host_mouse_position(float& x, float& y) { (void)x; (void)y; return {}; }
void host_mouse_set_relative(bool on) { (void)on; }
void host_mouse_set_visible(bool visible) { (void)visible; }
bool host_mouse_stage_position(float& x, float& y) { (void)x; (void)y; return {}; }
MouseState host_mouse_state() { return {}; }
void host_pad_rumble(std::uint8_t small_mag, std::uint8_t large_mag) {
    ApplyWinrtRumble(small_mag, large_mag);
}
bool host_plugin_input(const char* reader, PluginInput& out) { (void)reader; (void)out; return {}; }
void host_plugin_input_take(const char* owner, const std::uint16_t* scancodes, std::size_t n, std::uint32_t buttons, bool wheel, bool motion) {
    (void)owner; (void)scancodes; (void)n; (void)buttons; (void)wheel; (void)motion;
}
void host_plugin_pad(PluginPad& out) { (void)out; }
void host_plugin_pad_frame() {  }
void host_plugin_pad_take(const char* owner, std::uint32_t buttons, std::uint32_t axes) {
    (void)owner; (void)buttons; (void)axes;
}
std::uint32_t host_plugin_pad_taken_axes() { return {}; }
std::uint32_t host_plugin_pad_taken_buttons() { return {}; }
void host_plugin_pad_test(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry) {
    (void)buttons; (void)lx; (void)ly; (void)rx; (void)ry;
}
void host_plugin_take_drop(const char* owner) { (void)owner; }
std::uint32_t host_plugin_taken_buttons() { return {}; }
bool host_plugin_taken_wheel() { return {}; }
std::uint64_t host_present_last_ns(std::uint64_t* serial) {
    if (serial != nullptr) *serial = g_flips.load(std::memory_order_relaxed);
    return g_last_present_ns.load(std::memory_order_relaxed);
}
bool host_test_keys_play(const char* path) { (void)path; return {}; }
void host_text_entry_begin(const char* initial_utf8, unsigned max_chars, bool hidden, const char* label,
                           TextCharset charset, bool select_initial) {
    (void)charset;
    (void)select_initial;
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    g_text_entry_open = true;
    g_text_entry_state = 0;
    g_text_entry_result = initial_utf8 != nullptr ? std::string(initial_utf8) : std::string();
    g_text_entry_prompt.active = true;
    g_text_entry_prompt.label =
        (label != nullptr && label[0] != '\0') ? std::string(label) : std::string("Enter name");
    g_text_entry_prompt.initial = g_text_entry_result;
    g_text_entry_prompt.max_chars = max_chars == 0 ? 16u : max_chars;
    g_text_entry_prompt.hidden = hidden;
    ApplyWinrtRumble(0, 0);
}
void host_text_entry_end() {
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    g_text_entry_open = false;
    g_text_entry_state = 0;
    g_text_entry_prompt.active = false;
}
int host_text_entry_poll(std::string& text_utf8) {
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    if (!g_text_entry_open) return 2;
    if (g_text_entry_state == 1) {
        text_utf8 = g_text_entry_result;
        return 1;
    }
    if (g_text_entry_state == 2) {
        text_utf8.clear();
        return 2;
    }
    return 0;
}
void host_text_show(const char* text, float seconds, float x, float y, float scale, std::uint32_t align) {
    (void)x; (void)y; (void)scale; (void)align;
    SetToastMessage(text, seconds);
}
bool host_window_desktop_size(int* w, int* h) {
    if (w != nullptr) *w = g_display_w.load(std::memory_order_relaxed);
    if (h != nullptr) *h = g_display_h.load(std::memory_order_relaxed);
    return true;
}
bool host_window_display_now(int* w, int* h) {
    if (w != nullptr) *w = g_display_w.load(std::memory_order_relaxed);
    if (h != nullptr) *h = g_display_h.load(std::memory_order_relaxed);
    return true;
}
void host_window_error_box(const char* title, const char* text) {
    bbx::platform::LogMessage(bbx::platform::Level::kError, "%s: %s",
                              title != nullptr ? title : "error",
                              text != nullptr ? text : "");
}
void host_window_on_pixels(void (*fn)(int, int)) { (void)fn; }
bool host_window_pixels(int* w, int* h) {
    if (w != nullptr) *w = g_display_w.load(std::memory_order_relaxed);
    if (h != nullptr) *h = g_display_h.load(std::memory_order_relaxed);
    return true;
}
void host_window_recreate_swapchain() {  }
float host_window_refresh_hz() { return g_display_hz.load(std::memory_order_relaxed); }
void host_window_request_quit() { g_quit_requested.store(true, std::memory_order_relaxed); }
void host_window_set_mode(int mode, int width, int height) { (void)mode; (void)width; (void)height; }
void host_window_set_stay_borderless(bool on) { (void)on; }
void host_window_set_vsync(bool on) { g_vsync_enabled.store(on, std::memory_order_relaxed); }
bool host_window_start(int width, int height, const char* title) {
    (void)width; (void)height; (void)title;
    return true;
}


// The list pool's resting flag (`gxvk_lists_pool.cpp` is not compiled, and the GX
// walk reads it as "no list is being built", which is what it is here).
namespace gxvk_lists {
std::atomic<bool> g_resting{false};
}  // namespace gxvk_lists

// The event interpreter's registration (`decomp/events/interpreter.cpp` is not
// compiled: it includes a header the vendored tree does not carry).
void decomp_event_interpreter_add() {}


// --- the rest of the residue ----------------------------------------------
// What the census found next: the hooks, the option rows' drawing, the list
// pool, the settings listener and the two arm calls. Same quiet answer.

// engine_prologue_hook, engine_prologue_hook_exec, and engine_call_site_hook
// are defined in engine/graphics_patch.cpp.
bool menu_pointer_in_menu() { return {}; }
void hle_watch_arm(std::uint64_t lo, std::uint64_t hi) {  }
void hle_bp_arm(std::uint64_t va) {  }
bool host_gpu_list_committed(std::uint64_t id) { return {}; }
bool host_gpu_writes_pending(std::uint64_t va, std::size_t bytes) { return {}; }
bool host_gpu_texture_create_ahead(const std::uint32_t tsharp[8], std::uint64_t gen) { return {}; }
bool plugin_panel_native_step(int row, int dir) { return {}; }
bool plugin_panel_native_set(int row, int value) { return {}; }
bool plugin_panel_native_turn(int dir) { return {}; }
bool plugin_panel_native_back() { return {}; }
bool plugins_active(const std::string& name) { return {}; }
void host_settings_listen(HostSettingsListener fn) { (void)g_settings_store.listen(fn); }

namespace option_rows {
void* fn(std::uint64_t bn) { return {}; }
std::int64_t open_section(void* root, void* params, const char* name, void* handler) { return {}; }
void handler_begin() {  }
void add_text_row(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* label, const char16_t* help, const char16_t* text) {  }
void add_toggle_row(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
void add_pair_row(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* first, const char16_t* second, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
void add_slider_row(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def, const char16_t* label, const char16_t* help) {  }
std::uint8_t* rows_list(void* dialog) { return {}; }
std::uint8_t* value_list(std::uint8_t* widget) { return {}; }
void redraw(std::uint8_t* list) {  }
void set_cursor(std::uint8_t* list, int index) {  }
int cursor_of(std::uint8_t* list) { return {}; }
const char16_t* dialog_message(std::uint32_t id) { return {}; }
std::uint64_t ask(std::uint8_t* dialog, std::uint32_t message, void* yes) { return {}; }
void release(std::uint64_t obj) {  }
std::int32_t refs(std::uint64_t obj) { return {}; }
void add_command_row(void* builder, std::uint32_t id, void* open) {  }
void* section_screen(void* out, void* ctx, void* opener) { return {}; }
}  // namespace option_rows


// --- the second census ------------------------------------------------------

std::uint32_t menu_confirm_button() { return {}; }
std::uint32_t menu_back_button() { return {}; }
void hle_watch_arm_reads(std::uint64_t lo, std::uint64_t hi) {  }
void hle_watch_report() {  }
bool host_gpu_splice(std::uint64_t id) { return {}; }
bool ingame_menu_open() { return {}; }
bool host_options_open() { return {}; }
bool plugin_panel_native_corner_wanted() { return {}; }
HostSettings host_settings() { return g_settings_store.now(); }
const HostSettings& host_settings_ref() { return g_settings_store.now(); }
bool host_mouse_relative() { return {}; }
bool host_plugin_taken_motion() { return {}; }
bool host_text_entry_open() {
    std::lock_guard<std::mutex> lock(g_text_entry_mu);
    return g_text_entry_open;
}

namespace gxvk_lists {
std::string run_duty_report() { return {}; }
void cp_busy(std::uint64_t ns) {  }
void cp_held(std::uint64_t ns) {  }
void cp_thread_note(std::uintptr_t clock) {  }
void keep_off_pool_cpus() {  }
void draw_stored(std::uint64_t id, std::uint64_t ctx, std::uint64_t token_va, bool feed) {  }
void cp_look_begin(std::uint64_t id) {  }
void cp_look_end() {  }
void frame_paced(std::uint64_t wait_ns, float cap_fps, bool loading) {
    (void)wait_ns;
    (void)cap_fps;
    (void)loading;
}
void pool_report(bool at_exit) {  }
}  // namespace gxvk_lists

namespace option_rows {
bool ready() { return {}; }
std::uint64_t addr(std::uint64_t bn) { return {}; }
int row_count(void* dialog) { return {}; }
std::uint8_t* row_widget(void* dialog, int row) { return {}; }
std::uint64_t value_target(std::uint8_t* widget) { return {}; }
void layout(void* dialog) {  }
void clear_rows(void* dialog) {  }
}  // namespace option_rows

// The list pool's sampling interval: every draw, as its `BBHOST_GXVK_LISTS_SAMPLE`
// default says.
namespace gxvk_lists {
const unsigned g_look_every = 1;
}  // namespace gxvk_lists

// The launcher's text entry has no editable field on the console yet, so the
// guest's player-name prompt is a widget nobody can type into.
void TextEntry::begin(const char* initial_utf8, unsigned max, TextCharset cs, bool select_initial) {
    text = initial_utf8 == nullptr ? std::string() : std::string(initial_utf8);
    max_chars = max;
    charset = cs;
    // The suggestion shows selected, so the first character the guest "types"
    // replaces it - which is what a name box's preset wants.
    selected = select_initial;
}

// --- the 128-bit division the compiler emits --------------------------------
// One guest translation unit divides a 128-bit value (the GX walk's byte
// counters), and clang-cl asks the C runtime for `__udivti3`, which MSVC's
// libraries do not ship. Restoring division, written without any division, so
// the helper cannot call itself.
extern "C" unsigned __int128 __udivti3(unsigned __int128 n, unsigned __int128 d) {
    if (d == 0) return 0;
    unsigned __int128 q = 0, r = 0;
    for (int bit = 127; bit >= 0; --bit) {
        r = (r << 1) | ((n >> bit) & 1);
        if (r >= d) {
            r -= d;
            q |= (static_cast<unsigned __int128>(1) << bit);
        }
    }
    return q;
}

extern "C" unsigned __int128 __umodti3(unsigned __int128 n, unsigned __int128 d) {
    if (d == 0) return 0;
    unsigned __int128 r = 0;
    for (int bit = 127; bit >= 0; --bit) {
        r = (r << 1) | ((n >> bit) & 1);
        if (r >= d) r -= d;
    }
    return r;
}

// --- the boot --------------------------------------------------------------
#if defined(BBX_GUEST_BOOT)
namespace bbx::bridge {
namespace {

// The guest's memory, the entry point and the argv block all outlive this call:
// `_start` runs on its own thread until the game either finishes or the player
// closes the window, so none of it may live in a stack frame.
ElfImage g_image;
std::string g_eboot_path;

// The baked console shaders, loaded once at boot: the dump's copy, else the one
// a developer put in LocalState. Every blob in it was compiled for this build's
// DXIL target (dxil-6.4, tables, 11_0) by the bake, and ParseCache validated
// every entry before one was trusted.
bbx::d3d12::ShaderCache g_shader_cache;
alignas(16) std::uint64_t g_argv[4] = {0, 0, 0, 0};
std::atomic<bool> g_running{false};

// The guest's main thread. Eight MiB, reserved rather than committed (the
// launcher's own stack budget), and entered through the thunk that puts the
// guest's TCB in this thread's TLS slot - the rewritten `fs:[0]` reads it.
constexpr std::size_t kGuestStack = 8ull * 1024 * 1024;

struct GuestLaunch {
    GuestStart fn = nullptr;
    void* argv = nullptr;
};

DWORD WINAPI GuestMain(void* p) {
    using SetStackGuaranteeFn = BOOL(WINAPI*)(PULONG);
    if (const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll")) {
        if (const auto set_guarantee = reinterpret_cast<SetStackGuaranteeFn>(
                ::GetProcAddress(k32, "SetThreadStackGuarantee"))) {
            ULONG stack_guarantee = 65536;
            (void)set_guarantee(&stack_guarantee);
        }
    }
    auto* launch = static_cast<GuestLaunch*>(p);
    void* tcb = hle_thread_enter_guest();
    bbx::platform::LogMirroredMessage(bbx::platform::Level::kInfo, "guest: entering _start");
    launch->fn(launch->argv, nullptr);
    hle_thread_leave_guest(tcb);
    g_running.store(false);
    bbx::platform::LogF("guest: _start returned");
    return 0;
}

}  // namespace

namespace {

// <dump>/bbx_shaders.cache and its sidecar, else LocalState\data (where the
// on-console first-boot baker writes), else the LocalState root copy of both.
// All are read through the host-fs seam: a dump arrives through a
// FutureAccessList token, which fopen cannot open on the console.
void LoadShaderCache(const std::string& eboot, const std::string& data_dir) {
    std::string dir;
    const std::size_t slash = eboot.find_last_of("\\/");
    if (slash != std::string::npos) dir = eboot.substr(0, slash + 1);
    std::vector<std::string> candidates;
    if (!data_dir.empty()) candidates.push_back(bbx::hostfs::JoinPath(data_dir, "bbx_shaders.cache"));
    if (!dir.empty()) candidates.push_back(dir + "bbx_shaders.cache");
    candidates.push_back(bbx::platform::Local("bbx_shaders.cache"));

    bool found_best = false;
    bbx::d3d12::ShaderCache best_cache;
    std::string best_path;
    for (const std::string& path : candidates) {
        std::vector<std::uint8_t> bytes;
        if (!bbx::hostfs::ReadFileAll(path, &bytes)) continue;
        std::vector<std::uint8_t> stamp;
        // The sidecar is `<cache>.stamp.txt`: the bake's record of who compiled
        // it, which is what ExpectedConsoleStamp pairs with this build's target.
        if (!bbx::hostfs::ReadFileAll(path + ".stamp.txt", &stamp)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                      "shader_cache: %s has no .stamp.txt beside it; skipping", path.c_str());
            continue;
        }
        const std::string stamp_text(reinterpret_cast<const char*>(stamp.data()), stamp.size());
        bbx::d3d12::ShaderCache probe;
        const bbx::d3d12::CacheStatus status = probe.Load(bytes, stamp_text);
        bbx::platform::LogF("shader_cache: status=%s entries=%zu bytes=%zu path=%s detail=%s",
                            bbx::d3d12::CacheStatusName(status), probe.entries(), probe.bytes(),
                            path.c_str(), probe.detail().c_str());
        if (status == bbx::d3d12::CacheStatus::kOk) {
            if (!found_best || probe.entries() > best_cache.entries()) {
                best_cache = std::move(probe);
                best_path = path;
                found_best = true;
            }
        }
    }
    if (found_best) {
        g_shader_cache = std::move(best_cache);
        bbx::platform::LogF("shader_cache: selected %s (%zu entries, %zu bytes)",
                            best_path.c_str(), g_shader_cache.entries(), g_shader_cache.bytes());
        return;
    }
    bbx::platform::LogMessage(bbx::platform::Level::kInfo,
                              "shader_cache: no valid cache in dump or LocalState");
}

std::mutex g_last_start_error_mu;
std::string g_last_start_error;

void SetLastStartError(std::string message) {
    std::lock_guard<std::mutex> lock(g_last_start_error_mu);
    g_last_start_error = std::move(message);
}

#if defined(_WIN32)
void BbhostLogWrite(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) return;
    std::size_t pos = 0;
    while (pos < len) {
        std::size_t end = pos;
        while (end < len && data[end] != '\n') ++end;
        std::size_t line_len = end - pos;
        while (line_len > 0 && data[pos + line_len - 1] == '\r') --line_len;
        if (line_len > 0) {
            constexpr std::size_t kMaxChunk = 460;
            const int chunk = static_cast<int>(std::min<std::size_t>(line_len, kMaxChunk));
            bbx::platform::LogF("%.*s", chunk, data + pos);
        }
        pos = (end < len) ? (end + 1) : len;
    }
}
#endif

void InstallBbhostLogBridge() {
    static std::once_flag once;
    std::call_once(once, []() {
#if defined(_WIN32)
        host_log_detail::g_console_write.store(&BbhostLogWrite, std::memory_order_release);
#endif
        host_log_decide(true);
    });
}

}  // namespace

std::string LastStartError() {
    std::lock_guard<std::mutex> lock(g_last_start_error_mu);
    return g_last_start_error;
}

bool StartGuest(const GuestRequest& req) {
    SetLastStartError({});
    InstallBbhostLogBridge();
    if (req.eboot.empty()) {
        SetLastStartError("No eboot.bin found in the selected dump folder.");
        bbx::platform::LogMessage(bbx::platform::Level::kError, "boot: no eboot.bin in the dump");
        return false;
    }
    if (g_running.load()) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "boot: already running");
        return true;
    }
    g_eboot_path = req.eboot;

    // Publish the console settings snapshot, Hunter identity, intro-skip flag,
    // and Bloodborne gameplay enhancements before HLE patching installs
    // frame_rate, graphics_patch, camera, and patch_manifest hooks.
    const ConsoleSettings cfg = GetConsoleSettings();
    g_settings_store.publish(BuildHostSettings(cfg));
    const char* hunter = req.hunter_name.empty() ? "Hunter" : req.hunter_name.c_str();
    hle_dialog_set_default_name(hunter, true);
    hle_np_set_online_id(hunter);
    hle_video_set_fps_cap(cfg.fps_cap);
    config_set_skip_intro(cfg.skip_logos);
    config_set_enhancements(cfg.enhancements, cfg.enhancements, false, false, true,
                            cfg.enhancements);
    bbx::platform::LogF(
        "boot: settings fps=%d res=%dx%d vsync=%u perf=%u ssao=%u aa=%u dyn_sh=%u sh_res=%d lod=%d "
        "skip_logos=%u enhancements=%u ca=%u blur=%u dof=%u vignette=%u fov=%.2f cam_dist=%.2f "
        "deadzone=%.2f hunter=\"%s\"",
        cfg.fps_cap, cfg.render_width, cfg.render_height, static_cast<unsigned>(cfg.vsync),
        static_cast<unsigned>(cfg.performance_mode), static_cast<unsigned>(cfg.ssao),
        static_cast<unsigned>(cfg.anti_alias), static_cast<unsigned>(cfg.dynamic_shadows),
        cfg.shadow_resolution, cfg.lod_bias, static_cast<unsigned>(cfg.skip_logos),
        static_cast<unsigned>(cfg.enhancements), static_cast<unsigned>(cfg.chromatic_aberration),
        static_cast<unsigned>(cfg.motion_blur), static_cast<unsigned>(cfg.depth_of_field),
        static_cast<unsigned>(cfg.vignette), static_cast<double>(cfg.fov_scale),
        static_cast<double>(cfg.camera_distance_scale), static_cast<double>(cfg.stick_deadzone),
        hunter);

    // The HLE's file system: which folder is app0, which overlay is the update,
    // where saves and temporaries go, and where the player's loose files are.
    bbx::platform::LogCrumb("boot:roots");
    hle_fs_set_roots(req.app0.c_str(), req.data.empty() ? nullptr : req.data.c_str(),
                     req.tmp.empty() ? nullptr : req.tmp.c_str(), g_eboot_path.c_str(),
                     req.mods.empty() ? nullptr : req.mods.c_str(),
                     req.update.empty() ? nullptr : req.update.c_str(), req.explicit_update);
    // The import table: every sce* the guest can call, bound to the HLE.
    bbx::platform::LogCrumb("boot:register_hle");
    register_hle();

    bbx::platform::LogCrumb("boot:load_elf");
    g_image = ElfImage{};
    // The loader unwraps the dump's fake SELF (4F 15 3D 1D) itself, so a dumped
    // eboot.bin and a flat ELF both work.
    if (!load_orbis_elf(g_eboot_path.c_str(), &g_image, legacy_cpu_arena_size())) {
        SetLastStartError("Failed to load or unwrap eboot.bin (see loader.log).");
        bbx::platform::LogMessage(bbx::platform::Level::kError, "boot: %s did not load", g_eboot_path.c_str());
        return false;
    }
    const std::string raw_sha256 = g_image.sha256;
    bbx::platform::LogF("image: sha256 %s entry 0x%llx base 0x%llx size 0x%zx",
                        raw_sha256.c_str(), static_cast<unsigned long long>(g_image.entry),
                        static_cast<unsigned long long>(g_image.mem.slide),
                        static_cast<std::size_t>(g_image.mem.size));
    // The flip's view of the arena, before any guest instruction runs: the
    // display buffers are registered once the game is up, and host_present
    // resolves them here rather than trusting them.
    g_guest_base.store(static_cast<std::uint8_t*>(g_image.mem.base), std::memory_order_release);
    g_guest_slide.store(g_image.mem.slide, std::memory_order_relaxed);
    g_guest_size.store(g_image.mem.size, std::memory_order_relaxed);
    bool is_109 = (raw_sha256 == kEboot109Sha256);
    bbx::core::Eboot109AnchorCheck anchors{};
    if (!is_109) {
        const std::uint64_t entry_elf_va =
            g_image.entry >= g_image.mem.slide ? (g_image.entry - g_image.mem.slide) : 0ull;
        anchors = bbx::core::VerifyBloodborne109CodeAnchors(g_image.mem.base, g_image.mem.size,
                                                            entry_elf_va);
        if (anchors.ok()) {
            // An fSELF unwrapped by bbx_self.cpp or a regional v1.09 dump can
            // differ in outer ELF padding/headers from unfself's flat file while
            // carrying identical v1.09 machine code. Promoting sha256 once all
            // 6 untouched v1.09 prologues (107 bytes) + entry 0xa0 match lets
            // hle_patch_guest and every engine hook apply safely (each hook also
            // byte-checks its own site before writing).
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kInfo,
                "boot: eboot file sha256 %s differs from pinned unfself digest, but all %zu/%zu "
                "Bloodborne v1.09 .text anchors verified (%s); enabling full v1.09 patches",
                raw_sha256.c_str(), anchors.matched, anchors.total, anchors.detail.c_str());
            g_image.sha256 = kEboot109Sha256;
            is_109 = true;
        } else {
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kWarn,
                "boot: v1.09 .text anchor check did not match (%s)", anchors.detail.c_str());
        }
    }
    if (!is_109) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kWarn,
            "boot: starting unverified eboot (sha256 %s, %s, modded_eboot_ok=%d); v1.09 address patches skipped",
            raw_sha256.c_str(), anchors.detail.c_str(), req.modded_eboot_ok ? 1 : 0);
    }

    // The baked shaders, from the dump or from LocalState\data (produced by the
    // on-console first-boot baker): compiled for this build's DXIL 6.4 target so
    // the guest draws with zero shader compilation overhead at runtime.
    bbx::platform::LogCrumb("boot:shader_cache");
    LoadShaderCache(g_eboot_path, req.data);

    bbx::platform::LogCrumb("boot:patch_guest");
    // The instructions this processor cannot run (BMI1, LZCNT, MOVBE), rewritten
    // before anything else touches the code.
    legacy_cpu_rewrite(&g_image);
    perf_meter_start(g_image.mem.slide, g_image.mem.slide + g_image.mem.size, is_109);
    main_wait_guest_base(g_image.mem.slide);
    // The launcher has the window: the guest must not try to open one, and the
    // display buffers are sized for the television the launcher already has.
    host_set_headless(false);
    hle_patch_guest(&g_image);
    debug_menu_install(&g_image);
    legacy_cpu_verify("before the guest starts");

    // The guest sees no host flags: argc = 1, argv[0] = the eboot's path, and
    // the 16-byte alignment the SysV ABI's prologues assume.
    g_argv[0] = 1;
    g_argv[1] = reinterpret_cast<std::uint64_t>(g_eboot_path.c_str());
    g_argv[2] = 0;

    static GuestLaunch launch;
    launch.fn = reinterpret_cast<GuestStart>(g_image.entry);
    launch.argv = g_argv;
    bbx::platform::LogCrumb("boot:guest_start");
    bbx::platform::LogF("guest: calling _start 0x%llx on an %zu MiB stack",
                        static_cast<unsigned long long>(g_image.entry), kGuestStack >> 20);
    g_running.store(true);
    // Commit 2 MiB of the 8 MiB stack upfront (flags = 0 -> dwStackSize is
    // initial commit while PE /STACK:8388608 sets reserve) so Bloodborne's
    // unprobed PS4 frames (>4 KiB) cannot leap past Windows' guard page.
    constexpr std::size_t kGuestInitialCommit = 2u << 20;
    const HANDLE thread = CreateThread(nullptr, kGuestInitialCommit, GuestMain, &launch, 0, nullptr);
    if (thread == nullptr) {
        g_running.store(false);
        const DWORD err = GetLastError();
        SetLastStartError("CreateThread for Bloodborne failed (win32=" + std::to_string(err) + ").");
        bbx::platform::LogMessage(bbx::platform::Level::kError,
                                  "guest: CreateThread with an %zu MiB stack failed (error %lu)",
                                  kGuestStack >> 20, static_cast<unsigned long>(err));
        g_guest_base.store(nullptr, std::memory_order_release);
        g_guest_slide.store(0, std::memory_order_relaxed);
        g_guest_size.store(0, std::memory_order_relaxed);
        unload_elf(&g_image);
        return false;
    }
    CloseHandle(thread);  // the guest runs to _start's return; the handle is not needed
    return true;
}

std::string ShaderCacheLine() { return g_shader_cache.Report(); }

bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size) {
    bbx::d3d12::BakedBlob blob;
    if (!g_shader_cache.Lookup(key, &blob)) return false;
    if (bytes != nullptr) *bytes = blob.data;
    if (size != nullptr) *size = blob.size;
    if (raw_size != nullptr) *raw_size = blob.raw_size;
    return true;
}

bool Running() { return g_running.load(); }

}  // namespace bbx::bridge
#else
// The link is off (xbox/CMakeLists.txt, BBX_GUEST_LINK): the archives are
// compiled but not part of this binary, so there is no loader to call and no
// guest to start. The launcher says so and stays up. This arm is for renderer
// iteration only: CMake refuses `BBX_GUEST_LINK=OFF` without `BBX_GUESTLESS=ON`,
// and the workflow's "guest link evidence" step fails any package that reached
// this branch, so an app that boots nothing cannot be shipped by accident.
namespace bbx::bridge {
std::string LastStartError() { return "Guest runtime is not linked into this build."; }
bool StartGuest(const GuestRequest& req) {
    (void)req;
    bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                              "boot: the guest runtime is not linked into this build (BBX_GUEST_LINK=OFF)");
    return false;
}
bool Running() { return false; }
std::string ShaderCacheLine() { return "no cache: the guest runtime is not linked into this build"; }
bool ShaderCacheLookup(std::uint64_t key, const std::uint8_t** bytes, std::uint32_t* size,
                       std::uint32_t* raw_size) {
    (void)key;
    if (bytes != nullptr) *bytes = nullptr;
    if (size != nullptr) *size = 0;
    if (raw_size != nullptr) *raw_size = 0;
    return false;
}
}  // namespace bbx::bridge
#endif
