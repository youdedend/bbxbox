// SPDX-License-Identifier: GPL-3.0-or-later
// The device: one D3D12 device at feature level 11_0, one graphics queue, one
// fence, three frame allocators and four descriptor heaps.
//
// The decisions that are the port rather than boilerplate are in device.h; what
// this file adds is the console's own rules:
//
//  * Feature level 11_0, never 12_0. The console's part reports 11_0 and the
//    shaders are SM 6.4; asking for 12_0 makes D3D12CreateDevice fail on hardware
//    that runs the game at 60 FPS.
//  * No CreatePipelineLibrary, ever. It removes the device on a console (see
//    docs/ARCHITECTURE_XBOX_D3D12.md §2), and the PSO cache reads a manifest
//    instead.
//  * The budget starts with the Game Mode limit and every committed resource is
//    charged to an owner (`bbx_budget`); a device that allocates without asking is
//    how the guest's 5,120 MiB disappears.
//  * Device loss is a status, not an exception: the removed reason is captured at
//    the moment it is noticed, because DXGI forgets it after the next call.
#include "src/host/d3d12/device.h"

#include <cstdio>

#include "src/host/d3d12/bbx_caps.h"
#include "xbox/platform/log.h"

namespace bbx::d3d12 {
namespace {

// What the device reports, in the port's own vocabulary, so Validate() can refuse
// a mismatch instead of leaving it to a PSO that fails to create much later.
Caps CapsFromDevice(ID3D12Device* device) {
    Caps caps = XboxSeriesCaps();
    caps.gpu_class = GpuClass::kXboxSeries;

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                             sizeof(options)))) {
        caps.binding_tier = options.ResourceBindingTier;
        // ResourceDescriptorHeap indexing is a shader-model 6.6 feature and is not
        // reachable through the 12_0 options block; the port never uses it.
        caps.descriptor_heap_indexing = false;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{};
    shader_model.HighestShaderModel = D3D_SHADER_MODEL_6_4;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model,
                                             sizeof(shader_model)))) {
        // A device that reports more than 6.4 is fine; the port's shaders are 6.4.
        caps.shader_model = shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_4
                                ? shader_model.HighestShaderModel
                                : D3D_SHADER_MODEL_6_4;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                             sizeof(options1)))) {
        if (options1.WaveLaneCountMin != 0) { caps.wave_lanes = options1.WaveLaneCountMin; }
    }
    return caps;
}

}  // namespace

Device::~Device() { Destroy(); }

Status Device::Create(const DeviceDesc& desc) {
    Destroy();
    budget_ = Budget(desc.limit_bytes);

    const Status adapter = PickAdapter(desc.enable_debug_layer);
    if (adapter != Status::kOk) { return adapter; }

    Status status = Status::kOk;
    if (desc.enable_debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug),
                                             reinterpret_cast<void**>(debug.GetAddressOf())))) {
            debug->EnableDebugLayer();
            Log("debug layer enabled (non-shipping build)");
        } else {
            Log("debug layer requested but D3D12GetDebugInterface failed; continuing without it");
        }
    }

    // 11_0: the console's part. See the header comment.
    bbx::platform::LogMirroredMessage(
        bbx::platform::Level::kInfo,
        "d3d12: D3D12CreateDevice enter (adapter=%p, feature_level=11_0)",
        static_cast<const void*>(adapter_.Get()));
    HRESULT hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                                   __uuidof(ID3D12Device), reinterpret_cast<void**>(device_.GetAddressOf()));
    bbx::platform::LogMirroredMessage(
        hr == S_OK ? bbx::platform::Level::kInfo : bbx::platform::Level::kError,
        "d3d12: D3D12CreateDevice exit (hr=0x%08lX)",
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        Log("D3D12CreateDevice at 11_0 failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    // Device8 is the 1.4 core; the console ships it, and `bbx_descriptors` wants
    // its explicit subresource and heap APIs. A desktop development build without
    // it still works: Get8() is only used where it is known to exist.
    if (FAILED(device_.As(&device8_))) {
        Log("ID3D12Device8 unavailable; keeping ID3D12Device (1.0 core)");
        device8_.Reset();
    }
    NameObject(device_.Get(), L"bbxbox device");

    status = QueryAndValidate(desc.enable_debug_layer);
    if (status != Status::kOk) { return status; }

    status = CreateQueuesAndFence();
    if (status != Status::kOk) { return status; }
    status = CreateDescriptorHeaps();
    if (status != Status::kOk) { return status; }

    Log("device ready: %s, %s", uma_ ? "UMA" : "discrete", budget_.Describe().c_str());
    return Status::kOk;
}

Status Device::PickAdapter(bool debug_layer) {
    // 0 is "no flags": the SDK spells no DXGI_CREATE_FACTORY_FLAGS_NONE.
    const UINT flags = debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0u;
    HRESULT hr = CreateDXGIFactory2(flags, __uuidof(IDXGIFactory6),
                                   reinterpret_cast<void**>(factory_.GetAddressOf()));
    if (FAILED(hr)) {
        // The debug factory needs the Graphics Tools feature on demand; a shipping
        // build never asks for it, and a development machine may not have it.
        Log("CreateDXGIFactory2(debug=%d) failed: 0x%08lX; retrying without the debug flag",
            debug_layer ? 1 : 0, static_cast<unsigned long>(hr));
        hr = CreateDXGIFactory2(0u, __uuidof(IDXGIFactory6),
                                reinterpret_cast<void**>(factory_.GetAddressOf()));
    }
    if (hr != S_OK) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: CreateDXGIFactory2 failed (hr=0x%08lX)",
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        Log("no DXGI factory: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }

    // The console has one adapter and it is the high-performance one; a desktop
    // development machine may have several, and picking the integrated part would
    // make every measurement wrong.
    hr = factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                              __uuidof(IDXGIAdapter4),
                                              reinterpret_cast<void**>(adapter_.GetAddressOf()));
    if (hr != S_OK) {
        bbx::platform::LogMirroredMessage(
            bbx::platform::Level::kError,
            "d3d12: EnumAdapterByGpuPreference failed (hr=0x%08lX)",
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        Log("EnumAdapterByGpuPreference failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }

    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (SUCCEEDED(adapter_->GetDesc1(&adapter_desc)) && adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        // WARP would run the game at a few frames a second and hide every timing
        // problem the port exists to fix.
        Log("the selected adapter is the software rasterizer; refusing it");
        adapter_.Reset();
        return Status::kUnavailable;
    }
    return Status::kOk;
}

Status Device::QueryAndValidate(bool debug_layer) {
    const Caps reported = CapsFromDevice(device_.Get());
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                               sizeof(architecture)))) {
        uma_ = architecture.UMA != FALSE;
    }

    // The console is UMA; a desktop development build is not, and `IsUma()` is what
    // tells the budget that a texture's size is charged once, not twice.
    const Caps expected = XboxSeriesCaps();
    const CapsCheck check = Validate(expected, reported);
    if (!check.ok) {
        Log("caps check failed: %s", check.problem.c_str());
        return Status::kUnavailable;
    }
    if (reported.shader_model < expected.shader_model) {
        Log("device reports SM %u, shaders are compiled for %u", reported.shader_model,
            expected.shader_model);
        return Status::kUnavailable;
    }
    Log("caps: SM %u, binding tier %u, %u-lane waves, %s", reported.shader_model,
        reported.binding_tier, reported.wave_lanes, debug_layer ? "validation on" : "validation off");
    return Status::kOk;
}

Status Device::CreateQueuesAndFence() {
    // One queue: bbhost submits one command list per frame, and a dedicated compute
    // queue's overlap measured under 0.2 ms on Series S (device.h).
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = static_cast<INT>(D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    HRESULT hr = device_->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                             reinterpret_cast<void**>(graphics_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("CreateCommandQueue failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    NameObject(graphics_.Get(), L"bbxbox graphics queue");

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                              reinterpret_cast<void**>(fence_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("CreateFence failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    NameObject(fence_.Get(), L"bbxbox frame fence");

    fence_event_ = CreateEventExW(nullptr, nullptr, 0, 0x1F0003);
    if (fence_event_ == nullptr) {
        Log("CreateEventExW for the fence failed: %lu", static_cast<unsigned long>(GetLastError()));
        return Status::kUnavailable;
    }

    for (std::uint32_t index = 0; index < kFrameCount; ++index) {
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void**>(frames_[index].GetAddressOf()));
        if (FAILED(hr)) {
            Log("CreateCommandAllocator(%u) failed: 0x%08lX", index, static_cast<unsigned long>(hr));
            return FromHresult(hr);
        }
    }
    frame_ = 0;
    submitted_ = 0;
    return Status::kOk;
}

Status Device::CreateDescriptorHeaps() {
    struct Request {
        D3D12_DESCRIPTOR_HEAP_TYPE type;
        std::uint32_t count;
        D3D12_DESCRIPTOR_HEAP_FLAGS flags;
        ComPtr<ID3D12DescriptorHeap>* heap;
        std::uint32_t* stride;
        const wchar_t* name;
    };
    const Request requests[] = {
        {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCbvSrvUavDescriptors,
         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &cbv_srv_uav_heap_, &cbv_srv_uav_stride_,
         L"bbxbox cbv/srv/uav heap"},
        {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerDescriptors,
         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &sampler_heap_, &sampler_stride_,
         L"bbxbox sampler heap"},
        {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
         &rtv_heap_, &rtv_stride_, L"bbxbox rtv heap"},
        {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
         &dsv_heap_, &dsv_stride_, L"bbxbox dsv heap"},
    };

    for (const Request& request : requests) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = request.type;
        desc.NumDescriptors = request.count;
        desc.Flags = request.flags;
        const HRESULT hr = device_->CreateDescriptorHeap(
            &desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(request.heap->GetAddressOf()));
        if (FAILED(hr)) {
            Log("CreateDescriptorHeap(type %u, %u descriptors) failed: 0x%08lX",
                static_cast<unsigned>(request.type), request.count,
                static_cast<unsigned long>(hr));
            return FromHresult(hr);
        }
        NameObject(request.heap->Get(), request.name);
        *request.stride = device_->GetDescriptorHandleIncrementSize(request.type);
    }
    // The shader-visible heaps' descriptor memory is charged to kHeaps, so the
    // budget's report matches what the device actually holds.
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(kCbvSrvUavDescriptors) * cbv_srv_uav_stride_ +
        static_cast<std::uint64_t>(kSamplerDescriptors) * sampler_stride_ +
        static_cast<std::uint64_t>(kRtvDescriptors) * rtv_stride_ +
        static_cast<std::uint64_t>(kDsvDescriptors) * dsv_stride_;
    const AllocationResult result = budget_.Allocate(Owner::kHeaps, bytes);
    if (!result.ok) {
        // Descriptor memory is already committed by the device; failing here would
        // leak it. Report it and let the first real allocation decide.
        Log("descriptor heaps are %llu bytes, which crosses the %s budget: %s",
            static_cast<unsigned long long>(bytes), OwnerName(Owner::kHeaps),
            result.reason.c_str());
    }
    return Status::kOk;
}

void Device::Destroy() {
    if (device_ == nullptr) {
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        return;
    }
    // Everything the GPU was given must be finished before the device goes away,
    // or the next device is created on a driver that is still tearing down.
    (void)WaitIdle();
    for (auto& frame : frames_) { frame.Reset(); }
    sampler_heap_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    cbv_srv_uav_heap_.Reset();
    fence_.Reset();
    graphics_.Reset();
    device8_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    if (fence_event_ != nullptr) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    submitted_ = 0;
    frame_ = 0;
}

std::uint64_t Device::Signal() {
    if (fence_ == nullptr || graphics_ == nullptr) { return submitted_; }
    const std::uint64_t value = ++submitted_;
    const HRESULT hr = graphics_->Signal(fence_.Get(), value);
    if (FAILED(hr)) { NoteDeviceLost(hr); }
    return value;
}

bool Device::WaitFor(std::uint64_t value, std::uint32_t timeout_ms) {
    if (fence_ == nullptr) { return true; }
    if (fence_->GetCompletedValue() >= value) { return true; }
    // SetEventOnCompletion + a bounded wait: an unbounded wait on a hung GPU is how
    // a console app stops responding to the shell.
    if (fence_event_ == nullptr) { return false; }
    if (FAILED(fence_->SetEventOnCompletion(value, fence_event_))) {
        NoteDeviceLost(E_FAIL);
        return false;
    }
    const DWORD waited = WaitForSingleObjectEx(fence_event_, timeout_ms, FALSE);
    if (waited != WAIT_OBJECT_0) {
        Log("fence %llu did not complete within %u ms (wait %lu)",
            static_cast<unsigned long long>(value), timeout_ms, static_cast<unsigned long>(waited));
        if (const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : S_OK; FAILED(reason)) {
            NoteDeviceLost(reason);
        }
        return false;
    }
    return true;
}

bool Device::WaitIdle() { return WaitFor(submitted_, 4000); }

void Device::NoteDeviceLost(HRESULT hr) {
    if (device_lost_) { return; }
    device_lost_ = true;
    removed_reason_ = hr;
    HRESULT reason = S_OK;
    if (device_) { reason = device_->GetDeviceRemovedReason(); }
    Log("device lost: 0x%08lX (removed reason 0x%08lX); the title must be restarted",
        static_cast<unsigned long>(hr), static_cast<unsigned long>(reason));
}

std::string Device::DescribeMemory() const { return budget_.Describe(); }

}  // namespace bbx::d3d12
