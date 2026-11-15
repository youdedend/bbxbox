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

#include <string>
#include <cstdint>
#include <array>

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
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Status Create(const DeviceDesc& desc);
    void Destroy();

    [[nodiscard]] ID3D12Device* Get() const { return device_.Get(); }
    [[nodiscard]] ID3D12Device8* Get8() const { return device8_.Get(); }
    [[nodiscard]] IDXGIAdapter4* Adapter() const { return adapter_.Get(); }
    [[nodiscard]] Budget& Memory() { return budget_; }
    [[nodiscard]] const Budget& Memory() const { return budget_; }
    [[nodiscard]] bool IsUma() const { return uma_; }
    [[nodiscard]] bool DeviceLost() const { return device_lost_; }

    // Queues. The graphics queue also accepts compute and copy work; a dedicated
    // compute queue would let the untiling shader overlap with a submission, but
    // bbhost submits one command list per frame and the overlap measured under
    // 0.2 ms on Series S, so the port keeps one queue and one fence for the
    // frame's ordering (and the labels').
    [[nodiscard]] ID3D12CommandQueue* GraphicsQueue() const { return graphics_.Get(); }

    // Fences: the graphics fence's value after the frame's last submission.
    [[nodiscard]] ID3D12Fence* GraphicsFence() const { return fence_.Get(); }
    [[nodiscard]] std::uint64_t SubmittedValue() const { return submitted_; }
    [[nodiscard]] std::uint64_t CompletedValue() const { return fence_ ? fence_->GetCompletedValue() : 0; }
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

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D12Device8> device8_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> graphics_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::uint64_t submitted_ = 0;
    bool uma_ = false;
    bool device_lost_ = false;
    Budget budget_;
};

}  // namespace bbx::d3d12
