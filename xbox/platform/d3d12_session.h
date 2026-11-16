// SPDX-License-Identifier: GPL-3.0-or-later
// The console's device, swapchain, pipelines and replay, owned as one object and
// handed to the launcher as a small interface: create it with the CoreWindow, ask
// it for a boot report, draw a HUD frame.
//
// Why this is its own translation unit rather than living in main.cpp: main.cpp
// is C++/WinRT (activation, view provider, winrt types) which no host compiler can
// check. Everything here takes an `IUnknown*` and Windows SDK headers only, so
// tools/syntax_check.sh compiles it on every precheck, and the WinRT part of the
// wiring stays two lines wide.
#pragma once

#include <memory>
#include <string>

#include "src/host/d3d12/bbx_d3d12.h"
#include "xbox/platform/display.h"
#include "xbox/platform/guest_frames.h"
#include "xbox/platform/hud.h"

namespace bbx::platform {

// What the launcher's boot report says about the GPU, in the strings it prints.
// A failed creation is not an exception: the report carries the reason and the
// session stays unusable, so the launcher falls back to SDL's renderer.
struct GpuReport {
    bbx::d3d12::Status status = bbx::d3d12::Status::kUnavailable;
    bool ready = false;
    std::string adapter;         // the TV's adapter, as DXGI names it
    std::string feature_level;   // always 11_0: the console's floor
    std::string memory;          // the budget's own accounting
    std::string swapchain;       // size, buffer count, the plan's reason
    std::string selftest;        // the GPU self-test's verdict
    std::string guest;           // the guest's memory plan and compute dispatches
    bool uma = false;
    bool selftest_ok = false;
};

class D3d12Session {
public:
    D3d12Session();
    ~D3d12Session();
    D3d12Session(const D3d12Session&) = delete;
    D3d12Session& operator=(const D3d12Session&) = delete;

    // `core_window` is the CoreWindow's ABI pointer (`winrt::get_abi`). `window_*`
    // are the window's physical pixels; the swapchain is sized from them and the
    // television's mode by display.h's plan.
    bbx::d3d12::Status Create(IUnknown* core_window, std::uint32_t window_width,
                              std::uint32_t window_height, bool debug_layer = false);
    void Destroy();
    [[nodiscard]] bool Ready() const;

    // Runs the boot self-test once. Called by Create; exposed for the "run it
    // again" path the dev menu will have.
    bbx::d3d12::Status RunSelftest();

    [[nodiscard]] const GpuReport& Report() const;
    // The boot report and the self-test's row, laid out for the current size.
    [[nodiscard]] Hud BuildReportHud() const;

    // Draws one HUD frame and presents it. `hud` is consumed as it is.
    bbx::d3d12::Status Present(const Hud& hud, bool vsync = true);

    // The guest's scanout frame, handed over from the guest thread on its flip
    // (host_present): copied here and uploaded on the next Present, which keeps
    // only the newest. A memcpy and a short lock are all the guest thread pays,
    // so a flip never waits on the GPU, and a frame the presenter has not taken
    // yet is counted and dropped rather than queued behind it.
    void SubmitGuestFrame(const bbx::platform::GuestFrame& frame);

    // Frames the guest offered and the presenter took (and how many it dropped
    // because the guest outran it), for the HUD.
    [[nodiscard]] std::uint64_t GuestFrames() const;
    // A television mode change: re-plan, resize the swapchain, resize the HUD.
    bbx::d3d12::Status OnModeChanged(std::uint32_t window_width, std::uint32_t window_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bbx::platform
