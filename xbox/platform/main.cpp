// SPDX-License-Identifier: GPL-3.0-or-later
// P0: the shell. A WinRT view owns the CoreWindow, SDL3-uwp provides the window,
// input, audio and gamepads, and the launcher runs on that thread until the
// player starts the game (P1 hands the same thread to bbhost's guest).
//
// Why the view is C++/WinRT and not SDL's own app class: SDL3-uwp's app class is
// C++/CX (/ZW) and the port's translation units are clang-cl (bbhost's guest
// thunks need __attribute__((sysv_abi))). SDL3 is still the platform layer - the
// window, the gamepad, the audio device and the text renderer all come from it -
// it is only the activation object that the port provides. SDL3 is initialised
// explicitly (`SDL_Init`), so there is no SDL main to set ready.
#include <windows.h>

// HSTRING and WindowsGetStringRawBuffer: the activation export takes a raw
// HSTRING, so the string ABI has to be visible.
#include <winstring.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "xbox/platform/d3d12_session.h"
#include "xbox/platform/dump.h"
#include "xbox/platform/guest_memory.h"
#include "xbox/platform/ini_file.h"
#include "xbox/platform/launcher.h"
#include "xbox/platform/log.h"
#include "xbox/platform/memory_selftest.h"
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
    std::string usb_token;
    SDL_Window* window = nullptr;
    // The console path: device, 4K swapchain, pipelines, replay and the HUD. When
    // it comes up the SDL renderer is never created, because two renderers on one
    // CoreWindow fight over the backbuffer. A failed creation leaves it null and
    // the launcher draws with SDL exactly as before.
    std::unique_ptr<bbx::platform::D3d12Session> gpu;
    SDL_Renderer* renderer = nullptr;
    SDL_Gamepad* pad = nullptr;
};

Session g_session;

bbx::platform::ButtonMask MaskFromPad(SDL_Gamepad* pad) {
    using bbx::platform::Bit;
    bbx::platform::ButtonMask mask = 0;
    if (pad == nullptr) return mask;
    const auto down = [pad](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
    if (down(SDL_GAMEPAD_BUTTON_SOUTH)) mask |= Bit(Pad::kSouth);
    if (down(SDL_GAMEPAD_BUTTON_EAST)) mask |= Bit(Pad::kEast);
    if (down(SDL_GAMEPAD_BUTTON_WEST)) mask |= Bit(Pad::kWest);
    if (down(SDL_GAMEPAD_BUTTON_NORTH)) mask |= Bit(Pad::kNorth);
    if (down(SDL_GAMEPAD_BUTTON_BACK)) mask |= Bit(Pad::kView);
    if (down(SDL_GAMEPAD_BUTTON_START)) mask |= Bit(Pad::kMenu);
    if (down(SDL_GAMEPAD_BUTTON_LEFT_STICK)) mask |= Bit(Pad::kLeftStick);
    if (down(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) mask |= Bit(Pad::kRightStick);
    if (down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) mask |= Bit(Pad::kLeftBumper);
    if (down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) mask |= Bit(Pad::kRightBumper);
    if (down(SDL_GAMEPAD_BUTTON_DPAD_UP)) mask |= Bit(Pad::kDpadUp);
    if (down(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) mask |= Bit(Pad::kDpadDown);
    if (down(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) mask |= Bit(Pad::kDpadLeft);
    if (down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) mask |= Bit(Pad::kDpadRight);
    return mask;
}

void SaveLayoutSettings() {
    const std::string path = bbx::platform::Local("bbxbox.ini");
    const bool keyed = bbx::platform::IniSet(g_session.ini_text, "dump", "path", g_session.layout.base_path) &&
                       bbx::platform::IniSet(g_session.ini_text, "dump", "patch_path", g_session.layout.patch_path) &&
                       bbx::platform::IniSet(g_session.ini_text, "dump", "use_patch", g_session.layout.use_patch ? "1" : "0");
    if (!keyed) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not key the layout settings");
    }
    if (!bbx::platform::WriteText(path, g_session.ini_text)) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "could not write %s", path.c_str());
    }
}

void RefreshLauncher() {
    g_session.model.layout = g_session.layout;
    g_session.model.has_usb = !g_session.usb_token.empty();
    g_session.model.Rebuild();
}

void PickDump() {
    std::string path;
    std::string token;
    if (!bbx::platform::PickFolderGrant(path, token)) {
        bbx::platform::LogMessage(bbx::platform::Level::kWarn, "dump picker cancelled");
        return;
    }
    bbx::platform::LogF("dump folder granted: %s", path.c_str());
    const bbx::platform::DumpFs fs = bbx::platform::MakeDumpFs();
    g_session.layout = bbx::platform::DetectLayout(
        fs, path, bbx::platform::IniGet(g_session.ini, "dump.patch_path", ""),
        bbx::platform::IniFlag(g_session.ini, "dump.use_patch", true), false);
    bbx::platform::LogMessage(bbx::platform::Level::kInfo, "%s",
                              bbx::platform::LayoutLogLine(g_session.layout).c_str());
    if (!g_session.layout.layout_ok()) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "dump rejected: %s",
                                  g_session.layout.problem.c_str());
    }
    SaveLayoutSettings();
    RefreshLauncher();
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

    switch (action) {
    case PadAction::kNavigateUp:
        g_session.model.Move(-1);
        if (g_session.pad) SDL_RumbleGamepad(g_session.pad, 0x2000, 0x2000, 40);
        return;
    case PadAction::kNavigateDown:
        g_session.model.Move(1);
        if (g_session.pad) SDL_RumbleGamepad(g_session.pad, 0x2000, 0x2000, 40);
        return;
    case PadAction::kMenu:
        g_session.keyboard_open = true;
        g_session.keyboard.Reset();
        return;
    case PadAction::kOpenPcSettings:
        bbx::platform::LogF("PC settings menu requested (L3+R3)");
        if (g_session.pad) SDL_RumbleGamepad(g_session.pad, 0x8000, 0x8000, 120);
        return;
    case PadAction::kPersonalEffects:
        bbx::platform::LogF("personal effects requested (touchpad)");
        return;
    case PadAction::kGestures:
        bbx::platform::LogF("gestures requested (touchpad + left)");
        return;
    case PadAction::kConfirm: {
        const bbx::platform::Action selected = g_session.model.Activate();
        bbx::platform::LogF("launcher: %s", bbx::platform::ActionName(selected));
        switch (selected) {
        case bbx::platform::Action::kPickDump:
        case bbx::platform::Action::kPickPatch: PickDump(); return;
        case bbx::platform::Action::kTogglePatch:
            g_session.layout.use_patch = !g_session.layout.use_patch;
            g_session.layout.effective_ver =
                g_session.layout.CanRunPatched() ? g_session.layout.patch.Version()
                                                 : g_session.layout.base.Version();
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

void DrawLauncher();

// One frame of whichever renderer owns the window. The GPU path draws the boot
// report; a present that fails (a removed device) falls back to SDL once and logs
// why, rather than presenting into a dead swapchain every frame.
void DrawFrame() {
    if (g_session.gpu) {
        const bbx::platform::Hud hud = g_session.gpu->BuildReportHud();
        const bbx::d3d12::Status status = g_session.gpu->Present(hud);
        if (status == bbx::d3d12::Status::kOk) return;
        bbx::platform::LogF("d3d12: present failed (%s); SDL takes over",
                            bbx::d3d12::StatusName(status));
        g_session.gpu->Destroy();
        g_session.gpu.reset();
        if (g_session.renderer == nullptr && g_session.window != nullptr) {
            g_session.renderer = SDL_CreateRenderer(g_session.window, nullptr);
            if (g_session.renderer == nullptr) {
                bbx::platform::LogF("SDL_CreateRenderer after D3D12: %s", SDL_GetError());
            }
        }
    }
    DrawLauncher();
}

void DrawLauncher() {
    if (g_session.renderer == nullptr) return;
    const std::string caption = g_session.model.Title();
    SDL_SetRenderDrawColor(g_session.renderer, 12, 14, 18, 255);
    SDL_RenderClear(g_session.renderer);

    float y = 24.0f;
    SDL_SetRenderDrawColor(g_session.renderer, 226, 226, 226, 255);
    SDL_RenderDebugText(g_session.renderer, 24.0f, y, caption.c_str());
    y += 26.0f;

    for (const std::string& line : WrapLines(g_session.model.Footer())) {
        SDL_RenderDebugText(g_session.renderer, 24.0f, y, line.c_str());
        y += 16.0f;
    }
    y += 12.0f;

    if (g_session.keyboard_open) {
        SDL_SetRenderDrawColor(g_session.renderer, 220, 200, 120, 255);
        const std::string name = "Hunter name: " + g_session.keyboard.Text();
        SDL_RenderDebugText(g_session.renderer, 24.0f, y, name.c_str());
        y += 24.0f;
        for (std::size_t row = 0; row < g_session.keyboard.RowCount(); ++row) {
            std::string label = g_session.keyboard.Row(row);
            if (row == g_session.keyboard.CursorRow()) {
                const std::size_t column = g_session.keyboard.CursorColumn() * 2;
                if (column < label.size()) label[column] = '>';
            }
            SDL_SetRenderDrawColor(g_session.renderer, 200, 200, 200, 255);
            SDL_RenderDebugText(g_session.renderer, 48.0f, y, label.c_str());
            y += 14.0f;
        }
    } else {
        for (std::size_t i = 0; i < g_session.model.items.size(); ++i) {
            const bbx::platform::Item& item = g_session.model.items[i];
            const bool selected = i == g_session.model.selected;
            const bool enabled = item.enabled;
            SDL_SetRenderDrawColor(g_session.renderer, enabled ? (selected ? 255 : 190) : 90,
                                    enabled ? (item.dangerous ? 180 : (selected ? 255 : 190)) : 90,
                                    enabled ? (selected ? 255 : 190) : 90, 255);
            const std::string text = std::string(selected ? "> " : "  ") + item.label;
            SDL_RenderDebugText(g_session.renderer, 24.0f, y, text.c_str());
            y += 18.0f;
        }
    }
    SDL_RenderPresent(g_session.renderer);
}

bool InitSdl() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "SDL_Init: %s", SDL_GetError());
        return false;
    }
    g_session.window = SDL_CreateWindow("bbxbox", 1920, 1080, SDL_WINDOW_FULLSCREEN);
    if (g_session.window == nullptr) {
        bbx::platform::LogMessage(bbx::platform::Level::kError, "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    // The console's own path first: the D3D12 device, a swapchain sized for the
    // television, and the HUD. Only when it cannot come up does the SDL renderer
    // exist at all.
    {
        const winrt::Windows::UI::Core::CoreWindow core_window =
            winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        IUnknown* window_abi = nullptr;
        std::uint32_t window_width = 1920;
        std::uint32_t window_height = 1080;
        if (core_window) {
            const winrt::Windows::Foundation::Rect bounds = core_window.Bounds();
            if (bounds.Width >= 320.0f && bounds.Height >= 240.0f) {
                window_width = static_cast<std::uint32_t>(bounds.Width);
                window_height = static_cast<std::uint32_t>(bounds.Height);
            }
            window_abi = static_cast<IUnknown*>(winrt::get_abi(core_window));
        }
        g_session.gpu = std::make_unique<bbx::platform::D3d12Session>();
        const bbx::d3d12::Status status = g_session.gpu->Create(window_abi, window_width,
                                                               window_height, false);
        if (status == bbx::d3d12::Status::kOk) {
            const bbx::platform::GpuReport& report = g_session.gpu->Report();
            bbx::platform::LogF("d3d12: %s, %s, self-test %s", report.adapter.c_str(),
                                report.swapchain.c_str(), report.selftest.c_str());
        } else {
            bbx::platform::LogF("d3d12: unavailable (%s); the SDL renderer stays in use",
                                bbx::d3d12::StatusName(status));
            g_session.gpu.reset();
        }
    }
    // No GPU path means the launcher needs a renderer; when the GPU path works, a
    // renderer here would draw over the swapchain's backbuffer.
    if (g_session.gpu == nullptr) {
        g_session.renderer = SDL_CreateRenderer(g_session.window, nullptr);
        if (g_session.renderer == nullptr) {
            bbx::platform::LogMessage(bbx::platform::Level::kError, "SDL_CreateRenderer: %s",
                                      SDL_GetError());
            return false;
        }
    }
    int pads = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&pads);
    if (ids != nullptr && pads > 0) g_session.pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    // A null renderer is not an error any more: that is the GPU path, where the
    // swapchain owns the window.
    const char* renderer_name =
        g_session.renderer ? SDL_GetRendererName(g_session.renderer) : nullptr;
    bbx::platform::LogF("SDL: window %s, gamepads %d", renderer_name ? renderer_name : "d3d12",
                        g_session.pad ? 1 : 0);
    return true;
}

void ShutdownSdl() {
    if (g_session.gpu) {
        // Waits for the GPU before the device and the swapchain go away: a released
        // swapchain with a frame in flight is a device-removed in the log.
        bbx::platform::LogF("d3d12: shutting down; %s", g_session.gpu->Report().selftest.c_str());
        g_session.gpu->Destroy();
        g_session.gpu.reset();
    }
    if (g_session.pad) SDL_CloseGamepad(g_session.pad);
    if (g_session.renderer) SDL_DestroyRenderer(g_session.renderer);
    if (g_session.window) SDL_DestroyWindow(g_session.window);
    SDL_Quit();
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
}

void RestoreDump() {
    const std::string token = bbx::platform::IniGet(g_session.ini, "dump.token", "");
    std::string path = bbx::platform::IniGet(g_session.ini, "dump.path", "");
    if (!token.empty()) {
        std::string granted;
        if (bbx::platform::ResolveGrant(token, granted)) path = granted;
    }
    g_session.layout = bbx::platform::DetectLayout(
        bbx::platform::MakeDumpFs(), path, bbx::platform::IniGet(g_session.ini, "dump.patch_path", ""),
        bbx::platform::IniFlag(g_session.ini, "dump.use_patch", true), false);
    bbx::platform::LogMessage(bbx::platform::Level::kInfo, "%s",
                              bbx::platform::LayoutLogLine(g_session.layout).c_str());
}

}  // namespace

namespace {

struct LauncherView : winrt::implements<LauncherView, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};

    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&) {
        bbx::platform::InstallLastChanceLogger();
        if (!bbx::platform::StorageInit()) {
            bbx::platform::LogMessage(bbx::platform::Level::kError, "storage init failed");
        }
        if (!bbx::platform::LogOpen(bbx::platform::Paths().local_state)) {
            // Without a log the run is still worth doing; the crash handler's
            // OutputDebugStringA path remains.
        }
        bbx::platform::LogMessage(bbx::platform::Level::kInfo, "bbxbox launcher starting");
        LoadSettings();
        RestoreDump();
        RefreshLauncher();
    }

    void SetWindow(const winrt::Windows::UI::Core::CoreWindow& value) { window = value; }

    void Load(winrt::hstring) {}

    void Run() {
        if (!InitSdl()) {
            bbx::platform::LogMessage(bbx::platform::Level::kError, "SDL unavailable; exiting");
            bbx::platform::LogClose();
            return;
        }
        const bbx::platform::SelfTestResult self_test = bbx::platform::RunMemorySelfTest(false);

        bbx::platform::ButtonMask previous = 0;
        while (!g_session.quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) g_session.quit = true;
                if (event.type == SDL_EVENT_GAMEPAD_ADDED && g_session.pad == nullptr) {
                    g_session.pad = SDL_OpenGamepad(event.gdevice.which);
                }
                if (event.type == SDL_EVENT_KEY_DOWN && g_session.keyboard_open) {
                    if (event.key.key == SDLK_BACKSPACE) g_session.keyboard.Backspace();
                    else if (event.key.key == SDLK_RETURN)
                        OnAction(PadAction::kConfirm);
                    else if (event.key.key >= 0x20 && event.key.key < 0x7f)
                        g_session.keyboard.Append(static_cast<char>(event.key.key));
                }
            }

            const bbx::platform::ButtonMask held = MaskFromPad(g_session.pad);
            bbx::platform::PadEdges edges;
            edges.held = held;
            edges.pressed = held & ~previous;
            edges.released = previous & ~held;
            previous = held;
            const PadAction action = g_session.chords.Update(edges);
            if (action != PadAction::kNone) OnAction(action);

            DrawFrame();

            if (g_session.start_requested) {
                bbx::platform::LogF("launcher: start requested (version %s)",
                                    g_session.layout.EffectiveVersion().c_str());
                // P1 replaces this with bbhost's guest boot on this thread.
                bbx::platform::LogMessage(bbx::platform::Level::kWarn,
                                          "guest boot is P1: the launcher stays up");
                g_session.start_requested = false;
            }
            SDL_Delay(8);
        }
        bbx::platform::LogF("launcher: exit; self-test %s", self_test.Passed() ? "PASS" : "FAIL");
        ShutdownSdl();
        bbx::platform::LogFlush();
        bbx::platform::LogClose();
    }

    void Uninitialize() {}
};

}  // namespace

namespace bbxbox {

// The activation object: the view-provider factory. This is what the manifest's
// EntryPoint="bbxbox.App" names.
struct App : winrt::implements<App, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() {
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
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<bbxbox::App>());
    return 0;
}
