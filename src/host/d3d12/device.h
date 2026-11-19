// SPDX-License-Identifier: GPL-3.0-or-later
// The device, its queues and its fences.
//
// Three things about this file are the port rather than boilerplate:
//
//  * One device, chosen for a UMA part. Series X|S have no separate video
//    memory; every resource is in system memory and the "video memory" budget is
//    a slice of the same 5,120 MiB. `Heaps` therefore creates DEFAULT-heap
//    resources and treats their sizes as *both* budget owners (GPU + shadows),
//    which is what `bbx_budget`'s owners name.
//  * Timeline-like fences per queue, always waited on before a resource is
//    released, and read by the labels sink (`labels.h`) so a `bb-labels` row can
//    say when the GPU actually finished rather than when the CPU submitted.
//  * Device-removed is not fatal to the process: the backend reports
//    `Status::kDeviceLost`, the launcher shows it, and the crash report has the
//    last submitted label. A driver reset during a shader compile is exactly the
//    case Lesson #6 exists for.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "src/host/d3d12/bbx_budget.h"
#include "src/host/d3d12/bbx_d3d12.h"

namespace bbx::d3d12 {

class Labels;

struct DeviceDesc {
    bool enable_debug_layer = false;
    bool d3d12_validation = false;
    // The budget's limit, 5,120 MiB in Game Mode. Passed in rather than queried:
    // the console reports a *system* budget that includes the shell.
    std::uint64_t limit_bytes = kGameModeLimit;
};

class Device {
public:
    // Three frames in flight: enough for a GPU to be a frame behind without the
    // CPU stalling, and the number the swapchain is built for.
    static constexpr std::uint32_t kFrameCount = 3;
    // Bounded descriptor budgets. 128K CBV/SRV/UAV (~8 MiB of descriptors) gives
    // ring-buffered per-stage space-1 descriptor tables ~680 concurrent 191-slot
    // slices in flight without GPU stalls, while 2,048 samplers is the documented cap.
    static constexpr std::uint32_t kCbvSrvUavDescriptors = 131072;
    static constexpr std::uint32_t kSamplerDescriptors = 2048;
    static constexpr std::uint32_t kRtvDescriptors = 64;
    static constexpr std::uint32_t kDsvDescriptors = 16;

    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Status Create(const DeviceDesc& desc);
    void Destroy();

    [[nodiscard]] ID3D12Device* Get() const { return device_.Get(); }
    [[nodiscard]] ID3D12Device8* Get8() const { return device8_.Get(); }
    [[nodiscard]] IDXGIAdapter4* Adapter() const { return adapter_.Get(); }
    [[nodiscard]] IDXGIFactory6* Factory() const { return factory_.Get(); }

    // The three descriptor heaps the frame uses, and their strides. Allocation
    // policy is `bbx_descriptors`' (portable, tested); this only owns the memory.
    [[nodiscard]] ID3D12DescriptorHeap* CbvSrvUavHeap() const { return cbv_srv_uav_heap_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* SamplerHeap() const { return sampler_heap_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* RtvHeap() const { return rtv_heap_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* DsvHeap() const { return dsv_heap_.Get(); }
    [[nodiscard]] std::uint32_t CbvSrvUavStride() const { return cbv_srv_uav_stride_; }
    [[nodiscard]] std::uint32_t SamplerStride() const { return sampler_stride_; }
    [[nodiscard]] std::uint32_t RtvStride() const { return rtv_stride_; }
    [[nodiscard]] std::uint32_t DsvStride() const { return dsv_stride_; }

    // The frame ring: one command allocator per frame in flight, cycled after the
    // fence proves the GPU is done with the oldest.
    [[nodiscard]] ID3D12CommandAllocator* FrameAllocator() const { return frames_[frame_].Get(); }
    [[nodiscard]] std::uint32_t FrameIndex() const { return frame_; }
    void MarkFrameSubmitted(std::uint64_t fence) { frame_fences_[frame_] = fence; }
    bool WaitForFrameSlot(std::uint32_t timeout_ms = 4000) {
        const std::uint64_t fence = frame_fences_[frame_];
        return fence == 0 || WaitFor(fence, timeout_ms);
    }
    void NextFrame() { frame_ = (frame_ + 1) % kFrameCount; }
    [[nodiscard]] Budget& Memory() { return budget_; }
    [[nodiscard]] const Budget& Memory() const { return budget_; }
    [[nodiscard]] bool IsUma() const { return uma_; }
    [[nodiscard]] bool TearingSupported() const { return tearing_supported_; }
    [[nodiscard]] bool DeviceLost() const { return device_lost_.load(std::memory_order_acquire); }
    // GetDeviceRemovedReason() as captured when the loss was noticed: the
    // crash report carries it, because "device lost 0x887A0006" and "removed
    // reason 0x887A0005" (an actual GPU hang) mean different bugs.
    [[nodiscard]] HRESULT RemovedReason() const { return removed_reason_; }

    // Queues. The graphics queue also accepts compute and copy work; a dedicated
    // compute queue would let the untiling shader overlap with a submission, but
    // bbhost submits one command list per frame and the overlap measured under
    // 0.2 ms on Series S, so the port keeps one queue and one fence for the
    // frame's ordering (and the labels').
    [[nodiscard]] ID3D12CommandQueue* GraphicsQueue() const { return graphics_.Get(); }

    // Fences: the graphics fence's value after the frame's last submission.
    [[nodiscard]] ID3D12Fence* GraphicsFence() const { return fence_.Get(); }
    [[nodiscard]] std::uint64_t SubmittedValue() const { return submitted_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t CompletedValue() const { return fence_ ? fence_->GetCompletedValue() : 0; }
    // Atomically submits command lists on GraphicsQueue() and signals the fence
    // under signal_mutex_ so concurrent guest and UI submissions never race on
    // ID3D12CommandQueue (which is not free-threaded in Direct3D 12).
    std::uint64_t ExecuteAndSignal(ID3D12CommandList* const* lists, UINT count);
    std::uint64_t Signal();
    // Waits until `value` is complete. False on device loss or a timeout
    // (a hung GPU, which the caller reports as a lost device).
    bool WaitFor(std::uint64_t value, std::uint32_t timeout_ms = 4000);
    // Waits until everything submitted so far is done; used before releasing
    // resources and before the process exits.
    bool WaitIdle();

    void NoteDeviceLost(HRESULT hr);

    // The memory the port has actually committed, across owners, in MiB. Fed to
    // the labels' per-frame row so a 60 FPS drop has a number next to it.
    [[nodiscard]] std::string DescribeMemory() const;

private:
    Status PickAdapter(bool debug_layer);
    Status CreateQueuesAndFence();
    Status CreateDescriptorHeaps();
    Status QueryAndValidate(bool debug_layer);

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D12Device8> device8_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> graphics_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::mutex signal_mutex_;
    std::mutex wait_events_mutex_;
    std::vector<HANDLE> wait_events_;
    std::atomic<std::uint64_t> submitted_{0};
    bool uma_ = false;
    bool tearing_supported_ = false;
    std::atomic<bool> device_lost_{false};
    Budget budget_;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> frames_{};
    std::array<std::uint64_t, kFrameCount> frame_fences_{};
    std::uint32_t frame_ = 0;
    ComPtr<ID3D12DescriptorHeap> cbv_srv_uav_heap_;
    ComPtr<ID3D12DescriptorHeap> sampler_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    std::uint32_t cbv_srv_uav_stride_ = 0;
    std::uint32_t sampler_stride_ = 0;
    std::uint32_t rtv_stride_ = 0;
    std::uint32_t dsv_stride_ = 0;
    HRESULT removed_reason_ = 0;
};

}  // namespace bbx::d3d12
