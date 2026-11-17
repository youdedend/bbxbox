// SPDX-License-Identifier: GPL-3.0-or-later
// The swapchain, as described in swapchain.h: CoreWindow, flip-discard, three
// buffers, sized by the output plan and inverse-scaled onto the panel.
#include "src/host/d3d12/swapchain.h"

#include "src/host/d3d12/device.h"
#include "xbox/platform/log.h"

namespace bbx::d3d12 {

Swapchain::~Swapchain() { Destroy(); }

Status Swapchain::Create(Device& device, IUnknown* core_window, const platform::OutputPlan& plan,
                         bool allow_tearing) {
    Destroy();
    first_present_done_ = false;
    if (core_window == nullptr) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: CreateSwapChainForCoreWindow refused (core_window=null, hr=0x%08lX)",
            static_cast<unsigned long>(E_INVALIDARG));
        Log("swapchain: no CoreWindow; the device cannot present");
        return Status::kInvalidArgument;
    }
    if (device.Get() == nullptr || device.GraphicsQueue() == nullptr || device.Factory() == nullptr) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: CreateSwapChainForCoreWindow refused (device not ready, hr=0x%08lX)",
            static_cast<unsigned long>(E_INVALIDARG));
        Log("swapchain: the device is not ready");
        return Status::kInvalidArgument;
    }
    device_ = &device;
    tearing_allowed_ = allow_tearing;
    rtv_stride_ = device.RtvStride();
    rtv_base_ = device.RtvHeap()->GetCPUDescriptorHandleForHeapStart();

    width_ = plan.swap_width;
    height_ = plan.swap_height;
    if (width_ == 0 || height_ == 0) {
        Log("swapchain: the output plan has no size; using 1920x1080");
        width_ = 1920;
        height_ = 1080;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = kFormat;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    // STRETCH, not NONE: with the matrix transform doing the scaling, the
    // compositor must not add its own.
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = allow_tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;

    ComPtr<IDXGISwapChain1> swapchain1;
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "d3d12: CreateSwapChainForCoreWindow enter (core_window=%p, %ux%u, buffers=%u)",
        static_cast<const void*>(core_window), width_, height_, kBufferCount);
    HRESULT hr = device.Factory()->CreateSwapChainForCoreWindow(
        device.GraphicsQueue(), core_window, &desc, nullptr, swapchain1.GetAddressOf());
    bbx::platform::LogMirroredMessage(
        hr == S_OK ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
        "d3d12: CreateSwapChainForCoreWindow exit (hr=0x%08lX)",
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        Log("CreateSwapChainForCoreWindow(%ux%u, 3 buffers, flip-discard) failed: 0x%08lX", width_,
            height_, static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    const HRESULT qi_hr = swapchain1.As(&swapchain_);
    if (FAILED(qi_hr)) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: CreateSwapChainForCoreWindow QueryInterface(IDXGISwapChain3) failed (hr=0x%08lX)",
            static_cast<unsigned long>(qi_hr));
        Log("the swapchain is not IDXGISwapChain3; no matrix transform and no frame index");
        return Status::kUnavailable;
    }
    // No NameObject here: DXGI interfaces are not ID3D12Object and carry no debug
    // name; the debug layer labels the swapchain from its CoreWindow.

    const Status targets = CreateRenderTargets();
    if (targets != Status::kOk) { return targets; }
    return ApplyMatrixTransform(plan);
}

Status Swapchain::CreateRenderTargets() {
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "d3d12: RTV creation enter (buffers=%u, rtv_stride=%u)",
        buffer_count_, rtv_stride_);
    HRESULT rtv_hr = S_OK;
    for (std::uint32_t index = 0; index < buffer_count_; ++index) {
        HRESULT hr = swapchain_->GetBuffer(index, __uuidof(ID3D12Resource),
                                          reinterpret_cast<void**>(buffers_[index].GetAddressOf()));
        if (hr != S_OK) {
            rtv_hr = hr;
        }
        if (FAILED(hr)) {
            bbx::platform::LogMirroredMessage(
                bbx::platform::Level::kError,
                "d3d12: RTV creation exit (GetBuffer[%u] hr=0x%08lX)",
                index, static_cast<unsigned long>(hr));
            Log("swapchain GetBuffer(%u) failed: 0x%08lX", index, static_cast<unsigned long>(hr));
            return FromHresult(hr);
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = kFormat;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        view.Texture2D.PlaneSlice = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_base_;
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride_;
        device_->Get()->CreateRenderTargetView(buffers_[index].Get(), &view, handle);
    }
    bbx::platform::LogMirroredMessage(
        rtv_hr == S_OK ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
        "d3d12: RTV creation exit (hr=0x%08lX, buffers=%u)",
        static_cast<unsigned long>(rtv_hr), buffer_count_);
    return Status::kOk;
}

void Swapchain::ReleaseRenderTargets() {
    for (auto& buffer : buffers_) { buffer.Reset(); }
}

D3D12_CPU_DESCRIPTOR_HANDLE Swapchain::Rtv(std::uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_base_;
    handle.ptr += static_cast<SIZE_T>(index < buffer_count_ ? index : 0) * rtv_stride_;
    return handle;
}

Status Swapchain::ApplyMatrixTransform(const platform::OutputPlan& plan) {
    ComPtr<IDXGISwapChain2> swapchain2;
    if (FAILED(swapchain_.As(&swapchain2))) {
        // Without the matrix transform a 4K swapchain is stretched by the
        // compositor from a 1080p window, which is the "it says 4K but looks
        // 1080p" bug. Say so rather than pretend.
        Log("swapchain has no IDXGISwapChain2: the %ux%u picture will be composited at window scale",
            width_, height_);
        return Status::kOk;
    }
    DXGI_MATRIX_3X2_F matrix{};
    matrix._11 = plan.matrix[0];
    matrix._12 = plan.matrix[1];
    matrix._21 = plan.matrix[2];
    matrix._22 = plan.matrix[3];
    matrix._31 = 0.0f;
    matrix._32 = 0.0f;
    const HRESULT hr = swapchain2->SetMatrixTransform(&matrix);
    if (FAILED(hr)) {
        Log("SetMatrixTransform(%.4f %.4f %.4f %.4f) failed: 0x%08lX", matrix._11, matrix._12,
            matrix._21, matrix._22, static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    if (plan.scaled()) {
        Log("swapchain %ux%u with matrix inverse scale %.3f (%s)", width_, height_, plan.scale,
            plan.reason.c_str());
    }
    return Status::kOk;
}

Status Swapchain::Resize(const platform::OutputPlan& plan) {
    if (swapchain_ == nullptr || device_ == nullptr) { return Status::kInvalidArgument; }
    const std::uint32_t width = plan.swap_width ? plan.swap_width : width_;
    const std::uint32_t height = plan.swap_height ? plan.swap_height : height_;
    if (width == width_ && height == height_) {
        // A mode change that leaves the size alone still moves the matrix (the
        // window/panel relationship can change without the pixel count changing).
        return ApplyMatrixTransform(plan);
    }
    // Every buffer must be free before ResizeBuffers, and every RTV must be
    // released before the buffers: either the resize fails or the device is lost.
    if (!device_->WaitIdle()) {
        Log("swapchain: the GPU did not go idle; not resizing");
        return Status::kDeviceLost;
    }
    ReleaseRenderTargets();
    const UINT flags = tearing_allowed_ ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
    const HRESULT hr = swapchain_->ResizeBuffers(kBufferCount, width, height, kFormat, flags);
    if (FAILED(hr)) {
        Log("ResizeBuffers(%ux%u) failed: 0x%08lX", width, height, static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    width_ = width;
    height_ = height;
    const Status targets = CreateRenderTargets();
    if (targets != Status::kOk) { return targets; }
    Log("swapchain resized to %ux%u", width_, height_);
    return ApplyMatrixTransform(plan);
}

Status Swapchain::Present(bool vsync) {
    if (swapchain_ == nullptr) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: Present refused (swapchain=null, hr=0x%08lX)",
            static_cast<unsigned long>(E_INVALIDARG));
        return Status::kInvalidArgument;
    }
    // A flip-model swapchain with ALLOW_TEARING may only pass the tearing flag
    // with sync interval 0; the reverse pair is E_INVALIDARG.
    const UINT sync_interval = vsync ? 1u : 0u;
    const UINT flags = (!vsync && tearing_allowed_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const bool is_first = !first_present_done_;
    if (is_first) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kInfo,
            "d3d12: first Present enter (sync_interval=%u, flags=0x%X)",
            sync_interval, flags);
    }
    const HRESULT hr = swapchain_->Present(sync_interval, flags);
    if (is_first) {
        first_present_done_ = true;
        bbx::platform::LogMirroredMessage(
            hr == S_OK ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
            "d3d12: first Present exit (hr=0x%08lX)",
            static_cast<unsigned long>(hr));
    } else if (hr != S_OK) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: Present exit (hr=0x%08lX, sync_interval=%u, flags=0x%X)",
            static_cast<unsigned long>(hr), sync_interval, flags);
    }
    if (hr == static_cast<HRESULT>(0x887A0005L)) {  // DXGI_ERROR_DEVICE_REMOVED
        if (device_ != nullptr) { device_->NoteDeviceLost(hr); }
        return Status::kDeviceLost;
    }
    if (FAILED(hr)) {
        Log("Present(%u, %u) failed: 0x%08lX", sync_interval, flags, static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    return Status::kOk;
}

void Swapchain::Destroy() {
    // The caller is expected to have presented or discarded the last frame; a
    // swapchain released with work in flight is a device-removed on the next boot.
    ReleaseRenderTargets();
    swapchain_.Reset();
    device_ = nullptr;
    first_present_done_ = false;
}

}  // namespace bbx::d3d12
