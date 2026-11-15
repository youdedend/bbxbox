// SPDX-License-Identifier: GPL-3.0-or-later
// The WinRT half of display.h: the TV's current mode, and the composition scale
// the CoreWindow reports. Kept in its own translation unit so the planning
// arithmetic stays host-testable.
#include "xbox/platform/display.h"

#include "xbox/platform/log.h"

#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Foundation.h>

namespace bbx::platform {

TvMode QueryTvMode() {
    TvMode mode;
    try {
        const auto hdmi =
            winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
        if (hdmi == nullptr) {
            LogMessage(Level::kInfo, "display: HdmiDisplayInformation unavailable (not a console)");
            return mode;
        }
        const auto current = hdmi.GetCurrentDisplayMode();
        if (current == nullptr) return mode;
        mode.width = current.ResolutionWidthInRawPixels();
        mode.height = current.ResolutionHeightInRawPixels();
        mode.refresh_hz = current.RefreshRate();
        mode.valid = mode.width != 0 && mode.height != 0;
        LogF("display: TV mode %ux%u @ %.2f Hz", mode.width, mode.height, mode.refresh_hz);
    } catch (const winrt::hresult_error& error) {
        LogMessage(Level::kWarn, "display: TV mode query failed (0x%08x)",
                   static_cast<unsigned>(error.code().value));
    }
    return mode;
}

// The window's pixels include the composition scale (a console reports 100 %,
// a desktop can report 150 %); the caller multiplies bounds by this.
float CompositionScale() {
    try {
        const auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (window == nullptr) return 1.0f;
        const auto display = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        const float scale = display ? display.RawPixelsPerViewPixel() : 1.0f;
        return scale > 0.0f ? scale : 1.0f;
    } catch (const winrt::hresult_error&) {
        return 1.0f;
    }
}

}  // namespace bbx::platform
