// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/display.h"

#include <algorithm>
#include <string>

namespace bbx::platform {

OutputPlan PlanOutput(std::uint32_t window_width, std::uint32_t window_height, const TvMode& tv,
                      bool match_tv) {
    OutputPlan plan;
    if (window_width == 0 || window_height == 0) {
        plan.reason = "window has no pixels; keeping the default size";
        return plan;
    }
    plan.swap_width = window_width;
    plan.swap_height = window_height;
    if (!match_tv || !tv.valid || tv.width == 0 || tv.height == 0) {
        plan.reason = match_tv ? "TV mode unavailable; the picture keeps the window's pixels"
                               : "output resolution is 1080p; the picture keeps the window's pixels";
        return plan;
    }

    // G8: a console loses its device when the swapchain is larger than 4K.
    constexpr std::uint32_t kMaxWidth = 3840;
    constexpr std::uint32_t kMaxHeight = 2160;
    plan.swap_width = std::min(tv.width, kMaxWidth);
    plan.swap_height = std::min(tv.height, kMaxHeight);
    plan.clamped = plan.swap_width != tv.width || plan.swap_height != tv.height;

    // The scale is horizontal: HDMI modes keep 16:9, and a per-axis scale would
    // stretch the picture if a panel reported otherwise.
    plan.scale = static_cast<float>(plan.swap_width) / static_cast<float>(window_width);
    if (plan.scale < 1.0f) {
        // The TV is smaller than the window: the console downscales, the
        // swapchain stays at the window's size.
        plan.swap_width = window_width;
        plan.swap_height = window_height;
        plan.scale = 1.0f;
        plan.reason = "TV mode is not larger than the window; no scaling";
    } else {
        plan.reason = "swapchain matches the TV mode; inverse scale keeps it in the panel";
        if (plan.clamped) plan.reason += " (clamped to 3840x2160, G8)";
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
