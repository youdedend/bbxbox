// SPDX-License-Identifier: GPL-3.0-or-later
// P0: the shell. A C++/WinRT view owns the CoreWindow, D3D12 owns the television,
// and the launcher runs on that thread until the player starts the game (P1 hands
// the same thread to bbhost's guest).
//
// There is no SDL on this target, and there is no SDL fallback. SDL cannot
// initialise under UWP - its video subsystem wants an SDL_main that a packaged
// view provider does not have - so the window is the CoreWindow this view is
// handed, the display is a D3D12 swapchain created for it, and the pad comes
// from WinRT's Windows.Gaming.Input. A device that cannot be created is a
// plain-English log line plus the once-a-second gate line, and the window stays
// alive; it is never a reason to start a second renderer or to exit.
#include <windows.h>

// HSTRING and WindowsGetStringRawBuffer: the activation export takes a raw
// HSTRING, so the string ABI has to be visible.
#include <winstring.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

// No <SDL3/SDL.h>: nothing in this translation unit initialises SDL, opens an
// SDL window, or draws with SDL_Renderer. The pad comes from WinRT (MaskFromPad)
// and the frame from the D3D12 session.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "xbox/platform/boot_steps.h"
#include "xbox/platform/d3d12_session.h"
#include "xbox/platform/display.h"
#include "xbox/platform/dump.h"
#include "xbox/platform/guest_memory.h"
#include "xbox/platform/bbhost_bridge.h"
#include "xbox/platform/guest_windows.h"
#include "pthread.h"  // the winpthreads shim: bbxbox::winpthreads::SelfTest
#include "xbox/platform/host_fs.h"
#include "xbox/platform/ini_file.h"
#include "xbox/platform/launcher.h"
#include "xbox/platform/log.h"
#include "xbox/platform/memory_selftest.h"
#include "xbox/platform/protocol_uri.h"
#include "xbox/platform/shader_baker.h"
#include "xbox/platform/storage.h"
#include "xbox/platform/ux.h"

namespace {

using bbx::platform::Pad;
using bbx::platform::PadAction;

struct Session {
    bbx::platform::IniMap ini;
    std::string ini_text;
    bbx::platform::DumpLayout layout;
    bbx::platform::LauncherModel model;
    bbx::platform::Chords chords;
    bbx::platform::VirtualKeyboard keyboard;
    int launcher_tab = 0;  // 0 = PLAY, 1 = SETTINGS, 2 = STORAGE
    bool keyboard_open = false;
    bool quit = false;
    bool start_requested = false;
    bool start_force_unverified = false;
    bool baking = false;
    bool start_after_bake = false;
    std::atomic<bool> bake_finished{false};
    bbx::platform::BakeProgressState bake_progress;
    bbx::platform::BakeResult bake_result;
    std::thread bake_thread;
    bbx::bridge::GuestRequest pending_guest_req;

    // Background USB sync / save backup-restore worker so UI never stalls.
    bool usb_sync_running = false;
    std::atomic<bool> usb_sync_finished{false};
    bool usb_sync_ok = false;
    std::string usb_sync_message;
    std::thread usb_sync_thread;

    // In-game L3+R3 Quick Menu & guest IME text-entry overlay state.
    bool quick_menu_open = false;
    std::size_t quick_menu_selected = 0;
    bool ime_open = false;
    std::string ime_label;
    bool ime_hidden = false;
    std::size_t ime_max_chars = 16;
    bbx::platform::VirtualKeyboard ime_keyboard;

    std::string usb_token;
    std::string patch_token;
    // The only renderer: device, 4K swapchain, pipelines, replay and the HUD. A
    // failed creation leaves it null, which the launcher survives (the window stays
    // up and the gate line keeps printing); there is no SDL renderer to fall to.
    std::unique_ptr<bbx::platform::D3d12Session> gpu;
    // Why the display is not up, in plain English. Mirrored into g_display_fault
    // (a static pointer) for the 1 Hz gate line.
    std::string display_fault;
};

Session g_session;
winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> g_core_window;
std::atomic<std::uint32_t> g_view_thread_id{0};
std::atomic<void*> g_stored_window_abi{nullptr};
std::atomic<bool> g_window_closed{false};
std::atomic<bool> g_window_active{true};
std::atomic<bool> g_app_visible{true};
std::atomic<std::uint32_t> g_visibility_events{0};
std::atomic<bool> g_display_acquire_entered{false};
std::atomic<bool> g_display_ready{false};
std::atomic<bool> g_settings_loaded{false};
std::atomic<bool> g_display_resize_pending{false};
std::atomic<std::uint32_t> g_pending_resize_w{0};
std::atomic<std::uint32_t> g_pending_resize_h{0};
std::atomic<const char*> g_ui_blocker{"none"};
// Why the display is not up, for the gate line. A pointer to a static string, so
// the 1 Hz watcher thread can read it without a lock and without touching a
// std::string that ui-main is assigning.
std::atomic<const char*> g_display_fault{""};
std::atomic<std::uint64_t> g_present_count{0};

// Physical USB keyboard queue populated by CoreWindow::CharacterReceived / KeyDown
// and drained on ui-main for Hunter Name IME and menu navigation.
std::mutex g_key_mu;
std::deque<char> g_typed_chars;
std::atomic<bbx::platform::ButtonMask> g_key_nav_edges{0};
std::atomic<bool> g_key_backspace{false};
std::atomic<bool> g_key_enter{false};
std::atomic<bool> g_key_escape{false};
std::atomic<bool> g_key_quick_menu{false};

// LaunchPass / RetroPass URI protocol launch parameters (`bbxbox:?cmd=...&launchOnExit=...`).
std::mutex g_protocol_mu;
bbx::platform::ProtocolLaunchArgs g_protocol_args;
std::string g_launch_on_exit_uri;

void LaunchReturnFrontendUri() {
    std::string uri_utf8;
    {
        std::lock_guard<std::mutex> lk(g_protocol_mu);
        uri_utf8 = g_launch_on_exit_uri;
    }
    if (uri_utf8.empty()) return;
    try {
        const winrt::Windows::Foundation::Uri uri(winrt::to_hstring(uri_utf8));
        bbx::platform::LogMirroredMessage(bbx::platform::Level::kInfo,
                                          "protocol: launching return URI \"%s\"",
                                          uri_utf8.c_str());
        (void)winrt::Windows::System::Launcher::LaunchUriAsync(uri);
        ::Sleep(150);
    } catch (const winrt::hresult_error& err) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kWarn,
            "protocol: LaunchUriAsync(\"%s\") failed (0x%08x)",
            uri_utf8.c_str(), static_cast<unsigned>(err.code().value));
    } catch (...) {
    }
}

void SetUiBlocker(const char* blocker) noexcept {
    g_ui_blocker.store(blocker != nullptr ? blocker : "none", std::memory_order_relaxed);
}

// The display's fault, as a pointer the watcher thread can read. The strings are
// static literals or a small ring of buffers that ui-main owns, so the pointer is
// valid for the life of the process and reading it never races a std::string.
void SetDisplayFault(const std::string& fault) noexcept {
    static char ring[4][160];
    static std::atomic<unsigned> next{0};
    const unsigned slot = next.fetch_add(1, std::memory_order_relaxed) % 4u;
    char* buffer = ring[slot];
    std::snprintf(buffer, sizeof(ring[slot]), "%s", fault.c_str());
    g_display_fault.store(buffer, std::memory_order_relaxed);
}

void ClearDisplayFault() noexcept { g_display_fault.store("", std::memory_order_relaxed); }

// Process-lifetime boot step + heartbeat tracker: heap-owned so App::~App() and
// std::atexit handlers never race static destruction order.
bbx::platform::BootStepTracker& Steps() {
    static auto* tracker = new bbx::platform::BootStepTracker(
        [](bbx::platform::Level level, const std::string& line) {
            bbx::platform::LogMirroredMessage(level, "%s", line.c_str());
        },
        []() { return bbx::platform::LogElapsedMs(); });
    return *tracker;
}

void OnBootAtExit() noexcept {
    try {
        Steps().Shutdown("atexit");
        bbx::platform::LogFlush();
        bbx::platform::LogClose();
    } catch (...) {
    }
}

void StartBootInstrumentation() {
    static std::once_flag once;
    std::call_once(once, []() {
        (void)std::atexit(&OnBootAtExit);
        Steps().StartHeartbeatThread(
            std::chrono::milliseconds(bbx::platform::kHeartbeatPeriodMs),
            []() {
                const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
                (void)pthread_setname_np(pthread_self(), "boot-heartbeat");
                Steps().RegisterThread(tid, "boot-heartbeat");
            },
            []() {
                const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
                Steps().UnregisterThread(tid);
            });
    });
}

void ReplayStorageOnce() {
    static std::once_flag once;
    std::call_once(once, []() { bbx::platform::ReplayStorageResults(); });
}

bbx::platform::TelemetrySample QueryTelemetrySample() {
    bbx::platform::TelemetrySample sample;
    try {
        sample.used_mib = winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
        sample.limit_mib = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20;
        sample.usage_level = static_cast<std::uint32_t>(
            winrt::Windows::System::MemoryManager::AppMemoryUsageLevel());
    } catch (const winrt::hresult_error&) {
    }
    sample.presents = g_present_count.load(std::memory_order_relaxed);
    sample.visible = g_app_visible.load(std::memory_order_relaxed);
    return sample;
}

struct BbxThreadEntry32 {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
};

FARPROC ResolveKernelSymbol(const char* symbol) {
    HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
    if (kb != nullptr) {
        if (const FARPROC fn = ::GetProcAddress(kb, symbol)) return fn;
    }
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32 != nullptr) {
        if (const FARPROC fn = ::GetProcAddress(k32, symbol)) return fn;
    }
    return nullptr;
}

std::vector<bbx::platform::ThreadDiagnostic> SnapshotProcessThreads() {
    std::vector<bbx::platform::ThreadDiagnostic> known = Steps().RegisteredThreads();
    const auto find_known_name = [&known](DWORD tid) -> std::string {
        for (const auto& item : known) {
            if (item.tid == static_cast<std::uint32_t>(tid) && !item.name.empty()) {
                return item.name;
            }
        }
        return {};
    };

    using CreateSnapshotFn = HANDLE(WINAPI*)(DWORD, DWORD);
    using ThreadWalkFn = BOOL(WINAPI*)(HANDLE, BbxThreadEntry32*);
    using OpenThreadFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using GetThreadDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    using LocalFreeFn = HLOCAL(WINAPI*)(HLOCAL);

    static const auto create_snapshot =
        reinterpret_cast<CreateSnapshotFn>(ResolveKernelSymbol("CreateToolhelp32Snapshot"));
    static const auto thread_first =
        reinterpret_cast<ThreadWalkFn>(ResolveKernelSymbol("Thread32First"));
    static const auto thread_next =
        reinterpret_cast<ThreadWalkFn>(ResolveKernelSymbol("Thread32Next"));
    static const auto open_thread =
        reinterpret_cast<OpenThreadFn>(ResolveKernelSymbol("OpenThread"));
    static const auto get_thread_desc =
        reinterpret_cast<GetThreadDescFn>(ResolveKernelSymbol("GetThreadDescription"));
    static const auto local_free =
        reinterpret_cast<LocalFreeFn>(ResolveKernelSymbol("LocalFree"));

    if (create_snapshot == nullptr || thread_first == nullptr || thread_next == nullptr) {
        return known;
    }

    constexpr DWORD kSnapThread = 0x00000004u;
    constexpr DWORD kThreadQueryLimited = 0x0800u;
    const HANDLE snap = create_snapshot(kSnapThread, 0);
    if (snap == INVALID_HANDLE_VALUE || snap == nullptr) {
        return known;
    }

    const DWORD self_pid = ::GetCurrentProcessId();
    std::vector<bbx::platform::ThreadDiagnostic> out;
    BbxThreadEntry32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = thread_first(snap, &entry); ok != FALSE; ok = thread_next(snap, &entry)) {
        if (entry.th32OwnerProcessID == self_pid) {
            bbx::platform::ThreadDiagnostic diag;
            diag.tid = static_cast<std::uint32_t>(entry.th32ThreadID);
            diag.base_priority = static_cast<std::int32_t>(entry.tpBasePri);
            diag.name = find_known_name(entry.th32ThreadID);
            if (diag.name.empty() && open_thread != nullptr && get_thread_desc != nullptr) {
                const HANDLE th = open_thread(kThreadQueryLimited, FALSE, entry.th32ThreadID);
                if (th != nullptr) {
                    PWSTR wide = nullptr;
                    if (SUCCEEDED(get_thread_desc(th, &wide)) && wide != nullptr) {
                        char narrow[96] = {};
                        const int wrote = ::WideCharToMultiByte(
                            CP_UTF8, 0, wide, -1, narrow, static_cast<int>(sizeof(narrow)),
                            nullptr, nullptr);
                        if (wrote > 0) {
                            narrow[sizeof(narrow) - 1] = '\0';
                            diag.name = narrow;
                        }
                        if (local_free != nullptr) (void)local_free(wide);
                    }
                    ::CloseHandle(th);
                }
            }
            out.push_back(std::move(diag));
        }
        entry.dwSize = sizeof(entry);
    }
    ::CloseHandle(snap);
    return out.empty() ? known : out;
}

class ScopedTelemetryThread {
public:
    ScopedTelemetryThread()
        : thread_([this]() {
              try {
                  winrt::init_apartment(winrt::apartment_type::multi_threaded);
              } catch (const winrt::hresult_error&) {
              }
              const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
              (void)pthread_setname_np(pthread_self(), "telemetry");
              Steps().RegisterThread(tid, "telemetry");

              bbx::platform::StallWatchdog watchdog(
                  Steps(),
                  [](bbx::platform::Level level, const std::string& line) {
                      bbx::platform::LogMirroredMessage(level, "%s", line.c_str());
                  },
                  []() { return bbx::platform::LogRecentLines(); },
                  []() { return SnapshotProcessThreads(); },
                  g_present_count.load(std::memory_order_relaxed),
                  /*threshold_samples=*/2);

              std::unique_lock<std::mutex> lock(mu_);
              while (!cv_.wait_for(
                  lock, std::chrono::seconds(bbx::platform::kTelemetryPeriodSeconds),
                  [this]() { return stop_; })) {
                  lock.unlock();
                  (void)watchdog.OnSample(QueryTelemetrySample());
                  lock.lock();
              }

              Steps().UnregisterThread(tid);
          }) {}

    ~ScopedTelemetryThread() { Stop(); }

    ScopedTelemetryThread(const ScopedTelemetryThread&) = delete;
    ScopedTelemetryThread& operator=(const ScopedTelemetryThread&) = delete;

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// 1 s pre-acquisition gate/blocker monitor (Point 3): while display acquisition
// has not yet entered or completed, logs the step gate, ready/entered flags,
// visibility state, stored CoreWindow presence, and whatever call or wait
// primitive `ui-main` is currently blocked on.
class ScopedAcquireWatch {
public:
    ScopedAcquireWatch()
        : thread_([this]() {
              const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
              (void)pthread_setname_np(pthread_self(), "acquire-watch");
              Steps().RegisterThread(tid, "acquire-watch");

              std::unique_lock<std::mutex> lock(mu_);
              while (!cv_.wait_for(
                  lock, std::chrono::milliseconds(bbx::platform::kAcquireGatePeriodMs),
                  [this]() { return stop_ || g_display_ready.load(std::memory_order_relaxed); })) {
                  lock.unlock();
                  bbx::platform::AcquireGateSnapshot gate;
                  gate.acquire_entered =
                      g_display_acquire_entered.load(std::memory_order_relaxed);
                  gate.display_ready = g_display_ready.load(std::memory_order_relaxed);
                  gate.stored_window_present =
                      (g_stored_window_abi.load(std::memory_order_relaxed) != nullptr);
                  gate.visible = g_app_visible.load(std::memory_order_relaxed);
                  gate.visibility_events =
                      g_visibility_events.load(std::memory_order_relaxed);
                  const char* blocker = g_ui_blocker.load(std::memory_order_relaxed);
                  gate.ui_blocker = blocker != nullptr ? blocker : "none";
                  // A static string owned by the display code, so reading it here
                  // cannot race a std::string assignment on ui-main.
                  const char* fault = g_display_fault.load(std::memory_order_relaxed);
                  gate.fault = fault != nullptr ? fault : "";
                  const std::string line =
                      bbx::platform::FormatAcquireGateLine(Steps().SnapshotStep(), gate);
                  bbx::platform::LogMirroredMessage(bbx::platform::Level::kWarn, "%s",
                                                    line.c_str());
                  lock.lock();
              }

              Steps().UnregisterThread(tid);
          }) {}

    ~ScopedAcquireWatch() { Stop(); }

    ScopedAcquireWatch(const ScopedAcquireWatch&) = delete;
    ScopedAcquireWatch& operator=(const ScopedAcquireWatch&) = delete;

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// The player's controller, read from WinRT. There is no SDL pad: SDL never
// initialises on this target, and Windows.Gaming.Input is what the console
// actually exposes to a packaged app.
bbx::platform::ButtonMask MaskFromPad() {
    using bbx::platform::Bit;
    using winrt::Windows::Gaming::Input::GameControllerSwitchPosition;
    using winrt::Windows::Gaming::Input::Gamepad;
    using winrt::Windows::Gaming::Input::GamepadButtons;
    using winrt::Windows::Gaming::Input::RawGameController;
    bbx::platform::ButtonMask mask = 0;
    try {
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
        if (pad) {
            const GamepadButtons buttons = pad.GetCurrentReading().Buttons;
            const auto has = [buttons](GamepadButtons flag) {
                return (buttons & flag) == flag;
            };
            if (has(GamepadButtons::A)) mask |= Bit(Pad::kSouth);
            if (has(GamepadButtons::B)) mask |= Bit(Pad::kEast);
            if (has(GamepadButtons::X)) mask |= Bit(Pad::kWest);
            if (has(GamepadButtons::Y)) mask |= Bit(Pad::kNorth);
            if (has(GamepadButtons::View)) mask |= Bit(Pad::kView);
            if (has(GamepadButtons::Menu)) mask |= Bit(Pad::kMenu);
            if (has(GamepadButtons::LeftThumbstick)) mask |= Bit(Pad::kLeftStick);
            if (has(GamepadButtons::RightThumbstick)) mask |= Bit(Pad::kRightStick);
            if (has(GamepadButtons::LeftShoulder)) mask |= Bit(Pad::kLeftBumper);
            if (has(GamepadButtons::RightShoulder)) mask |= Bit(Pad::kRightBumper);
            if (has(GamepadButtons::DPadUp)) mask |= Bit(Pad::kDpadUp);
            if (has(GamepadButtons::DPadDown)) mask |= Bit(Pad::kDpadDown);
            if (has(GamepadButtons::DPadLeft)) mask |= Bit(Pad::kDpadLeft);
            if (has(GamepadButtons::DPadRight)) mask |= Bit(Pad::kDpadRight);
        } else if (raw_pad) {
            const std::uint32_t btn_count =
                static_cast<std::uint32_t>(std::clamp(raw_pad.ButtonCount(), 0, 32));
            const std::uint32_t sw_count =
                static_cast<std::uint32_t>(std::clamp(raw_pad.SwitchCount(), 0, 8));
            const std::uint32_t ax_count =
                static_cast<std::uint32_t>(std::clamp(raw_pad.AxisCount(), 0, 16));
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
            if (b(0)) mask |= Bit(Pad::kSouth);
            if (b(1)) mask |= Bit(Pad::kEast);
            if (b(2)) mask |= Bit(Pad::kWest);
            if (b(3)) mask |= Bit(Pad::kNorth);
            if (b(4)) mask |= Bit(Pad::kLeftBumper);
            if (b(5)) mask |= Bit(Pad::kRightBumper);
            if (b(6)) mask |= Bit(Pad::kView);
            if (b(7)) mask |= Bit(Pad::kMenu);
            if (b(8)) mask |= Bit(Pad::kLeftStick);
            if (b(9)) mask |= Bit(Pad::kRightStick);
            if (sw_count > 0) {
                switch (sws[0]) {
                case GameControllerSwitchPosition::Up: mask |= Bit(Pad::kDpadUp); break;
                case GameControllerSwitchPosition::UpRight:
                    mask |= Bit(Pad::kDpadUp) | Bit(Pad::kDpadRight);
                    break;
                case GameControllerSwitchPosition::Right: mask |= Bit(Pad::kDpadRight); break;
                case GameControllerSwitchPosition::DownRight:
                    mask |= Bit(Pad::kDpadDown) | Bit(Pad::kDpadRight);
                    break;
                case GameControllerSwitchPosition::Down: mask |= Bit(Pad::kDpadDown); break;
                case GameControllerSwitchPosition::DownLeft:
                    mask |= Bit(Pad::kDpadDown) | Bit(Pad::kDpadLeft);
                    break;
                case GameControllerSwitchPosition::Left: mask |= Bit(Pad::kDpadLeft); break;
                case GameControllerSwitchPosition::UpLeft:
                    mask |= Bit(Pad::kDpadUp) | Bit(Pad::kDpadLeft);
                    break;
                default: break;
                }
            }
        }
    } catch (...) {
    }
    return mask;
}

int ParseIntClamped(const std::string& s, int fallback, int lo, int hi) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long val = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || (end != nullptr && *end != '\0')) return fallback;
    if (val < static_cast<long>(lo)) return lo;
    if (val > static_cast<long>(hi)) return hi;
    return static_cast<int>(val);
}

int RenderWidthForMode(int mode) {
    if (mode == 1) return 1600;
    if (mode == 2) return 1280;
    if (mode >= 3) return 960;
    return 1920;
}

int RenderHeightForMode(int mode) {
    if (mode == 1) return 900;
    if (mode == 2) return 720;
    if (mode >= 3) return 540;
    return 1080;
}

const char* RenderResShortLabel(int mode) {
    if (mode == 1) return "900P";
    if (mode == 2) return "720P";
    if (mode >= 3) return "540P";
    return "1080P";
}

const char* ShadowResLabel(int tier) {
    if (tier <= 0) return "LOWEST";
    if (tier == 1) return "LOW";
    if (tier >= 3) return "HIGH";
    return "DEFAULT";
}

std::string LodBiasLabel(int bias) {
    if (bias <= -2) return "-2 (FINEST)";
    if (bias == -1) return "-1 (SHARP)";
    if (bias == 0) return "0 (DEFAULT)";
    if (bias == 1) return "+1 (FAST)";
    return "+2 (COARSEST)";
}

void SyncBridgeSettings() {
    bbx::bridge::ConsoleSettings cfg;
    cfg.fps_cap = g_session.model.fps_cap >= 60 ? 60 : 30;
    cfg.render_width = RenderWidthForMode(g_session.model.render_res_mode);
    cfg.render_height = RenderHeightForMode(g_session.model.render_res_mode);
    cfg.vsync = g_session.model.vsync;
    cfg.skip_logos = g_session.model.skip_logos;
    cfg.enhancements = g_session.model.enhancements;
    cfg.performance_mode = g_session.model.performance_mode;
    cfg.ssao = g_session.model.ssao;
    cfg.anti_alias = g_session.model.anti_alias;
    cfg.dynamic_shadows = g_session.model.dynamic_shadows;
    cfg.shadow_resolution = std::clamp(g_session.model.shadow_resolution, 0, 3);
    cfg.lod_bias = std::clamp(g_session.model.lod_bias, -2, 2);
    cfg.chromatic_aberration = g_session.model.chromatic_aberration;
    cfg.motion_blur = g_session.model.motion_blur;
    cfg.depth_of_field = g_session.model.depth_of_field;
    cfg.vignette = g_session.model.vignette;
    cfg.fov_scale = static_cast<float>(g_session.model.fov_percent) / 100.0f;
    cfg.camera_distance_scale = static_cast<float>(g_session.model.camera_distance_percent) / 100.0f;
    cfg.stick_deadzone = static_cast<float>(g_session.model.stick_deadzone_percent) / 100.0f;
    bbx::bridge::ApplyConsoleSettings(cfg);
}

void LoadHunterName() {
    std::string raw;
    if (!bbx::platform::ReadText(bbx::platform::Local("hunter-name.txt"), raw)) {
        g_session.model.hunter_name.clear();
        return;
    }
    while (!raw.empty() &&
           (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ' || raw.back() == '\t')) {
        raw.pop_back();
    }
    std::size_t first = 0;
    while (first < raw.size() && (raw[first] == ' ' || raw[first] == '\t')) ++first;
    std::string trimmed = raw.substr(first);
    if (trimmed.size() > 16u) trimmed.resize(16u);
    g_session.model.hunter_name = std::move(trimmed);
}

void SaveLayoutSettings() {
    const std::string path = bbx::platform::Local("bbxbox.ini");
    // IniSet returns false when a key's value is unchanged; never short-circuit
    // across keys with && or later toggled settings will be skipped.
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "path", g_session.layout.base_path);
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "token", g_session.usb_token);
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "patch_path", g_session.layout.patch_path);
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "patch_token", g_session.patch_token);
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "use_patch", g_session.layout.use_patch ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "mods", g_session.model.mods_enabled ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "dump", "modded_eboot",
                                g_session.model.modded_eboot_ok ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "video", "fps_cap",
                                g_session.model.fps_cap >= 60 ? "60" : "30");
    (void)bbx::platform::IniSet(g_session.ini_text, "video", "render_res_mode",
                                std::to_string(std::clamp(g_session.model.render_res_mode, 0, 3)));
    (void)bbx::platform::IniSet(g_session.ini_text, "video", "vsync", g_session.model.vsync ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "video", "sharp_upscale",
                                g_session.model.sharp_upscale ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "video", "perf_hud",
                                g_session.model.perf_hud ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "gameplay", "skip_logos",
                                g_session.model.skip_logos ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "gameplay", "enhancements",
                                g_session.model.enhancements ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "gameplay", "auto_backup_saves",
                                g_session.model.auto_backup_saves ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "performance_mode",
                                g_session.model.performance_mode ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "ssao",
                                g_session.model.ssao ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "anti_alias",
                                g_session.model.anti_alias ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "dynamic_shadows",
                                g_session.model.dynamic_shadows ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "shadow_resolution",
                                std::to_string(std::clamp(g_session.model.shadow_resolution, 0, 3)));
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "lod_bias",
                                std::to_string(std::clamp(g_session.model.lod_bias, -2, 2)));
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "chromatic_aberration",
                                g_session.model.chromatic_aberration ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "motion_blur",
                                g_session.model.motion_blur ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "depth_of_field",
                                g_session.model.depth_of_field ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "vignette",
                                g_session.model.vignette ? "1" : "0");
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "fov_percent",
                                std::to_string(g_session.model.fov_percent));
    (void)bbx::platform::IniSet(g_session.ini_text, "graphics", "camera_distance_percent",
                                std::to_string(g_session.model.camera_distance_percent));
    (void)bbx::platform::IniSet(g_session.ini_text, "controls", "stick_deadzone_percent",
                                std::to_string(g_session.model.stick_deadzone_percent));
    g_session.ini = bbx::platform::ParseIni(g_session.ini_text);
    if (!bbx::platform::WriteText(path, g_session.ini_text)) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not write %s", path.c_str());
    }
}

// `<dump>\mods`: the overlay the player drops loose files into. Asked of the
// dump's own root each time the launcher is rebuilt, through the host-fs seam,
// because the folder may be on a stick that was granted a minute ago.
void RefreshModsOverlay() {
    const std::string& root = g_session.layout.base_path;
    if (root.empty()) {
        g_session.model.mods_present = false;
        return;
    }
    // The detection is the same question the dump scanner asks ("is there a
    // directory at this path"), so it goes through the same seam - `DumpFs`
    // wraps `bbx::hostfs` on the console and the test harness's map in a test.
    const std::string mods = root + "\\mods";
    const bbx::platform::DumpFs fs = bbx::platform::MakeDumpFs();
    const bool present = fs.is_dir && fs.is_dir(mods);
    if (present != g_session.model.mods_present) {
        bbx::platform::LogF("mods overlay: %s at %s", present ? "present" : "absent", mods.c_str());
    }
    g_session.model.mods_present = present;
}

std::vector<std::string> CacheSearchDirs() {
    std::vector<std::string> dirs;
    if (!g_session.layout.base_path.empty()) {
        const std::string& eboot_dir = g_session.layout.CanRunPatched() ? g_session.layout.patch_path
                                                                        : g_session.layout.base_path;
        if (!eboot_dir.empty()) dirs.push_back(eboot_dir);
        if (g_session.layout.base_path != eboot_dir) dirs.push_back(g_session.layout.base_path);
    }
    dirs.push_back(bbx::platform::Local("data"));
    dirs.push_back(bbx::platform::Paths().local_state);
    return dirs;
}

void RefreshLauncher(bool probe_storage = false) {
    g_session.model.layout = g_session.layout;
    g_session.model.has_usb = !g_session.usb_token.empty() || !g_session.layout.base_path.empty();
    g_session.model.precompiling = g_session.baking;
    g_session.model.syncing = g_session.usb_sync_running;
    if (probe_storage) {
        RefreshModsOverlay();

        // Probe console DXIL 6.4 shader cache status whenever storage/layout
        // changed and the baker is idle so the launcher shows accurate stage
        // counts and offers Rebuild vs Precompile without stalling menu toggles.
        if (!g_session.baking) {
            std::size_t stages = 0;
            std::string stamp;
            if (bbx::platform::HasValidConsoleCache(CacheSearchDirs(), nullptr, nullptr, &stages, &stamp)) {
                g_session.model.cache_current = true;
                g_session.model.stages = stages;
                g_session.model.cache_stamp = stamp;
            } else {
                g_session.model.cache_current = false;
                g_session.model.stages = 0;
                g_session.model.cache_stamp.clear();
            }
        }
    }
    g_session.model.Rebuild();
}

void StartShaderBake(bool start_after, bool force_rebuild) {
    if (g_session.baking || g_session.usb_sync_running) return;
    if (!g_session.layout.layout_ok() || g_session.layout.base_path.empty()) {
        g_session.model.status_banner = "Select a valid Bloodborne dump folder before baking shaders.";
        RefreshLauncher();
        return;
    }

    const std::string data_dir = bbx::platform::Local("data");
    (void)bbx::hostfs::MkDir(data_dir, true);
    const std::string& eboot_dir = g_session.layout.CanRunPatched() ? g_session.layout.patch_path
                                                                    : g_session.layout.base_path;

    bbx::platform::BakeRequest bake_req;
    bake_req.app0 = g_session.layout.base_path;
    if (g_session.layout.CanRunPatched()) {
        bake_req.update_dir = g_session.layout.patch_path;
    }
    bake_req.eboot = eboot_dir + "\\eboot.bin";
    bake_req.out_dir = data_dir;
    bake_req.max_workers = 0;  // auto (up to 4 DXC worker threads)
    bake_req.force_rebuild = force_rebuild;

    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "launcher: starting native Xbox shader bake into %s (start_after=%u, force_rebuild=%u)",
        data_dir.c_str(), static_cast<unsigned>(start_after), static_cast<unsigned>(force_rebuild));

    g_session.start_after_bake = start_after;
    g_session.bake_progress.Reset();
    g_session.bake_result = bbx::platform::BakeResult{};
    g_session.bake_finished.store(false, std::memory_order_relaxed);
    g_session.baking = true;
    g_session.model.status_banner.clear();
    RefreshLauncher();

    g_session.bake_thread = std::thread([bake_req]() {
        const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        (void)pthread_setname_np(pthread_self(), "shader-baker");
        Steps().RegisterThread(tid, "shader-baker");
        g_session.bake_result = bbx::platform::BakeConsoleShaders(bake_req, g_session.bake_progress);
        Steps().UnregisterThread(tid);
        g_session.bake_finished.store(true, std::memory_order_release);
    });
}

enum class UsbTaskKind {
    kSyncCacheToUsb,
    kSyncCacheFromUsb,
    kBackupSavesToUsb,
    kRestoreSavesFromUsb,
};

void StartUsbTask(UsbTaskKind kind) {
    if (g_session.usb_sync_running || g_session.baking) return;
    if (g_session.layout.base_path.empty()) {
        g_session.model.status_banner = "Pick a USB dump folder first to enable USB sync/backup.";
        RefreshLauncher();
        return;
    }

    const std::string local_state = bbx::platform::Paths().local_state;
    const std::string data_dir = bbx::platform::Local("data");
    const std::string usb_root = g_session.layout.base_path;

    const char* label = "Working on USB...";
    switch (kind) {
    case UsbTaskKind::kSyncCacheToUsb: label = "Copying shader cache to USB..."; break;
    case UsbTaskKind::kSyncCacheFromUsb: label = "Importing shader cache from USB..."; break;
    case UsbTaskKind::kBackupSavesToUsb: label = "Backing up saves and logs to USB..."; break;
    case UsbTaskKind::kRestoreSavesFromUsb: label = "Restoring saves from USB backup..."; break;
    }

    g_session.usb_sync_running = true;
    g_session.usb_sync_finished.store(false, std::memory_order_relaxed);
    g_session.usb_sync_ok = false;
    g_session.usb_sync_message.clear();
    g_session.model.status_banner = label;
    RefreshLauncher();

    g_session.usb_sync_thread = std::thread([kind, local_state, data_dir, usb_root]() {
        const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        (void)pthread_setname_np(pthread_self(), "usb-sync");
        Steps().RegisterThread(tid, "usb-sync");
        std::string detail;
        bool ok = false;
        switch (kind) {
        case UsbTaskKind::kSyncCacheToUsb:
            ok = bbx::platform::SyncShaderCacheFiles(data_dir, usb_root, &detail);
            break;
        case UsbTaskKind::kSyncCacheFromUsb:
            ok = bbx::platform::SyncShaderCacheFiles(usb_root, data_dir, &detail);
            break;
        case UsbTaskKind::kBackupSavesToUsb:
            ok = bbx::platform::BackupSavesAndLogsToUsb(local_state, data_dir, usb_root, &detail);
            break;
        case UsbTaskKind::kRestoreSavesFromUsb:
            ok = bbx::platform::RestoreSavesFromUsb(usb_root, data_dir, &detail);
            break;
        }
        g_session.usb_sync_ok = ok;
        g_session.usb_sync_message = detail;
        Steps().UnregisterThread(tid);
        g_session.usb_sync_finished.store(true, std::memory_order_release);
    });
}

void PollUsbSync() {
    if (!g_session.usb_sync_running) return;
    if (!g_session.usb_sync_finished.load(std::memory_order_acquire)) return;
    if (g_session.usb_sync_thread.joinable()) g_session.usb_sync_thread.join();
    g_session.usb_sync_running = false;
    g_session.usb_sync_finished.store(false, std::memory_order_relaxed);
    g_session.model.status_banner = g_session.usb_sync_message;
    bbx::platform::LogMirroredMessage(
        g_session.usb_sync_ok ? bbx::platform::Level::kInfo : bbx::platform::Level::kWarn,
        "usb: %s", g_session.usb_sync_message.c_str());
    RefreshLauncher(/*probe_storage=*/true);
}

// The launcher loop is the thread that pumps the CoreWindow dispatcher, and the
// folder picker can only run on that dispatcher. Calling PickFolderGrant here
// blocked the pump thread on an event only the picker could signal, so picking a
// dump folder froze the launcher. The pick starts on a worker thread and the
// loop collects the result on a later frame.
std::atomic<int> g_pick_state{0};  // 0 idle, 1 pending, 2 granted, -1 failed
bool g_pick_for_patch = false;
std::string g_pick_path;
std::string g_pick_token;

void FinishDumpGrant(const std::string& path, const std::string& token, bool for_patch) {
    const bbx::platform::DumpFs fs = bbx::platform::MakeDumpFs();
    if (for_patch && !g_session.layout.base_path.empty()) {
        bbx::platform::LogF("patch folder granted: %s", path.c_str());
        if (!token.empty()) g_session.patch_token = token;
        g_session.layout = bbx::platform::DetectLayout(fs, g_session.layout.base_path, path, true, false);
    } else {
        bbx::platform::LogF("dump folder granted: %s", path.c_str());
        if (!token.empty()) g_session.usb_token = token;
        g_session.layout = bbx::platform::DetectLayout(
            fs, path, bbx::platform::IniGet(g_session.ini, "dump.patch_path", ""),
            bbx::platform::IniFlag(g_session.ini, "dump.use_patch", true), false);
    }
    bbx::platform::LogMessage(bbx::platform::Level::kInfo, "%s",
                              bbx::platform::LayoutLogLine(g_session.layout).c_str());
    if (!g_session.layout.layout_ok()) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "dump rejected: %s",
                                  g_session.layout.problem.c_str());
    }
    SaveLayoutSettings();
    RefreshLauncher(/*probe_storage=*/true);
}

void PickDump(bool for_patch) {
    if (g_pick_state.load() != 0) {
        bbx::platform::LogMessage(bbx::platform::Level::kInfo,
                                  "picker: already waiting for the system folder picker");
        return;
    }
    g_pick_for_patch = for_patch;
    g_pick_state.store(1);
    std::thread([] {
        std::string path;
        std::string token;
        if (bbx::platform::PickFolderGrant(path, token) && !path.empty()) {
            g_pick_path = path;
            g_pick_token = token;
            g_pick_state.store(2);
        } else {
            g_pick_state.store(-1);
        }
    }).detach();
    bbx::platform::LogMessage(bbx::platform::Level::kInfo,
                              "picker: the system folder picker is open; the launcher keeps drawing");
}

void PollDumpPick() {
    const int state = g_pick_state.load();
    if (state == 2) {
        const std::string path = g_pick_path;
        const std::string token = g_pick_token;
        const bool for_patch = g_pick_for_patch;
        g_pick_state.store(0);
        FinishDumpGrant(path, token, for_patch);
    } else if (state == -1) {
        g_pick_state.store(0);
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "dump picker cancelled");
    }
}

int LauncherTabForAction(bbx::platform::Action action) {
    using bbx::platform::Action;
    switch (action) {
    case Action::kStartGame:
    case Action::kContinueUnpatched:
    case Action::kPrecompile:
    case Action::kPickDump:
    case Action::kPickPatch:
    case Action::kTogglePatch:
    case Action::kToggleModdedEboot:
    case Action::kQuit:
        return 0;  // PLAY
    case Action::kToggleFpsCap:
    case Action::kCycleResolution:
    case Action::kToggleVsync:
    case Action::kToggleSharpUpscale:
    case Action::kTogglePerformanceMode:
    case Action::kCycleShadowRes:
    case Action::kToggleSkipLogos:
    case Action::kToggleEnhancements:
    case Action::kCycleDeadzone:
    case Action::kTogglePerfHud:
        return 1;  // SETTINGS
    default:
        return 2;  // STORAGE
    }
}

void MoveInTab(int delta) {
    const std::size_t count = g_session.model.items.size();
    if (count == 0) return;
    std::vector<std::size_t> tab_indices;
    for (std::size_t i = 0; i < count; ++i) {
        if (LauncherTabForAction(g_session.model.items[i].action) == g_session.launcher_tab &&
            g_session.model.items[i].enabled) {
            tab_indices.push_back(i);
        }
    }
    if (tab_indices.empty()) {
        g_session.model.Move(delta);
        return;
    }
    std::size_t pos = 0;
    for (std::size_t j = 0; j < tab_indices.size(); ++j) {
        if (tab_indices[j] == g_session.model.selected) {
            pos = j;
            break;
        }
    }
    const std::size_t n = tab_indices.size();
    const std::size_t next = delta >= 0 ? (pos + 1) % n : (pos + n - 1) % n;
    g_session.model.selected = tab_indices[next];
}

void SwitchTab(int delta) {
    g_session.launcher_tab = (g_session.launcher_tab + delta + 3) % 3;
    const std::size_t count = g_session.model.items.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (LauncherTabForAction(g_session.model.items[i].action) == g_session.launcher_tab &&
            g_session.model.items[i].enabled) {
            g_session.model.selected = i;
            return;
        }
    }
}

void OnAction(PadAction action) {
    if (g_session.keyboard_open) {
        switch (action) {
        case PadAction::kNavigateUp: g_session.keyboard.Move(0, -1); return;
        case PadAction::kNavigateDown: g_session.keyboard.Move(0, 1); return;
        case PadAction::kNavigateLeft: g_session.keyboard.Move(-1, 0); return;
        case PadAction::kNavigateRight: g_session.keyboard.Move(1, 0); return;
        case PadAction::kConfirm:
            if (!g_session.keyboard.Accept()) {
                std::string name = g_session.keyboard.Text();
                if (name.size() > 16u) name.resize(16u);
                g_session.model.hunter_name = name;
                bbx::platform::LogF("hunter name: \"%s\"", name.c_str());
                if (!bbx::platform::WriteText(bbx::platform::Local("hunter-name.txt"), name)) {
                    bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not write hunter-name.txt");
                }
                g_session.keyboard_open = false;
                RefreshLauncher();
            }
            return;
        case PadAction::kCancel:
            g_session.keyboard.Backspace();
            return;
        default: return;
        }
    }

    // No rumble call here: the pad belongs to WinRT on this target and a launcher
    // menu does not need haptics badly enough to initialise a second input stack.
    switch (action) {
    case PadAction::kNavigateUp:
        MoveInTab(-1);
        return;
    case PadAction::kNavigateDown:
        MoveInTab(1);
        return;
    case PadAction::kNavigateLeft:
        SwitchTab(-1);
        return;
    case PadAction::kNavigateRight:
        SwitchTab(1);
        return;
    case PadAction::kMenu:
        g_session.keyboard_open = true;
        g_session.keyboard.Reset(g_session.model.hunter_name);
        return;
    case PadAction::kOpenPcSettings:
        bbx::platform::LogF("PC settings menu requested (L3+R3)");
        g_session.model.status_banner =
            "In-game quick menu (L3+R3) opens once Bloodborne starts; configure settings here.";
        RefreshLauncher();
        return;
    case PadAction::kPersonalEffects:
        bbx::platform::LogF("personal effects requested (touchpad)");
        return;
    case PadAction::kGestures:
        bbx::platform::LogF("gestures requested (touchpad + left)");
        return;
    case PadAction::kConfirm: {
        const bbx::platform::Action selected = g_session.model.Activate();
        if (selected != bbx::platform::Action::kNone) {
            Steps().SelectionAccepted(bbx::platform::ActionName(selected));
        }
        bbx::platform::LogF("launcher: %s", bbx::platform::ActionName(selected));
        switch (selected) {
        case bbx::platform::Action::kPickDump: PickDump(false); return;
        case bbx::platform::Action::kPickPatch: PickDump(true); return;
        case bbx::platform::Action::kTogglePatch:
            g_session.layout.use_patch = !g_session.layout.use_patch;
            g_session.layout.effective_ver =
                g_session.layout.CanRunPatched() ? g_session.layout.patch.Version()
                                                 : g_session.layout.base.Version();
            SaveLayoutSettings();
            RefreshLauncher(/*probe_storage=*/true);
            return;
        case bbx::platform::Action::kToggleMods:
            g_session.model.mods_enabled = !g_session.model.mods_enabled;
            bbx::platform::LogF("mods overlay %s", g_session.model.mods_enabled ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleModdedEboot:
            g_session.model.modded_eboot_ok = !g_session.model.modded_eboot_ok;
            bbx::platform::LogF("modded eboot %s", g_session.model.modded_eboot_ok ? "allowed" : "refused");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleFpsCap:
            g_session.model.fps_cap = (g_session.model.fps_cap >= 60) ? 30 : 60;
            SyncBridgeSettings();
            bbx::platform::LogF("video: fps_cap=%d", g_session.model.fps_cap);
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kCycleResolution:
            g_session.model.render_res_mode = (g_session.model.render_res_mode + 1) % 4;
            SyncBridgeSettings();
            bbx::platform::LogF("video: render_res=%dx%d",
                                RenderWidthForMode(g_session.model.render_res_mode),
                                RenderHeightForMode(g_session.model.render_res_mode));
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleVsync:
            g_session.model.vsync = !g_session.model.vsync;
            SyncBridgeSettings();
            bbx::platform::LogF("video: vsync=%s", g_session.model.vsync ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleSharpUpscale:
            g_session.model.sharp_upscale = !g_session.model.sharp_upscale;
            bbx::platform::LogF("video: sharp_upscale=%s", g_session.model.sharp_upscale ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kTogglePerformanceMode:
            g_session.model.performance_mode = !g_session.model.performance_mode;
            SyncBridgeSettings();
            bbx::platform::LogF("graphics: performance_mode=%s",
                                g_session.model.performance_mode ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kCycleShadowRes:
            g_session.model.shadow_resolution = (g_session.model.shadow_resolution + 1) % 4;
            SyncBridgeSettings();
            bbx::platform::LogF("graphics: shadow_resolution=%s",
                                ShadowResLabel(g_session.model.shadow_resolution));
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kTogglePerfHud:
            g_session.model.perf_hud = !g_session.model.perf_hud;
            bbx::platform::LogF("video: perf_hud=%s", g_session.model.perf_hud ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleSkipLogos:
            g_session.model.skip_logos = !g_session.model.skip_logos;
            SyncBridgeSettings();
            bbx::platform::LogF("gameplay: skip_logos=%s", g_session.model.skip_logos ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kToggleEnhancements:
            g_session.model.enhancements = !g_session.model.enhancements;
            SyncBridgeSettings();
            bbx::platform::LogF("gameplay: enhancements=%s",
                                g_session.model.enhancements ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kCycleDeadzone: {
            const int next = ((g_session.model.stick_deadzone_percent / 5) + 1) % 5;
            g_session.model.stick_deadzone_percent = next * 5;
            SyncBridgeSettings();
            bbx::platform::LogF("controls: stick_deadzone_percent=%d",
                                g_session.model.stick_deadzone_percent);
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        }
        case bbx::platform::Action::kToggleAutoBackup:
            g_session.model.auto_backup_saves = !g_session.model.auto_backup_saves;
            bbx::platform::LogF("gameplay: auto_backup_saves=%s",
                                g_session.model.auto_backup_saves ? "on" : "off");
            SaveLayoutSettings();
            RefreshLauncher();
            return;
        case bbx::platform::Action::kPrecompile:
            StartShaderBake(/*start_after=*/false, /*force_rebuild=*/g_session.model.cache_current);
            return;
        case bbx::platform::Action::kSyncCacheToUsb:
            StartUsbTask(UsbTaskKind::kSyncCacheToUsb);
            return;
        case bbx::platform::Action::kSyncCacheFromUsb:
            StartUsbTask(UsbTaskKind::kSyncCacheFromUsb);
            return;
        case bbx::platform::Action::kBackupSavesToUsb:
            StartUsbTask(UsbTaskKind::kBackupSavesToUsb);
            return;
        case bbx::platform::Action::kRestoreSavesFromUsb:
            StartUsbTask(UsbTaskKind::kRestoreSavesFromUsb);
            return;
        case bbx::platform::Action::kMemorySelfTest: {
            const bbx::platform::SelfTestResult result =
                bbx::platform::RunMemorySelfTest(false);
            bbx::platform::LogF("self-test %s", result.Passed() ? "PASS" : "FAIL");
            g_session.model.status_banner =
                result.Passed() ? "Memory self-test PASSED." : "Memory self-test FAILED (see loader.log).";
            RefreshLauncher();
            return;
        }
        case bbx::platform::Action::kStartGame:
            g_session.start_force_unverified = false;
            g_session.start_requested = true;
            return;
        case bbx::platform::Action::kContinueUnpatched:
            g_session.start_force_unverified = true;
            g_session.start_requested = true;
            return;
        case bbx::platform::Action::kQuit: g_session.quit = true; return;
        default: return;
        }
    }
    case PadAction::kCancel:
        g_session.model.Move(-1);
        return;
    default: return;
    }
}

std::vector<std::string> WrapLines(const std::string& text, std::size_t max_chars) {
    std::vector<std::string> lines;
    std::string current;
    auto flush_wrapped = [&](const std::string& line) {
        if (max_chars == 0 || line.size() <= max_chars) {
            lines.push_back(line);
            return;
        }
        std::size_t pos = 0;
        while (pos < line.size()) {
            const std::size_t remaining = line.size() - pos;
            if (remaining <= max_chars) {
                lines.push_back(line.substr(pos));
                break;
            }
            std::size_t split = line.rfind(' ', pos + max_chars);
            if (split == std::string::npos || split <= pos) {
                split = pos + max_chars;
                lines.push_back(line.substr(pos, split - pos));
                pos = split;
            } else {
                lines.push_back(line.substr(pos, split - pos));
                pos = split + 1;
            }
        }
    };
    for (const char c : text) {
        if (c == '\n') {
            flush_wrapped(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    flush_wrapped(current);
    return lines;
}

std::string TruncateLabel(const std::string& s, std::size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars <= 3) return s.substr(0, max_chars);
    return s.substr(0, max_chars - 3) + "...";
}

void AppendLauncherToHud(bbx::platform::Hud& hud) {
    if (bbx::bridge::Flips() != 0) return;
    const float width = hud.Width();
    const float height = hud.Height();
    const std::uint32_t scale = width >= 3000.0f ? 5u : (height >= 950.0f ? 3u : 2u);
    const std::uint32_t small_scale = scale > 2u ? (scale - 1u) : scale;
    const float px = static_cast<float>(scale);
    const float pad_x = px * 11.0f;
    const float pad_y = px * 8.0f;
    const float row_h = bbx::platform::Hud::TextHeight(scale) + px * 3.6f;

    // Collect items for the active tab (PLAY / SETTINGS / STORAGE).
    std::vector<std::size_t> tab_items;
    if (!g_session.baking && !g_session.keyboard_open) {
        if (const bbx::platform::Item* cur = g_session.model.Current()) {
            const int cur_tab = LauncherTabForAction(cur->action);
            if (cur_tab != g_session.launcher_tab) {
                for (std::size_t i = 0; i < g_session.model.items.size(); ++i) {
                    if (LauncherTabForAction(g_session.model.items[i].action) == g_session.launcher_tab &&
                        g_session.model.items[i].enabled) {
                        g_session.model.selected = i;
                        break;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < g_session.model.items.size(); ++i) {
            if (LauncherTabForAction(g_session.model.items[i].action) == g_session.launcher_tab) {
                tab_items.push_back(i);
            }
        }
    }

    const std::size_t body_rows =
        g_session.baking
            ? 7u
            : (g_session.keyboard_open ? (1u + g_session.keyboard.RowCount()) : std::max<std::size_t>(1u, tab_items.size()));

    const float panel_w = std::min(width * 0.72f, px * 420.0f);
    const float inner_w = panel_w - pad_x * 2.0f;
    const std::size_t max_chars = std::max<std::size_t>(
        32u, static_cast<std::size_t>(inner_w / static_cast<float>(bbx::platform::HudFont::kColumns * small_scale)));

    const bool has_status = !g_session.model.status_banner.empty();
    std::vector<std::string> hint_lines;
    if (!g_session.baking && !g_session.keyboard_open) {
        if (const bbx::platform::Item* cur = g_session.model.Current(); cur && !cur->hint.empty()) {
            hint_lines = WrapLines(cur->hint, max_chars);
            if (hint_lines.size() > 2u) hint_lines.resize(2u);
        }
    }
    const std::size_t footer_rows = (has_status ? 1u : 0u) + hint_lines.size() + 1u;

    const float header_h = row_h * 1.45f;
    const float tab_h = (!g_session.baking && !g_session.keyboard_open) ? (row_h * 1.25f) : 0.0f;
    const float body_h = row_h * static_cast<float>(body_rows);
    const float footer_h = (bbx::platform::Hud::TextHeight(small_scale) + px * 2.2f) * static_cast<float>(footer_rows);
    const float panel_h = std::min(height - px * 16.0f, pad_y * 2.0f + header_h + tab_h + body_h + footer_h + px * 6.0f);

    const float panel_x = (width - panel_w) * 0.5f;
    const float panel_y = (height - panel_h) * 0.5f;

    // Apple-style deep obsidian glass card with Bloodborne hairline gold border & crimson top accent.
    const bbx::platform::HudColor card_bg{0.055f, 0.048f, 0.060f, 0.94f};
    const bbx::platform::HudColor hairline{0.62f, 0.48f, 0.24f, 0.45f};
    const bbx::platform::HudColor divider{0.62f, 0.48f, 0.24f, 0.22f};
    const bbx::platform::HudColor crimson_bar{0.58f, 0.10f, 0.16f, 0.95f};
    const float border_t = std::max(1.5f, px * 0.5f);

    hud.Panel(panel_x, panel_y, panel_w, panel_h, card_bg);
    hud.Panel(panel_x, panel_y, panel_w, border_t, hairline);
    hud.Panel(panel_x, panel_y + panel_h - border_t, panel_w, border_t, hairline);
    hud.Panel(panel_x, panel_y, border_t, panel_h, hairline);
    hud.Panel(panel_x + panel_w - border_t, panel_y, border_t, panel_h, hairline);
    hud.Panel(panel_x, panel_y, panel_w, px * 1.1f, crimson_bar);

    const float x = panel_x + pad_x;
    float y = panel_y + pad_y;

    // Clean Header: Hunter's Mark + Title on left, compact version/cache pill on right.
    const float rune_cx = x + px * 3.5f;
    const float rune_cy = y + bbx::platform::Hud::TextHeight(scale) * 0.55f;
    hud.HuntersMark(rune_cx, rune_cy, scale, bbx::platform::HudColor::TarnishedGold(),
                    bbx::platform::HudColor::BloodCrimson());
    hud.Text(x + px * 10.5f, y, scale, "BLOODBORNE", bbx::platform::HudColor::BoneIvory());

    std::string badge = "NO DUMP";
    bbx::platform::HudColor badge_col = bbx::platform::HudColor::AshSilver();
    if (!g_session.layout.base_path.empty()) {
        const std::string ver = g_session.layout.EffectiveVersion();
        badge = ver.empty() ? "DUMP READY" : ("V" + ver);
        if (g_session.model.pipelines > 0) {
            badge += "  |  " + std::to_string(g_session.model.pipelines) + " SHADERS";
        }
        badge_col = bbx::platform::HudColor::TarnishedGold();
    }
    const float badge_w = bbx::platform::Hud::TextWidth(badge, small_scale);
    hud.Text(x + inner_w - badge_w, y + px * 0.6f, small_scale, badge, badge_col);
    y += row_h * 1.15f;

    // Apple-style Segmented Control for tabs: PLAY | SETTINGS | STORAGE
    if (!g_session.baking && !g_session.keyboard_open) {
        static constexpr const char* kTabs[3] = {"PLAY", "SETTINGS", "STORAGE"};
        const float seg_h = bbx::platform::Hud::TextHeight(small_scale) + px * 2.6f;
        const float seg_w = inner_w / 3.0f;
        hud.Panel(x, y, inner_w, seg_h, bbx::platform::HudColor{0.032f, 0.028f, 0.036f, 0.96f});
        for (int t = 0; t < 3; ++t) {
            const float sx = x + seg_w * static_cast<float>(t);
            const bool active = (t == g_session.launcher_tab);
            if (active) {
                hud.Panel(sx + px * 0.6f, y + px * 0.6f, seg_w - px * 1.2f, seg_h - px * 1.2f,
                          bbx::platform::HudColor{0.46f, 0.09f, 0.14f, 0.90f});
                hud.Panel(sx + px * 0.6f, y + seg_h - px * 1.1f, seg_w - px * 1.2f, px * 0.7f,
                          bbx::platform::HudColor::TarnishedGold());
            }
            const float tw = bbx::platform::Hud::TextWidth(kTabs[t], small_scale);
            const float ty = y + (seg_h - bbx::platform::Hud::TextHeight(small_scale)) * 0.5f;
            hud.Text(sx + (seg_w - tw) * 0.5f, ty, small_scale, kTabs[t],
                     active ? bbx::platform::HudColor::BoneIvory() : bbx::platform::HudColor::AshSilver());
        }
        y += seg_h + px * 3.5f;
    } else {
        hud.Panel(x, y, inner_w, 1.0f, divider);
        y += px * 3.5f;
    }

    if (g_session.baking) {
        const bbx::d3d12::BakeProgressSnapshot snap = g_session.bake_progress.Snapshot();
        const std::string headline = bbx::d3d12::FormatBakeHudLine(snap);
        hud.Text(x, y, scale, headline, bbx::platform::HudColor::TarnishedGold());
        y += row_h * 1.1f;

        const float bar_w = inner_w;
        const float bar_h = px * 4.5f;
        hud.Panel(x, y, bar_w, bar_h, bbx::platform::HudColor{0.08f, 0.05f, 0.07f, 0.96f});
        float frac = 0.0f;
        if (snap.total > 0) {
            frac = std::min(1.0f, static_cast<float>(snap.done) / static_cast<float>(snap.total));
        } else if (snap.phase == bbx::d3d12::BakePhase::kDone) {
            frac = 1.0f;
        }
        if (frac > 0.0f) {
            const float fill_w = bar_w * frac;
            hud.Panel(x, y, fill_w, bar_h, bbx::platform::HudColor::BloodCrimson());
            const float tip_w = std::min(fill_w, px * 2.5f);
            hud.Panel(x + fill_w - tip_w, y, tip_w, bar_h, bbx::platform::HudColor::TarnishedGold());
        }
        y += bar_h + px * 3.5f;

        char stats[160];
        std::snprintf(stats, sizeof(stats),
                      "WORKERS: %u  |  RESUMED: %u  |  REFUSED: %u  |  QUARANTINED: %u",
                      static_cast<unsigned>(snap.workers),
                      static_cast<unsigned>(snap.resumed_count),
                      static_cast<unsigned>(snap.refused_count),
                      static_cast<unsigned>(snap.quarantined_count));
        hud.Text(x, y, small_scale, stats, bbx::platform::HudColor::BoneIvory());
        y += row_h;

        if (!snap.current_name.empty()) {
            hud.Text(x, y, small_scale, "SHADER: " + TruncateLabel(snap.current_name, 54),
                     bbx::platform::HudColor::AshSilver());
        } else {
            hud.Text(x, y, small_scale, "COMPILING PS4 GCN SHADERS TO DXIL 6.4...",
                     bbx::platform::HudColor::AshSilver());
        }
        y += row_h;

        const bool cancelling = g_session.bake_progress.CancelRequested();
        hud.Text(x, y, small_scale,
                 cancelling ? "SAVING CHECKPOINT..." : "PRESS B TO PAUSE (CHECKPOINT SAVED)",
                 bbx::platform::HudColor::TarnishedGold());
        y += row_h;
    } else if (g_session.keyboard_open) {
        hud.Text(x, y, scale, "HUNTER NAME: " + g_session.keyboard.Text(),
                 bbx::platform::HudColor::TarnishedGold());
        y += row_h;
        for (std::size_t row = 0; row < g_session.keyboard.RowCount(); ++row) {
            std::string label = g_session.keyboard.Row(row);
            if (row == g_session.keyboard.CursorRow()) {
                const std::size_t column = g_session.keyboard.CursorColumn() * 2;
                if (column < label.size()) label[column] = '>';
            }
            hud.Text(x + px * 3.0f, y, scale, label, bbx::platform::HudColor::BoneIvory());
            y += row_h;
        }
    } else {
        for (const std::size_t idx : tab_items) {
            const bbx::platform::Item& item = g_session.model.items[idx];
            const bool selected = (idx == g_session.model.selected);
            const float text_y = y + (row_h - bbx::platform::Hud::TextHeight(scale)) * 0.5f;

            if (selected) {
                hud.Panel(x, y + px * 0.4f, inner_w, row_h - px * 0.8f,
                          bbx::platform::HudColor{0.38f, 0.08f, 0.12f, 0.55f});
                hud.Panel(x, y + px * 0.4f, px * 1.2f, row_h - px * 0.8f,
                          bbx::platform::HudColor::TarnishedGold());
            }

            bbx::platform::HudColor label_col = bbx::platform::HudColor::AshSilver();
            if (item.enabled) {
                if (item.dangerous) {
                    label_col = bbx::platform::HudColor::PalebloodEmber();
                } else if (selected) {
                    label_col = bbx::platform::HudColor::TarnishedGold();
                } else {
                    label_col = bbx::platform::HudColor::BoneIvory();
                }
            }

            const std::size_t colon = item.label.find(": ");
            const float rx = x + px * 3.5f;
            if (colon != std::string::npos) {
                const std::string left = item.label.substr(0, colon);
                const std::string right = item.label.substr(colon + 2);
                bbx::platform::HudColor val_col = bbx::platform::HudColor::AshSilver();
                if (right == "off" || right == "OFF") {
                    val_col = bbx::platform::HudColor::AshSilver();
                } else if (item.dangerous) {
                    val_col = bbx::platform::HudColor::PalebloodEmber();
                } else {
                    val_col = bbx::platform::HudColor::TarnishedGold();
                }
                hud.Text(rx, text_y, scale, left, label_col);
                const float vw = bbx::platform::Hud::TextWidth(right, scale);
                hud.Text(x + inner_w - vw - px * 2.5f, text_y, scale, right, val_col);
            } else {
                hud.Text(rx, text_y, scale, item.label, label_col);
            }
            y += row_h;
        }
    }

    // Minimal Apple-style bottom footer: optional status pill, 1-2 line hint, and control legend.
    y += px * 2.0f;
    hud.Panel(x, y, inner_w, 1.0f, divider);
    y += px * 2.5f;
    const float small_line = bbx::platform::Hud::TextHeight(small_scale) + px * 1.8f;

    if (has_status) {
        const std::vector<std::string> st = WrapLines(g_session.model.status_banner, max_chars);
        hud.Text(x, y, small_scale, st[0], bbx::platform::HudColor::PalebloodEmber());
        y += small_line;
    }
    for (const std::string& hl : hint_lines) {
        hud.Text(x, y, small_scale, hl, bbx::platform::HudColor::AshSilver());
        y += small_line;
    }
    hud.Text(x, y, small_scale,
             g_session.keyboard_open
                 ? "A: TYPE   B: BACKSPACE   MENU: SAVE   VIEW: CANCEL"
                 : "A: SELECT   LEFT/RIGHT: TAB   MENU: HUNTER NAME",
             bbx::platform::HudColor::TarnishedGold());
}

void AppendInGameOverlays(bbx::platform::Hud& hud) {
    if (!bbx::bridge::Running() && bbx::bridge::Flips() == 0) return;

    const float width = hud.Width();
    const float height = hud.Height();
    const std::uint32_t scale = width >= 3000.0f ? 5u : 3u;
    const std::uint32_t small_scale = width >= 3000.0f ? 4u : 2u;
    const float margin = static_cast<float>(scale) * 6.0f;

    // 1. Compact corner Performance HUD (toggleable via launcher or L3+R3 Quick Menu).
    if (g_session.model.perf_hud) {
        const bbx::bridge::PerfSnapshot perf = bbx::bridge::SnapshotPerf();
        const bbx::platform::TelemetrySample mem = QueryTelemetrySample();
        const float line =
            bbx::platform::Hud::TextHeight(small_scale) + static_cast<float>(small_scale) * 2.5f;
        char line1[160];
        std::snprintf(line1, sizeof(line1), "FPS %4.1f (%4.1f MS) | %s | CAP %d | VSYNC %s | RCAS %s",
                      static_cast<double>(perf.fps), static_cast<double>(perf.frame_ms),
                      RenderResShortLabel(g_session.model.render_res_mode),
                      g_session.model.fps_cap,
                      bbx::bridge::VsyncEnabled() ? "ON" : "OFF",
                      g_session.model.sharp_upscale ? "ON" : "OFF");
        char line2[128];
        std::snprintf(line2, sizeof(line2), "RAM %llu/%llu MIB | FLIPS %llu | L3+R3 DREAM MENU",
                      static_cast<unsigned long long>(mem.used_mib),
                      static_cast<unsigned long long>(mem.limit_mib),
                      static_cast<unsigned long long>(perf.flips));
        const float box_w =
            std::max(bbx::platform::Hud::TextWidth(line1, small_scale),
                     bbx::platform::Hud::TextWidth(line2, small_scale)) +
            margin;
        const float box_h = line * 2.0f + static_cast<float>(small_scale) * 5.0f;
        hud.GothicPanel(margin * 0.5f, margin * 0.5f, box_w, box_h);
        const float tx = margin * 0.5f + static_cast<float>(small_scale) * 3.2f;
        const float ty = margin * 0.5f + static_cast<float>(small_scale) * 2.6f;
        hud.Text(tx, ty, small_scale, line1, bbx::platform::HudColor::TarnishedGold());
        hud.Text(tx, ty + line, small_scale, line2, bbx::platform::HudColor::BoneIvory());
    }

    // 2. Toast banner from host_message_show / host_text_show.
    const std::string toast = bbx::bridge::ActiveToastMessage();
    if (!toast.empty()) {
        const std::string clipped = TruncateLabel(toast, 64);
        const float tw = bbx::platform::Hud::TextWidth(clipped, scale);
        const float th = bbx::platform::Hud::TextHeight(scale);
        const float pad = static_cast<float>(scale) * 4.0f;
        const float bx = (width - (tw + pad * 2.0f)) * 0.5f;
        const float by = margin;
        hud.GothicPanel(bx, by, tw + pad * 2.0f, th + pad * 2.0f);
        hud.Text(bx + pad, by + pad, scale, clipped, bbx::platform::HudColor::TarnishedGold());
    }

    // 3. In-game Virtual Keyboard IME overlay (when guest calls host_text_entry_begin).
    if (g_session.ime_open) {
        const float line = bbx::platform::Hud::TextHeight(scale) + static_cast<float>(scale) * 3.0f;
        const float panel_w = width * 0.72f;
        const float panel_h = line * static_cast<float>(5 + g_session.ime_keyboard.RowCount()) + margin;
        const float px = (width - panel_w) * 0.5f;
        const float py = (height - panel_h) * 0.5f;
        hud.GothicPanel(px, py, panel_w, panel_h);

        float y = py + margin * 0.65f;
        const float tx = px + margin * 0.9f;
        const float inner_w = panel_w - margin * 1.8f;
        hud.Text(tx, y, scale, "HUNTER'S CONTRACT  |  " + TruncateLabel(g_session.ime_label, 38),
                 bbx::platform::HudColor::TarnishedGold());
        y += line * 1.0f;
        hud.GothicDivider(tx, y, inner_w, scale);
        y += line * 0.45f;

        std::string shown = g_session.ime_keyboard.Text();
        if (g_session.ime_hidden) shown.assign(shown.size(), '*');
        hud.Text(tx, y, scale, "INSCRIPTION: " + shown + "_",
                 bbx::platform::HudColor::BoneIvory());
        y += line * 1.2f;

        for (std::size_t row = 0; row < g_session.ime_keyboard.RowCount(); ++row) {
            std::string label = g_session.ime_keyboard.Row(row);
            if (row == g_session.ime_keyboard.CursorRow()) {
                const std::size_t column = g_session.ime_keyboard.CursorColumn() * 2;
                if (column < label.size()) label[column] = '>';
            }
            hud.Text(tx + margin * 0.4f, y, scale, label, bbx::platform::HudColor::BoneIvory());
            y += line;
        }
        y += line * 0.3f;
        hud.Text(tx, y, small_scale, "A: INSCRIBE  B: ERASE  MENU: SEAL CONTRACT  VIEW: CANCEL",
                 bbx::platform::HudColor::AshSilver());
        return;
    }

    // 4. In-game L3+R3 Hunter's Dream Quick Menu overlay (live engine/graphics/camera/controls).
    if (g_session.quick_menu_open) {
        const std::uint32_t menu_scale = width >= 3000.0f ? 5u : (height >= 950.0f ? 3u : 2u);
        const std::uint32_t hint_scale = menu_scale > 2u ? (menu_scale - 1u) : menu_scale;
        const float menu_margin = static_cast<float>(menu_scale) * 6.0f;
        const float line =
            bbx::platform::Hud::TextHeight(menu_scale) + static_cast<float>(menu_scale) * 2.6f;
        const std::vector<std::string> rows = {
            "RETURN TO THE HUNT (RESUME)",
            std::string("FRAME RATE CAP: ") +
                (g_session.model.fps_cap >= 60 ? "60 FPS++" : "30 FPS"),
            std::string("RENDER RESOLUTION: ") +
                RenderResShortLabel(g_session.model.render_res_mode),
            std::string("VSYNC: ") +
                (bbx::bridge::VsyncEnabled() ? "ON" : "OFF (VRR / FREESYNC)"),
            std::string("SHARP UPSCALE (FSR RCAS): ") +
                (g_session.model.sharp_upscale ? "ON" : "OFF"),
            std::string("PERFORMANCE MODE (YEBIS OFF): ") +
                (g_session.model.performance_mode ? "ON" : "OFF"),
            std::string("SHADOW RESOLUTION: ") +
                ShadowResLabel(g_session.model.shadow_resolution),
            std::string("DYNAMIC SHADOWS: ") +
                (g_session.model.dynamic_shadows ? "ON" : "OFF"),
            std::string("AMBIENT OCCLUSION (SSAO): ") +
                (g_session.model.ssao ? "ON" : "OFF"),
            std::string("ANTI-ALIASING: ") +
                (g_session.model.anti_alias ? "ON" : "OFF"),
            "MODEL LOD BIAS: " + LodBiasLabel(g_session.model.lod_bias),
            std::string("CHROMATIC ABERRATION: ") +
                (g_session.model.chromatic_aberration ? "ON" : "OFF"),
            std::string("MOTION BLUR: ") + (g_session.model.motion_blur ? "ON" : "OFF"),
            std::string("DEPTH OF FIELD: ") + (g_session.model.depth_of_field ? "ON" : "OFF"),
            std::string("VIGNETTE: ") + (g_session.model.vignette ? "ON" : "OFF"),
            "CAMERA FOV: " + std::to_string(g_session.model.fov_percent) + "%",
            "CAMERA DISTANCE: " + std::to_string(g_session.model.camera_distance_percent) + "%",
            "STICK DEADZONE: " + std::to_string(g_session.model.stick_deadzone_percent) + "%",
            std::string("PERFORMANCE HUD: ") + (g_session.model.perf_hud ? "ON" : "OFF"),
            g_session.model.auto_backup_saves ? "AWAKEN FROM THE DREAM (EXIT & SYNC SAVES)"
                                              : "AWAKEN FROM THE DREAM (EXIT GAME)",
        };
        const std::size_t total_rows = rows.size();
        const std::size_t max_visible_rows = 12u;
        const bool scrolling = total_rows > max_visible_rows;
        const std::size_t visible_rows = scrolling ? max_visible_rows : total_rows;
        std::size_t first_row = 0;
        if (scrolling) {
            const std::size_t half = visible_rows / 2u;
            if (g_session.quick_menu_selected > half) {
                first_row = g_session.quick_menu_selected - half;
            }
            if (first_row + visible_rows > total_rows) {
                first_row = total_rows - visible_rows;
            }
        }
        const std::size_t end_row = std::min(total_rows, first_row + visible_rows);

        const float panel_w = width * 0.72f;
        const float panel_h =
            line * static_cast<float>(visible_rows + (scrolling ? 5u : 4u)) + menu_margin;
        const float px = (width - panel_w) * 0.5f;
        const float py = (height - panel_h) * 0.5f;
        hud.GothicPanel(px, py, panel_w, panel_h);

        float y = py + menu_margin * 0.65f;
        const float tx = px + menu_margin * 0.9f;
        const float inner_w = panel_w - menu_margin * 1.8f;
        hud.HuntersMark(tx + static_cast<float>(menu_scale) * 3.5f, y + line * 0.35f, menu_scale,
                        bbx::platform::HudColor::TarnishedGold(),
                        bbx::platform::HudColor::BloodCrimson());
        hud.Text(tx + static_cast<float>(menu_scale) * 10.0f, y, menu_scale,
                 "HUNTER'S DREAM  |  MESSENGER QUICK MENU",
                 bbx::platform::HudColor::TarnishedGold());
        y += line * 1.15f;
        hud.GothicDivider(tx, y, inner_w, menu_scale);
        y += line * 0.5f;

        for (std::size_t i = first_row; i < end_row; ++i) {
            const bool sel = (i == g_session.quick_menu_selected);
            if (sel) {
                const float sel_pad = static_cast<float>(menu_scale) * 1.1f;
                hud.Panel(tx - sel_pad, y - sel_pad * 0.5f, inner_w + sel_pad * 2.0f,
                          line - sel_pad * 0.3f, bbx::platform::HudColor::CrimsonSelection());
                hud.Panel(tx - sel_pad, y - sel_pad * 0.5f, static_cast<float>(menu_scale) * 1.4f,
                          line - sel_pad * 0.3f, bbx::platform::HudColor::TarnishedGold());
            }
            const bbx::platform::HudColor col =
                sel ? (i == total_rows - 1 ? bbx::platform::HudColor::PalebloodEmber()
                                           : bbx::platform::HudColor::TarnishedGold())
                    : bbx::platform::HudColor::BoneIvory();
            hud.Text(tx + static_cast<float>(menu_scale) * 1.5f, y, menu_scale,
                     std::string(sel ? "<> " : "   ") + rows[i], col);
            y += line;
        }
        if (scrolling) {
            char scroll_hint[96];
            std::snprintf(scroll_hint, sizeof(scroll_hint),
                          "%s OPTIONS %zu-%zu OF %zu  |  D-PAD UP/DOWN TO SCROLL %s",
                          first_row > 0u ? "[^]" : "   ", first_row + 1u, end_row, total_rows,
                          end_row < total_rows ? "[v]" : "   ");
            hud.Text(tx + static_cast<float>(menu_scale) * 1.5f, y, hint_scale, scroll_hint,
                     bbx::platform::HudColor::AshSilver());
            y += line;
        }
        y += line * 0.35f;
        hud.GothicDivider(tx, y, inner_w, menu_scale);
        y += line * 0.4f;
        hud.Text(tx, y, hint_scale, "A / LEFT-RIGHT: ADJUST   B OR L3+R3: RETURN TO THE HUNT",
                 bbx::platform::HudColor::AshSilver());
    }
}

// One frame, drawn by the only renderer this target has. When the device is not
// up - or a present fails because it went away - the loop keeps running and the
// 1 Hz gate line keeps printing why; there is no second renderer to fall back to
// and no reason to exit.
void DrawFrame() {
    if (g_session.gpu) {
        bbx::platform::Hud hud = g_session.gpu->BuildReportHud();
        AppendLauncherToHud(hud);
        AppendInGameOverlays(hud);
        // Only unlock VSync / VRR tearing while Bloodborne is actually running;
        // keeping the launcher and shader baker synced to display refresh avoids
        // spinning at 110+ FPS in a static menu and frees CPU/GPU for DXC baking.
        const bool vsync = bbx::bridge::Running() ? bbx::bridge::VsyncEnabled() : true;
        const bbx::d3d12::Status status =
            g_session.gpu->Present(hud, vsync, g_session.model.sharp_upscale);
        if (status == bbx::d3d12::Status::kOk) {
            g_present_count.fetch_add(1, std::memory_order_relaxed);
            (void)Steps().FirstFramePresented();
            return;
        }
        g_session.display_fault = std::string("present failed (") +
                                  bbx::d3d12::StatusName(status) + ")";
        SetDisplayFault(g_session.display_fault);
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: present failed (%s); the window stays up and the gate line keeps "
            "reporting, with no SDL fallback",
            bbx::d3d12::StatusName(status));
        // A removed device is not a renderer problem: drop the session, keep the
        // window and the loop, and say why once per second from the gate line.
        g_session.gpu->Destroy();
        g_session.gpu.reset();
        SetUiBlocker("display down (no D3D12 device)");
    }
}

// Acquires the console's display on the CoreWindow stored in SetWindow (never
// re-fetched via CoreWindow::GetForCurrentThread) and on nothing else: no
// SDL_Init, no SDL_CreateWindow, no SDL renderer. SDL cannot initialise under
// UWP, so a fallback to it is not a fallback - it is a guaranteed failure that
// hides the real reason. When D3D12 cannot come up the reason is logged in plain
// English, the window stays alive, and the 1 Hz gate line keeps saying so.
bool AcquireDisplay(const winrt::Windows::UI::Core::CoreWindow& stored_window) {
    IUnknown* window_abi = nullptr;
    std::uint32_t window_width = 1920;
    std::uint32_t window_height = 1080;
    if (stored_window) {
        try {
            const winrt::Windows::Foundation::Rect bounds = stored_window.Bounds();
            const float scale = bbx::platform::CompositionScale();
            const float phys_w = std::round(bounds.Width * scale);
            const float phys_h = std::round(bounds.Height * scale);
            if (phys_w >= 320.0f && phys_h >= 240.0f) {
                window_width = static_cast<std::uint32_t>(phys_w);
                window_height = static_cast<std::uint32_t>(phys_h);
            }
        } catch (const winrt::hresult_error& err) {
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kWarn,
                "display: stored_window.Bounds() threw 0x%08x; keeping 1920x1080",
                static_cast<unsigned>(err.code().value));
        }
        window_abi = static_cast<IUnknown*>(winrt::get_abi(stored_window));
    }
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "display: using stored CoreWindow=%p (%ux%u, not CoreWindow::GetForCurrentThread)",
        static_cast<const void*>(window_abi), window_width, window_height);

    SetUiBlocker("D3d12Session::Create");
    g_session.gpu = std::make_unique<bbx::platform::D3d12Session>();
    const bbx::d3d12::Status status =
        g_session.gpu->Create(window_abi, window_width, window_height, false);
    if (status == bbx::d3d12::Status::kOk) {
        bbx::bridge::SetDisplayMetrics(static_cast<int>(g_session.gpu->OutputWidth()),
                                       static_cast<int>(g_session.gpu->OutputHeight()),
                                       g_session.gpu->RefreshHz());
        const bbx::platform::GpuReport& report = g_session.gpu->Report();
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kInfo, "d3d12: %s, %s, self-test %s",
            report.adapter.c_str(), report.swapchain.c_str(), report.selftest.c_str());
        if (!report.guest.empty()) {
            bbx::platform::LogF("d3d12: %s", report.guest.c_str());
        }
        g_session.display_fault.clear();
        ClearDisplayFault();
        g_display_ready.store(true, std::memory_order_relaxed);
        SetUiBlocker("none");
        Steps().DisplayAcquired("d3d12");
        return true;
    }

    // No SDL fallback exists on this target. Say what failed, in words, and leave
    // the window up: the loop below keeps pumping the dispatcher and the gate line
    // reports the same line once a second.
    g_session.display_fault = std::string("d3d12 unavailable (") +
                              bbx::d3d12::StatusName(status) + ")";
    SetDisplayFault(g_session.display_fault);
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kError,
        "display: the D3D12 display could not be acquired (%s). This console build "
        "has no SDL fallback - SDL cannot initialise under UWP - so the window stays "
        "up and the gate line below repeats this once a second.",
        bbx::d3d12::StatusName(status));
    if (!stored_window) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "display: no CoreWindow was ever stored in SetWindow, so there is nothing "
            "to create a swapchain for");
    }
    if (g_session.gpu) {
        const bbx::platform::GpuReport& report = g_session.gpu->Report();
        if (!report.adapter.empty() || !report.swapchain.empty()) {
            bbx::platform::LogMirroredMessage(bbx::platform::Level::kError,
                                              "display: adapter \"%s\", swapchain \"%s\"",
                                              report.adapter.c_str(),
                                              report.swapchain.c_str());
        }
    }
    g_session.gpu.reset();
    SetUiBlocker("display down (no D3D12 device)");
    g_display_ready.store(false, std::memory_order_relaxed);
    return false;
}

void InitGamepadInput() {
    unsigned winrt_pads = 0;
    unsigned raw_pads = 0;
    try {
        using winrt::Windows::Gaming::Input::Gamepad;
        using winrt::Windows::Gaming::Input::RawGameController;
        winrt_pads = Gamepad::Gamepads().Size();
        raw_pads = RawGameController::RawGameControllers().Size();
        static std::once_flag hotplug_once;
        std::call_once(hotplug_once, []() {
            try {
                Gamepad::GamepadAdded([](const auto&, const Gamepad&) {
                    bbx::platform::LogF("input: Gamepad connected (total=%u)",
                                        Gamepad::Gamepads().Size());
                });
                Gamepad::GamepadRemoved([](const auto&, const Gamepad&) {
                    bbx::platform::LogF("input: Gamepad disconnected (total=%u)",
                                        Gamepad::Gamepads().Size());
                });
                RawGameController::RawGameControllerAdded(
                    [](const auto&, const RawGameController&) {
                        bbx::platform::LogF("input: RawGameController connected (total=%u)",
                                            RawGameController::RawGameControllers().Size());
                    });
                RawGameController::RawGameControllerRemoved(
                    [](const auto&, const RawGameController&) {
                        bbx::platform::LogF("input: RawGameController disconnected (total=%u)",
                                            RawGameController::RawGameControllers().Size());
                    });
            } catch (...) {
            }
        });
    } catch (const winrt::hresult_error& err) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kWarn,
            "input: Windows.Gaming.Input threw 0x%08x while listing gamepads",
            static_cast<unsigned>(err.code().value));
    }
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "input: winrt_gamepads=%u, raw_controllers=%u (no SDL input stack)", winrt_pads, raw_pads);
}

// Only the device and the swapchain go away here: there is no SDL window, pad or
// renderer to shut down.
void ShutdownDisplay() {
    if (g_session.gpu) {
        // Waits for the GPU before the device and the swapchain go away: a released
        // swapchain with a frame in flight is a device-removed in the log.
        bbx::platform::LogF("d3d12: shutting down; %s", g_session.gpu->Report().selftest.c_str());
        g_session.gpu->Destroy();
        g_session.gpu.reset();
    }
    g_display_ready.store(false, std::memory_order_relaxed);
}

void LoadSettings() {
    std::string text;
    const std::string path = bbx::platform::Local("bbxbox.ini");
    const bool first_run = !bbx::platform::ReadText(path, text) || text.empty();
    if (first_run) {
        text = bbx::platform::DefaultIni();
        // Default Xbox Series S to 900p internal render (upscaled via FSR RCAS)
        // on a fresh install for smoother 60 FPS headroom; user can still toggle freely.
        const bbx::platform::ConsoleCaps caps =
            g_session.gpu ? g_session.gpu->Console()
                          : bbx::platform::ConsoleCapsFor(bbx::platform::QueryConsoleModel());
        if (caps.default_render_res_mode != 0) {
            (void)bbx::platform::IniSet(text, "video", "render_res_mode",
                                        std::to_string(caps.default_render_res_mode));
        }
        if (!bbx::platform::WriteText(path, text)) {
            bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not write %s", path.c_str());
        }
    }
    g_session.ini_text = text;
    g_session.ini = bbx::platform::ParseIni(text);
    // The two overlay switches the launcher offers. The overlay defaults to on
    // (a player who put files in `mods\` meant them); a modded eboot defaults
    // to off, because it is the one that steps outside the verified digests.
    g_session.model.mods_enabled = bbx::platform::IniFlag(g_session.ini, "dump.mods", true);
    g_session.model.modded_eboot_ok = bbx::platform::IniFlag(g_session.ini, "dump.modded_eboot", false);

    // Video & overlay settings persisted across launches.
    const std::string fps_str = bbx::platform::IniGet(g_session.ini, "video.fps_cap", "60");
    g_session.model.fps_cap = (fps_str == "30") ? 30 : 60;
    g_session.model.render_res_mode = ParseIntClamped(
        bbx::platform::IniGet(g_session.ini, "video.render_res_mode", "0"), 0, 0, 3);
    g_session.model.vsync = bbx::platform::IniFlag(g_session.ini, "video.vsync", true);
    g_session.model.sharp_upscale =
        bbx::platform::IniFlag(g_session.ini, "video.sharp_upscale", true);
    g_session.model.perf_hud = bbx::platform::IniFlag(g_session.ini, "video.perf_hud", false);

    // Gameplay, performance, shadows, post-processing, camera, and controller deadzone settings.
    g_session.model.skip_logos =
        bbx::platform::IniFlag(g_session.ini, "gameplay.skip_logos", true);
    g_session.model.enhancements =
        bbx::platform::IniFlag(g_session.ini, "gameplay.enhancements", true);
    g_session.model.auto_backup_saves =
        bbx::platform::IniFlag(g_session.ini, "gameplay.auto_backup_saves", true);
    g_session.model.performance_mode =
        bbx::platform::IniFlag(g_session.ini, "graphics.performance_mode", false);
    g_session.model.ssao = bbx::platform::IniFlag(g_session.ini, "graphics.ssao", true);
    g_session.model.anti_alias =
        bbx::platform::IniFlag(g_session.ini, "graphics.anti_alias", true);
    g_session.model.dynamic_shadows =
        bbx::platform::IniFlag(g_session.ini, "graphics.dynamic_shadows", true);
    g_session.model.shadow_resolution = ParseIntClamped(
        bbx::platform::IniGet(g_session.ini, "graphics.shadow_resolution", "2"), 2, 0, 3);
    g_session.model.lod_bias = ParseIntClamped(
        bbx::platform::IniGet(g_session.ini, "graphics.lod_bias", "0"), 0, -2, 2);
    g_session.model.chromatic_aberration =
        bbx::platform::IniFlag(g_session.ini, "graphics.chromatic_aberration", true);
    g_session.model.motion_blur =
        bbx::platform::IniFlag(g_session.ini, "graphics.motion_blur", true);
    g_session.model.depth_of_field =
        bbx::platform::IniFlag(g_session.ini, "graphics.depth_of_field", true);
    g_session.model.vignette =
        bbx::platform::IniFlag(g_session.ini, "graphics.vignette", true);
    g_session.model.fov_percent =
        ParseIntClamped(bbx::platform::IniGet(g_session.ini, "graphics.fov_percent", "100"),
                        100, 100, 125);
    g_session.model.camera_distance_percent = ParseIntClamped(
        bbx::platform::IniGet(g_session.ini, "graphics.camera_distance_percent", "100"),
        100, 90, 130);
    g_session.model.stick_deadzone_percent = ParseIntClamped(
        bbx::platform::IniGet(g_session.ini, "controls.stick_deadzone_percent", "10"),
        10, 0, 20);

    // Honor per-launch protocol overrides (`bbxbox:?cmd=... --fps=60 --res=900p`).
    {
        std::lock_guard<std::mutex> lk(g_protocol_mu);
        if (g_protocol_args.fps_cap == 30 || g_protocol_args.fps_cap == 60) {
            g_session.model.fps_cap = g_protocol_args.fps_cap;
        }
        if (g_protocol_args.render_res_mode >= 0 && g_protocol_args.render_res_mode <= 3) {
            g_session.model.render_res_mode = g_protocol_args.render_res_mode;
        }
    }

    LoadHunterName();
    SyncBridgeSettings();
    g_settings_loaded.store(true, std::memory_order_release);
}

std::string NormalizeScanPath(std::string path) {
    for (char& ch : path) {
        if (ch == '/') ch = '\\';
    }
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
    return path;
}

std::string JoinScanPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return NormalizeScanPath(child);
    std::string joined = parent;
    if (joined.back() != '\\' && joined.back() != '/') joined.push_back('\\');
    joined += child;
    return NormalizeScanPath(std::move(joined));
}

void RestoreDump() {
    Steps().DumpScanStarted();
    bbx::platform::ResetUsbAccessDenied();
    const bbx::platform::DumpFs fs = bbx::platform::MakeDumpFs();
    const std::string token = bbx::platform::IniGet(g_session.ini, "dump.token", "");
    std::string path = bbx::platform::IniGet(g_session.ini, "dump.path", "");
    if (!token.empty()) {
        std::string granted;
        if (bbx::platform::ResolveGrant(token, granted)) {
            path = granted;
            g_session.usb_token = token;
        }
    }
    std::string protocol_dump_path;
    {
        std::lock_guard<std::mutex> lk(g_protocol_mu);
        protocol_dump_path = g_protocol_args.dump_path;
    }
    if (!protocol_dump_path.empty()) {
        path = protocol_dump_path;
    }

    std::vector<std::string> candidates;
    const auto add_candidate = [&candidates](const std::string& raw) {
        if (raw.empty()) return;
        const std::string norm = NormalizeScanPath(raw);
        if (norm.empty()) return;
        if (std::find(candidates.begin(), candidates.end(), norm) == candidates.end()) {
            candidates.push_back(norm);
        }
    };

    const auto ascii_lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto equals_icase = [&ascii_lower](const std::string& a, const char* b) {
        std::size_t i = 0;
        for (; i < a.size() && b[i] != '\0'; ++i) {
            if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
        }
        return i == a.size() && b[i] == '\0';
    };
    const auto starts_with_icase = [&ascii_lower](const std::string& a, const char* prefix) {
        std::size_t i = 0;
        for (; prefix[i] != '\0'; ++i) {
            if (i >= a.size() || ascii_lower(a[i]) != ascii_lower(prefix[i])) return false;
        }
        return true;
    };

    // Fast-path: probe the explicit protocol or saved bbxbox.ini dump path first
    // so launches with an already-configured dump skip the ~3s multi-drive crawl.
    std::vector<std::string> found_dumps;
    const auto probe_dump_candidate = [&](const std::string& candidate) -> bool {
        if (!bbx::platform::IsDumpRoot(fs, candidate)) return false;
        const std::optional<bbx::platform::SfoInfo> info =
            bbx::platform::ReadDumpInfo(fs, candidate);
        if (info && !info->IsPatchContainer()) {
            found_dumps.push_back(candidate);
            return true;
        }
        return false;
    };

    add_candidate(protocol_dump_path);
    add_candidate(path);
    for (const std::string& saved_candidate : candidates) {
        if (probe_dump_candidate(saved_candidate)) break;
    }

    if (found_dumps.empty()) {
        const std::size_t already_checked = candidates.size();
        for (const std::string& root : bbx::platform::CandidateDumpRoots()) {
            add_candidate(root);
            if (fs.list_dirs) {
                const bool dev_files_root = starts_with_icase(root, "D:\\DevelopmentFiles");
                for (const std::string& child : fs.list_dirs(root)) {
                    if (child == "." || child == ".." || child == "bbxbox_logs") continue;
                    const std::string child_path = JoinScanPath(root, child);
                    add_candidate(child_path);
                    // Also inspect common 2-level layouts such as E:\dumps\Bloodborne or
                    // D:\DevelopmentFiles\Games\Bloodborne without walking the whole disk.
                    if (dev_files_root || equals_icase(child, "dumps") ||
                        equals_icase(child, "games") || equals_icase(child, "ps4")) {
                        for (const std::string& grandchild : fs.list_dirs(child_path)) {
                            if (grandchild == "." || grandchild == ".." || grandchild == "bbxbox_logs") continue;
                            add_candidate(JoinScanPath(child_path, grandchild));
                        }
                    }
                }
            }
        }
        for (std::size_t i = already_checked; i < candidates.size(); ++i) {
            (void)probe_dump_candidate(candidates[i]);
        }
    }

    if (path.empty() && !found_dumps.empty()) {
        path = found_dumps.front();
    }

    std::string preferred_patch = bbx::platform::IniGet(g_session.ini, "dump.patch_path", "");
    const std::string patch_token = bbx::platform::IniGet(g_session.ini, "dump.patch_token", "");
    if (!patch_token.empty()) {
        std::string granted_patch;
        if (bbx::platform::ResolveGrant(patch_token, granted_patch)) {
            preferred_patch = granted_patch;
            g_session.patch_token = patch_token;
        }
    }
    const bool use_patch = bbx::platform::IniFlag(g_session.ini, "dump.use_patch", true);
    g_session.layout = bbx::platform::DetectLayout(fs, path, preferred_patch, use_patch, false);
    if (!g_session.layout.layout_ok() && !found_dumps.empty() &&
        NormalizeScanPath(path) != found_dumps.front()) {
        g_session.layout =
            bbx::platform::DetectLayout(fs, found_dumps.front(), preferred_patch, use_patch, false);
    }
    if (!g_session.layout.layout_ok() && bbx::platform::SawUsbAccessDenied()) {
        g_session.model.status_banner = bbx::platform::UsbPermissionHint();
    }
    bbx::platform::LogMessage(bbx::platform::Level::kInfo, "%s",
                              bbx::platform::LayoutLogLine(g_session.layout).c_str());
    // The black box: from here on every line is also written to
    // `<dump>\bbxbox_logs\loader.log` and `errors.log` (and a fault's report to
    // `crash.log`), so a run that dies on the console is read off the player's
    // USB stick.
    if (g_session.layout.layout_ok() && !g_session.layout.base_path.empty()) {
        (void)bbx::platform::LogMirrorToUsb(g_session.layout.base_path);
    }
    Steps().DumpScanFinished(found_dumps.size());
    // Early-boot 100 ms hang heartbeat is no longer needed once the launcher is
    // live; TelemetrySampler's 5 s watchdog + stall detector takes over from here
    // without flooding errors.log or flushing USB 10 times per second.
    Steps().StopHeartbeatThread();
}

}  // namespace

namespace {

struct LauncherView : winrt::implements<LauncherView, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    winrt::event_token closed_token_{};
    winrt::event_token visibility_token_{};
    winrt::event_token activated_token_{};
    winrt::event_token size_changed_token_{};
    winrt::event_token back_requested_token_{};
    winrt::event_token dpi_changed_token_{};
    winrt::event_token hdmi_mode_changed_token_{};
    winrt::event_token char_received_token_{};
    winrt::event_token key_down_token_{};
    winrt::event_token key_up_token_{};

    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& app) {
        const auto init_tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        std::uint32_t expected = 0;
        (void)g_view_thread_id.compare_exchange_strong(expected, init_tid,
                                                       std::memory_order_relaxed);

        app.Activated([](const auto&,
                         const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args) {
            // Parse LaunchPass / RetroPass URI protocol launch parameters
            // (`bbxbox:?cmd=...&launchOnExit=...`), matching XboxEmulationHub XBSX2/RetroArch.
            try {
                using winrt::Windows::ApplicationModel::Activation::ActivationKind;
                using winrt::Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs;
                if (args && args.Kind() == ActivationKind::Protocol) {
                    const auto proto = args.as<ProtocolActivatedEventArgs>();
                    if (proto && proto.Uri()) {
                        const std::string raw = winrt::to_string(proto.Uri().RawUri());
                        const bbx::platform::ProtocolLaunchArgs parsed =
                            bbx::platform::ParseProtocolUri(raw);
                        bbx::platform::LogMirroredMessage(
                            bbx::platform::Level::kInfo,
                            "protocol: activated URI=\"%s\" (autoboot=%u, force_exit=%u, path=\"%s\")",
                            raw.c_str(), static_cast<unsigned>(parsed.autoboot),
                            static_cast<unsigned>(parsed.force_exit), parsed.dump_path.c_str());
                        if (parsed.force_exit) {
                            g_session.quit = true;
                        }
                        if (parsed.valid) {
                            std::lock_guard<std::mutex> lk(g_protocol_mu);
                            g_protocol_args = parsed;
                            if (!parsed.launch_on_exit.empty()) {
                                g_launch_on_exit_uri = parsed.launch_on_exit;
                            }
                        }
                    }
                }
            } catch (const winrt::hresult_error& err) {
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kWarn,
                    "protocol: failed to read activation URI (0x%08x)",
                    static_cast<unsigned>(err.code().value));
            } catch (...) {
            }

            // Point 5: use the stored CoreWindow from SetWindow, never re-fetching
            // via CoreWindow::GetForCurrentThread().
            winrt::Windows::UI::Core::CoreWindow target = g_core_window.get();
            if (target) {
                target.Activate();
                Steps().LogCallback("Activated (window.Activate() called on stored CoreWindow)");
                try {
                    target.Dispatcher().ProcessEvents(
                        winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
                } catch (...) {
                }
            } else {
                Steps().LogCallback("Activated (no stored CoreWindow yet)");
            }
        });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
            [](const auto&, const winrt::Windows::ApplicationModel::SuspendingEventArgs& args) {
                winrt::Windows::ApplicationModel::SuspendingDeferral deferral{nullptr};
                try {
                    if (args) deferral = args.SuspendingOperation().GetDeferral();
                } catch (...) {
                }
                const bbx::platform::TelemetrySample mem = QueryTelemetrySample();
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kInfo,
                    "view: Suspending (memory=%llu/%llu MiB, presents=%llu)",
                    static_cast<unsigned long long>(mem.used_mib),
                    static_cast<unsigned long long>(mem.limit_mib),
                    static_cast<unsigned long long>(mem.presents));
                bbx::bridge::StopPadRumble();
                if (g_settings_loaded.load(std::memory_order_acquire)) {
                    SaveLayoutSettings();
                }
                Steps().Shutdown("suspend event");
                bbx::platform::LogFlush();
                if (deferral) {
                    try {
                        deferral.Complete();
                    } catch (...) {
                    }
                }
            });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming(
            [](const auto&, const auto&) {
                Steps().LogCallback("Resuming");
                // Re-check display mode after resume in case TV/HDMI state changed.
                g_display_resize_pending.store(true, std::memory_order_release);
                bbx::platform::LogFlush();
            });
        Steps().LogCallback("Initialize (registered Activated, Suspending, Resuming)");
    }

    void SetWindow(const winrt::Windows::UI::Core::CoreWindow& value) {
        window = value;
        g_core_window = winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow>(value);
        const auto view_tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        g_view_thread_id.store(view_tid, std::memory_order_relaxed);
        void* const stored_abi = winrt::get_abi(value);
        g_stored_window_abi.store(stored_abi, std::memory_order_relaxed);

        bool initial_visible = false;
        try {
            if (window) initial_visible = window.Visible();
        } catch (...) {
        }
        g_app_visible.store(initial_visible, std::memory_order_relaxed);

        // Hide the OS pointer cursor so an attached USB mouse never floats over Bloodborne.
        try {
            if (window) window.PointerCursor(nullptr);
        } catch (...) {
        }

        // CRITICAL Xbox UWP fix (from XboxEmulationHub XBSX2 & RetroArch):
        // Intercept SystemNavigationManager::BackRequested so pressing B on the
        // controller (dodge/back in Bloodborne) never suspends or minimizes the app.
        try {
            const auto nav = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
            if (nav) {
                back_requested_token_ = nav.BackRequested(
                    [](const auto&, const winrt::Windows::UI::Core::BackRequestedEventArgs& ev) {
                        ev.Handled(true);
                    });
            }
        } catch (...) {
        }

        closed_token_ = window.Closed(
            [](const auto&, const winrt::Windows::UI::Core::CoreWindowEventArgs&) {
                g_window_closed.store(true, std::memory_order_relaxed);
                g_session.quit = true;
                Steps().LogCallback("Closed");
                Steps().Shutdown("window closed event");
                bbx::platform::LogFlush();
            });

        visibility_token_ = window.VisibilityChanged(
            [](const auto&, const winrt::Windows::UI::Core::VisibilityChangedEventArgs& ev) {
                const bool vis = ev.Visible();
                g_app_visible.store(vis, std::memory_order_relaxed);
                const std::uint32_t count =
                    g_visibility_events.fetch_add(1, std::memory_order_relaxed) + 1u;
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kInfo,
                    "view: VisibilityChanged fired (visible=%u, count=%u)",
                    static_cast<unsigned>(vis), count);
            });

        // Pause rumble and suppress guest controller input while the Xbox Guide or
        // another system overlay steals window focus, preventing stuck buttons.
        activated_token_ = window.Activated(
            [](const auto&, const winrt::Windows::UI::Core::WindowActivatedEventArgs& ev) {
                const bool active =
                    ev.WindowActivationState() !=
                    winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated;
                g_window_active.store(active, std::memory_order_release);
                if (!active) {
                    bbx::bridge::StopPadRumble();
                    bbx::bridge::SetGuestInputSuppressed(true);
                } else {
                    bbx::bridge::SetGuestInputSuppressed(g_session.quick_menu_open ||
                                                         g_session.ime_open);
                }
            });

        // Dynamically resize the swapchain if CoreWindow bounds, DPI, or HDMI TV mode change.
        size_changed_token_ = window.SizeChanged(
            [](const auto&, const winrt::Windows::UI::Core::WindowSizeChangedEventArgs& ev) {
                const float scale = bbx::platform::CompositionScale();
                const float phys_w = std::round(ev.Size().Width * scale);
                const float phys_h = std::round(ev.Size().Height * scale);
                if (phys_w >= 320.0f && phys_h >= 240.0f) {
                    g_pending_resize_w.store(static_cast<std::uint32_t>(phys_w),
                                             std::memory_order_relaxed);
                    g_pending_resize_h.store(static_cast<std::uint32_t>(phys_h),
                                             std::memory_order_relaxed);
                    g_display_resize_pending.store(true, std::memory_order_release);
                }
            });

        try {
            const auto disp = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            if (disp) {
                dpi_changed_token_ = disp.DpiChanged([](const auto&, const auto&) {
                    g_pending_resize_w.store(0, std::memory_order_relaxed);
                    g_pending_resize_h.store(0, std::memory_order_relaxed);
                    g_display_resize_pending.store(true, std::memory_order_release);
                });
            }
        } catch (...) {
        }

        try {
            const auto hdmi =
                winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
            if (hdmi) {
                hdmi_mode_changed_token_ = hdmi.DisplayModesChanged([](const auto&, const auto&) {
                    g_pending_resize_w.store(0, std::memory_order_relaxed);
                    g_pending_resize_h.store(0, std::memory_order_relaxed);
                    g_display_resize_pending.store(true, std::memory_order_release);
                });
            }
        } catch (...) {
        }

        // Physical USB keyboard character stream for Hunter Name / IME typing.
        char_received_token_ = window.CharacterReceived(
            [](const auto&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& ev) {
                const std::uint32_t code = ev.KeyCode();
                if (code >= 32u && code <= 126u) {
                    std::lock_guard<std::mutex> lk(g_key_mu);
                    if (g_typed_chars.size() < 64u) {
                        g_typed_chars.push_back(static_cast<char>(code));
                    }
                }
            });

        key_down_token_ = window.KeyDown(
            [](const auto&, const winrt::Windows::UI::Core::KeyEventArgs& ev) {
                using winrt::Windows::System::VirtualKey;
                const VirtualKey vk = ev.VirtualKey();
                if (vk == VirtualKey::Back) {
                    g_key_backspace.store(true, std::memory_order_release);
                } else if (vk == VirtualKey::Enter) {
                    g_key_enter.store(true, std::memory_order_release);
                } else if (vk == VirtualKey::Escape) {
                    g_key_escape.store(true, std::memory_order_release);
                } else if (vk == VirtualKey::F1) {
                    g_key_quick_menu.store(true, std::memory_order_release);
                } else if (vk == VirtualKey::Up) {
                    g_key_nav_edges.fetch_or(bbx::platform::Bit(Pad::kDpadUp),
                                             std::memory_order_release);
                } else if (vk == VirtualKey::Down) {
                    g_key_nav_edges.fetch_or(bbx::platform::Bit(Pad::kDpadDown),
                                             std::memory_order_release);
                } else if (vk == VirtualKey::Left) {
                    g_key_nav_edges.fetch_or(bbx::platform::Bit(Pad::kDpadLeft),
                                             std::memory_order_release);
                } else if (vk == VirtualKey::Right) {
                    g_key_nav_edges.fetch_or(bbx::platform::Bit(Pad::kDpadRight),
                                             std::memory_order_release);
                }
            });

        key_up_token_ = window.KeyUp(
            [](const auto&, const winrt::Windows::UI::Core::KeyEventArgs&) {});

        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kInfo,
            "view: SetWindow (stored CoreWindow=%p + agile_ref on tid=%u, initial_visible=%u; "
            "registered BackRequested, Activated, SizeChanged, Dpi/HdmiMode, CharacterReceived, KeyDown)",
            stored_abi, view_tid, static_cast<unsigned>(initial_visible));
    }

    void Load(const winrt::hstring&) {}

    void Run() {
        const auto ui_tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        Steps().RegisterThread(ui_tid, "ui-main");
        Steps().LogCallback("Run entered");

        const bbx::platform::StorageInitResult& storage = bbx::platform::StorageInit();
        (void)bbx::platform::LogOpen(bbx::platform::Paths().local_state);
        StartBootInstrumentation();
        bbx::platform::InstallLastChanceLogger();

        // 5 s telemetry + 2-sample stall detector and 1 s pre-acquisition gate
        // logger start immediately so nothing before the loop can block silently.
        ScopedTelemetryThread telemetry;
        ScopedAcquireWatch acquire_watch;

        // Point 5: always use the CoreWindow stored in SetWindow, never re-fetching
        // via CoreWindow::GetForCurrentThread().
        winrt::Windows::UI::Core::CoreWindow stored_window = window ? window : g_core_window.get();
        if (stored_window) {
            SetUiBlocker("Dispatcher::ProcessEvents(initial)");
            stored_window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
        }

        SetUiBlocker("ReplayStorageOnce");
        ReplayStorageOnce();
        if (!storage.ok) {
            bbx::platform::LogMessage(bbx::platform::Level::kError, "%s",
                                      bbx::platform::FormatStorageFailureLine(storage).c_str());
        }
        SetUiBlocker("ReserveGuestWindows");
        (void)bbx::platform::ReserveGuestWindows();
        bbx::platform::LogMessage(bbx::platform::Level::kInfo, "bbxbox launcher starting");
        {
            SetUiBlocker("hostfs::SelfTest");
            std::string why;
            const bool fs_ok = bbx::hostfs::SelfTest(bbx::platform::Local("guestfs"), &why);
            bbx::platform::LogMirroredMessage(
                fs_ok ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
                "host fs: %s (%s)", fs_ok ? "ok" : "FAILED",
                fs_ok ? bbx::hostfs::Table().name : why.c_str());
        }

        // Point 1: immediately after "host fs: ok", log "display: acquire begin"
        // with tid, whether this is the view thread, and the CoreWindow pointer
        // stored in SetWindow; on ui-main also log CoreWindow::GetForCurrentThread()
        // and state explicitly whether it is null.
        const auto acquire_tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        const auto view_tid = g_view_thread_id.load(std::memory_order_relaxed);
        const bool is_view_thread = (view_tid != 0 && acquire_tid == view_tid);
        void* const stored_abi = stored_window ? winrt::get_abi(stored_window)
                                               : g_stored_window_abi.load(std::memory_order_relaxed);
        g_display_acquire_entered.store(true, std::memory_order_relaxed);
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kInfo,
            "display: acquire begin (tid=%u, is_view_thread=%s [view_tid=%u], stored_core_window=%p)",
            acquire_tid, is_view_thread ? "yes" : "no", view_tid, stored_abi);
        if (acquire_tid == ui_tid) {
            void* current_abi = nullptr;
            try {
                const auto current_win =
                    winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
                current_abi = winrt::get_abi(current_win);
            } catch (const winrt::hresult_error& err) {
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kWarn,
                    "display: CoreWindow::GetForCurrentThread() on ui-main threw 0x%08x",
                    static_cast<unsigned>(err.code().value));
            }
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kInfo,
                "display: ui-main CoreWindow::GetForCurrentThread()=%p (%s)",
                current_abi, current_abi == nullptr ? "null" : "non-null");
        }

        if (stored_window) {
            SetUiBlocker("stored_window.Activate");
            stored_window.Activate();
            Steps().LogCallback("window.Activate() completed before first present");
            SetUiBlocker("Dispatcher::ProcessEvents(post-Activate)");
            stored_window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            bool now_visible = g_app_visible.load(std::memory_order_relaxed);
            try {
                now_visible = stored_window.Visible();
                g_app_visible.store(now_visible, std::memory_order_relaxed);
            } catch (...) {
            }
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kInfo,
                "view: post-Activate window.Visible()=%u (VisibilityChanged_count=%u)",
                static_cast<unsigned>(now_visible),
                g_visibility_events.load(std::memory_order_relaxed));
        }

        RefreshLauncher();

        // The display comes up (or does not) with no SDL in the path. A failure is
        // not fatal and never a fallback: the window stays up and the acquire
        // watch below keeps printing the reason once a second.
        const bool display_up = AcquireDisplay(stored_window);
        if (display_up) {
            // Present the initial frame immediately so step 3 fires and PLM sees an
            // active swapchain before any background self-test or USB dump scan runs.
            SetUiBlocker("first DrawFrame");
            DrawFrame();
            SetUiBlocker("none");
        } else {
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kError,
                "display: staying up without a swapchain (%s); the gate line repeats "
                "this once a second until the window closes",
                g_session.display_fault.empty() ? "no device" : g_session.display_fault.c_str());
        }
        if (display_up) acquire_watch.Stop();

        InitGamepadInput();
        {
            char why[64] = {0};
            const int step = bbx::winpthreads::SelfTest(why, sizeof(why));
            bbx::platform::LogMirroredMessage(
                step == 0 ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
                "winpthreads: %s%s%s", step == 0 ? "PASS" : "FAILED",
                step == 0 ? "" : " at ", step == 0 ? "" : why);
        }
        (void)pthread_setname_np(pthread_self(), "ui-main");
        LoadSettings();
        RestoreDump();
        RefreshLauncher();
        {
            bool protocol_autoboot = false;
            {
                std::lock_guard<std::mutex> lk(g_protocol_mu);
                protocol_autoboot = g_protocol_args.autoboot;
            }
            if (protocol_autoboot && g_session.layout.layout_ok() && !g_session.quit) {
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kInfo,
                    "protocol: auto-launching Bloodborne from \"%s\"",
                    g_session.layout.base_path.c_str());
                g_session.start_requested = true;
            }
        }
        DrawFrame();
        const bbx::platform::SelfTestResult self_test = bbx::platform::RunMemorySelfTest(false);

        bbx::platform::ButtonMask previous = 0;
        bool guest_ever_started = false;
        bool emergency_holding = false;
        std::chrono::steady_clock::time_point emergency_hold_start{};
        const bbx::platform::ButtonMask kEmergencyQuitMask =
            bbx::platform::Bit(Pad::kView) | bbx::platform::Bit(Pad::kMenu);
        const bbx::platform::ButtonMask kQuickMenuMask =
            bbx::platform::Bit(Pad::kLeftStick) | bbx::platform::Bit(Pad::kRightStick);
        constexpr std::size_t kQuickMenuCount = 20;

        while (!g_session.quit && !g_window_closed.load(std::memory_order_relaxed)) {
            if (stored_window) {
                stored_window.Dispatcher().ProcessEvents(
                    winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            }
            if (g_window_closed.load(std::memory_order_relaxed)) break;
            if (bbx::bridge::QuitRequested()) {
                g_session.quit = true;
                break;
            }

            // Handle CoreWindow SizeChanged, DPI, or HDMI TV mode changes cleanly on ui-main.
            if (g_display_resize_pending.exchange(false, std::memory_order_acq_rel) &&
                g_session.gpu != nullptr) {
                std::uint32_t rw = g_pending_resize_w.exchange(0, std::memory_order_relaxed);
                std::uint32_t rh = g_pending_resize_h.exchange(0, std::memory_order_relaxed);
                if ((rw == 0 || rh == 0) && stored_window) {
                    try {
                        const auto bounds = stored_window.Bounds();
                        const float scale = bbx::platform::CompositionScale();
                        rw = static_cast<std::uint32_t>(std::round(bounds.Width * scale));
                        rh = static_cast<std::uint32_t>(std::round(bounds.Height * scale));
                    } catch (...) {
                    }
                }
                if (rw < 320u || rh < 240u) {
                    rw = 1920u;
                    rh = 1080u;
                }
                if (g_session.gpu->OnModeChanged(rw, rh) == bbx::d3d12::Status::kOk) {
                    bbx::bridge::SetDisplayMetrics(static_cast<int>(g_session.gpu->OutputWidth()),
                                                   static_cast<int>(g_session.gpu->OutputHeight()),
                                                   g_session.gpu->RefreshHz());
                }
            }

            // Drain physical USB keyboard events collected on CoreWindow callbacks.
            std::vector<char> typed_chars;
            {
                std::lock_guard<std::mutex> lk(g_key_mu);
                while (!g_typed_chars.empty()) {
                    typed_chars.push_back(g_typed_chars.front());
                    g_typed_chars.pop_front();
                }
            }
            const bbx::platform::ButtonMask key_nav =
                g_key_nav_edges.exchange(0, std::memory_order_acq_rel);
            const bool key_back = g_key_backspace.exchange(false, std::memory_order_acq_rel);
            const bool key_enter = g_key_enter.exchange(false, std::memory_order_acq_rel);
            const bool key_esc = g_key_escape.exchange(false, std::memory_order_acq_rel);
            const bool key_quick_menu = g_key_quick_menu.exchange(false, std::memory_order_acq_rel);

            // The pad is WinRT's; there is no SDL event queue on this target.
            const bbx::platform::ButtonMask held = MaskFromPad();
            const bbx::platform::ButtonMask pressed = (held & ~previous) | key_nav;
            const bbx::platform::ButtonMask released = previous & ~held;
            previous = held;

            if (bbx::bridge::Running()) {
                // Keep the 3-second View+Menu emergency exit chord active at all times.
                if ((held & kEmergencyQuitMask) == kEmergencyQuitMask) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!emergency_holding) {
                        emergency_holding = true;
                        emergency_hold_start = now;
                    } else if (now - emergency_hold_start >= std::chrono::seconds(3)) {
                        bbx::platform::LogMirroredMessage(
                            bbx::platform::Level::kInfo,
                            "launcher: View+Menu held for 3 s; exiting running guest");
                        g_session.quit = true;
                    }
                } else {
                    emergency_holding = false;
                }

                // Poll guest IME text-entry requests (Hunter Name, Chalice Glyph, Password).
                const bbx::bridge::TextEntryPrompt prompt = bbx::bridge::PollTextEntryPrompt();
                if (prompt.active && !g_session.ime_open) {
                    g_session.ime_open = true;
                    g_session.ime_label = prompt.label;
                    g_session.ime_hidden = prompt.hidden;
                    g_session.ime_max_chars = prompt.max_chars == 0 ? 16u : prompt.max_chars;
                    g_session.ime_keyboard.Reset(prompt.initial);
                    g_session.quick_menu_open = false;
                    bbx::bridge::SetGuestInputSuppressed(true);
                } else if (!prompt.active && g_session.ime_open) {
                    g_session.ime_open = false;
                    bbx::bridge::SetGuestInputSuppressed(g_session.quick_menu_open);
                }

                if (g_session.ime_open) {
                    for (char ch : typed_chars) {
                        if (g_session.ime_keyboard.Text().size() < g_session.ime_max_chars) {
                            g_session.ime_keyboard.Append(ch);
                        }
                    }
                    if (pressed & bbx::platform::Bit(Pad::kDpadUp)) g_session.ime_keyboard.Move(0, -1);
                    if (pressed & bbx::platform::Bit(Pad::kDpadDown)) g_session.ime_keyboard.Move(0, 1);
                    if (pressed & bbx::platform::Bit(Pad::kDpadLeft)) g_session.ime_keyboard.Move(-1, 0);
                    if (pressed & bbx::platform::Bit(Pad::kDpadRight)) g_session.ime_keyboard.Move(1, 0);
                    if ((pressed & bbx::platform::Bit(Pad::kEast)) || key_back) {
                        g_session.ime_keyboard.Backspace();
                    }
                    if ((pressed & bbx::platform::Bit(Pad::kMenu)) || key_enter) {
                        std::string result = g_session.ime_keyboard.Text();
                        if (result.size() > g_session.ime_max_chars) {
                            result.resize(g_session.ime_max_chars);
                        }
                        bbx::bridge::CompleteTextEntry(result, true);
                        g_session.ime_open = false;
                        bbx::bridge::SetGuestInputSuppressed(false);
                    } else if ((pressed & bbx::platform::Bit(Pad::kView)) || key_esc) {
                        bbx::bridge::CompleteTextEntry({}, false);
                        g_session.ime_open = false;
                        bbx::bridge::SetGuestInputSuppressed(false);
                    } else if (pressed & bbx::platform::Bit(Pad::kSouth)) {
                        if (!g_session.ime_keyboard.Accept()) {
                            std::string result = g_session.ime_keyboard.Text();
                            if (result.size() > g_session.ime_max_chars) {
                                result.resize(g_session.ime_max_chars);
                            }
                            bbx::bridge::CompleteTextEntry(result, true);
                            g_session.ime_open = false;
                            bbx::bridge::SetGuestInputSuppressed(false);
                        }
                    }
                } else {
                    // Toggle the in-game L3+R3 Quick Menu (or F1 on USB keyboard).
                    if (((held & kQuickMenuMask) == kQuickMenuMask && (pressed & kQuickMenuMask) != 0) ||
                        key_quick_menu) {
                        g_session.quick_menu_open = !g_session.quick_menu_open;
                        bbx::bridge::SetGuestInputSuppressed(g_session.quick_menu_open);
                    } else if (g_session.quick_menu_open) {
                        const bool confirm =
                            (pressed & bbx::platform::Bit(Pad::kSouth)) != 0 || key_enter;
                        const bool step_left = (pressed & bbx::platform::Bit(Pad::kDpadLeft)) != 0;
                        const bool step_right = (pressed & bbx::platform::Bit(Pad::kDpadRight)) != 0;
                        if (pressed & bbx::platform::Bit(Pad::kDpadUp)) {
                            g_session.quick_menu_selected =
                                (g_session.quick_menu_selected + kQuickMenuCount - 1) % kQuickMenuCount;
                        } else if (pressed & bbx::platform::Bit(Pad::kDpadDown)) {
                            g_session.quick_menu_selected =
                                (g_session.quick_menu_selected + 1) % kQuickMenuCount;
                        } else if ((pressed & bbx::platform::Bit(Pad::kEast)) || key_esc) {
                            g_session.quick_menu_open = false;
                            bbx::bridge::SetGuestInputSuppressed(false);
                        } else if (confirm || step_left || step_right) {
                            bool settings_changed = false;
                            switch (g_session.quick_menu_selected) {
                            case 0:
                                if (confirm) {
                                    g_session.quick_menu_open = false;
                                    bbx::bridge::SetGuestInputSuppressed(false);
                                }
                                break;
                            case 1:
                                g_session.model.fps_cap = (g_session.model.fps_cap >= 60) ? 30 : 60;
                                settings_changed = true;
                                break;
                            case 2:
                                if (confirm) {
                                    g_session.model.render_res_mode =
                                        (g_session.model.render_res_mode + 1) % 4;
                                } else if (step_left) {
                                    g_session.model.render_res_mode =
                                        std::max(0, g_session.model.render_res_mode - 1);
                                } else if (step_right) {
                                    g_session.model.render_res_mode =
                                        std::min(3, g_session.model.render_res_mode + 1);
                                }
                                settings_changed = true;
                                break;
                            case 3:
                                g_session.model.vsync = !bbx::bridge::VsyncEnabled();
                                settings_changed = true;
                                break;
                            case 4:
                                g_session.model.sharp_upscale = !g_session.model.sharp_upscale;
                                SaveLayoutSettings();
                                break;
                            case 5:
                                g_session.model.performance_mode = !g_session.model.performance_mode;
                                settings_changed = true;
                                break;
                            case 6:
                                if (confirm) {
                                    g_session.model.shadow_resolution =
                                        (g_session.model.shadow_resolution + 1) % 4;
                                } else if (step_left) {
                                    g_session.model.shadow_resolution =
                                        std::max(0, g_session.model.shadow_resolution - 1);
                                } else if (step_right) {
                                    g_session.model.shadow_resolution =
                                        std::min(3, g_session.model.shadow_resolution + 1);
                                }
                                settings_changed = true;
                                break;
                            case 7:
                                g_session.model.dynamic_shadows = !g_session.model.dynamic_shadows;
                                settings_changed = true;
                                break;
                            case 8:
                                g_session.model.ssao = !g_session.model.ssao;
                                settings_changed = true;
                                break;
                            case 9:
                                g_session.model.anti_alias = !g_session.model.anti_alias;
                                settings_changed = true;
                                break;
                            case 10:
                                if (confirm) {
                                    g_session.model.lod_bias =
                                        (g_session.model.lod_bias >= 2) ? -2
                                                                        : (g_session.model.lod_bias + 1);
                                } else if (step_left) {
                                    g_session.model.lod_bias =
                                        std::max(-2, g_session.model.lod_bias - 1);
                                } else if (step_right) {
                                    g_session.model.lod_bias =
                                        std::min(2, g_session.model.lod_bias + 1);
                                }
                                settings_changed = true;
                                break;
                            case 11:
                                g_session.model.chromatic_aberration =
                                    !g_session.model.chromatic_aberration;
                                settings_changed = true;
                                break;
                            case 12:
                                g_session.model.motion_blur = !g_session.model.motion_blur;
                                settings_changed = true;
                                break;
                            case 13:
                                g_session.model.depth_of_field = !g_session.model.depth_of_field;
                                settings_changed = true;
                                break;
                            case 14:
                                g_session.model.vignette = !g_session.model.vignette;
                                settings_changed = true;
                                break;
                            case 15:
                                if (confirm) {
                                    g_session.model.fov_percent =
                                        (g_session.model.fov_percent >= 125)
                                            ? 100
                                            : (g_session.model.fov_percent + 5);
                                } else if (step_left) {
                                    g_session.model.fov_percent =
                                        std::max(100, g_session.model.fov_percent - 5);
                                } else if (step_right) {
                                    g_session.model.fov_percent =
                                        std::min(125, g_session.model.fov_percent + 5);
                                }
                                settings_changed = true;
                                break;
                            case 16:
                                if (confirm) {
                                    g_session.model.camera_distance_percent =
                                        (g_session.model.camera_distance_percent >= 130)
                                            ? 90
                                            : (g_session.model.camera_distance_percent + 10);
                                } else if (step_left) {
                                    g_session.model.camera_distance_percent =
                                        std::max(90, g_session.model.camera_distance_percent - 10);
                                } else if (step_right) {
                                    g_session.model.camera_distance_percent =
                                        std::min(130, g_session.model.camera_distance_percent + 10);
                                }
                                settings_changed = true;
                                break;
                            case 17:
                                if (confirm) {
                                    g_session.model.stick_deadzone_percent =
                                        (g_session.model.stick_deadzone_percent >= 20)
                                            ? 0
                                            : (g_session.model.stick_deadzone_percent + 5);
                                } else if (step_left) {
                                    g_session.model.stick_deadzone_percent =
                                        std::max(0, g_session.model.stick_deadzone_percent - 5);
                                } else if (step_right) {
                                    g_session.model.stick_deadzone_percent =
                                        std::min(20, g_session.model.stick_deadzone_percent + 5);
                                }
                                settings_changed = true;
                                break;
                            case 18:
                                g_session.model.perf_hud = !g_session.model.perf_hud;
                                SaveLayoutSettings();
                                break;
                            case 19:
                                if (confirm) {
                                    g_session.quick_menu_open = false;
                                    bbx::bridge::SetGuestInputSuppressed(false);
                                    g_session.quit = true;
                                }
                                break;
                            default: break;
                            }
                            if (settings_changed) {
                                SyncBridgeSettings();
                                SaveLayoutSettings();
                            }
                        }
                    }
                }
            } else if (g_session.baking) {
                emergency_holding = false;
                if ((pressed & bbx::platform::Bit(Pad::kEast)) != 0 &&
                    !g_session.bake_progress.CancelRequested()) {
                    bbx::platform::LogMirroredMessage(
                        bbx::platform::Level::kInfo,
                        "launcher: B pressed during shader bake; cancelling and saving checkpoint");
                    g_session.bake_progress.RequestCancel();
                }
                if (g_session.bake_finished.load(std::memory_order_acquire)) {
                    if (g_session.bake_thread.joinable()) g_session.bake_thread.join();
                    g_session.baking = false;
                    g_session.bake_finished.store(false, std::memory_order_relaxed);
                    g_session.model.status_banner = g_session.bake_result.summary;
                    bbx::platform::LogMirroredMessage(
                        g_session.bake_result.ok ? bbx::platform::Level::kInfo
                                                 : bbx::platform::Level::kWarn,
                        "launcher: shader bake finished (%s): %s",
                        g_session.bake_result.ok ? "ok" : "incomplete",
                        g_session.bake_result.summary.c_str());
                    RefreshLauncher(/*probe_storage=*/true);
                    if (g_session.start_after_bake && !g_session.bake_result.cancelled) {
                        SyncBridgeSettings();
                        if (bbx::bridge::StartGuest(g_session.pending_guest_req)) {
                            guest_ever_started = true;
                            if (g_session.gpu) g_session.gpu->PrewarmShaders();
                        } else {
                            const std::string err = bbx::bridge::LastStartError();
                            g_session.model.status_banner =
                                err.empty() ? "Bloodborne did not start after baking (see loader.log)."
                                            : err;
                            RefreshLauncher();
                            bbx::platform::LogMessage(
                                bbx::platform::Level::kError,
                                "launcher: the guest did not start after baking; staying in the launcher");
                        }
                    }
                }
            } else {
                emergency_holding = false;
                if (g_session.keyboard_open) {
                    for (char ch : typed_chars) {
                        g_session.keyboard.Append(ch);
                    }
                    if (key_back) g_session.keyboard.Backspace();
                    if (key_enter) {
                        std::string name = g_session.keyboard.Text();
                        if (name.size() > 16u) name.resize(16u);
                        g_session.model.hunter_name = name;
                        bbx::platform::LogF("hunter name: \"%s\"", name.c_str());
                        if (!bbx::platform::WriteText(bbx::platform::Local("hunter-name.txt"), name)) {
                            bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                                      "could not write hunter-name.txt");
                        }
                        g_session.keyboard_open = false;
                        RefreshLauncher();
                    } else if (key_esc) {
                        g_session.keyboard_open = false;
                        RefreshLauncher();
                    }
                }
                bbx::platform::PadEdges edges;
                edges.held = held;
                edges.pressed = pressed;
                if (!g_session.keyboard_open) {
                    if (key_enter) edges.pressed |= bbx::platform::Bit(Pad::kSouth);
                    if (key_esc) edges.pressed |= bbx::platform::Bit(Pad::kEast);
                }
                edges.released = released;
                const PadAction action = g_session.chords.Update(edges);
                if (action != PadAction::kNone) OnAction(action);
                PollDumpPick();
                PollUsbSync();
            }

            DrawFrame();

            if (g_session.start_requested && !g_session.baking && !g_session.usb_sync_running) {
                bbx::platform::LogF("launcher: start requested (version %s)",
                                    g_session.layout.EffectiveVersion().c_str());
                g_session.start_requested = false;
                const bool force_unverified = g_session.start_force_unverified;
                g_session.start_force_unverified = false;
                // The guest boots on its own thread and the launcher keeps
                // drawing: `_start` runs until the game exits, and the window
                // that presents it is this one (platform/bbhost_bridge.cpp).
                bbx::bridge::GuestRequest req;
                req.app0 = g_session.layout.base_path;
                req.data = bbx::platform::Local("data");
                req.tmp = bbx::platform::Local("tmp");
                (void)bbx::hostfs::MkDir(req.data, true);
                (void)bbx::hostfs::MkDir(req.tmp, true);
                const std::string& eboot_dir =
                    g_session.layout.CanRunPatched() ? g_session.layout.patch_path : g_session.layout.base_path;
                req.eboot = eboot_dir + "\\eboot.bin";
                if (g_session.model.mods_enabled && g_session.model.mods_present) {
                    req.mods = g_session.layout.base_path + "\\mods";
                }
                req.hunter_name = g_session.model.hunter_name;
                req.modded_eboot_ok = g_session.model.modded_eboot_ok || force_unverified;
                SyncBridgeSettings();

                std::string found_cache_dir;
                std::string found_cache_report;
                if (bbx::platform::HasValidConsoleCache(CacheSearchDirs(), &found_cache_dir,
                                                        &found_cache_report)) {
                    bbx::platform::LogF(
                        "launcher: valid DXIL 6.4 cache found in %s (%s); skipping DXC load",
                        found_cache_dir.c_str(), found_cache_report.c_str());
                    if (bbx::bridge::StartGuest(req)) {
                        guest_ever_started = true;
                        g_session.model.status_banner.clear();
                        if (g_session.gpu) g_session.gpu->PrewarmShaders();
                    } else {
                        const std::string err = bbx::bridge::LastStartError();
                        g_session.model.status_banner =
                            err.empty() ? "Bloodborne did not start (see loader.log)." : err;
                        RefreshLauncher();
                        bbx::platform::LogMessage(
                            bbx::platform::Level::kError,
                            "launcher: the guest did not start; staying in the launcher");
                    }
                } else {
                    g_session.pending_guest_req = req;
                    StartShaderBake(/*start_after=*/true, /*force_rebuild=*/false);
                }
            }
            if (bbx::bridge::Running() && !g_session.quick_menu_open && !g_session.ime_open &&
                g_session.gpu != nullptr) {
                (void)g_session.gpu->WaitForGuestFrame(8);
            } else if (g_session.baking) {
                ::Sleep(16);
            } else {
                ::Sleep(8);
            }
        }
        bbx::bridge::StopPadRumble();
        if (g_session.bake_thread.joinable()) {
            g_session.bake_progress.RequestCancel();
            g_session.bake_thread.join();
        }
        if (g_session.usb_sync_thread.joinable()) {
            g_session.usb_sync_thread.join();
        }
        bbx::platform::LogF("launcher: exit; self-test %s", self_test.Passed() ? "PASS" : "FAIL");
        if (guest_ever_started || bbx::bridge::Running()) {
            if (g_session.model.auto_backup_saves && !g_session.layout.base_path.empty()) {
                std::string detail;
                const bool synced = bbx::platform::BackupSavesAndLogsToUsb(
                    bbx::platform::Paths().local_state, bbx::platform::Local("data"),
                    g_session.layout.base_path, &detail);
                bbx::platform::LogMirroredMessage(
                    synced ? bbx::platform::Level::kInfo : bbx::platform::Level::kWarn,
                    "usb: exit auto-backup %s (%s)", synced ? "ok" : "skipped", detail.c_str());
            }
            LaunchReturnFrontendUri();
            // Normal UWP exit runs static destructors and DLL teardown while
            // detached guest threads are still running, hanging the console on
            // the last frame for minutes. Flush logs and terminate cleanly.
            Steps().Shutdown("guest exit");
            bbx::platform::LogFlush();
            bbx::platform::LogClose();
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }
        LaunchReturnFrontendUri();
        acquire_watch.Stop();
        telemetry.Stop();
        ShutdownDisplay();
        Steps().UnregisterThread(ui_tid);
        bbx::platform::LogFlush();
    }

    void Uninitialize() { Steps().LogUninitialize(); }
};

}  // namespace

namespace bbxbox {

// The activation object: the view-provider factory. This is what the manifest's
// EntryPoint="bbxbox.App" names.
struct App : winrt::implements<App, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    ~App() noexcept {
        try {
            Steps().Shutdown("App destructor");
            bbx::platform::LogFlush();
        } catch (...) {
        }
    }

    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() {
        (void)bbx::platform::StorageInit();
        (void)bbx::platform::LogOpen(bbx::platform::Paths().local_state);
        StartBootInstrumentation();
        Steps().LauncherViewCreated();
        return winrt::make<LauncherView>();
    }
};

// The factory the platform activates through the EntryPoint name. A packaged UWP
// process is started by activating its entry point, which for a C++ executable
// means the runtime calls DllGetActivationFactory (exported by bbxbox.def);
// wWinMain below is the other half, for the paths that run the image directly.
struct AppFactory : winrt::implements<AppFactory, winrt::Windows::Foundation::IActivationFactory> {
    winrt::hstring GetRuntimeClassName() const { return L"bbxbox.App"; }
    winrt::Windows::Foundation::IInspectable ActivateInstance() const {
        return winrt::make<App>();
    }
};

}  // namespace bbxbox

// Exported as DllGetActivationFactory by bbxbox.def: the packaged launch path
// asks for "bbxbox.App" and gets the IFrameworkViewSource above. The symbol is
// port-owned rather than C++/WinRT's WINRT_GetActivationFactory, whose declared
// signature in winrt/base.h differs (it takes an opaque class id).
extern "C" HRESULT WINAPI BbxGetActivationFactory(HSTRING class_id, void** factory) noexcept {
    try {
        *factory = nullptr;
        const wchar_t* const name = ::WindowsGetStringRawBuffer(class_id, nullptr);
        if (name != nullptr && std::wcscmp(name, L"bbxbox.App") == 0) {
            *factory = winrt::detach_abi(winrt::make<bbxbox::AppFactory>());
            return S_OK;
        }
        return winrt::hresult_class_not_available().to_abi();
    } catch (...) {
        return winrt::to_hresult();
    }
}

// Exported as DllCanUnloadNow by bbxbox.def: a packaged app's image is never
// unloaded, so the answer is always S_FALSE.
extern "C" HRESULT WINAPI BbxCanUnloadNow() noexcept { return S_FALSE; }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // The order here is the whole point of the function, and it is the order the
    // console taught: the app that was installed on a Series X left LocalState
    // empty because the *loader* failed (0xC0000135, VCRUNTIME140_APP.dll), and
    // nothing this file can do sees that. What it can do is guarantee that every
    // failure from the first instruction that *does* run onwards leaves a
    // `loader.log` behind, so the next launch is a file on the player's stick
    // instead of a black screen.
    //
    // The apartment comes first out of necessity, not preference: StorageInit
    // asks ApplicationData for LocalState, and a WinRT call without an apartment
    // fails with CO_E_NOTINITIALIZED. StorageInit records each probe into its
    // static list (with a kernel32-only fallback when LocalState is missing),
    // LogOpen opens loader.log on that path, and ReplayStorageResults writes the
    // recorded lines into it before anything else runs.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
        // An apartment already on this thread is not a launch failure.
    }
    const bbx::platform::StorageInitResult& storage_early = bbx::platform::StorageInit();
    const bool log_early = bbx::platform::LogOpen(bbx::platform::Paths().local_state);
    if (log_early) {
        if (storage_early.ok) {
            bbx::platform::LogMessage(bbx::platform::Level::kInfo,
                                      "wWinMain: localstate %s, storage ok",
                                      bbx::platform::Paths().local_state.c_str());
        } else {
            bbx::platform::LogMessage(
                bbx::platform::Level::kError,
                "wWinMain: localstate %s, %s (log open on a derived path)",
                bbx::platform::Paths().local_state.c_str(),
                bbx::platform::FormatStorageFailureLine(storage_early).c_str());
        }
    }
    StartBootInstrumentation();
    bbx::platform::InstallLastChanceLogger();
    try {
        winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<bbxbox::App>());
    } catch (const winrt::hresult_error& error) {
        // Before this existed, a launch that failed to activate was a window
        // that closed: the reason went to the debugger output and nowhere else.
        ReplayStorageOnce();
        bbx::platform::LogMessage(bbx::platform::Level::kError, "wWinMain: activation failed (0x%08x)",
                                  static_cast<unsigned>(error.code().value));
        Steps().Shutdown("return from wWinMain");
        bbx::platform::LogFlush();
        return 1;
    }
    Steps().Shutdown("return from wWinMain");
    bbx::platform::LogFlush();
    return 0;
}
