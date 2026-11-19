// SPDX-License-Identifier: GPL-3.0-or-later
// The WinRT half of display.h: the TV's current mode, and the composition scale
// the CoreWindow reports. Kept in its own translation unit so the planning
// arithmetic stays host-testable.
#include "xbox/platform/display.h"

#include "xbox/platform/log.h"

#include <cmath>
#include <windows.h>
#include <gamingdeviceinformation.h>

#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Foundation.h>

namespace bbx::platform {

ConsoleModel QueryConsoleModel() {
    GAMING_DEVICE_MODEL_INFORMATION info{};
    if (FAILED(::GetGamingDeviceModelInformation(&info))) {
        return ConsoleModel::kUnknown;
    }
    if (info.vendorId != GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
        return ConsoleModel::kUnknown;
    }
    switch (info.deviceId) {
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
        return ConsoleModel::kXboxOne;
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
        return ConsoleModel::kXboxOneS;
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
        return ConsoleModel::kXboxOneX;
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
        return ConsoleModel::kXboxSeriesS;
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
        return ConsoleModel::kXboxSeriesX;
    default:
        return ConsoleModel::kUnknown;
    }
}

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
        mode.refresh_hz = static_cast<float>(current.RefreshRate());
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
        const auto display = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        const float scale = display ? static_cast<float>(display.RawPixelsPerViewPixel()) : 1.0f;
        return (std::isfinite(scale) && scale > 0.0f) ? scale : 1.0f;
    } catch (const winrt::hresult_error&) {
        return 1.0f;
    }
}

}  // namespace bbx::platform
