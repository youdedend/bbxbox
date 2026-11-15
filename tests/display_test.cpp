// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/display.h"

using namespace bbx::platform;

namespace {

TvMode Mode(std::uint32_t width, std::uint32_t height, float hz = 60.0f) {
    return TvMode{width, height, hz, true};
}

}  // namespace

TEST(FourKTvGetsAFourKSwapchain) {
    const OutputPlan plan = PlanOutput(1920, 1080, Mode(3840, 2160), true);
    CHECK_EQ(plan.swap_width, 3840u);
    CHECK_EQ(plan.swap_height, 2160u);
    CHECK(plan.scale == 2.0f);
    CHECK(plan.scaled());
    // The inverse keeps the picture in the panel.
    CHECK(plan.matrix[0] == 0.5f);
    CHECK(plan.matrix[3] == 0.5f);
    CHECK(!plan.clamped);
}

TEST(EightKIsClampedTo2160p) {
    const OutputPlan plan = PlanOutput(1920, 1080, Mode(7680, 4320), true);
    CHECK_EQ(plan.swap_width, 3840u);   // G8
    CHECK_EQ(plan.swap_height, 2160u);
    CHECK(plan.clamped);
}

TEST(MatchTvOffKeepsTheWindowPixels) {
    const OutputPlan plan = PlanOutput(1920, 1080, Mode(3840, 2160), false);
    CHECK_EQ(plan.swap_width, 1920u);
    CHECK(!plan.scaled());
    CHECK(plan.matrix[0] == 1.0f);
}

TEST(SmallerTvIsNotUpscaledByUs) {
    const OutputPlan plan = PlanOutput(1920, 1080, Mode(1280, 720), true);
    CHECK_EQ(plan.swap_width, 1920u);
    CHECK(plan.scale == 1.0f);
}

TEST(NoTvModeFallsBackToTheWindow) {
    const OutputPlan plan = PlanOutput(1920, 1080, TvMode{}, true);
    CHECK_EQ(plan.swap_width, 1920u);
    CHECK(plan.reason.find("unavailable") != std::string::npos || !plan.scaled());
    const OutputPlan empty = PlanOutput(0, 0, Mode(3840, 2160), true);
    CHECK_EQ(empty.swap_width, 1920u);  // default, not a zero-sized swapchain
}

TEST(DescribeOutputStatesTheSizes) {
    const TvMode tv = Mode(3840, 2160, 120.0f);
    const std::string text = DescribeOutput(PlanOutput(1920, 1080, tv, true), tv);
    CHECK(text.find("3840x2160") != std::string::npos);
    CHECK(text.find("120 Hz") != std::string::npos);
}
