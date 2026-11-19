// SPDX-License-Identifier: GPL-3.0-or-later
// The console session (see d3d12_session.h): device, swapchain, pipelines, replay.
#include "xbox/platform/d3d12_session.h"

#include "hle/modules.h"
#include "src/host/d3d12/bbx_descriptors.h"
#include "src/host/d3d12/bbx_guest_mem.h"
#include "src/host/d3d12/bbx_guest_root.h"
#include "src/host/d3d12/host_gpu_d3d12.h"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "src/host/d3d12/device.h"
#include "src/host/d3d12/pipeline.h"
#include "src/host/d3d12/replay.h"
#include "src/host/d3d12/swapchain.h"
#include "xbox/platform/bbhost_bridge.h"
#include "xbox/platform/guest_compute.h"
#include "xbox/platform/guest_frames.h"
#include "xbox/platform/guest_graphics.h"
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

    // The guest's memory (Slice 5B-1): the page table and the lazily resident
    // slabs, and the binding host/gpu.h's implementation sees. The hooks are
    // static functions over this Impl, installed at Create and cleared at
    // Destroy, so a boot without a device falls back to the stub's answers.
    d3d12::GuestPlan guest_plan;
    d3d12::GuestMemory guest_memory;
    d3d12::GuestGpuBinding guest_binding;
    std::string guest_memory_line;
    // The guest arena's descriptors, reserved once at Create: space 3's 33 and
    // three stage regions of 191 (bbx_guest_root.h's GuestHeapLayout). One slice
    // for the CBV/SRV/UAV heap, one for the samplers, both permanent: the guest
    // heap is fixed, and a dispatch rewrites slots in place.
    std::unique_ptr<d3d12::DescriptorRing> guest_arena;
    std::unique_ptr<d3d12::DescriptorRing> guest_sampler_arena;
    std::uint32_t guest_arena_base = 0, guest_sampler_base = 0;
    GuestCompute guest_compute;
    GuestGraphics guest_graphics;

    [[nodiscard]] d3d12::DeviceDesc Desc(bool debug) const {
        d3d12::DeviceDesc desc{};
        desc.enable_debug_layer = debug;
        return desc;
    }

    // The binding's hooks. Each one is the device's own answer, or a plain false
    // when the thing it names does not exist: a null hook is never installed.
    static std::uint64_t FenceSubmitted(void* user) { return static_cast<Impl*>(user)->device.SubmittedValue(); }
    static std::uint64_t FenceCompleted(void* user) { return static_cast<Impl*>(user)->device.CompletedValue(); }
    static std::uint64_t FenceSignal(void* user) { return static_cast<Impl*>(user)->device.Signal(); }
    static bool FenceWait(void* user, std::uint64_t value, std::uint64_t timeout_ms) {
        // Device::WaitFor takes a 32-bit timeout: a longer ask is a series of
        // them, and a zero ask is "is it done", which the caller has asked.
        Impl& impl = *static_cast<Impl*>(user);
        while (true) {
            const auto slice = static_cast<std::uint32_t>(timeout_ms > 0xffffffffull ? 0xffffffffull : timeout_ms);
            if (impl.device.WaitFor(value, slice)) return true;
            if (impl.device.DeviceLost() || timeout_ms <= 0xffffffffull) return false;
            timeout_ms -= slice;
        }
    }
    static bool MemWrite(void* user, std::uint64_t va, const void* data, std::size_t bytes) {
        Impl& impl = *static_cast<Impl*>(user);
        return impl.guest_memory.WriteGuest(impl.guest_plan.table, va, data, bytes) == d3d12::Status::kOk;
    }
    static bool MemFill(void* user, std::uint64_t va, std::uint32_t value, std::size_t bytes) {
        Impl& impl = *static_cast<Impl*>(user);
        return impl.guest_memory.FillGuest(impl.guest_plan.table, va, value, bytes) == d3d12::Status::kOk;
    }
    static bool MemCopy(void* user, std::uint64_t dst, std::uint64_t src, std::size_t bytes) {
        Impl& impl = *static_cast<Impl*>(user);
        return impl.guest_memory.CopyGuest(impl.guest_plan.table, dst, src, bytes) == d3d12::Status::kOk;
    }
    static bool ReadGuest(void* user, std::uint64_t va, void* out, std::size_t bytes) {
        Impl& impl = *static_cast<Impl*>(user);
        return impl.guest_memory.ReadGuest(impl.guest_plan.table, va, out, bytes) == d3d12::Status::kOk;
    }
    // The pipeline halves (guest_compute.cpp + guest_graphics.cpp): compute keys
    // and dispatches, plus graphics shader creation notes and draw execution.
    static std::uint64_t DispatchKey(void* user, const GpuDispatch& d) {
        return static_cast<Impl*>(user)->guest_compute.KeyFor(d);
    }
    static bool Dispatch(void* user, std::uint64_t key, const GpuDispatch& d) {
        return static_cast<Impl*>(user)->guest_compute.Run(key, d);
    }
    static void NoteShaderCreated(void* user, int stage, const std::uint8_t* container, std::size_t size) {
        static_cast<Impl*>(user)->guest_graphics.NoteShaderCreated(stage, container, size);
    }
    static bool Draw(void* user, const GpuDraw& d, const d3d12::DrawIdentity& identity) {
        return static_cast<Impl*>(user)->guest_graphics.Draw(d, identity);
    }
    static bool ClearView(void* user, const d3d12::DecodedClearView& view, bool apply, bool* out_created) {
        return static_cast<Impl*>(user)->guest_graphics.ClearView(view, apply, out_created);
    }
    static bool ClearTarget(void* user, std::uint64_t va, std::uint32_t value, std::size_t bytes) {
        return static_cast<Impl*>(user)->guest_graphics.ClearTargetAt(va, value, bytes);
    }
    static bool CopyTarget(void* user, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) {
        return static_cast<Impl*>(user)->guest_graphics.CopyTarget(dst_va, src_va, bytes);
    }
    static void ForgetRegion(void* user, std::uint64_t lo, std::uint64_t hi) {
        static_cast<Impl*>(user)->guest_graphics.ForgetRegion(lo, hi);
    }
    static void TextureWritten(void* user, std::uint64_t va, std::size_t bytes) {
        Impl& impl = *static_cast<Impl*>(user);
        if (va != 0 && bytes != 0 && hle_kernel_va_mapped(va, bytes)) {
            (void)impl.guest_memory.WriteGuest(
                impl.guest_plan.table, va,
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), bytes);
        }
        impl.guest_graphics.InvalidatePrograms(va, va + bytes);
    }
    static bool MemoryGlance(void* user, GpuMemoryGlance* out) {
        if (out == nullptr) return false;
        const Impl& impl = *static_cast<const Impl*>(user);
        const d3d12::Budget& b = impl.device.Memory();
        out->vram = b.used();
        out->cache = b.used_by(d3d12::Owner::kShaderTools) + b.used_by(d3d12::Owner::kHeaps);
        out->shared = b.used_by(d3d12::Owner::kGuest) + b.used_by(d3d12::Owner::kGpuShadows);
        out->cache_known = true;
        out->integrated = impl.device.IsUma();
        return true;
    }
    static std::string MemoryBudgetReport(void* user) {
        return static_cast<const Impl*>(user)->device.DescribeMemory();
    }
    static std::string DeviceIdentity(void* user) {
        return static_cast<const Impl*>(user)->report.adapter;
    }

    // Installs the binding the guest's HLE calls through (host_gpu_d3d12.h).
    void InstallGuestBinding() {
        guest_binding = d3d12::GuestGpuBinding{};
        guest_binding.user = this;
        guest_binding.fence_submitted = &Impl::FenceSubmitted;
        guest_binding.fence_completed = &Impl::FenceCompleted;
        guest_binding.fence_signal = &Impl::FenceSignal;
        guest_binding.fence_wait = &Impl::FenceWait;
        guest_binding.mem_write = &Impl::MemWrite;
        guest_binding.mem_fill = &Impl::MemFill;
        guest_binding.mem_copy = &Impl::MemCopy;
        guest_binding.read_guest = &Impl::ReadGuest;
        guest_binding.texture_written = &Impl::TextureWritten;
        guest_binding.memory_glance = &Impl::MemoryGlance;
        guest_binding.memory_budget_report = &Impl::MemoryBudgetReport;
        guest_binding.device_identity = &Impl::DeviceIdentity;
        if (guest_compute.Ready()) {
            guest_binding.dispatch_key = &Impl::DispatchKey;
            guest_binding.dispatch = &Impl::Dispatch;
        }
        if (guest_graphics.Ready()) {
            guest_binding.note_shader_created = &Impl::NoteShaderCreated;
            guest_binding.draw = &Impl::Draw;
            guest_binding.clear_view = &Impl::ClearView;
            guest_binding.clear_target = &Impl::ClearTarget;
            guest_binding.copy_target = &Impl::CopyTarget;
            guest_binding.forget_region = &Impl::ForgetRegion;
        }
        guest_binding.ready = ready && guest_memory.Ready();
        d3d12::SetGuestGpuBinding(&guest_binding);
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
        report.guest = guest_memory_line + " | " + guest_compute.Report() + " | " + guest_graphics.Report();
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
        // Without a CoreWindow there is nothing to present into. There is no SDL
        // fallback on this target: the launcher keeps its window up and reports
        // this line once a second from its gate line.
        impl.report.status = d3d12::Status::kInvalidArgument;
        LogMessage(Level::kError,
                   "d3d12: no CoreWindow was stored in SetWindow, so there is nothing to "
                   "create a swapchain for; there is no SDL fallback on this target");
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
    // The guest's memory: the console's own windows (5 GiB of RAM and the GPU's
    // 4 GiB alias over it) in 256 MiB slabs, and the two tables the shaders walk
    // (bbx_guest_mem.h). A plan that does not fit is a boot report line, not a
    // failed device: the rest of the port still runs.
    impl.guest_plan = d3d12::PlanGuestMemory();
    impl.guest_memory_line = impl.guest_plan.Report();
    // The guest arena's descriptors first: space 3 (the page table and 32 slabs)
    // and one region per stage, out of the device's single shader-visible heap
    // (bbx_guest_root.h's rule 4 - one heap per command list, and a copy may not
    // come from a shader-visible source).
    impl.guest_arena = std::make_unique<d3d12::DescriptorRing>(impl.device.CbvSrvUavHeap() != nullptr
                                                                  ? d3d12::Device::kCbvSrvUavDescriptors
                                                                  : 0);
    impl.guest_sampler_arena = std::make_unique<d3d12::DescriptorRing>(d3d12::Device::kSamplerDescriptors);
    // Reserve Replay's own descriptors at the front of the shader-visible heap
    // so GuestMemory/GuestCompute never overwrite frame_srv_/white_srv_/atlas_srv_.
    const std::optional<d3d12::DescriptorSlice> replay_reserved =
        impl.guest_arena->ReservePermanent(d3d12::Replay::kReservedCbvSrvUavDescriptors);
    const std::optional<d3d12::DescriptorSlice> arena =
        replay_reserved.has_value()
            ? impl.guest_arena->ReservePermanent(d3d12::GuestHeapLayout::kCbvSrvUavDescriptors)
            : std::nullopt;
    const std::optional<d3d12::DescriptorSlice> samplers =
        impl.guest_sampler_arena->ReservePermanent(d3d12::GuestHeapLayout::kStageRegions *
                                                   d3d12::GuestHeapLayout::kSamplerCount);
    if (!arena.has_value() || !samplers.has_value()) {
        LogMessage(Level::kError, "d3d12: the guest descriptor arena does not fit the device's heaps");
        impl.guest_arena_base = 0;
        impl.guest_sampler_base = 0;
    } else {
        impl.guest_arena_base = arena->offset;
        impl.guest_sampler_base = samplers->offset;
    }
    if (impl.guest_plan.ok()) {
        const d3d12::Status memory = impl.guest_memory.Create(
            impl.device, impl.device.Memory(), impl.guest_plan,
            impl.guest_arena_base + d3d12::GuestHeapLayout::kSpace3Base);
        if (memory != d3d12::Status::kOk) {
            LogMessage(Level::kWarn, "d3d12: guest memory not created (%s): guest dispatches will be refused",
                       d3d12::StatusName(memory));
        }
        LogMessage(Level::kInfo, "d3d12: %s", impl.guest_memory.Describe().c_str());
    } else {
        LogMessage(Level::kError, "d3d12: %s", impl.guest_plan.refused.c_str());
    }
    if (impl.guest_memory.Ready()) {
        const d3d12::Status compute = impl.guest_compute.Create(impl.device, impl.pipelines, impl.guest_memory,
                                                                impl.guest_arena_base, impl.guest_sampler_base);
        if (compute != d3d12::Status::kOk) {
            LogMessage(Level::kWarn, "d3d12: guest compute not created (%s): dispatches will be refused",
                       d3d12::StatusName(compute));
        } else {
            std::string line = impl.guest_compute.Report();
            for (char& c : line) {
                if (c == '\n') c = ' ';
            }
            LogMessage(Level::kInfo, "d3d12: %s", line.c_str());
            const d3d12::Status graphics = impl.guest_graphics.Create(
                impl.device, impl.pipelines, impl.guest_memory, impl.guest_compute,
                impl.guest_arena_base, impl.guest_sampler_base);
            if (graphics != d3d12::Status::kOk) {
                LogMessage(Level::kWarn, "d3d12: guest graphics not created (%s): draws will be refused",
                           d3d12::StatusName(graphics));
            } else {
                LogMessage(Level::kInfo, "d3d12: %s", impl.guest_graphics.Report().c_str());
            }
        }
    }
    // The guest's HLE now sees the device's fence and the guest's slabs
    // (host_gpu_d3d12.h): a dispatch checks `available` before anything else.
    impl.InstallGuestBinding();
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
    if (bridge::Flips() != 0) {
        return hud;
    }
    const float width = hud.Width();
    const float height = hud.Height();
    // A compact gothic hardware telemetry panel across the lower edge, scaled off
    // the swapchain so the same layout works at 1080p and at 2160p.
    const float scale = width >= 3000.0f ? 6.0f : 3.0f;
    const float margin = static_cast<float>(scale) * 7.0f;
    const float line = Hud::TextHeight(static_cast<std::uint32_t>(scale)) +
                       static_cast<float>(scale) * 2.5f;

    const std::uint32_t lines = 8;
    const float panel_height = line * static_cast<float>(lines) + margin;
    const float panel_y = height - panel_height - margin * 0.7f;
    hud.GothicPanel(margin, panel_y, width - margin * 2.0f, panel_height);

    float y = panel_y + margin * 0.45f;
    const float x = margin * 1.6f;
    const float row_width = width - margin * 3.2f;
    const auto s = static_cast<std::uint32_t>(scale);
    hud.Text(x, y, s, "YHARNAM CHALICE ALTAR", HudColor::TarnishedGold());
    hud.Text(x + Hud::TextWidth("YHARNAM CHALICE ALTAR   ", s), y, s,
             "DIRECT3D 12  |  SHADER MODEL 6.4", HudColor::BloodCrimson());
    y += line * 1.05f;
    hud.GothicDivider(x, y - line * 0.22f, row_width, s);
    hud.Row(x, y, row_width, s, "ADAPTER", impl.report.adapter, HudColor::BoneIvory());
    y += line;
    hud.Row(x, y, row_width, s, "FEATURE LEVEL", impl.report.feature_level, HudColor::BoneIvory());
    y += line;
    hud.Row(x, y, row_width, s, "MEMORY", impl.report.memory, HudColor::BoneIvory());
    y += line;
    hud.Row(x, y, row_width, s, "SWAPCHAIN", impl.report.swapchain, HudColor::BoneIvory());
    y += line;
    hud.Status(x, y, s, impl.report.selftest_ok, impl.report.selftest);
    y += line;
    hud.Row(x, y, row_width, s, "CARYLL SHADER CACHE", bridge::ShaderCacheLine(),
            HudColor::TarnishedGold());
    y += line;
    hud.Text(x, y, s,
             "FEAR THE OLD BLOOD  |  L3+R3 IN-GAME OPENS HUNTER'S DREAM QUICK MENU",
             HudColor::AshSilver());
    return hud;
}

bbx::d3d12::Status D3d12Session::Present(const Hud& hud, bool vsync, bool sharp_upscale) {
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
    bool has_guest_frame = false;
    {
        std::lock_guard<std::mutex> lk(impl.frame_mutex);
        if (impl.frame_pending) {
            impl.frame_upload.swap(impl.frame_pixels);
            frame.swap(impl.frame_upload);
            frame_desc = impl.frame_desc;
            impl.frame_pending = false;
        }
        has_guest_frame = impl.guest_frames > 0;
    }
    if (!frame.empty() && frame_desc.width != 0 && frame_desc.height != 0) {
        // The upload is the description's: the image is made in the guest's
        // format, and a tiled buffer is detiled on the GPU before the present
        // pass samples it.
        const d3d12::Status uploaded = impl.replay.UploadFrame(frame.data(), frame_desc);
        if (uploaded != d3d12::Status::kOk) {
            LogCrumb("replay: guest frame dropped");
        }
        // It goes back to the pool for the next flip, with its capacity intact.
        {
            std::lock_guard<std::mutex> lk(impl.frame_mutex);
            if (uploaded != d3d12::Status::kOk) {
                ++impl.guest_frames_dropped;
            } else {
                ++impl.guest_frames;
                has_guest_frame = true;
            }
            impl.frame_upload.swap(frame);
        }
    }
    // Before the first guest flip, tint the placeholder quad near-black behind
    // the launcher HUD. Once Bloodborne presents frames, draw at full 1.0
    // brightness with optional HLSL SM 6.4 RCAS contrast-adaptive sharpening.
    if (has_guest_frame) {
        status = impl.replay.DrawPresent(1.0f, 1.0f, 1.0f, sharp_upscale ? 0.75f : 0.0f);
    } else {
        status = impl.replay.DrawPresent(0.02f, 0.02f, 0.03f, 0.0f);
    }
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
    const d3d12::Status presented = impl.replay.EndFrame(vsync);
    // The frame's fence has passed (or the present failed and the driver is
    // waiting anyway), so the guest's compute PSOs whose eviction it proves are
    // released now: a pipeline object freed while a recorded list still names it
    // is the one lifetime mistake D3D12 does not report.
    const std::uint64_t completed = impl.device.CompletedValue();
    impl.guest_compute.OnFrame(completed);
    impl.guest_graphics.OnFrame(completed);
    return presented;
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

std::uint64_t D3d12Session::GuestFramesDropped() const {
    if (impl_ == nullptr) { return 0; }
    std::lock_guard<std::mutex> lk(impl_->frame_mutex);
    return impl_->guest_frames_dropped;
}

std::uint32_t D3d12Session::OutputWidth() const {
    if (impl_ == nullptr || !impl_->ready) { return 0; }
    return impl_->swapchain.Width();
}

std::uint32_t D3d12Session::OutputHeight() const {
    if (impl_ == nullptr || !impl_->ready) { return 0; }
    return impl_->swapchain.Height();
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
    // The guest's HLE must stop calling into this device before it is destroyed.
    d3d12::SetGuestGpuBinding(nullptr);
    impl_->guest_graphics.Destroy();
    impl_->guest_compute.Destroy();
    impl_->guest_arena.reset();
    impl_->guest_sampler_arena.reset();
    impl_->guest_memory.Destroy();
    impl_->guest_binding = d3d12::GuestGpuBinding{};
    impl_->replay.Destroy();
    impl_->pipelines.Destroy();
    impl_->swapchain.Destroy();
    impl_->device.Destroy();
}

}  // namespace bbx::platform
