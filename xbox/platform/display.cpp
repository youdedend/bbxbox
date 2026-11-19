// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/display.h"

#include <algorithm>
#include <string>

namespace bbx::platform {

ConsoleCaps ConsoleCapsFor(ConsoleModel model) {
    switch (model) {
    case ConsoleModel::kXboxOne:
        return ConsoleCaps{model, "Xbox One", 1920, 1080, 2};
    case ConsoleModel::kXboxOneS:
        return ConsoleCaps{model, "Xbox One S", 1920, 1080, 2};
    case ConsoleModel::kXboxSeriesS:
        // Xbox Series S can output a 4K HDMI mode, but creating a 3840x2160 UWP
        // swapchain crashes on console hardware; cap safely at 1440p.
        return ConsoleCaps{model, "Xbox Series S", 2560, 1440, 1};
    case ConsoleModel::kXboxOneX:
        return ConsoleCaps{model, "Xbox One X", 3840, 2160, 1};
    case ConsoleModel::kXboxSeriesX:
        return ConsoleCaps{model, "Xbox Series X", 3840, 2160, 0};
    case ConsoleModel::kUnknown:
    default:
        return ConsoleCaps{ConsoleModel::kUnknown, "Xbox", 3840, 2160, 0};
    }
}

OutputPlan PlanOutput(std::uint32_t window_width, std::uint32_t window_height, const TvMode& tv,
                      bool match_tv, std::uint32_t max_width, std::uint32_t max_height) {
    OutputPlan plan;
    if (window_width == 0 || window_height == 0) {
        plan.reason = "window has no pixels; keeping the default size";
        return plan;
    }
    // G8: never allow a swapchain larger than 4K or larger than the console cap.
    const std::uint32_t cap_w =
        max_width == 0 ? 3840u : std::min<std::uint32_t>(max_width, 3840u);
    const std::uint32_t cap_h =
        max_height == 0 ? 2160u : std::min<std::uint32_t>(max_height, 2160u);

    plan.swap_width = std::min(window_width, cap_w);
    plan.swap_height = std::min(window_height, cap_h);
    plan.clamped = (plan.swap_width != window_width || plan.swap_height != window_height);
    if (!match_tv || !tv.valid || tv.width == 0 || tv.height == 0) {
        plan.reason = match_tv ? "TV mode unavailable; the picture keeps the window's pixels"
                               : "output resolution is 1080p; the picture keeps the window's pixels";
        return plan;
    }

    plan.swap_width = std::min(tv.width, cap_w);
    plan.swap_height = std::min(tv.height, cap_h);
    plan.clamped = plan.swap_width != tv.width || plan.swap_height != tv.height;

    // The scale is horizontal: HDMI modes keep 16:9, and a per-axis scale would
    // stretch the picture if a panel reported otherwise.
    plan.scale = static_cast<float>(plan.swap_width) / static_cast<float>(window_width);
    if (plan.scale < 1.0f) {
        // The TV is smaller than the window: the console downscales, the
        // swapchain stays at the window's size (still bounded by the console cap).
        plan.swap_width = std::min(window_width, cap_w);
        plan.swap_height = std::min(window_height, cap_h);
        plan.scale = 1.0f;
        plan.reason = "TV mode is not larger than the window; no scaling";
    } else {
        plan.reason = "swapchain matches the TV mode; inverse scale keeps it in the panel";
        if (plan.clamped) {
            plan.reason += " (clamped to " + std::to_string(cap_w) + "x" +
                           std::to_string(cap_h) + ", G8)";
        }
    }
    // SetMatrixTransform takes the inverse of the panel scale.
    const float inverse = plan.scale > 0.0f ? 1.0f / plan.scale : 1.0f;
    plan.matrix[0] = inverse;
    plan.matrix[1] = 0.0f;
    plan.matrix[2] = 0.0f;
    plan.matrix[3] = inverse;
    return plan;
}

std::string DescribeOutput(const OutputPlan& plan, const TvMode& tv) {
    std::string text = "output: window ";
    text += std::to_string(plan.swap_width) + "x" + std::to_string(plan.swap_height);
    text += " swapchain, scale " + std::to_string(plan.scale);
    if (tv.valid) {
        text += ", TV " + std::to_string(tv.width) + "x" + std::to_string(tv.height) + " @ " +
                std::to_string(static_cast<int>(tv.refresh_hz + 0.5f)) + " Hz";
    }
    if (!plan.reason.empty()) text += "; " + plan.reason;
    return text;
}

}  // namespace bbx::platform
