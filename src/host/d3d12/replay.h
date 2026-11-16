// SPDX-License-Identifier: GPL-3.0-or-later
// Replay: the one place the port turns a recorded stream of operations into D3D12
// calls, and the one place the port's own passes are drawn.
//
// What it does, in the order a frame happens:
//
//   BeginFrame   reset the frame's allocator and command list
//   ExecuteStream  WalkStream() the block first (a truncated or garbage block is
//                  reported and not replayed), then emit: clears, buffer fills and
//                  copies through an upload ring, barriers via bbx_barriers,
//                  markers as PIX events, dispatches through the untile pipeline
//   DrawPresent  the fullscreen triangle that samples the frame image
//   DrawOverlay  glyph quads and solid panels: telemetry, launcher, boot report
//   EndFrame     transition to PRESENT, execute, signal the fence, Present()
//
// What it does not do yet, and says so in the stats rather than silently: image
// copies (kCopyImage, kCopyBufferToImage, kCopyImageToBuffer) need the guest's
// resource table, which arrives with P3's draw path. They are counted as skipped
// with the reason logged once.
//
// `RunStreamSelftest` is the console's own lie detector: it dispatches the
// build-time `SelftestCs`, waits on the fence, reads the buffer back through a
// READBACK heap and compares against the CPU's copy of the same arithmetic. A GPU
// that never sees the fence advance fails here, once, at boot, on the launcher.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "src/host/d3d12/bbx_d3d12.h"
#include "src/host/d3d12/bbx_stream.h"
#include "src/host/d3d12/bbx_untile.h"

namespace bbx::d3d12 {

class Device;
class PipelineSet;
class Swapchain;

struct ReplayStats {
    std::uint64_t frames = 0;
    std::uint64_t records = 0;
    std::uint64_t clears = 0;
    std::uint64_t copies = 0;
    std::uint64_t barriers = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t markers = 0;
    std::uint64_t skipped_images = 0;   // image copies: P3's resource table
    std::uint64_t upload_bytes = 0;
    std::uint64_t walk_errors = 0;
    [[nodiscard]] std::string Describe() const;
};

class Replay {
public:
    // One vertex of the overlay pass; 32 bytes, the same layout pipeline.cpp's
    // input layout describes and overlay.hlsl's OverlayVertex expects.
    struct OverlayVertex {
        float x = 0, y = 0;
        float u = 0, v = 0;
        float r = 1, g = 1, b = 1, a = 1;
    };

    Replay() = default;
    ~Replay();
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;

    Status Create(Device& device, Swapchain& swapchain, PipelineSet& pipelines);
    void Destroy();

    [[nodiscard]] bool Ready() const { return command_list_ != nullptr; }

    Status BeginFrame();
    // Replays one recorded block. Walks it first: a truncated block replays what
    // is structurally valid and reports the error instead of reading past it.
    Status ExecuteStreamBlock(std::span<const std::uint8_t> block);
    Status DrawPresent(float red, float green, float blue);

    // The guest's scanout buffer, uploaded into the image DrawPresent samples.
    // Called inside a frame, between BeginFrame and DrawPresent: the copy goes on
    // that frame's command list, so the guest thread never waits on the GPU, and
    // a frame that does not fit is dropped rather than stalled. A size change
    // recreates the image (the placeholder is 256x256 until the first flip).
    // The pixels are RGBA8; the display's own format arrives with the display
    // HLE, which is where the guest's tiling is stated too.
    Status UploadFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);
    Status DrawOverlay();

    // The glyph atlas is uploaded once (an R8_UNORM coverage texture).
    Status UploadGlyphAtlas(const std::uint8_t* coverage, std::uint32_t width, std::uint32_t height);
    void ResetOverlay();
    void AddOverlayQuad(float x, float y, float width, float height, float u0, float v0, float u1,
                        float v1, const float rgba[4]);
    void AddOverlaySolid(float x, float y, float width, float height, const float rgba[4]);

    // The untiling compute pass: one Dispatch per planned tile row block, with the
    // plan's own group counts (bbx_untile decides them, not this file).
    Status DispatchUntile(const UntilePlan& plan, ID3D12Resource* tiled_source,
                          ID3D12Resource* linear_destination, std::uint32_t bytes_per_tile);

    Status EndFrame(bool vsync);

    // Boot: dispatch, fence, readback, compare. `elements` is the buffer length.
    Status RunStreamSelftest(std::uint32_t elements = 64);
    [[nodiscard]] bool SelftestPassed() const { return selftest_passed_; }
    [[nodiscard]] const std::string& SelftestReport() const { return selftest_report_; }

    [[nodiscard]] const ReplayStats& Stats() const { return stats_; }
    [[nodiscard]] std::string Describe() const;

private:
    struct Slice {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    };
    Slice AllocateCbvSrvUav(std::uint32_t count = 1);
    Slice ReserveSamplers(std::uint32_t count = 1);
    Status CreateUploadRing();
    Status CreateOverlayBuffer();
    Status CreateWhiteTexture();
    Status CreateFrameImage(std::uint32_t width, std::uint32_t height);
    Status CreateSelftestResources(std::uint32_t elements);
    Status CloseAndSubmit();
    Status TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after, bool uav = false);

    Device* device_ = nullptr;
    Swapchain* swapchain_ = nullptr;
    PipelineSet* pipelines_ = nullptr;

    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Resource> upload_;         // the per-frame ring, mapped
    void* upload_mapped_ = nullptr;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t upload_used_ = 0;

    ComPtr<ID3D12Resource> frame_image_;    // the guest's scanout image (256x256 until the first flip)
    // Its size, and the staging buffer the guest's frame is copied through: one
    // UPLOAD heap buffer, remade when the frame grows, kept out of the 4 MiB
    // per-frame ring a 1080p frame does not fit in.
    std::uint32_t frame_image_width_ = 0, frame_image_height_ = 0;
    ComPtr<ID3D12Resource> frame_staging_;
    void* frame_staging_mapped_ = nullptr;
    std::uint64_t frame_staging_bytes_ = 0;
    ComPtr<ID3D12Resource> atlas_;          // R8_UNORM glyph coverage
    ComPtr<ID3D12Resource> white_;          // 1x1 white: solid overlay panels
    Slice frame_srv_{};
    Slice atlas_srv_{};
    Slice white_srv_{};

    ComPtr<ID3D12Resource> overlay_vertices_;
    void* overlay_mapped_ = nullptr;
    std::uint64_t overlay_capacity_ = 0;
    std::uint32_t overlay_count_ = 0;

    ComPtr<ID3D12Resource> selftest_uav_;
    ComPtr<ID3D12Resource> selftest_readback_;
    Slice selftest_descriptor_{};

    // The swapchain's buffers keep their state across frames: FLIP_DISCARD hands
    // them back in RENDER_TARGET the first time and PRESENT after every EndFrame,
    // and a transition that lies about `before` is a device removal.
    std::array<D3D12_RESOURCE_STATES, 3> buffer_state_{};
    std::uint32_t next_cbv_srv_uav_ = 0;
    std::uint32_t next_sampler_ = 0;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    bool in_frame_ = false;
    bool selftest_passed_ = false;
    std::string selftest_report_ = "not run";
    ReplayStats stats_{};
};

}  // namespace bbx::d3d12
