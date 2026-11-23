// SPDX-License-Identifier: GPL-3.0-or-later
// The replay, as described in replay.h.
#include "src/host/d3d12/replay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "src/host/d3d12/bbx_barriers.h"
#include "src/host/d3d12/device.h"
#include "src/host/d3d12/pipeline.h"
#include "src/host/d3d12/swapchain.h"

namespace bbx::d3d12 {
namespace {

constexpr std::uint64_t kUploadRingBytes = 4ull << 20;  // 4 MiB a frame
// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT: a texture upload's source rows land on it,
// which is now the plan's own dst_row_pitch (bbx_untile aligns it when it plans).
constexpr std::uint64_t kOverlayBytes = 1ull << 20;     // 32,768 vertices (~5,461 6-vertex quads)
constexpr std::uint32_t kFrameImageSize = 256;  // the placeholder final image

// The CPU's copy of stream_selftest.hlsl's Pattern(). Both must agree; the
// arithmetic is pinned by the host test for the shader's contract.
std::uint32_t SelftestPattern(std::uint32_t index, std::uint32_t seed) {
    return index * 2654435761u + seed;
}

D3D12_RESOURCE_DESC BufferDesc(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
    // Only Type may be set for the three standard heap types. CPUPageProperty and
    // MemoryPoolPreference describe *custom* heaps; the runtime rejects anything
    // else with E_INVALIDARG (0x80070057), which is exactly what the first
    // CreateCommittedResource (the upload ring) returned on the console:
    //     d3d12: replay: upload ring failed: 0x80070057
    // and the display was then reported as unavailable. Both fields stay
    // D3D12_CPU_PAGE_PROPERTY_UNKNOWN / D3D12_MEMORY_POOL_UNKNOWN - i.e. zero.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    return heap;
}

D3D12_RANGE kNoRead{0, 0};

}  // namespace

Replay::~Replay() { Destroy(); }

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

Status Replay::Create(Device& device, Swapchain& swapchain, PipelineSet& pipelines) {
    Destroy();
    device_ = &device;
    swapchain_ = &swapchain;
    pipelines_ = &pipelines;
    frame_width_ = swapchain.Width();
    frame_height_ = swapchain.Height();
    // A freshly created DXGI swapchain buffer starts in PRESENT (COMMON);
    // BeginFrame transitions it to RENDER_TARGET and EndFrame returns it to PRESENT.
    buffer_state_.fill(D3D12_RESOURCE_STATE_PRESENT);

    // One command list: bbhost builds the frame on the CPU and submits once, so a
    // second list would only add a sync point. (P3's draw path keeps this.)
    const HRESULT list = device.Get()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, device.FrameAllocator(), nullptr,
        __uuidof(ID3D12GraphicsCommandList),
        reinterpret_cast<void**>(command_list_.GetAddressOf()));
    if (FAILED(list)) {
        Log("replay: CreateCommandList failed: 0x%08lX", static_cast<unsigned long>(list));
        return FromHresult(list);
    }
    command_list_->Close();

    Status status = CreateUploadRing();
    if (status != Status::kOk) { return status; }
    status = CreateOverlayBuffer();
    if (status != Status::kOk) { return status; }
    status = CreateFrameImage(kFrameImageSize, kFrameImageSize, DxgiFormat::kR8G8B8A8Unorm);
    if (status != Status::kOk) { return status; }
    // A 1x1 white texture: the source for solid overlay panels, so the overlay
    // pass has one pipeline and one descriptor shape whether or not a glyph atlas
    // was uploaded.
    status = CreateWhiteTexture();
    if (status != Status::kOk) { return status; }
    untile_srv_ = AllocateCbvSrvUav();
    untile_uav_ = AllocateCbvSrvUav();
    for (Slice& direct_srv : direct_frame_srvs_) {
        direct_srv = AllocateCbvSrvUav();
    }

    // The pipelines are created lazily by PipelineSet::Ensure; ask for the two the
    // frame needs now so a missing PSO is reported at boot, not mid-frame.
    for (const PassKind kind : {PassKind::kPresent, PassKind::kOverlay, PassKind::kUntile,
                                PassKind::kSelftest}) {
        if (pipelines_->Ensure(kind) == nullptr) {
            Log("replay: %s will not be available this run", PassName(kind));
        }
    }
    Log("replay: ready (%ux%u, %llu KiB upload ring)", frame_width_, frame_height_,
        static_cast<unsigned long long>(kUploadRingBytes >> 10));
    return Status::kOk;
}

Status Replay::CreateUploadRing() {
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(kUploadRingBytes, D3D12_RESOURCE_FLAG_NONE);
    const HRESULT hr = device_->Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(upload_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: upload ring failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    const HRESULT mapped = upload_->Map(0, &kNoRead, &upload_mapped_);
    if (FAILED(mapped)) { return FromHresult(mapped); }
    upload_capacity_ = kUploadRingBytes;
    upload_used_ = 0;
    return Status::kOk;
}

Status Replay::CreateOverlayBuffer() {
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(kOverlayBytes, D3D12_RESOURCE_FLAG_NONE);
    const HRESULT hr = device_->Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(overlay_vertices_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: overlay vertex buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    const HRESULT mapped = overlay_vertices_->Map(0, &kNoRead, &overlay_mapped_);
    if (FAILED(mapped)) { return FromHresult(mapped); }
    overlay_capacity_ = kOverlayBytes / sizeof(OverlayVertex);
    return Status::kOk;
}

Status Replay::CreateFrameImage(std::uint32_t width, std::uint32_t height, DxgiFormat format) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    // The guest's scanout format, as DXGI states it - B8G8R8A8 for the console's
    // A8R8G8B8. Making the image in the memory order keeps the copy a copy: a
    // swizzle here is where red and blue get swapped.
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const HRESULT hr = device_->Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(frame_image_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: frame image failed: 0x%08lX", static_cast<unsigned long>(hr));
        // The present pass can still run against the atlas texture; say so.
        frame_image_.Reset();
        return FromHresult(hr);
    }
    if (frame_srv_.cpu.ptr == 0) {
        frame_srv_ = AllocateCbvSrvUav();
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = static_cast<DXGI_FORMAT>(format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->Get()->CreateShaderResourceView(frame_image_.Get(), &srv, frame_srv_.cpu);
    frame_image_width_ = width;
    frame_image_height_ = height;
    frame_image_format_ = format;
    return Status::kOk;
}

Status Replay::UploadFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    // RGBA8, linearly: what the stream's transfers and the HLE's own frames are.
    // In the console's own names that is A8B8G8R8 (the COMP_SWAP_ALT of the CB's
    // 8-bit pair: bytes R,G,B,A) with tiling mode 1. A display buffer says more
    // than that - bbx_scanout - and its describing overload is the one that has
    // to be exactly right about the format.
    ScanoutDesc desc;
    desc.format = ScanoutFormat::kA8B8G8R8;
    desc.tiling = ScanoutTiling::kLinear;
    desc.width = width;
    desc.height = height;
    desc.pitch_pixels = width;
    return UploadFrame(rgba, desc);
}

Status Replay::UploadFrame(const std::uint8_t* pixels, const ScanoutDesc& desc) {
    if (!in_frame_) { return Status::kInvalidArgument; }
    const ScanoutState scanout = DescribeScanout(desc);
    const std::uint32_t width = desc.width;
    const std::uint32_t height = desc.height;
    if (pixels == nullptr || !scanout.ok) {
        if (!scanout.ok) { Log("replay: frame not presented: %s", scanout.reason.c_str()); }
        return Status::kInvalidArgument;
    }
    if (!frame_image_ || frame_image_width_ != width || frame_image_height_ != height ||
        frame_image_format_ != scanout.format) {
        // The guest's display changed size or format (or this is the first
        // flip): the old image goes rather than being stretched over the frame,
        // or sampled in a format its bytes are not in.
        frame_image_.Reset();
        const Status recreated = CreateFrameImage(width, height, scanout.format);
        if (recreated != Status::kOk) { return recreated; }
    }
    // D3D12 wants the source rows on a 256-byte pitch, and the guest's own rows
    // sit at the pitch it stated (the same pitch when it stated none).
    // A row is the guest's own pitch of pixels; the copy never reads more of a
    // row than the image can hold.
    const std::uint64_t row_bytes = std::min<std::uint64_t>(scanout.row_pitch, scanout.dst_row_pitch);
    const std::uint64_t pitch = scanout.dst_row_pitch;
    const std::uint64_t bytes = pitch * height;
    // The staging buffer holds the guest's bytes whole (a tiled buffer is copied
    // up in one run) and then the detiled rows, so it is the larger of the two.
    const std::uint64_t staging_bytes = std::max(bytes, scanout.bytes);
    if (frame_staging_ == nullptr || frame_staging_bytes_ < staging_bytes) {
        frame_staging_.Reset();
        frame_staging_mapped_ = nullptr;
        frame_staging_bytes_ = 0;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC desc = BufferDesc(staging_bytes, D3D12_RESOURCE_FLAG_NONE);
        const HRESULT hr = device_->Get()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(frame_staging_.GetAddressOf()));
        if (FAILED(hr)) {
            Log("replay: frame staging buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
            frame_staging_.Reset();
            return FromHresult(hr);
        }
        if (FAILED(frame_staging_->Map(0, &kNoRead, &frame_staging_mapped_)) ||
            frame_staging_mapped_ == nullptr) {
            Log("replay: frame staging buffer could not be mapped");
            frame_staging_.Reset();
            frame_staging_mapped_ = nullptr;
            return Status::kUnavailable;
        }
        frame_staging_bytes_ = staging_bytes;
    }
    auto* rows = static_cast<std::uint8_t*>(frame_staging_mapped_);
    if (scanout.tiled) {
        // The guest's bytes are micro tiles, so a row copy would shear the
        // frame. They go up whole and UntileCs linearises them into a buffer
        // whose rows are the texture's - the copy after it is then the linear
        // one below, from bytes instead of from the guest's memory.
        std::memcpy(rows, pixels, static_cast<std::size_t>(std::min<std::uint64_t>(scanout.bytes, staging_bytes)));
        if (frame_linear_ == nullptr || frame_linear_bytes_ < bytes) {
            frame_linear_.Reset();
            const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC linear = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const HRESULT hr = device_->Get()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &linear, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void**>(frame_linear_.GetAddressOf()));
            if (FAILED(hr)) {
                Log("replay: linear frame buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
                frame_linear_.Reset();
                return FromHresult(hr);
            }
            frame_linear_bytes_ = bytes;
        }
        const Status untiled =
            DispatchUntile(scanout.plan, frame_staging_.Get(), staging_bytes, frame_linear_.Get(),
                           bytes, scanout.bytes_per_pixel * 64u, /*source_is_upload=*/true);
        if (untiled != Status::kOk) {
            Log("replay: the tiled scanout was not detiled (%s); the frame is dropped",
                StatusName(untiled));
            return untiled;
        }
        (void)TransitionBarrier(frame_linear_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION linear_source{};
        linear_source.pResource = frame_linear_.Get();
        linear_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        linear_source.PlacedFootprint.Offset = 0;
        linear_source.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(scanout.format);
        linear_source.PlacedFootprint.Footprint.Width = width;
        linear_source.PlacedFootprint.Footprint.Height = height;
        linear_source.PlacedFootprint.Footprint.Depth = 1;
        linear_source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
        D3D12_TEXTURE_COPY_LOCATION frame_target{};
        frame_target.pResource = frame_image_.Get();
        frame_target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        frame_target.SubresourceIndex = 0;
        (void)TransitionBarrier(frame_image_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        command_list_->CopyTextureRegion(&frame_target, 0, 0, 0, &linear_source, nullptr);
        (void)TransitionBarrier(frame_image_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        (void)TransitionBarrier(frame_linear_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        ++stats_.copies;
        return Status::kOk;
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(rows + static_cast<std::uint64_t>(row) * pitch,
                    pixels + static_cast<std::uint64_t>(row) * scanout.row_pitch,
                    static_cast<std::size_t>(row_bytes));
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = frame_staging_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(scanout.format);
    source.PlacedFootprint.Footprint.Width = width;
    source.PlacedFootprint.Footprint.Height = height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = frame_image_.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    // The image is created readable (PIXEL_SHADER_RESOURCE); each upload takes it
    // out of that state and puts it back, so DrawPresent always samples a
    // resource in the state it declares.
    (void)TransitionBarrier(frame_image_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    (void)TransitionBarrier(frame_image_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ++stats_.copies;
    return Status::kOk;
}

Status Replay::CreateWhiteTexture() {
    constexpr std::uint32_t kWhitePixel = 0xFFFFFFFFu;
    constexpr std::uint32_t kPitchAlignment = 256;

    // D3D12_HEAP_TYPE_UPLOAD only permits BUFFER resources; a 1x1 TEXTURE2D must
    // live in D3D12_HEAP_TYPE_DEFAULT and receive its pixel through a 256-byte
    // aligned UPLOAD buffer copy.
    ComPtr<ID3D12Resource> staging;
    const D3D12_HEAP_PROPERTIES upload_heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC staging_desc = BufferDesc(kPitchAlignment, D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = device_->Get()->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &staging_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(staging.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: white staging buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, &kNoRead, &mapped)) || mapped == nullptr) {
        Log("replay: white staging buffer could not be mapped");
        return Status::kUnavailable;
    }
    std::memset(mapped, 0, kPitchAlignment);
    std::memcpy(mapped, &kWhitePixel, sizeof(kWhitePixel));
    staging->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES default_heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> white;
    hr = device_->Get()->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(white.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: white texture failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }

    ID3D12CommandAllocator* allocator = device_->FrameAllocator();
    const HRESULT reset = allocator->Reset();
    if (FAILED(reset)) { return FromHresult(reset); }
    if (FAILED(command_list_->Reset(allocator, nullptr))) { return Status::kUnavailable; }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = 1;
    source.PlacedFootprint.Footprint.Height = 1;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = kPitchAlignment;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = white.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    (void)TransitionBarrier(white.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const Status submitted = CloseAndSubmit();
    if (submitted != Status::kOk) { return submitted; }
    if (!device_->WaitFor(device_->SubmittedValue())) {
        Log("replay: white texture upload fence did not pass");
        return device_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;
    }
    device_->NextFrame();
    white_ = std::move(white);

    white_srv_ = AllocateCbvSrvUav();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->Get()->CreateShaderResourceView(white_.Get(), &srv, white_srv_.cpu);
    return Status::kOk;
}

Replay::Slice Replay::AllocateCbvSrvUav(std::uint32_t count) {
    Slice slice{};
    ID3D12DescriptorHeap* heap = device_->CbvSrvUavHeap();
    if (heap == nullptr) { return slice; }
    const std::uint32_t stride = device_->CbvSrvUavStride();
    slice.cpu = heap->GetCPUDescriptorHandleForHeapStart();
    slice.gpu = heap->GetGPUDescriptorHandleForHeapStart();
    slice.cpu.ptr += static_cast<SIZE_T>(next_cbv_srv_uav_) * stride;
    slice.gpu.ptr += static_cast<UINT64>(next_cbv_srv_uav_) * stride;
    next_cbv_srv_uav_ += count;
    if (next_cbv_srv_uav_ >= kReservedCbvSrvUavDescriptors) {
        // The permanent descriptors are bounded; a boot that allocated them all is
        // a bug in the port, not a guest's doing. Wrap and log once.
        Log("replay: the permanent descriptor block is full (%u used)",
            kReservedCbvSrvUavDescriptors);
        next_cbv_srv_uav_ = 0;
    }
    return slice;
}

Replay::Slice Replay::ReserveSamplers(std::uint32_t count) {
    Slice slice{};
    ID3D12DescriptorHeap* heap = device_->SamplerHeap();
    if (heap == nullptr) { return slice; }
    const std::uint32_t stride = device_->SamplerStride();
    slice.cpu = heap->GetCPUDescriptorHandleForHeapStart();
    slice.gpu = heap->GetGPUDescriptorHandleForHeapStart();
    slice.cpu.ptr += static_cast<SIZE_T>(next_sampler_) * stride;
    slice.gpu.ptr += static_cast<UINT64>(next_sampler_) * stride;
    next_sampler_ += count;
    if (next_sampler_ >= Device::kSamplerDescriptors) { next_sampler_ = 0; }
    return slice;
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

Status Replay::BeginFrame() {
    if (command_list_ == nullptr || device_ == nullptr) { return Status::kUnavailable; }
    if (in_frame_) { return Status::kInvalidArgument; }
    // The allocator and direct_frame_srvs_[frame_] may only be reused once the
    // GPU has finished the previous submission that used this frame slot (critical
    // when VRR / ALLOW_TEARING lets Present(0) return immediately).
    if (!device_->WaitForFrameSlot()) {
        return device_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;
    }
    const HRESULT reset = device_->FrameAllocator()->Reset();
    if (FAILED(reset)) {
        Log("replay: allocator reset failed: 0x%08lX", static_cast<unsigned long>(reset));
        return FromHresult(reset);
    }
    const HRESULT list = command_list_->Reset(device_->FrameAllocator(), nullptr);
    if (FAILED(list)) { return FromHresult(list); }

    upload_used_ = 0;
    overlay_count_ = 0;
    frame_width_ = swapchain_->Width();
    frame_height_ = swapchain_->Height();

    ID3D12DescriptorHeap* heaps[] = {device_->CbvSrvUavHeap()};
    command_list_->SetDescriptorHeaps(1, heaps);
    in_frame_ = true;
    return Status::kOk;
}

Status Replay::ExecuteStreamBlock(std::span<const std::uint8_t> block) {
    if (!in_frame_) { return Status::kInvalidArgument; }
    // WalkStream is callback-driven, so the replay counts as it emits and keeps
    // the walk's verdict: a block that stops at a structural error is replayed up
    // to that point and reported, never read past.
    const WalkResult walk = WalkStream(
        block, [this](const RecordHeader& header, std::span<const std::uint8_t> payload) {
            ++stats_.records;
            switch (header.kind) {
                case RecordKind::kClearColor:
                case RecordKind::kClearDepth:
                    // A colour clear needs the guest's image until P3's resource
                    // table exists; the count still proves the record was read.
                    ++stats_.clears;
                    break;
                case RecordKind::kFill:
                case RecordKind::kUpdateBuffer:
                case RecordKind::kCopyBuffer:
                    ++stats_.copies;
                    break;
                case RecordKind::kCopyImage:
                case RecordKind::kCopyBufferToImage:
                case RecordKind::kCopyImageToBuffer:
                    ++stats_.skipped_images;
                    break;
                case RecordKind::kBarrier:
                case RecordKind::kTransferBarrier:
                case RecordKind::kCopyOrderBarrier:
                case RecordKind::kPassBarrier:
                    ++stats_.barriers;
                    break;
                case RecordKind::kMarker: {
                    // The payload is the marker's ASCII name; a PIX event makes a
                    // capture on a dev console line up with the breadcrumb log.
                    const char* name = payload.empty()
                                           ? RecordKindName(header.kind)
                                           : reinterpret_cast<const char*>(payload.data());
                    command_list_->BeginEvent(0, name, static_cast<UINT>(payload.size()));
                    command_list_->EndEvent();
                    ++stats_.markers;
                    break;
                }
                case RecordKind::kDispatch:
                case RecordKind::kDispatchIndirect: ++stats_.dispatches; break;
                case RecordKind::kTimestamp:
                case RecordKind::kEndQuery:
                case RecordKind::kCopyQueryResults:
                    // Timestamps need a query heap per frame; the HUD's frame time
                    // comes from the fence instead until P3's label writebacks.
                    ++stats_.markers;
                    break;
                default: break;
            }
        });
    if (!walk.ok()) {
        ++stats_.walk_errors;
        Log("replay: stream block did not walk cleanly (%s at byte %zu, %zu record(s) read)",
            WalkErrorName(walk.error), walk.bad_offset, walk.records);
        return Status::kUnavailable;
    }
    return Status::kOk;
}

Status Replay::DrawPresent(float red, float green, float blue, float sharpness) {
    if (!in_frame_) { return Status::kInvalidArgument; }
    ID3D12PipelineState* pso = pipelines_->Ensure(PassKind::kPresent);
    if (pso == nullptr) { return Status::kUnavailable; }
    ID3D12Resource* buffer = swapchain_->CurrentBackBuffer();
    if (buffer == nullptr) { return Status::kUnavailable; }

    const std::uint32_t index = swapchain_->CurrentIndex();
    D3D12_RESOURCE_STATES& state = buffer_state_[index % buffer_state_.size()];
    const Status transition = TransitionBarrier(buffer, state,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (transition != Status::kOk) { return transition; }
    state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->Rtv(index);
    const float clear[4] = {red, green, blue, 1.0f};
    command_list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float texel_x = frame_image_width_ > 0 ? (1.0f / static_cast<float>(frame_image_width_)) : 0.0f;
    const float texel_y = frame_image_height_ > 0 ? (1.0f / static_cast<float>(frame_image_height_)) : 0.0f;
    const float clamped_sharp = sharpness < 0.0f ? 0.0f : (sharpness > 1.0f ? 1.0f : sharpness);
    const float backdrop_aspect =
        (red < 0.5f)
            ? (frame_height_ > 0 ? (static_cast<float>(frame_width_) / static_cast<float>(frame_height_))
                                 : (16.0f / 9.0f))
            : 0.0f;
    const float present_constants[8] = {red,     green,   blue,          1.0f,
                                        texel_x, texel_y, clamped_sharp, backdrop_aspect};
    command_list_->SetGraphicsRootSignature(pipelines_->RootSignature(PassKind::kPresent));
    command_list_->SetPipelineState(pso);
    command_list_->SetGraphicsRoot32BitConstants(0, 8, present_constants, 0);
    if (frame_srv_.gpu.ptr != 0) {
        command_list_->SetGraphicsRootDescriptorTable(1, frame_srv_.gpu);
    } else if (white_srv_.gpu.ptr != 0) {
        command_list_->SetGraphicsRootDescriptorTable(1, white_srv_.gpu);
    }
    // The fullscreen triangle is generated in present.hlsl from SV_VertexID.
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(frame_width_),
                                  static_cast<float>(frame_height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(frame_width_),
                             static_cast<LONG>(frame_height_)};
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->DrawInstanced(3, 1, 0, 0);
    return Status::kOk;
}

Status Replay::DrawPresentDirect(ID3D12Resource* src_rt, DXGI_FORMAT format, std::uint32_t width,
                                 std::uint32_t height, float red, float green, float blue,
                                 float sharpness) {
    if (!in_frame_ || src_rt == nullptr || width == 0 || height == 0) {
        return Status::kInvalidArgument;
    }
    ID3D12PipelineState* pso = pipelines_->Ensure(PassKind::kPresent);
    if (pso == nullptr) { return Status::kUnavailable; }
    ID3D12Resource* buffer = swapchain_->CurrentBackBuffer();
    if (buffer == nullptr) { return Status::kUnavailable; }

    const std::uint32_t index = swapchain_->CurrentIndex();
    D3D12_RESOURCE_STATES& state = buffer_state_[index % buffer_state_.size()];
    const Status transition = TransitionBarrier(buffer, state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (transition != Status::kOk) { return transition; }
    state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // Sample the native GPU render target directly without CPU copy or UntileCs.
    (void)TransitionBarrier(src_rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    const std::uint32_t slot = device_->FrameIndex() % static_cast<std::uint32_t>(direct_frame_srvs_.size());
    const Slice& direct_srv = direct_frame_srvs_[slot];
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->Get()->CreateShaderResourceView(src_rt, &srv, direct_srv.cpu);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->Rtv(index);
    const float clear[4] = {red, green, blue, 1.0f};
    command_list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float texel_x = 1.0f / static_cast<float>(width);
    const float texel_y = 1.0f / static_cast<float>(height);
    const float clamped_sharp = sharpness < 0.0f ? 0.0f : (sharpness > 1.0f ? 1.0f : sharpness);
    const float present_constants[8] = {red, green, blue, 1.0f, texel_x, texel_y, clamped_sharp, 0.0f};
    command_list_->SetGraphicsRootSignature(pipelines_->RootSignature(PassKind::kPresent));
    command_list_->SetPipelineState(pso);
    command_list_->SetGraphicsRoot32BitConstants(0, 8, present_constants, 0);
    command_list_->SetGraphicsRootDescriptorTable(1, direct_srv.gpu);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(frame_width_),
                                  static_cast<float>(frame_height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(frame_width_),
                             static_cast<LONG>(frame_height_)};
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->DrawInstanced(3, 1, 0, 0);

    // Restore the native render target to GuestGraphics' invariant RENDER_TARGET state.
    (void)TransitionBarrier(src_rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    return Status::kOk;
}

Status Replay::UploadGlyphAtlas(const std::uint8_t* coverage, std::uint32_t width,
                                std::uint32_t height) {
    if (device_ == nullptr || coverage == nullptr || width == 0 || height == 0) {
        return Status::kInvalidArgument;
    }
    // A placed-footprint upload needs a 256-aligned row pitch (the SDK's
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); the atlas is 1 byte a pixel, so the
    // rows are repacked here rather than assumed contiguous.
    constexpr std::uint32_t kPitchAlignment = 256;
    const std::uint32_t pitch = ((width + kPitchAlignment - 1) / kPitchAlignment) * kPitchAlignment;
    const std::uint64_t staging_bytes = static_cast<std::uint64_t>(pitch) * height;

    ComPtr<ID3D12Resource> staging;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(staging_bytes, D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = device_->Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(staging.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: atlas staging buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, &kNoRead, &mapped)) || mapped == nullptr) {
        return Status::kUnavailable;
    }
    auto* rows = static_cast<std::uint8_t*>(mapped);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(rows + static_cast<std::uint64_t>(row) * pitch,
                    coverage + static_cast<std::uint64_t>(row) * width, width);
    }

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8_UNORM;
    texture.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES default_heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    atlas_.Reset();
    hr = device_->Get()->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(atlas_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("replay: glyph atlas texture failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }

    // The copy gets its own submission, outside the frame: the atlas is uploaded
    // once, at boot, before the first BeginFrame.
    ID3D12CommandAllocator* allocator = device_->FrameAllocator();
    const HRESULT reset = allocator->Reset();
    if (FAILED(reset)) { return FromHresult(reset); }
    if (FAILED(command_list_->Reset(allocator, nullptr))) { return Status::kUnavailable; }
    ID3D12DescriptorHeap* heaps[] = {device_->CbvSrvUavHeap()};
    command_list_->SetDescriptorHeaps(1, heaps);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    source.PlacedFootprint.Footprint.Width = width;
    source.PlacedFootprint.Footprint.Height = height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = atlas_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    (void)TransitionBarrier(atlas_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const Status submitted = CloseAndSubmit();
    if (submitted != Status::kOk) { return submitted; }
    // The staging buffer must outlive the copy: wait for the fence that covers it
    // rather than letting it be released with the frame.
    if (!device_->WaitFor(device_->SubmittedValue())) {
        Log("replay: the atlas upload's fence did not pass");
        return device_->DeviceLost() ? Status::kDeviceLost : Status::kUnavailable;
    }
    staging->Unmap(0, nullptr);
    device_->NextFrame();

    atlas_srv_ = AllocateCbvSrvUav();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->Get()->CreateShaderResourceView(atlas_.Get(), &srv, atlas_srv_.cpu);
    Log("replay: glyph atlas %ux%u uploaded (%llu bytes, pitch %u)", width, height,
        static_cast<unsigned long long>(staging_bytes), pitch);
    return Status::kOk;
}

void Replay::ResetOverlay() { overlay_count_ = 0; }

void Replay::AddOverlayQuad(float x, float y, float width, float height, float u0, float v0,
                            float u1, float v1, const float rgba[4]) {
    if (overlay_count_ + 6 > overlay_capacity_ || overlay_mapped_ == nullptr) { return; }
    auto* vertices = static_cast<OverlayVertex*>(overlay_mapped_);
    auto push = [&](float px, float py, float tu, float tv) {
        OverlayVertex& vertex = vertices[overlay_count_++];
        vertex.x = px;
        vertex.y = py;
        vertex.u = tu;
        vertex.v = tv;
        vertex.r = rgba[0];
        vertex.g = rgba[1];
        vertex.b = rgba[2];
        vertex.a = rgba[3];
    };
    push(x, y, u0, v0);
    push(x + width, y, u1, v0);
    push(x, y + height, u0, v1);
    push(x + width, y, u1, v0);
    push(x + width, y + height, u1, v1);
    push(x, y + height, u0, v1);
}

void Replay::AddOverlaySolid(float x, float y, float width, float height, const float rgba[4]) {
    // The white texture is the whole quad, so the UVs do not matter past 0..1.
    AddOverlayQuad(x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, rgba);
}

Status Replay::DrawOverlay() {
    if (!in_frame_) { return Status::kInvalidArgument; }
    if (overlay_count_ == 0) { return Status::kOk; }
    ID3D12PipelineState* pso = pipelines_->Ensure(PassKind::kOverlay);
    if (pso == nullptr) { return Status::kUnavailable; }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->Rtv(swapchain_->CurrentIndex());
    // The overlay draws with the swapchain's target, not the present pass's: no
    // scene texture is read, only the atlas (or the white 1x1 as a solid).
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float constants[4] = {static_cast<float>(frame_width_),
                                static_cast<float>(frame_height_), 1.0f, 0.0f};
    command_list_->SetGraphicsRootSignature(pipelines_->RootSignature(PassKind::kOverlay));
    command_list_->SetPipelineState(pso);
    command_list_->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    const Slice& texture = atlas_srv_.gpu.ptr != 0 ? atlas_srv_ : white_srv_;
    if (texture.gpu.ptr != 0) {
        command_list_->SetGraphicsRootDescriptorTable(1, texture.gpu);
    }
    const D3D12_VERTEX_BUFFER_VIEW view{overlay_vertices_->GetGPUVirtualAddress(),
                                        static_cast<UINT>(overlay_count_ * sizeof(OverlayVertex)),
                                        static_cast<UINT>(sizeof(OverlayVertex))};
    command_list_->IASetVertexBuffers(0, 1, &view);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(frame_width_),
                                  static_cast<float>(frame_height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(frame_width_),
                             static_cast<LONG>(frame_height_)};
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->DrawInstanced(overlay_count_, 1, 0, 0);
    return Status::kOk;
}

Status Replay::DispatchUntile(const UntilePlan& plan, ID3D12Resource* tiled_source,
                              std::uint64_t source_bytes, ID3D12Resource* linear_destination,
                              std::uint64_t destination_bytes, std::uint32_t bytes_per_tile,
                              bool source_is_upload) {
    if (!in_frame_) { return Status::kInvalidArgument; }
    if (!plan.valid || plan.dispatches.empty()) {
        Log("untile: no plan (%s); the copy is skipped", plan.reason.c_str());
        return Status::kInvalidArgument;
    }
    if (tiled_source == nullptr || linear_destination == nullptr || source_bytes < 4 ||
        destination_bytes < 4 || bytes_per_tile == 0) {
        return Status::kInvalidArgument;
    }
    ID3D12PipelineState* pso = pipelines_->Ensure(PassKind::kUntile);
    if (pso == nullptr) { return Status::kUnavailable; }

    // Source readable, destination writable: UPLOAD heap buffers must remain in
    // GENERIC_READ, while DEFAULT heap sources transition from COPY_DEST.
    if (!source_is_upload) {
        (void)TransitionBarrier(tiled_source, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    (void)TransitionBarrier(linear_destination, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const Slice source = untile_srv_;
    const Slice destination = untile_uav_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    // ByteAddressBuffer in untile.hlsl: a raw buffer, so R32_TYPELESS + RAW.
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(source_bytes / 4u);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->Get()->CreateShaderResourceView(tiled_source, &srv, source.cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(destination_bytes / 4u);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->Get()->CreateUnorderedAccessView(linear_destination, nullptr, &uav, destination.cpu);

    command_list_->SetComputeRootSignature(pipelines_->RootSignature(PassKind::kUntile));
    command_list_->SetPipelineState(pso);
    for (const UntileDispatch& dispatch : plan.dispatches) {
        // b0, in untile.hlsl's own declaration order: uint2 tile_counts,
        // uint2 pixels, uint swizzle, uint bytes_per_tile, uint src_origin,
        // uint dst_row_pitch. The dispatch's src_offset is the only origin the
        // shader is given - the plan already folded the tile row into it.
        const std::uint32_t constants[8] = {dispatch.tiles_x,
                                            dispatch.tiles_y,
                                            plan.width,
                                            plan.height,
                                            static_cast<std::uint32_t>(plan.tiling),
                                            bytes_per_tile,
                                            static_cast<std::uint32_t>(dispatch.src_offset),
                                            plan.dst_row_pitch};
        command_list_->SetComputeRoot32BitConstants(0, 8, constants, 0);
        // Root parameters 1 and 2 are the source and destination tables (b0 is 0).
        command_list_->SetComputeRootDescriptorTable(1, source.gpu);
        command_list_->SetComputeRootDescriptorTable(2, destination.gpu);
        command_list_->Dispatch(dispatch.groups_x, dispatch.groups_y, dispatch.groups_z);
        (void)TransitionBarrier(linear_destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, /*uav=*/true);
        ++stats_.dispatches;
    }
    return Status::kOk;
}

Status Replay::EndFrame(bool vsync) {
    if (!in_frame_) { return Status::kInvalidArgument; }
    in_frame_ = false;
    ID3D12Resource* buffer = swapchain_->CurrentBackBuffer();
    if (buffer == nullptr) { return Status::kUnavailable; }
    const std::uint32_t index = swapchain_->CurrentIndex();
    D3D12_RESOURCE_STATES& state = buffer_state_[index % buffer_state_.size()];
    const Status transition = TransitionBarrier(buffer, state, D3D12_RESOURCE_STATE_PRESENT);
    if (transition != Status::kOk) { return transition; }
    state = D3D12_RESOURCE_STATE_PRESENT;
    const Status submitted = CloseAndSubmit();
    if (submitted != Status::kOk) { return submitted; }
    const Status present = swapchain_->Present(vsync);
    if (present != Status::kOk) { return present; }
    ++stats_.frames;
    device_->NextFrame();
    return Status::kOk;
}

Status Replay::CloseAndSubmit() {
    const HRESULT closed = command_list_->Close();
    if (FAILED(closed)) {
        Log("replay: command list close failed: 0x%08lX", static_cast<unsigned long>(closed));
        return FromHresult(closed);
    }
    ID3D12CommandList* lists[] = {command_list_.Get()};
    const std::uint64_t fence = device_->ExecuteAndSignal(lists, 1);
    device_->MarkFrameSubmitted(fence);
    return Status::kOk;
}

Status Replay::TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                 D3D12_RESOURCE_STATES after, bool uav) {
    if (resource == nullptr) { return Status::kInvalidArgument; }
    D3D12_RESOURCE_BARRIER barrier{};
    if (uav) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
    } else {
        // A barrier that changes nothing is elided: the console charges for it.
        if (before == after) { return Status::kOk; }
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
    }
    command_list_->ResourceBarrier(1, &barrier);
    return Status::kOk;
}

Status Replay::CreateSelftestResources(std::uint32_t elements) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(elements) * sizeof(std::uint32_t);
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC desc = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = device_->Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(selftest_uav_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("selftest: UAV buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    const D3D12_HEAP_PROPERTIES readback = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC readback_desc = BufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
    hr = device_->Get()->CreateCommittedResource(
        &readback, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(selftest_readback_.GetAddressOf()));
    if (FAILED(hr)) {
        Log("selftest: readback buffer failed: 0x%08lX", static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    selftest_descriptor_ = AllocateCbvSrvUav();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    // RWByteAddressBuffer in stream_selftest.hlsl: raw, R32_TYPELESS + RAW.
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = elements;
    uav.Buffer.StructureByteStride = 0;
    device_->Get()->CreateUnorderedAccessView(selftest_uav_.Get(), nullptr, &uav,
                                             selftest_descriptor_.cpu);
    return Status::kOk;
}

Status Replay::RunStreamSelftest(std::uint32_t elements) {
    if (device_ == nullptr || command_list_ == nullptr || pipelines_ == nullptr) {
        selftest_report_ = "not run (no device)";
        return Status::kUnavailable;
    }
    ID3D12PipelineState* pso = pipelines_->Ensure(PassKind::kSelftest);
    if (pso == nullptr) {
        selftest_report_ = "unavailable (SelftestCs PSO refused)";
        return Status::kUnavailable;
    }
    const Status created = CreateSelftestResources(elements);
    if (created != Status::kOk) {
        selftest_report_ = "unavailable (buffer creation failed)";
        return created;
    }

    const std::uint32_t seed = 0x9E3779B9u;
    const HRESULT reset = device_->FrameAllocator()->Reset();
    if (FAILED(reset)) {
        selftest_report_ = "failed (allocator reset)";
        return FromHresult(reset);
    }
    if (FAILED(command_list_->Reset(device_->FrameAllocator(), pso))) {
        selftest_report_ = "failed (command list reset)";
        return Status::kUnavailable;
    }
    ID3D12DescriptorHeap* heaps[] = {device_->CbvSrvUavHeap()};
    command_list_->SetDescriptorHeaps(1, heaps);
    command_list_->SetComputeRootSignature(pipelines_->RootSignature(PassKind::kSelftest));
    command_list_->SetPipelineState(pso);
    // b0 = { count, seed, 0, 0 }
    const std::uint32_t constants[4] = {elements, seed, 0, 0};
    command_list_->SetComputeRoot32BitConstants(0, 4, constants, 0);
    command_list_->SetComputeRootDescriptorTable(1, selftest_descriptor_.gpu);
    // 64 threads a group, one dispatch group per 64 elements.
    command_list_->Dispatch((elements + 63u) / 64u, 1, 1);
    (void)TransitionBarrier(selftest_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, /*uav=*/true);
    (void)TransitionBarrier(selftest_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list_->CopyBufferRegion(selftest_readback_.Get(), 0, selftest_uav_.Get(), 0,
                                    static_cast<UINT64>(elements) * sizeof(std::uint32_t));
    const Status submitted = CloseAndSubmit();
    if (submitted != Status::kOk) {
        selftest_report_ = "failed (submission)";
        return submitted;
    }
    const std::uint64_t value = device_->SubmittedValue();
    if (!device_->WaitFor(value)) {
        selftest_report_ = device_->DeviceLost() ? "failed (device lost)" : "failed (fence timeout)";
        Log("selftest: %s", selftest_report_.c_str());
        return Status::kDeviceLost;
    }

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(elements * sizeof(std::uint32_t))};
    if (FAILED(selftest_readback_->Map(0, &read_range, &mapped)) || mapped == nullptr) {
        selftest_report_ = "failed (readback map)";
        return Status::kUnavailable;
    }
    const auto* results = static_cast<const std::uint32_t*>(mapped);
    std::uint32_t bad_index = 0;
    std::uint32_t bad_value = 0;
    std::uint32_t bad_expected = 0;
    bool match = true;
    for (std::uint32_t index = 0; index < elements; ++index) {
        const std::uint32_t expected = SelftestPattern(index, seed);
        if (results[index] != expected) {
            match = false;
            bad_index = index;
            bad_value = results[index];
            bad_expected = expected;
            break;
        }
    }
    const D3D12_RANGE no_write{0, 0};
    selftest_readback_->Unmap(0, &no_write);

    char text[256];
    if (match) {
        std::snprintf(text, sizeof(text),
                      "passed (%u elements, fence %llu, seed 0x%08X)", elements,
                      static_cast<unsigned long long>(value), seed);
    } else {
        std::snprintf(text, sizeof(text),
                      "FAILED at element %u: got 0x%08X want 0x%08X (fence %llu)", bad_index,
                      bad_value, bad_expected, static_cast<unsigned long long>(value));
    }
    selftest_report_ = text;
    selftest_passed_ = match;
    Log("GPU self-test: %s", selftest_report_.c_str());
    // Reset for the next real frame; the selftest's list is closed and submitted.
    device_->NextFrame();
    return match ? Status::kOk : Status::kUnavailable;
}

std::string ReplayStats::Describe() const {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "replay: %llu frame(s), %llu record(s): %llu clear(s), %llu copy/copies, "
                  "%llu barrier(s), %llu dispatch(es), %llu marker(s), %llu image copy(s) deferred, "
                  "%llu walk error(s)",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(records),
                  static_cast<unsigned long long>(clears), static_cast<unsigned long long>(copies),
                  static_cast<unsigned long long>(barriers),
                  static_cast<unsigned long long>(dispatches),
                  static_cast<unsigned long long>(markers),
                  static_cast<unsigned long long>(skipped_images),
                  static_cast<unsigned long long>(walk_errors));
    return text;
}

std::string Replay::Describe() const { return stats_.Describe(); }

void Replay::Destroy() {
    in_frame_ = false;
    if (upload_ && upload_mapped_ != nullptr) {
        upload_->Unmap(0, nullptr);
        upload_mapped_ = nullptr;
    }
    if (overlay_vertices_ && overlay_mapped_ != nullptr) {
        overlay_vertices_->Unmap(0, nullptr);
        overlay_mapped_ = nullptr;
    }
    selftest_readback_.Reset();
    selftest_uav_.Reset();
    selftest_descriptor_ = Slice{};
    untile_srv_ = Slice{};
    untile_uav_ = Slice{};
    direct_frame_srvs_.fill(Slice{});
    overlay_vertices_.Reset();
    atlas_.Reset();
    white_.Reset();
    frame_image_.Reset();
    frame_linear_.Reset();
    frame_linear_bytes_ = 0;
    if (frame_staging_ != nullptr && frame_staging_mapped_ != nullptr) {
        frame_staging_->Unmap(0, nullptr);
    }
    frame_staging_mapped_ = nullptr;
    frame_staging_bytes_ = 0;
    frame_staging_.Reset();
    upload_.Reset();
    command_list_.Reset();
    overlay_count_ = 0;
    overlay_capacity_ = 0;
    upload_capacity_ = 0;
    upload_used_ = 0;
    next_cbv_srv_uav_ = 0;
    next_sampler_ = 0;
    device_ = nullptr;
    swapchain_ = nullptr;
    pipelines_ = nullptr;
}

}  // namespace bbx::d3d12
