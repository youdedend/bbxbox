// SPDX-License-Identifier: GPL-3.0-or-later
// Present mode, tearing and 60 Hz pacing.
//
// Three console rules, learned the hard way by every UWP port:
//   1. DXGI_PRESENT_ALLOW_TEARING is legal only when the swapchain was created
//      with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING *and* CheckFeatureSupport says
//      yes *and* vsync is off. Anything else is DXGI_ERROR_INVALID_CALL.
//   2. With vsync on, presenting more than once per refresh costs a frame.
//   3. At 60 Hz, a frame that misses 16.67 ms must not be followed by a second
//      missed deadline: catch up once, then settle.
#pragma once

#include <cstdint>

namespace bbx::d3d12 {

enum class PresentMode : std::uint8_t {
    kVsync = 0,      // FLIP_DISCARD, SyncInterval 1
    kTearing,        // FLIP_DISCARD, SyncInterval 0, ALLOW_TEARING
    kComposed,       // SyncInterval 0 without tearing (Xbox composition clock)
};

struct PresentConfig {
    bool vsync = true;
    bool tearing_supported = false;   // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    bool allow_tearing_setting = true;  // the player's "allow tearing" preference
    bool hdr = false;                 // affects only the colour space, not the mode
};

struct PresentDecision {
    PresentMode mode = PresentMode::kVsync;
    std::uint32_t sync_interval = 1;
    std::uint32_t flags = 0;   // DXGI_PRESENT_*; only ALLOW_TEARING is ever set
    bool swapchain_allow_tearing = false;  // the flag the swapchain must be created with
};

// The whole rule, in one function, with the reason in the log if a request is
// not honoured.
[[nodiscard]] PresentDecision DecidePresent(const PresentConfig& config,
                                            std::uint32_t refresh_hz = 60);

// Pacing. The swapchain's waitable object does the waiting; this decides
// whether to present, to skip (the compositor already has the previous frame)
// and how many deadlines have been missed.
class FramePacer {
public:
    FramePacer(std::uint32_t refresh_hz, std::uint32_t max_in_flight = 2);

    // `now_ns` is the monotonic clock at the top of the frame. Returns 1 when a
    // frame should be presented on this call and 0 when it is too early -- the
    // swapchain's waitable has not signalled, and presenting anyway would cost
    // the frame the previous present just bought. `late()` then says whether
    // the caller should skip the waitable wait for this frame (a deadline was
    // already missed: catch up once, do not compound it).
    std::uint32_t OnFrame(std::uint64_t now_ns);
    [[nodiscard]] bool late() const { return late_; }

    // Called after a present with the time it happened; used to learn the
    // compositor's actual period (some panels are 59.94 Hz).
    void OnPresented(std::uint32_t count, std::uint64_t now_ns);

    [[nodiscard]] std::uint64_t period_ns() const { return period_ns_; }
    [[nodiscard]] std::uint32_t missed() const { return missed_; }
    [[nodiscard]] std::uint32_t presented() const { return presented_; }

    void SetRefresh(std::uint32_t refresh_hz);

private:
    std::uint64_t period_ns_ = 16'666'667;
    std::uint64_t last_present_ns_ = 0;
    bool started_ = false;
    bool late_ = false;
    std::uint32_t max_in_flight_ = 2;
    std::uint32_t missed_ = 0;
    std::uint32_t presented_ = 0;
};

}  // namespace bbx::d3d12
