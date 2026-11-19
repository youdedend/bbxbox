// SPDX-License-Identifier: GPL-3.0-or-later
// The picture at the TV's size, not the window's (guard G8).
//
// On Xbox the app's CoreWindow is 1920x1080 whatever mode the TV is in, so a 1080p
// swapchain is upscaled by the console and a 4K-capable title never shows 4K. The
// verified fix (KytyPS5 `uwp/src/app.cpp`) asks `HdmiDisplayInformation` for the
// TV's current mode, sizes the swapchain to it, and applies the inverse of the
// window/panel scale as the swapchain's matrix transform so the picture lands on
// the panel 1:1. Sizes above 3840x2160 are clamped: a console loses its device when
// asked for more (G8).
//
// The arithmetic is here, pure and tested; `display_winrt.cpp` does the query.
#pragma once

#include <cstdint>
#include <string>

namespace bbx::platform {

enum class ConsoleModel : std::uint8_t {
    kUnknown = 0,
    kXboxOne,
    kXboxOneS,
    kXboxOneX,
    kXboxSeriesS,
    kXboxSeriesX,
};

struct ConsoleCaps {
    ConsoleModel model = ConsoleModel::kUnknown;
    const char* name = "Xbox";
    std::uint32_t max_swap_width = 3840;
    std::uint32_t max_swap_height = 2160;
    // Default internal guest render resolution mode (0=1080p, 1=900p, 2=720p).
    int default_render_res_mode = 0;
};

struct TvMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float refresh_hz = 0.0f;
    bool valid = false;
};

struct OutputPlan {
    std::uint32_t swap_width = 1920;
    std::uint32_t swap_height = 1080;
    // Composition/window -> swapchain scale. 1.0 means "the window's pixels".
    float scale = 1.0f;
    // What `IDXGISwapChain2::SetMatrixTransform` wants: the inverse scale, plus
    // offset in exchange units (0 here: the picture is centred by the panel).
    float matrix[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    bool clamped = false;   // the TV mode exceeded the console's safe swapchain cap
    std::string reason;     // why the plan is what it is, for the log

    [[nodiscard]] bool scaled() const { return scale > 1.0f + 1e-3f; }
};

// Safe per-console swapchain limits (from XboxEmulationHub/XBSX2):
// Xbox Series S can drive a 4K HDMI signal, but creating a 3840x2160 UWP
// swapchain crashes the process, so Series S is capped at 2560x1440.
[[nodiscard]] ConsoleCaps ConsoleCapsFor(ConsoleModel model);

// `window_pixels` is the CoreWindow's size in physical pixels (bounds x
// composition scale). `match_tv` is the player's "Output resolution: match TV".
// `max_width`/`max_height` enforce the console's safe swapchain ceiling.
[[nodiscard]] OutputPlan PlanOutput(std::uint32_t window_width, std::uint32_t window_height,
                                    const TvMode& tv, bool match_tv,
                                    std::uint32_t max_width = 3840,
                                    std::uint32_t max_height = 2160);

// The device-side queries. Return safe defaults off-console.
[[nodiscard]] ConsoleModel QueryConsoleModel();
[[nodiscard]] TvMode QueryTvMode();
[[nodiscard]] float CompositionScale();

// The plan in one log line.
[[nodiscard]] std::string DescribeOutput(const OutputPlan& plan, const TvMode& tv);

}  // namespace bbx::platform
