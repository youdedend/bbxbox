// SPDX-License-Identifier: GPL-3.0-or-later
// The console session (see d3d12_session.h): device, swapchain, pipelines, replay.
#include "xbox/platform/d3d12_session.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "src/host/d3d12/device.h"
#include "src/host/d3d12/pipeline.h"
#include "src/host/d3d12/replay.h"
#include "src/host/d3d12/swapchain.h"
#include "xbox/platform/bbhost_bridge.h"
#include "xbox/platform/guest_frames.h"
#include "xbox/platform/log.h"

namespace bbx::platform {
namespace {

std::string Narrow(const wchar_t* text) {
    if (text == nullptr) { return {}; }
    // Adapter descriptions are ASCII in practice; a character the C locale cannot
    // map becomes '?', which is better than a printf that truncates the line.
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) { return {}; }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

}  // namespace

struct D3d12Session::Impl {
    d3d12::Device device;
    d3d12::Swapchain swapchain;
    d3d12::PipelineSet pipelines;
    d3d12::Replay replay;
    OutputPlan plan;
    Hud hud{1920.0f, 1080.0f};
    GpuReport report;
    bool ready = false;

    // The guest's frame, triple buffered: the guest thread writes `frame_pixels`,
    // Present swaps it with `frame_upload` and copies that into the scanout image
    // outside the lock. Two buffers are enough to never wait for the third: the
    // third frame that arrives before Present takes one is dropped and counted.
    std::mutex frame_mutex;
    std::vector<std::uint8_t> frame_pixels;
    std::vector<std::uint8_t> frame_upload;
    std::uint32_t frame_width = 0, frame_height = 0;
    // The buffer's attribute, carried from the flip: the pixels are in the
    // guest's own layout (format, tiling, pitch), and this says which.
    d3d12::ScanoutDesc frame_desc;
    bool frame_pending = false;
    std::uint64_t guest_frames = 0, guest_frames_dropped = 0;

    [[nodiscard]] d3d12::DeviceDesc Desc(bool debug) const {
        d3d12::DeviceDesc desc{};
        desc.enable_debug_layer = debug;
        return desc;
    }

    void FillReport() {
        report.ready = ready;
        report.uma = device.IsUma();
        report.feature_level = "11_0";
        if (device.Adapter() != nullptr) {
            DXGI_ADAPTER_DESC1 adapter{};
            if (SUCCEEDED(device.Adapter()->GetDesc1(&adapter))) {
                report.adapter = Narrow(adapter.Description);
            }
        }
        if (report.adapter.empty()) { report.adapter = "unknown adapter"; }
        report.memory = device.DescribeMemory();
        char text[192];
        std::snprintf(text, sizeof(text), "%ux%u x%u%s%s", swapchain.Width(), swapchain.Height(),
                      swapchain.BufferCount(), plan.clamped ? ", clamped" : "",
                      plan.scaled() ? ", scaled by the panel" : "");
        report.swapchain = text;
    }
};

D3d12Session::D3d12Session() : impl_(std::make_unique<Impl>()) {}

D3d12Session::~D3d12Session() { Destroy(); }

bool D3d12Session::Ready() const { return impl_ != nullptr && impl_->ready; }

const GpuReport& D3d12Session::Report() const {
    static const GpuReport kMissing{};
    return impl_ != nullptr ? impl_->report : kMissing;
}

namespace {

// The session host_present hands frames to: the window's, which is the only one
// the swapchain belongs to. Null while no device is up, which is what the seam
// treats as "drop the frame".
D3d12Session* g_frame_target = nullptr;

void TakeGuestFrame(const bbx::platform::GuestFrame& frame) {
    if (g_frame_target != nullptr) { g_frame_target->SubmitGuestFrame(frame); }
}

}  // namespace

bbx::d3d12::Status D3d12Session::Create(IUnknown* core_window, std::uint32_t window_width,
                                        std::uint32_t window_height, bool debug_layer) {
    if (impl_ == nullptr) { return d3d12::Status::kUnavailable; }
    Impl& impl = *impl_;
    impl.ready = false;
    if (core_window == nullptr) {
        // Without a CoreWindow there is nothing to present into. The launcher
        // treats this as "no GPU path" and falls back to SDL's renderer.
        impl.report.status = d3d12::Status::kInvalidArgument;
        LogMessage(Level::kWarn, "d3d12: no CoreWindow; the SDL renderer stays in use");
        return impl.report.status;
    }

    impl.report.status = impl.device.Create(impl.Desc(debug_layer));
    if (impl.report.status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: device creation failed (%s)",
                   d3d12::StatusName(impl.report.status));
        impl.FillReport();
        return impl.report.status;
    }

    // The television's mode decides the swapchain's size; the window's pixels only
    // matter when the mode is unknown (display.h explains the clamp).
    impl.plan = PlanOutput(window_width, window_height, QueryTvMode(), /*allow_4k=*/true);
    LogMessage(Level::kInfo, "d3d12: output plan %s", impl.plan.reason.c_str());

    impl.report.status = impl.swapchain.Create(impl.device, core_window, impl.plan);
    if (impl.report.status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: swapchain creation failed (%s)",
                   d3d12::StatusName(impl.report.status));
        impl.device.Destroy();
        impl.FillReport();
        return impl.report.status;
    }

    impl.report.status = impl.pipelines.Create(impl.device);
    if (impl.report.status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: pipelines failed (%s)",
                   d3d12::StatusName(impl.report.status));
        impl.swapchain.Destroy();
        impl.device.Destroy();
        impl.FillReport();
        return impl.report.status;
    }

    impl.report.status = impl.replay.Create(impl.device, impl.swapchain, impl.pipelines);
    if (impl.report.status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: replay failed (%s)",
                   d3d12::StatusName(impl.report.status));
        impl.pipelines.Destroy();
        impl.swapchain.Destroy();
        impl.device.Destroy();
        impl.FillReport();
        return impl.report.status;
    }

    impl.ready = true;
    // The guest's flips find this session from now on (guest_frames.h): the
    // swapchain is up, so a frame handed over has somewhere to go.
    g_frame_target = this;
    SetGuestFrameSink(&TakeGuestFrame);
    impl.hud.Resize(static_cast<float>(impl.swapchain.Width()),
                    static_cast<float>(impl.swapchain.Height()));
    impl.FillReport();
    LogMessage(Level::kInfo, "d3d12: ready, %s, %s", impl.report.adapter.c_str(),
               impl.report.memory.c_str());
    // The self-test runs before the first present: a GPU that cannot advance a
    // fence is a launcher row, not a hang in the middle of a frame.
    RunSelftest();
    return impl.report.status;
}

bbx::d3d12::Status D3d12Session::RunSelftest() {
    if (impl_ == nullptr || !impl_->ready) { return d3d12::Status::kUnavailable; }
    Impl& impl = *impl_;
    const d3d12::Status status = impl.replay.RunStreamSelftest(64);
    impl.report.selftest_ok = impl.replay.SelftestPassed();
    impl.report.selftest = impl.replay.SelftestReport();
    if (status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: GPU self-test %s", impl.report.selftest.c_str());
        return status;
    }
    LogMessage(Level::kInfo, "d3d12: GPU self-test %s", impl.report.selftest.c_str());
    return status;
}

Hud D3d12Session::BuildReportHud() const {
    if (impl_ == nullptr) { return Hud(1920.0f, 1080.0f); }
    const Impl& impl = *impl_;
    Hud hud = impl.hud;
    hud.Clear();
    const float width = hud.Width();
    const float height = hud.Height();
    // A panel across the bottom third, scaled off the swapchain so the same layout
    // works at 1080p and at 2160p.
    const float scale = width >= 3000.0f ? 8.0f : 4.0f;
    const float margin = static_cast<float>(scale) * 6.0f;
    const float line = Hud::TextHeight(static_cast<std::uint32_t>(scale)) +
                       static_cast<float>(scale) * 3.0f;

    const std::uint32_t lines = 9;
    const float panel_height = line * static_cast<float>(lines) + margin;
    const float panel_y = height - panel_height - margin;
    hud.Panel(margin, panel_y, width - margin * 2.0f, panel_height, HudColor::Panel());

    float y = panel_y + margin * 0.5f;
    const float x = margin * 2.0f;
    const float row_width = width - margin * 4.0f;
    const auto s = static_cast<std::uint32_t>(scale);
    hud.Text(x, y, s, "BBXBOX", HudColor::White());
    hud.Text(x + Hud::TextWidth("BBXBOX  ", s), y, s, "BLOODBORNE ON XBOX SERIES",
             HudColor::Green());
    y += line;
    hud.Row(x, y, row_width, s, "ADAPTER", impl.report.adapter, HudColor::White());
    y += line;
    hud.Row(x, y, row_width, s, "FEATURE LEVEL", impl.report.feature_level, HudColor::White());
    y += line;
    hud.Row(x, y, row_width, s, "MEMORY", impl.report.memory, HudColor::White());
    y += line;
    hud.Row(x, y, row_width, s, "SWAPCHAIN", impl.report.swapchain, HudColor::White());
    y += line;
    hud.Status(x, y, s, impl.report.selftest_ok, impl.report.selftest);
    y += line;
    // The baked console shaders, and how many frames the guest has offered: the
    // two facts a live test reads off the television (StartGuest loads the cache
    // and logs the same line; host_present counts the flips).
    hud.Row(x, y, row_width, s, "SHADER CACHE", bridge::ShaderCacheLine(), HudColor::White());
    y += line;
    hud.Row(x, y, row_width, s, "GUEST FLIPS",
            std::to_string(static_cast<unsigned long long>(bridge::Flips())), HudColor::White());
    y += line;
    hud.Text(x, y, s, "LAUNCHER UI AND GUEST BOOT ARE P1B: THIS IS THE 4K GPU PATH",
             HudColor::Grey());
    return hud;
}

bbx::d3d12::Status D3d12Session::Present(const Hud& hud, bool vsync) {
    if (impl_ == nullptr || !impl_->ready) { return d3d12::Status::kUnavailable; }
    Impl& impl = *impl_;
    if (impl.device.DeviceLost()) {
        // A removed device is reported once and never presented into again.
        LogDeviceRemoved(static_cast<long>(impl.device.RemovedReason()));
        impl.ready = false;
        impl.report.ready = false;
        impl.report.status = d3d12::Status::kDeviceLost;
        LogMessage(Level::kError, "d3d12: device removed; the HUD stops here");
        return d3d12::Status::kDeviceLost;
    }
    LogCrumb("replay: begin frame");
    d3d12::Status status = impl.replay.BeginFrame();
    if (status != d3d12::Status::kOk) { return status; }
    // The guest's frame first, beneath everything: the swap is under the lock and
    // the copy is not, so the guest thread's flip costs it a memcpy.
    std::vector<std::uint8_t> frame;
    d3d12::ScanoutDesc frame_desc;
    {
        std::lock_guard<std::mutex> lk(impl.frame_mutex);
        if (impl.frame_pending) {
            impl.frame_upload.swap(impl.frame_pixels);
            frame.swap(impl.frame_upload);
            frame_desc = impl.frame_desc;
            impl.frame_pending = false;
        }
    }
    if (!frame.empty() && frame_desc.width != 0 && frame_desc.height != 0) {
        // The upload is the description's: the image is made in the guest's
        // format, and a tiled buffer is detiled on the GPU before the present
        // pass samples it.
        const d3d12::Status uploaded = impl.replay.UploadFrame(frame.data(), frame_desc);
        if (uploaded != d3d12::Status::kOk) {
            LogCrumb("replay: guest frame dropped");
            ++impl.guest_frames_dropped;
        } else {
            ++impl.guest_frames;
        }
        // It goes back to the pool for the next flip, with its capacity intact.
        std::lock_guard<std::mutex> lk(impl.frame_mutex);
        impl.frame_upload.swap(frame);
    }
    // The boot colour: near-black, so a missed quad is visible rather than hidden.
    status = impl.replay.DrawPresent(0.02f, 0.02f, 0.03f);
    if (status != d3d12::Status::kOk) { return status; }
    LogCrumb("replay: overlay");
    impl.replay.ResetOverlay();
    for (const HudQuad& quad : hud.Quads()) {
        // The HUD is all solid rectangles, which is the white 1x1 texture: the UVs
        // in the quad are already 0..1.
        const float rgba[4] = {quad.r, quad.g, quad.b, quad.a};
        impl.replay.AddOverlayQuad(quad.x, quad.y, quad.width, quad.height, quad.u0, quad.v0,
                                  quad.u1, quad.v1, rgba);
    }
    status = impl.replay.DrawOverlay();
    if (status != d3d12::Status::kOk) { return status; }
    return impl.replay.EndFrame(vsync);
}

void D3d12Session::SubmitGuestFrame(const bbx::platform::GuestFrame& frame) {
    if (impl_ == nullptr || frame.pixels == nullptr || frame.width == 0 || frame.height == 0 || frame.bytes == 0) {
        return;
    }
    // The console's words, decoded once here: a word this port has no name for
    // is a frame it cannot read, and a frame kept anyway would be presented as
    // the wrong picture on its first flip.
    d3d12::ScanoutFormat format = d3d12::ScanoutFormat::kA8R8G8B8;
    d3d12::ScanoutTiling tiling = d3d12::ScanoutTiling::kTile;
    if (!d3d12::ScanoutFormatFromWord(frame.format, format) ||
        !d3d12::ScanoutTilingFromWord(frame.tiling, tiling)) {
        std::lock_guard<std::mutex> lk(impl_->frame_mutex);
        ++impl_->guest_frames_dropped;
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->frame_mutex);
    // Only the newest survives: a frame the presenter has not taken yet is the
    // one this replaces, which is what a television shows anyway.
    if (impl_->frame_pending) { ++impl_->guest_frames_dropped; }
    // The copy is of the guest's own bytes - a tiled buffer is the same size, in
    // micro tiles - and the detile and the format are the GPU's work, not this
    // thread's: the guest is between two waits here.
    if (impl_->frame_pixels.size() != frame.bytes) { impl_->frame_pixels.resize(static_cast<std::size_t>(frame.bytes)); }
    std::memcpy(impl_->frame_pixels.data(), frame.pixels, static_cast<std::size_t>(frame.bytes));
    impl_->frame_width = frame.width;
    impl_->frame_height = frame.height;
    impl_->frame_desc.format = format;
    impl_->frame_desc.tiling = tiling;
    impl_->frame_desc.width = frame.width;
    impl_->frame_desc.height = frame.height;
    impl_->frame_desc.pitch_pixels = frame.pitch;
    impl_->frame_pending = true;
}

std::uint64_t D3d12Session::GuestFrames() const {
    if (impl_ == nullptr) { return 0; }
    std::lock_guard<std::mutex> lk(impl_->frame_mutex);
    return impl_->guest_frames;
}

bbx::d3d12::Status D3d12Session::OnModeChanged(std::uint32_t window_width,
                                               std::uint32_t window_height) {
    if (impl_ == nullptr || !impl_->ready) { return d3d12::Status::kUnavailable; }
    Impl& impl = *impl_;
    const OutputPlan plan = PlanOutput(window_width, window_height, QueryTvMode(), true);
    const d3d12::Status status = impl.swapchain.Resize(plan);
    if (status != d3d12::Status::kOk) {
        LogMessage(Level::kError, "d3d12: resize failed (%s)", d3d12::StatusName(status));
        return status;
    }
    impl.plan = plan;
    impl.hud.Resize(static_cast<float>(impl.swapchain.Width()),
                    static_cast<float>(impl.swapchain.Height()));
    impl.FillReport();
    return status;
}

void D3d12Session::Destroy() {
    if (impl_ == nullptr) { return; }
    // The guest's flips go back to being dropped: the swapchain is going away.
    if (g_frame_target == this) {
        g_frame_target = nullptr;
        SetGuestFrameSink(nullptr);
    }
    impl_->ready = false;
    impl_->replay.Destroy();
    impl_->pipelines.Destroy();
    impl_->swapchain.Destroy();
    impl_->device.Destroy();
}

}  // namespace bbx::platform
