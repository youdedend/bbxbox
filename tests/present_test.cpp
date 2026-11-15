// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_present.h"

using namespace bbx::d3d12;

TEST(VsyncIsTheDefaultAndNeverTears) {
    PresentConfig config;  // vsync on
    config.tearing_supported = true;
    const PresentDecision d = DecidePresent(config);
    CHECK(d.mode == PresentMode::kVsync);
    CHECK_EQ(d.sync_interval, 1u);
    CHECK_EQ(d.flags, 0u);
    CHECK(!d.swapchain_allow_tearing);
}

TEST(TearingNeedsSupportAndVsyncOff) {
    PresentConfig config;
    config.vsync = false;
    config.tearing_supported = true;
    const PresentDecision d = DecidePresent(config);
    CHECK(d.mode == PresentMode::kTearing);
    CHECK_EQ(d.sync_interval, 0u);
    CHECK_EQ(d.flags, 0x200u);
    CHECK(d.swapchain_allow_tearing);
}

TEST(NoTearingFlagWithoutSupport) {
    PresentConfig config;
    config.vsync = false;
    config.tearing_supported = false;
    const PresentDecision d = DecidePresent(config);
    CHECK(d.mode == PresentMode::kComposed);
    CHECK_EQ(d.flags, 0u);
    CHECK(!d.swapchain_allow_tearing);
}

TEST(PlayerCanTurnTearingOff) {
    PresentConfig config;
    config.vsync = false;
    config.tearing_supported = true;
    config.allow_tearing_setting = false;
    const PresentDecision d = DecidePresent(config);
    CHECK(d.mode == PresentMode::kComposed);
    CHECK_EQ(d.flags, 0u);
}

TEST(PacerPresentsOncePerPeriod) {
    FramePacer pacer(60);
    CHECK_EQ(pacer.period_ns(), 16'666'666ull);
    // First frame always presents.
    CHECK_EQ(pacer.OnFrame(0), 1u);
    pacer.OnPresented(1, 0);
    // 16 ms later: on time.
    CHECK_EQ(pacer.OnFrame(16'000'000), 1u);
    pacer.OnPresented(1, 16'000'000);
    // 33 ms after 16 ms: 17 ms, past the period.
    CHECK_EQ(pacer.OnFrame(33'000'000), 1u);
    pacer.OnPresented(1, 33'000'000);
    CHECK_EQ(pacer.missed(), 0u);
    CHECK_EQ(pacer.presented(), 3u);
}

TEST(PacerCatchesUpOneMissOnly) {
    FramePacer pacer(60, 2);
    pacer.OnFrame(0);
    pacer.OnPresented(1, 0);
    pacer.OnFrame(16'000'000);
    pacer.OnPresented(1, 16'000'000);
    // 40 ms later: a deadline was missed. Present straight away (no waitable
    // wait) so the miss is not compounded, and count it.
    CHECK_EQ(pacer.OnFrame(56'000'000), 1u);
    CHECK(pacer.late());
    CHECK_EQ(pacer.missed(), 1u);
}

TEST(PacerSkipsEarlyFrames) {
    FramePacer pacer(60, 2);
    pacer.OnFrame(0);
    pacer.OnPresented(1, 0);
    // 2 ms after a present is far too early: the waitable has not signalled.
    CHECK_EQ(pacer.OnFrame(2'000'000), 0u);
    CHECK(!pacer.late());
}

TEST(PacerLearnsTheRealPeriod) {
    FramePacer pacer(60);
    pacer.OnFrame(0);
    pacer.OnPresented(1, 0);
    // A 59.94 Hz display: present every 16.683 ms.
    std::uint64_t now = 0;
    for (int i = 0; i < 60; ++i) {
        now += 16'683'000;
        pacer.OnFrame(now);
        pacer.OnPresented(1, now);
    }
    CHECK(pacer.period_ns() > 16'670'000ull);
    CHECK(pacer.period_ns() < 16'700'000ull);
}
