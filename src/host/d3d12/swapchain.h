// SPDX-License-Identifier: GPL-3.0-or-later
// The swapchain: what is on the TV.
//
// Three decisions, and each of them is a measured failure from the reference port
// (KytyPS5 `src/uwp/src/app.cpp`, docs/ARCHITECTURE_XBOX_D3D12.md §5):
//
//  * `CreateSwapChainForCoreWindow` with `DXGI_SWAP_EFFECT_FLIP_DISCARD` and three
//    buffers. The old blt-model `DISCARD` path goes through a copy the console
//    charges a frame for; flip-discard is what the compositor wants.
//  * The size comes from `bbx::platform::PlanOutput`, not from the CoreWindow:
//    the window is 1920x1080 whatever the TV is doing, so a title that sizes its
//    swapchain from the window never shows 4K on a 4K panel. The plan clamps at
//    3840x2160 (guard G8: above it the console loses the device).
//  * The inverse of the window/panel scale goes to
//    `IDXGISwapChain2::SetMatrixTransform`, so a 4K swapchain lands on the panel
//    1:1 while the window remains 1080p.
#pragma once

#include <array>
#include <cstdint>

#include "src/host/d3d12/bbx_d3d12.h"
#include "xbox/platform/display.h"

namespace bbx::d3d12 {

class Device;

class Swapchain {
public:
    static constexpr std::uint32_t kBufferCount = 3;
    // The format the CoreWindow composite expects. R10G10B10A2 for HDR is a
    // later step; the backbuffer stays SDR-ranged for now and the present pass
    // applies the transfer.
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    Swapchain() = default;
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // `core_window` is the UWP CoreWindow's IUnknown (the ABI pointer), as
    // `CreateSwapChainForCoreWindow` takes it.
    Status Create(Device& device, IUnknown* core_window, const platform::OutputPlan& plan,
                  bool allow_tearing = false);
    void Destroy();

    // Called when the output plan changes (a TV mode change, or the player
    // choosing a different resolution). Waits for the GPU, because resizing a
    // swapchain with a frame in flight is a device-removed.
    Status Resize(const platform::OutputPlan& plan);

    // `sync_interval` 1 with vsync, 0 for the tearing path when it is allowed.
    Status Present(bool vsync);

    [[nodiscard]] IDXGISwapChain3* Get() const { return swapchain_.Get(); }
    [[nodiscard]] std::uint32_t Width() const { return width_; }
    [[nodiscard]] std::uint32_t Height() const { return height_; }
    [[nodiscard]] std::uint32_t BufferCount() const { return buffer_count_; }

    // The backbuffer the compositor will show next, and its RTV.
    [[nodiscard]] std::uint32_t CurrentIndex() const {
        return swapchain_ ? swapchain_->GetCurrentBackBufferIndex() : 0;
    }
    [[nodiscard]] ID3D12Resource* BackBuffer(std::uint32_t index) const {
        return index < buffers_.size() ? buffers_[index].Get() : nullptr;
    }
    [[nodiscard]] ID3D12Resource* CurrentBackBuffer() const { return BackBuffer(CurrentIndex()); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Rtv(std::uint32_t index) const;

private:
    Status CreateRenderTargets();
    void ReleaseRenderTargets();
    Status ApplyMatrixTransform(const platform::OutputPlan& plan);

    Device* device_ = nullptr;
    ComPtr<IDXGISwapChain3> swapchain_;
    std::array<ComPtr<ID3D12Resource>, kBufferCount> buffers_{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_base_{};
    std::uint32_t rtv_stride_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t buffer_count_ = kBufferCount;
    bool tearing_allowed_ = false;
    bool first_present_done_ = false;
};

}  // namespace bbx::d3d12
