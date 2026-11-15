// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_present.h"

#include <algorithm>

namespace bbx::d3d12 {
namespace {
// DXGI_PRESENT_ALLOW_TEARING. Repeated rather than included so this file
// compiles (and its test runs) without the SDK.
constexpr std::uint32_t kDxgiPresentAllowTearing = 0x200;
}  // namespace

PresentDecision DecidePresent(const PresentConfig& config, std::uint32_t refresh_hz) {
    PresentDecision d;
    const bool want_tearing = !config.vsync && config.allow_tearing_setting;
    if (want_tearing && config.tearing_supported) {
        d.mode = PresentMode::kTearing;
        d.sync_interval = 0;
        d.flags = kDxgiPresentAllowTearing;
        d.swapchain_allow_tearing = true;
        return d;
    }
    if (!config.vsync) {
        // Tearing was asked for but the display path does not support it (or
        // the player turned it off). SyncInterval 0 without the flag is legal
        // only in composition; it is the safe "as fast as the compositor
        // allows" mode.
        d.mode = PresentMode::kComposed;
        d.sync_interval = 0;
        d.flags = 0;
        d.swapchain_allow_tearing = false;
        return d;
    }
    d.mode = PresentMode::kVsync;
    d.sync_interval = 1;
    d.flags = 0;
    d.swapchain_allow_tearing = false;
    (void)refresh_hz;
    return d;
}

FramePacer::FramePacer(std::uint32_t refresh_hz, std::uint32_t max_in_flight)
    : max_in_flight_(std::max(1u, max_in_flight)) {
    SetRefresh(refresh_hz);
}

void FramePacer::SetRefresh(std::uint32_t refresh_hz) {
    if (refresh_hz == 0) refresh_hz = 60;
    period_ns_ = 1'000'000'000ull / refresh_hz;
}

std::uint32_t FramePacer::OnFrame(std::uint64_t now_ns) {
    if (!started_) {
        started_ = true;
        last_present_ns_ = now_ns;
        late_ = false;
        return 1;
    }
    const std::uint64_t since = now_ns > last_present_ns_ ? now_ns - last_present_ns_ : 0;
    // A frame that arrives before the compositor's period (with a small
    // tolerance for clock and scheduling jitter) must not be presented: the
    // swapchain would block on its waitable and the present would be charged to
    // the next period.
    if (since + period_ns_ / 8 < period_ns_) {
        late_ = false;
        return 0;
    }
    // One deadline missed: present now without waiting for the next, so the
    // stutter does not double. Two or more: count it, the frame is late anyway.
    late_ = since >= period_ns_ * 3 / 2;
    if (since >= 2 * period_ns_) ++missed_;
    return 1;
}

void FramePacer::OnPresented(std::uint32_t count, std::uint64_t now_ns) {
    if (count == 0) return;
    presented_ += count;
    // Learn the real period: a 59.94 Hz panel is 16.683 ms, and pacing to
    // 16.667 there produces a periodic missed deadline.
    if (started_ && now_ns > last_present_ns_) {
        const std::uint64_t observed = now_ns - last_present_ns_;
        if (observed > period_ns_ / 2 && observed < period_ns_ * 3 / 2) {
            period_ns_ = (period_ns_ * 7 + observed) / 8;  // slow, stable average
        }
    }
    started_ = true;
    last_present_ns_ = now_ns;
    (void)max_in_flight_;
}

}  // namespace bbx::d3d12
