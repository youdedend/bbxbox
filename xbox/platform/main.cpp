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
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

// No <SDL3/SDL.h>: nothing in this translation unit initialises SDL, opens an
// SDL window, or draws with SDL_Renderer. The pad comes from WinRT (MaskFromPad)
// and the frame from the D3D12 session.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "xbox/platform/boot_steps.h"
#include "xbox/platform/d3d12_session.h"
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
    bool keyboard_open = false;
    bool quit = false;
    bool start_requested = false;
    bool baking = false;
    std::atomic<bool> bake_finished{false};
    bbx::platform::BakeProgressState bake_progress;
    bbx::platform::BakeResult bake_result;
    std::thread bake_thread;
    bbx::bridge::GuestRequest pending_guest_req;
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
std::atomic<bool> g_app_visible{true};
std::atomic<std::uint32_t> g_visibility_events{0};
std::atomic<bool> g_display_acquire_entered{false};
std::atomic<bool> g_display_ready{false};
std::atomic<const char*> g_ui_blocker{"none"};
// Why the display is not up, for the gate line. A pointer to a static string, so
// the 1 Hz watcher thread can read it without a lock and without touching a
// std::string that ui-main is assigning.
std::atomic<const char*> g_display_fault{""};
std::atomic<std::uint64_t> g_present_count{0};

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
    using winrt::Windows::Gaming::Input::Gamepad;
    using winrt::Windows::Gaming::Input::GamepadButtons;
    bbx::platform::ButtonMask mask = 0;
    try {
        const auto pads = Gamepad::Gamepads();
        if (pads.Size() == 0) return 0;
        const GamepadButtons buttons = pads.GetAt(0).GetCurrentReading().Buttons;
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
    } catch (...) {
    }
    return mask;
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

void RefreshLauncher() {
    g_session.model.layout = g_session.layout;
    g_session.model.has_usb = !g_session.usb_token.empty();
    RefreshModsOverlay();
    g_session.model.Rebuild();
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
    RefreshLauncher();
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

void OnAction(PadAction action) {
    if (g_session.keyboard_open) {
        switch (action) {
        case PadAction::kNavigateUp: g_session.keyboard.Move(0, -1); return;
        case PadAction::kNavigateDown: g_session.keyboard.Move(0, 1); return;
        case PadAction::kNavigateLeft: g_session.keyboard.Move(-1, 0); return;
        case PadAction::kNavigateRight: g_session.keyboard.Move(1, 0); return;
        case PadAction::kConfirm:
            if (!g_session.keyboard.Accept()) {
                bbx::platform::LogF("hunter name: \"%s\"", g_session.keyboard.Text().c_str());
                if (!bbx::platform::WriteText(bbx::platform::Local("hunter-name.txt"),
                                              g_session.keyboard.Text())) {
                    bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not write hunter-name.txt");
                }
                g_session.keyboard_open = false;
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
        g_session.model.Move(-1);
        return;
    case PadAction::kNavigateDown:
        g_session.model.Move(1);
        return;
    case PadAction::kMenu:
        g_session.keyboard_open = true;
        g_session.keyboard.Reset();
        return;
    case PadAction::kOpenPcSettings:
        bbx::platform::LogF("PC settings menu requested (L3+R3)");
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
            RefreshLauncher();
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
        case bbx::platform::Action::kMemorySelfTest: {
            const bbx::platform::SelfTestResult result =
                bbx::platform::RunMemorySelfTest(false);
            bbx::platform::LogF("self-test %s", result.Passed() ? "PASS" : "FAIL");
            return;
        }
        case bbx::platform::Action::kStartGame:
        case bbx::platform::Action::kContinueUnpatched: g_session.start_requested = true; return;
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

std::vector<std::string> WrapLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

void AppendLauncherToHud(bbx::platform::Hud& hud) {
    if (bbx::bridge::Flips() != 0) return;
    const float width = hud.Width();
    const std::uint32_t scale = width >= 3000.0f ? 6u : 3u;
    const float margin = static_cast<float>(scale) * 8.0f;
    const float line = bbx::platform::Hud::TextHeight(scale) + static_cast<float>(scale) * 3.0f;

    const std::vector<std::string> footer_lines = WrapLines(g_session.model.Footer());
    const std::size_t body_rows =
        g_session.baking
            ? 6u
            : (g_session.keyboard_open ? (1 + g_session.keyboard.RowCount()) : g_session.model.items.size());
    const float panel_height =
        line * static_cast<float>(2 + footer_lines.size() + body_rows) + margin;
    hud.Panel(margin, margin, width - margin * 2.0f, panel_height, bbx::platform::HudColor::Panel());

    float y = margin + static_cast<float>(scale) * 4.0f;
    const float x = margin * 1.5f;
    hud.Text(x, y, scale, g_session.model.Title(), bbx::platform::HudColor::White());
    y += line * 1.2f;

    for (const std::string& info : footer_lines) {
        hud.Text(x, y, scale, info, bbx::platform::HudColor::Grey());
        y += line;
    }
    y += line * 0.4f;

    if (g_session.keyboard_open) {
        hud.Text(x, y, scale, "HUNTER NAME: " + g_session.keyboard.Text(),
                 bbx::platform::HudColor::Amber());
        y += line;
        for (std::size_t row = 0; row < g_session.keyboard.RowCount(); ++row) {
            std::string label = g_session.keyboard.Row(row);
            if (row == g_session.keyboard.CursorRow()) {
                const std::size_t column = g_session.keyboard.CursorColumn() * 2;
                if (column < label.size()) label[column] = '>';
            }
            hud.Text(x + margin * 0.5f, y, scale, label, bbx::platform::HudColor::White());
            y += line;
        }
    } else {
        for (std::size_t i = 0; i < g_session.model.items.size(); ++i) {
            const bbx::platform::Item& item = g_session.model.items[i];
            const bool selected = (i == g_session.model.selected);
            bbx::platform::HudColor colour = bbx::platform::HudColor::Grey();
            if (item.enabled) {
                if (item.dangerous) {
                    colour = bbx::platform::HudColor::Amber();
                } else if (selected) {
                    colour = bbx::platform::HudColor::Green();
                } else {
                    colour = bbx::platform::HudColor::White();
                }
            }
            const std::string label = std::string(selected ? "> " : "  ") + item.label;
            hud.Text(x, y, scale, label, colour);
            y += line;
        }
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
        const bbx::d3d12::Status status = g_session.gpu->Present(hud);
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
            if (bounds.Width >= 320.0f && bounds.Height >= 240.0f) {
                window_width = static_cast<std::uint32_t>(bounds.Width);
                window_height = static_cast<std::uint32_t>(bounds.Height);
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
    try {
        winrt_pads = winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size();
    } catch (const winrt::hresult_error& err) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kWarn,
            "input: Windows.Gaming.Input threw 0x%08x while listing gamepads",
            static_cast<unsigned>(err.code().value));
    }
    bbx::platform::LogMirroredMessage(bbx::platform::Level::kInfo,
                                      "input: winrt_gamepads=%u (no SDL input stack)", winrt_pads);
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
    if (!bbx::platform::ReadText(path, text) || text.empty()) {
        text = bbx::platform::DefaultIni();
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

    add_candidate(path);
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

    std::vector<std::string> found_dumps;
    for (const std::string& candidate : candidates) {
        if (!bbx::platform::IsDumpRoot(fs, candidate)) continue;
        const std::optional<bbx::platform::SfoInfo> info =
            bbx::platform::ReadDumpInfo(fs, candidate);
        if (info && !info->IsPatchContainer()) {
            found_dumps.push_back(candidate);
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
}

}  // namespace

namespace {

struct LauncherView : winrt::implements<LauncherView, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    winrt::event_token closed_token_{};
    winrt::event_token visibility_token_{};
    winrt::event_token key_down_token_{};
    winrt::event_token key_up_token_{};

    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& app) {
        const auto init_tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
        std::uint32_t expected = 0;
        (void)g_view_thread_id.compare_exchange_strong(expected, init_tid,
                                                       std::memory_order_relaxed);

        app.Activated([](const auto&, const auto&) {
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

        key_down_token_ = window.KeyDown(
            [](const auto&, const winrt::Windows::UI::Core::KeyEventArgs& ev) {
                const auto vk = static_cast<unsigned>(ev.VirtualKey());
                const unsigned repeat = ev.KeyStatus().WasKeyDown ? 1u : 0u;
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kInfo, "view: KeyDown (vk=%u, repeat=%u)", vk, repeat);
            });

        key_up_token_ = window.KeyUp(
            [](const auto&, const winrt::Windows::UI::Core::KeyEventArgs& ev) {
                const auto vk = static_cast<unsigned>(ev.VirtualKey());
                bbx::platform::LogMirroredMessage(
                    bbx::platform::Level::kInfo, "view: KeyUp (vk=%u)", vk);
            });

        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kInfo,
            "view: SetWindow (stored CoreWindow=%p + agile_ref on tid=%u, initial_visible=%u; "
            "registered Closed, VisibilityChanged, KeyDown, KeyUp)",
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
        DrawFrame();
        const bbx::platform::SelfTestResult self_test = bbx::platform::RunMemorySelfTest(false);

        bbx::platform::ButtonMask previous = 0;
        bool guest_ever_started = false;
        bool emergency_holding = false;
        std::chrono::steady_clock::time_point emergency_hold_start{};
        const bbx::platform::ButtonMask kEmergencyQuitMask =
            bbx::platform::Bit(Pad::kView) | bbx::platform::Bit(Pad::kMenu);

        while (!g_session.quit && !g_window_closed.load(std::memory_order_relaxed)) {
            if (stored_window) {
                stored_window.Dispatcher().ProcessEvents(
                    winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            }
            if (g_window_closed.load(std::memory_order_relaxed)) break;

            // The pad is WinRT's; there is no SDL event queue on this target.
            const bbx::platform::ButtonMask held = MaskFromPad();
            if (bbx::bridge::Running()) {
                // Once the guest is running, do not feed gameplay buttons into
                // the launcher menu (pressing Menu/D-pad/A in-game would open
                // the virtual keyboard or activate hidden launcher items).
                // Keep only a 3-second View+Menu emergency exit chord.
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
                previous = held;
            } else if (g_session.baking) {
                emergency_holding = false;
                previous = held;
                if (g_session.bake_finished.load(std::memory_order_acquire)) {
                    if (g_session.bake_thread.joinable()) g_session.bake_thread.join();
                    g_session.baking = false;
                    g_session.bake_finished.store(false, std::memory_order_relaxed);
                    bbx::platform::LogMirroredMessage(
                        g_session.bake_result.ok ? bbx::platform::Level::kInfo
                                                 : bbx::platform::Level::kWarn,
                        "launcher: first-boot shader bake finished (%s): %s",
                        g_session.bake_result.ok ? "ok" : "incomplete",
                        g_session.bake_result.summary.c_str());
                    if (!g_session.bake_result.cancelled) {
                        if (bbx::bridge::StartGuest(g_session.pending_guest_req)) {
                            guest_ever_started = true;
                        } else {
                            bbx::platform::LogMessage(
                                bbx::platform::Level::kError,
                                "launcher: the guest did not start after baking; staying in the launcher");
                        }
                    }
                }
            } else {
                emergency_holding = false;
                bbx::platform::PadEdges edges;
                edges.held = held;
                edges.pressed = held & ~previous;
                edges.released = previous & ~held;
                previous = held;
                const PadAction action = g_session.chords.Update(edges);
                if (action != PadAction::kNone) OnAction(action);
                PollDumpPick();
            }

            DrawFrame();

            if (g_session.start_requested && !g_session.baking) {
                bbx::platform::LogF("launcher: start requested (version %s)",
                                    g_session.layout.EffectiveVersion().c_str());
                g_session.start_requested = false;
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
                req.modded_eboot_ok = g_session.model.modded_eboot_ok;

                std::vector<std::string> cache_dirs;
                cache_dirs.push_back(eboot_dir);
                if (req.app0 != eboot_dir) cache_dirs.push_back(req.app0);
                cache_dirs.push_back(req.data);
                cache_dirs.push_back(bbx::platform::Paths().local_state);
                std::string found_cache_dir;
                std::string found_cache_report;
                if (bbx::platform::HasValidConsoleCache(cache_dirs, &found_cache_dir,
                                                        &found_cache_report)) {
                    bbx::platform::LogF(
                        "launcher: valid DXIL 6.4 cache found in %s (%s); skipping DXC load",
                        found_cache_dir.c_str(), found_cache_report.c_str());
                    if (bbx::bridge::StartGuest(req)) {
                        guest_ever_started = true;
                    } else {
                        bbx::platform::LogMessage(
                            bbx::platform::Level::kError,
                            "launcher: the guest did not start; staying in the launcher");
                    }
                } else {
                    bbx::platform::LogMirroredMessage(
                        bbx::platform::Level::kInfo,
                        "launcher: no valid DXIL 6.4 cache found; starting one-time native Xbox shader bake into %s",
                        req.data.c_str());
                    g_session.pending_guest_req = req;
                    g_session.bake_progress.Reset();
                    g_session.bake_result = bbx::platform::BakeResult{};
                    g_session.bake_finished.store(false, std::memory_order_relaxed);
                    g_session.baking = true;

                    bbx::platform::BakeRequest bake_req;
                    bake_req.app0 = req.app0;
                    if (g_session.layout.CanRunPatched()) {
                        bake_req.update_dir = g_session.layout.patch_path;
                    }
                    bake_req.eboot = req.eboot;
                    bake_req.out_dir = req.data;

                    g_session.bake_thread = std::thread([bake_req]() {
                        const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
                        (void)pthread_setname_np(pthread_self(), "shader-baker");
                        Steps().RegisterThread(tid, "shader-baker");
                        g_session.bake_result =
                            bbx::platform::BakeConsoleShaders(bake_req, g_session.bake_progress);
                        Steps().UnregisterThread(tid);
                        g_session.bake_finished.store(true, std::memory_order_release);
                    });
                }
            }
            ::Sleep(8);
        }
        if (g_session.bake_thread.joinable()) {
            g_session.bake_progress.RequestCancel();
            g_session.bake_thread.join();
        }
        bbx::platform::LogF("launcher: exit; self-test %s", self_test.Passed() ? "PASS" : "FAIL");
        if (guest_ever_started || bbx::bridge::Running()) {
            // Normal UWP exit runs static destructors and DLL teardown while
            // detached guest threads are still running, hanging the console on
            // the last frame for minutes. Flush logs and terminate cleanly.
            Steps().Shutdown("guest exit");
            bbx::platform::LogFlush();
            bbx::platform::LogClose();
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }
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
