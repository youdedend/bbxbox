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
    bool clamped = false;   // the TV mode was larger than 3840x2160
    std::string reason;     // why the plan is what it is, for the log

    [[nodiscard]] bool scaled() const { return scale > 1.0f + 1e-3f; }
};

// `window_pixels` is the CoreWindow's size in physical pixels (bounds x
// composition scale). `match_tv` is the player's "Output resolution: match TV".
[[nodiscard]] OutputPlan PlanOutput(std::uint32_t window_width, std::uint32_t window_height,
                                    const TvMode& tv, bool match_tv);

// The device-side query. Returns an invalid TvMode off-console.
[[nodiscard]] TvMode QueryTvMode();

// The plan in one log line.
[[nodiscard]] std::string DescribeOutput(const OutputPlan& plan, const TvMode& tv);

}  // namespace bbx::platform
