// SPDX-License-Identifier: GPL-3.0-or-later
// Native Xbox D3D12 graphics draw executor (Slice 5B-2b).
//
// Translates decoded Bloodborne draw calls (`DrawIdentity` + `GpuDraw`) into
// native Xbox Series D3D12 graphics commands backed by pre-baked vs_6_4 and
// ps_6_4 DXIL shaders:
//
//   1. Exact bake-identical VS/PS cache key matching: when the guest creates a
//      shader from a Sony Shdr container (`NoteShaderCreated`), we run the
//      exact same `ReadConsoleContainerStage` + `shader_patch_apply` +
//      `gcn::decode` + `gcn::translate` + `predict_image_dims` +
//      `ConsolePsInputMap` + `ConsoleStageKeyOf` pipeline as `bbx_bake_cache`,
//      caching the key by patched program hash. If a draw references a program
//      never seen through `NoteShaderCreated`, a register-derived fallback key
//      is computed so standalone draws can still hit the cache.
//   2. Guard-G4-compliant graphics PSO caching: builds `D3D12_GRAPHICS_PIPELINE_STATE_DESC`
//      from `DrawIdentity`'s decoded blend, depth/stencil, rasterizer, MRT
//      formats (preserving slot gaps with DXGI_FORMAT_UNKNOWN), DSV format,
//      topology class, and VS input layout (`IN0..IN15` matching `gcn/hlsl.cpp`),
//      keyed by 128-bit `GraphicsPsoKey` in a budget-bounded `PsoCache` with
//      fence-gated deferred release (no pipeline library object).
//   3. Native 2D RTV/DSV pools: allocates bounded `DEFAULT`-heap render target
//      and depth-stencil textures tracked in `Device::Memory()` under
//      `Owner::kHeaps` with private non-shader-visible RTV/DSV heaps and
//      fence-deferred reclamation on eviction.
//   4. Space-1 `u#`/`t#` resource binding & index/indirect staging: resolves
//      VS and PS space-1 plans from `bbx_shaders.paths`, snapshots read-only
//      `t#` slices into `GuestCompute`'s GPU SRV snapshot ring, stages indexed
//      and indirect draw buffers cleanly so space-3 guest slabs never alias
//      between `UNORDERED_ACCESS` and `INDEX_BUFFER`/`INDIRECT_ARGUMENT`/SRV,
//      and submits using a fence-tracked command allocator pool.
#include "xbox/platform/guest_graphics.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/translate.h"
#include "hle/modules.h"
#include "host/shader_patch.h"
#include "src/host/d3d12/bbx_cache.h"
#include "src/host/d3d12/bbx_descriptors.h"
#include "src/host/d3d12/bbx_guest_binding.h"
#include "src/host/d3d12/bbx_guest_path.h"
#include "src/host/d3d12/bbx_guest_root.h"
#include "src/host/d3d12/bbx_path_table.h"
#include "src/host/d3d12/bbx_psocache.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "src/host/d3d12/device.h"
#include "src/host/d3d12/pipeline.h"
#include "xbox/platform/guest_compute.h"
#include "xbox/platform/log.h"

#if defined(BBX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace bbx::platform {
namespace {

using bbx::d3d12::ComPtr;
using bbx::d3d12::ConsoleContainerStage;
using bbx::d3d12::ConsoleStageKey;
using bbx::d3d12::DrawBlendFactor;
using bbx::d3d12::DrawBlendOp;
using bbx::d3d12::DrawCompareOp;
using bbx::d3d12::DrawIdentity;
using bbx::d3d12::DrawStencilFace;
using bbx::d3d12::DrawStencilOp;
using bbx::d3d12::DrawTarget;
using bbx::d3d12::DrawTopology;
using bbx::d3d12::DrawTopologyInfo;
using bbx::d3d12::DrawTopologyType;
using bbx::d3d12::GuestHeapLayout;
using bbx::d3d12::GuestStage;
using bbx::d3d12::PsoKey;
using bbx::d3d12::Stage;

struct ExtractedProgram {
    std::vector<std::uint32_t> words;
    std::string name;
    std::uint64_t hash = 0;
};

struct NotedShader {
    int stage = 0;
    std::uint64_t key = 0;
    std::uint32_t element_count = 0;
    bool formats_from_params = false;
};

bool ExtractProgram(std::uint64_t code_va, ExtractedProgram* out) {
    if (code_va == 0 || !hle_kernel_va_mapped(code_va, 4)) return false;
    const auto* p = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(code_va));
    std::size_t limit = 0;
    while (limit < (1u << 20) && hle_kernel_va_mapped(code_va + limit, 4096)) limit += 4096;
    if (limit == 0) return false;
    for (std::size_t i = 0; i + 32 <= limit; i += 4) {
        if (std::memcmp(p + i, "OrbShdr", 7) != 0) continue;
        std::uint32_t lenfield = 0;
        std::memcpy(&lenfield, p + i + 8, 4);
        const std::uint32_t length = lenfield >> 8;
        if (length == 0 || length > i || (length & 3) != 0) return false;
        out->words.resize(length / 4);
        std::memcpy(out->words.data(), p, length);
        std::uint32_t h0 = 0;
        std::memcpy(&h0, p + i + 16, 4);
        char name[32];
        std::snprintf(name, sizeof(name), "%08x", h0);
        out->name = name;
        shader_patch_apply(out->name, out->words);
        out->hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(out->words.data()), out->words.size() * sizeof(std::uint32_t)));
        return true;
    }
    const gcn::Program prog = gcn::decode(reinterpret_cast<const std::uint32_t*>(p), limit / 4);
    std::uint32_t end = 0;
    for (const gcn::Inst& in : prog.insts) {
        if (in.enc == gcn::Enc::SOPP && in.op == 1) end = in.offset + 4;
        if (!prog.errors.empty() && in.offset >= prog.errors[0].offset) break;
    }
    if (end == 0 || end > limit) return false;
    out->words.resize(end / 4);
    std::memcpy(out->words.data(), p, end);
    out->name = "no-footer";
    out->hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(out->words.data()), out->words.size() * sizeof(std::uint32_t)));
    return true;
}

std::string FindOrbShdrName(const std::vector<std::uint8_t>& container) {
    for (std::size_t i = 0; i + 32 <= container.size(); i += 4) {
        if (std::memcmp(container.data() + i, "OrbShdr", 7) != 0) continue;
        std::uint32_t h0 = 0;
        std::memcpy(&h0, container.data() + i + 16, 4);
        char name[32];
        std::snprintf(name, sizeof(name), "%08x", h0);
        return name;
    }
    return {};
}

bool Inflate(const bbx::d3d12::BakedBlob& blob, std::vector<std::uint8_t>* out) {
#if defined(BBX_HAVE_ZLIB)
    uLongf inflated = blob.raw_size;
    return ::uncompress(out->data(), &inflated, blob.data, blob.size) == Z_OK && inflated == blob.raw_size;
#else
    (void)blob;
    (void)out;
    return false;
#endif
}

bool ReadGuestMemory(void*, std::uint64_t va, void* out, std::size_t bytes) {
    if (!hle_kernel_va_mapped(va, bytes)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), bytes);
    return true;
}

D3D12_BLEND ToD3d12Blend(DrawBlendFactor factor) {
    switch (factor) {
    case DrawBlendFactor::kZero: return D3D12_BLEND_ZERO;
    case DrawBlendFactor::kOne: return D3D12_BLEND_ONE;
    case DrawBlendFactor::kSrcColor: return D3D12_BLEND_SRC_COLOR;
    case DrawBlendFactor::kInvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case DrawBlendFactor::kSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case DrawBlendFactor::kInvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case DrawBlendFactor::kDestAlpha: return D3D12_BLEND_DEST_ALPHA;
    case DrawBlendFactor::kInvDestAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    case DrawBlendFactor::kDestColor: return D3D12_BLEND_DEST_COLOR;
    case DrawBlendFactor::kInvDestColor: return D3D12_BLEND_INV_DEST_COLOR;
    case DrawBlendFactor::kSrcAlphaSat: return D3D12_BLEND_SRC_ALPHA_SAT;
    case DrawBlendFactor::kSrc1Color: return D3D12_BLEND_SRC1_COLOR;
    case DrawBlendFactor::kInvSrc1Color: return D3D12_BLEND_INV_SRC1_COLOR;
    case DrawBlendFactor::kSrc1Alpha: return D3D12_BLEND_SRC1_ALPHA;
    case DrawBlendFactor::kInvSrc1Alpha: return D3D12_BLEND_INV_SRC1_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP ToD3d12BlendOp(DrawBlendOp op) {
    switch (op) {
    case DrawBlendOp::kAdd: return D3D12_BLEND_OP_ADD;
    case DrawBlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
    case DrawBlendOp::kRevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case DrawBlendOp::kMin: return D3D12_BLEND_OP_MIN;
    case DrawBlendOp::kMax: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

D3D12_COMPARISON_FUNC ToD3d12Compare(DrawCompareOp op) {
    switch (op) {
    case DrawCompareOp::kNever: return D3D12_COMPARISON_FUNC_NEVER;
    case DrawCompareOp::kLess: return D3D12_COMPARISON_FUNC_LESS;
    case DrawCompareOp::kEqual: return D3D12_COMPARISON_FUNC_EQUAL;
    case DrawCompareOp::kLessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case DrawCompareOp::kGreater: return D3D12_COMPARISON_FUNC_GREATER;
    case DrawCompareOp::kNotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case DrawCompareOp::kGreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case DrawCompareOp::kAlways: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_STENCIL_OP ToD3d12StencilOp(DrawStencilOp op) {
    switch (op) {
    case DrawStencilOp::kKeep: return D3D12_STENCIL_OP_KEEP;
    case DrawStencilOp::kZero: return D3D12_STENCIL_OP_ZERO;
    case DrawStencilOp::kReplace: return D3D12_STENCIL_OP_REPLACE;
    case DrawStencilOp::kIncrSat: return D3D12_STENCIL_OP_INCR_SAT;
    case DrawStencilOp::kDecrSat: return D3D12_STENCIL_OP_DECR_SAT;
    case DrawStencilOp::kInvert: return D3D12_STENCIL_OP_INVERT;
    case DrawStencilOp::kIncr: return D3D12_STENCIL_OP_INCR;
    case DrawStencilOp::kDecr: return D3D12_STENCIL_OP_DECR;
    }
    return D3D12_STENCIL_OP_KEEP;
}

D3D12_DEPTH_STENCILOP_DESC ToD3d12StencilFace(const DrawStencilFace& face) {
    D3D12_DEPTH_STENCILOP_DESC out{};
    out.StencilFailOp = ToD3d12StencilOp(face.fail_op);
    out.StencilDepthFailOp = ToD3d12StencilOp(face.depth_fail_op);
    out.StencilPassOp = ToD3d12StencilOp(face.pass_op);
    out.StencilFunc = ToD3d12Compare(face.func);
    return out;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3d12TopologyType(DrawTopologyType type) {
    switch (type) {
    case DrawTopologyType::kPoint: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case DrawTopologyType::kLine: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case DrawTopologyType::kTriangle: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case DrawTopologyType::kUndefined: break;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

D3D12_PRIMITIVE_TOPOLOGY ToD3d12Topology(DrawTopology topo) {
    switch (topo) {
    case DrawTopology::kPointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case DrawTopology::kLineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case DrawTopology::kLineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case DrawTopology::kTriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case DrawTopology::kTriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case DrawTopology::kUndefined: break;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

std::uint64_t EstimateTextureBytes(std::uint32_t width, std::uint32_t height, std::uint32_t dxgi_format) {
    std::uint32_t bpp = 4;
    switch (dxgi_format) {
    case DXGI_FORMAT_R8_UNORM: bpp = 1; break;
    case DXGI_FORMAT_R16_UINT: bpp = 2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: bpp = 8; break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: bpp = 16; break;
    default: bpp = 4; break;
    }
    return static_cast<std::uint64_t>(width) * height * bpp;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

bool IsDirectPresentFormat(std::uint32_t fmt) {
    switch (static_cast<DXGI_FORMAT>(fmt)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

struct PsoMapKey {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    bool operator==(const PsoMapKey&) const = default;
};

struct PsoMapKeyHash {
    std::size_t operator()(const PsoMapKey& k) const noexcept {
        return static_cast<std::size_t>(k.lo ^ (k.hi + 0x9e3779b97f4a7c15ull + (k.lo << 6) + (k.lo >> 2)));
    }
};

struct TargetKey {
    std::uint64_t va = 0;
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const TargetKey&) const = default;
};

struct TargetKeyHash {
    std::size_t operator()(const TargetKey& k) const noexcept {
        std::uint64_t h = k.va ^ 0x9e3779b97f4a7c15ull;
        h ^= (static_cast<std::uint64_t>(k.format) << 32 | k.width) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(k.height) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

struct NotedKey {
    int stage = 0;
    std::uint64_t hash = 0;
    bool operator==(const NotedKey&) const = default;
};

struct NotedKeyHash {
    std::size_t operator()(const NotedKey& k) const noexcept {
        return static_cast<std::size_t>(k.hash ^ (static_cast<std::uint64_t>(k.stage) * 0x9e3779b97f4a7c15ull));
    }
};

struct FallbackKey {
    int stage = 0;
    std::uint64_t hash = 0;
    std::uint32_t rsrc1 = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t mask = 0;
    bool operator==(const FallbackKey&) const = default;
};

struct FallbackKeyHash {
    std::size_t operator()(const FallbackKey& k) const noexcept {
        std::uint64_t h = k.hash ^ (static_cast<std::uint64_t>(k.stage) * 0x9e3779b97f4a7c15ull);
        h ^= (static_cast<std::uint64_t>(k.rsrc1) << 32 | k.rsrc2) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(k.mask) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

}  // namespace

struct GuestGraphics::Impl {
    static constexpr std::uint32_t kParamSlices = 4;
    static constexpr std::uint32_t kParamSlotsPerSlice = 1024;
    static constexpr std::uint32_t kParamSlots = kParamSlices * kParamSlotsPerSlice;  // 2 MiB (2 slots per draw)
    static constexpr std::uint32_t kMaxRtvs = 128;
    static constexpr std::uint32_t kMaxDsvs = 32;
    static constexpr std::uint32_t kIndexSlices = 4;
    static constexpr std::uint64_t kIndexSliceBytes = 1ull << 20;
    static constexpr std::uint64_t kIndexStagingBytes = kIndexSlices * kIndexSliceBytes;  // 4 MiB DEFAULT IB ring
    static constexpr std::uint32_t kIndexUploadSlices = 4;
    static constexpr std::uint64_t kIndexUploadSliceBytes = 1ull << 20;
    static constexpr std::uint64_t kIndexUploadBytes = kIndexUploadSlices * kIndexUploadSliceBytes;  // 4 MiB UPLOAD IB ring
    static constexpr std::uint32_t kIndirectSlices = 4;
    static constexpr std::uint64_t kIndirectSliceBytes = 16384ull;
    static constexpr std::uint64_t kIndirectStagingBytes = kIndirectSlices * kIndirectSliceBytes;  // 64 KiB DEFAULT indirect
    static constexpr std::uint32_t kMaxBatchOps = 256;

    d3d12::Device* device = nullptr;
    d3d12::PipelineSet* pipelines = nullptr;
    d3d12::GuestMemory* memory = nullptr;
    GuestCompute* compute = nullptr;
    d3d12::DescriptorRing* descriptor_ring = nullptr;
    ID3D12RootSignature* root = nullptr;  // GuestStage::kGraphics
    std::uint32_t arena_base = 0;
    std::uint32_t sampler_base = 0;
    std::uint32_t cbv_stride = 0;
    std::uint32_t sampler_stride = 0;
    std::uint32_t rtv_stride = 0;
    std::uint32_t dsv_stride = 0;

    // Private non-shader-visible descriptor heaps for guest render/depth targets
    // so guest draws never collide with the swapchain's own RTV slots.
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;

    // StageParams 4-slice upload ring (512 B per stage, 2 slots consumed per draw).
    ComPtr<ID3D12Resource> params;
    void* params_mapped = nullptr;
    std::uint32_t param_slice = 0;
    std::uint32_t param_slot_in_slice = 0;
    std::array<std::uint64_t, kParamSlices> param_slice_fence{};
    std::uint32_t param_slices_pending_mask = 0;

    // 4-slice fence-tracked staging rings for indexed and indirect draws.
    ComPtr<ID3D12Resource> index_staging;
    bool index_staging_budgeted = false;
    std::uint32_t idx_slice = 0;
    std::uint64_t idx_slice_cursor = 0;
    std::array<std::uint64_t, kIndexSlices> idx_slice_fence{};
    std::uint32_t idx_slices_pending_mask = 0;

    ComPtr<ID3D12Resource> index_upload;
    void* index_upload_mapped = nullptr;
    bool index_upload_budgeted = false;
    std::uint32_t idx_up_slice = 0;
    std::uint64_t idx_up_slice_cursor = 0;
    std::array<std::uint64_t, kIndexUploadSlices> idx_up_slice_fence{};
    std::uint32_t idx_up_slices_pending_mask = 0;

    ComPtr<ID3D12Resource> indirect_staging;
    std::uint32_t indirect_slice = 0;
    std::uint64_t indirect_slice_cursor = 0;
    std::array<std::uint64_t, kIndirectSlices> indirect_slice_fence{};
    std::uint32_t indirect_slices_pending_mask = 0;
    ComPtr<ID3D12CommandSignature> indirect_draw_sig;
    ComPtr<ID3D12CommandSignature> indirect_draw_indexed_sig;

    // Fence-tracked reusable command allocator/list pool + batched recording state.
    struct CommandSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence = 0;
    };
    std::vector<CommandSlot> free_lists;
    std::vector<CommandSlot> in_flight;
    CommandSlot active_cmd;
    bool batch_open = false;
    std::uint32_t batch_ops = 0;
    bool batch_heaps_bound = false;
    bool batch_root_bound = false;
    bool batch_samplers_bound = false;
    bool batch_space3_bound = false;
    ID3D12PipelineState* batch_pso = nullptr;
    std::uint32_t batch_vs_table = UINT32_MAX;
    std::uint32_t batch_ps_table = UINT32_MAX;
    D3D12_PRIMITIVE_TOPOLOGY batch_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool batch_stencil_ref_set = false;
    std::uint32_t batch_stencil_ref = 0;
    bool batch_om_set = false;
    std::uint32_t batch_num_rtvs = 0;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> batch_rtvs{};
    bool batch_has_dsv = false;
    D3D12_CPU_DESCRIPTOR_HANDLE batch_dsv{};
    bool batch_vp_set = false;
    D3D12_VIEWPORT batch_vp{};
    bool batch_scissor_set = false;
    D3D12_RECT batch_scissor{};
    std::vector<d3d12::DescriptorSlice> batch_retired_slices;
    std::vector<ID3D12PipelineState*> batch_retired_psos;
    std::uint64_t last_draw_fence = 0;

    // Bounded graphics PSO cache + deferred fence-gated retirement.
    struct RetiredPso {
        ID3D12PipelineState* pso = nullptr;
        std::uint64_t fence = 0;
    };
    std::unique_ptr<d3d12::PsoCache> psos;
    std::unordered_map<PsoMapKey, ID3D12PipelineState*, PsoMapKeyHash> pso_objects;
    std::vector<RetiredPso> retired_psos;
    std::uint64_t clock = 0;

    // Cached native 2D render targets and depth-stencil textures.
    struct TargetEntry {
        ComPtr<ID3D12Resource> resource;
        std::uint32_t slot = 0;
        std::uint64_t bytes = 0;
        std::uint64_t last_used_seq = 0;
        bool gpu_rendered = false;
    };
    struct RetiredTarget {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t bytes = 0;
        std::uint64_t fence = 0;
    };
    std::unordered_map<TargetKey, TargetEntry, TargetKeyHash> rtv_pool;
    std::unordered_map<TargetKey, TargetEntry, TargetKeyHash> dsv_pool;
    std::vector<std::uint32_t> free_rtv_slots;
    std::vector<std::uint32_t> free_dsv_slots;
    std::vector<RetiredTarget> retired_targets;
    std::uint64_t target_use_seq = 0;

    // Shader analysis caches: container-noted keys (exact bake match) +
    // extracted guest programs + fallback register-derived stage keys.
    std::unordered_map<NotedKey, NotedShader, NotedKeyHash> noted_by_hash;
    std::unordered_map<std::uint64_t, ExtractedProgram> programs;
    std::unordered_map<FallbackKey, std::uint64_t, FallbackKeyHash> fallback_keys;

    // Ring-buffered space-1 descriptor state for VS (stage 0) and PS (stage 1).
    struct SrvCopy {
        std::uint32_t slab = 0;
        std::uint64_t src_offset = 0;
        std::uint64_t dst_offset = 0;
        std::uint64_t bytes = 0;
    };
    struct StageBindState {
        std::uint64_t key = 0;
        std::array<std::uint32_t, 16> user_data{};
        d3d12::DescriptorSlice slice{};
        std::uint64_t srv_ring_gen = 0;
        bool valid = false;
        std::vector<SrvCopy> srv_copies;
    };
    std::array<StageBindState, 2> stage_state{};

    std::unordered_set<std::uint64_t> logged_misses;
    std::unordered_set<std::uint64_t> logged_pso_failures;
    std::unordered_set<std::uint64_t> logged_binding_refusals;
    Counts counts;
    std::uint64_t completed = 0;

    static void FlushThunk(void* user) {
        if (user != nullptr) static_cast<Impl*>(user)->FlushBatch();
    }

    void ReclaimCompleted(std::uint64_t done) {
        for (auto it = in_flight.begin(); it != in_flight.end();) {
            if (it->fence <= done) {
                free_lists.push_back(std::move(*it));
                it = in_flight.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = retired_psos.begin(); it != retired_psos.end();) {
            if (it->fence <= done) {
                if (it->pso != nullptr) it->pso->Release();
                it = retired_psos.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = retired_targets.begin(); it != retired_targets.end();) {
            if (it->fence <= done) {
                if (device != nullptr && it->bytes != 0) {
                    device->Memory().Free(d3d12::Owner::kHeaps, it->bytes);
                }
                it = retired_targets.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool AcquireCommandList(CommandSlot* out) {
        if (device == nullptr || out == nullptr) return false;
        ReclaimCompleted(device->CompletedValue());
        if (!free_lists.empty()) {
            *out = std::move(free_lists.back());
            free_lists.pop_back();
            if (FAILED(out->allocator->Reset()) || FAILED(out->list->Reset(out->allocator.Get(), nullptr))) {
                return false;
            }
            return true;
        }
        if (FAILED(device->Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(out->allocator.GetAddressOf())))) {
            return false;
        }
        if (FAILED(device->Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, out->allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(out->list.GetAddressOf())))) {
            return false;
        }
        return true;
    }

    void ResetBatchState() {
        batch_open = false;
        batch_ops = 0;
        batch_heaps_bound = false;
        batch_root_bound = false;
        batch_samplers_bound = false;
        batch_space3_bound = false;
        batch_pso = nullptr;
        batch_vs_table = UINT32_MAX;
        batch_ps_table = UINT32_MAX;
        batch_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        batch_stencil_ref_set = false;
        batch_stencil_ref = 0;
        batch_om_set = false;
        batch_num_rtvs = 0;
        batch_has_dsv = false;
        batch_vp_set = false;
        batch_scissor_set = false;
    }

    bool BeginBatch() {
        if (batch_open) return true;
        if (!AcquireCommandList(&active_cmd)) return false;
        ResetBatchState();
        batch_open = true;
        return true;
    }

    void FlushBatch() {
        if (!batch_open) return;
        ID3D12GraphicsCommandList* list = active_cmd.list.Get();
        const HRESULT close_hr = list->Close();
        ResetBatchState();

        std::uint64_t fence = device->SubmittedValue();
        if (SUCCEEDED(close_hr)) {
            ID3D12CommandList* lists[1] = {list};
            fence = device->ExecuteAndSignal(lists, 1);
            active_cmd.fence = fence;
            last_draw_fence = fence;
            in_flight.push_back(std::move(active_cmd));
            ++counts.batches;
        } else {
            ++counts.record_failed;
            active_cmd = CommandSlot{};
        }

        for (std::uint32_t s = 0; s < kParamSlices; ++s) {
            if ((param_slices_pending_mask & (1u << s)) != 0) {
                if (fence > param_slice_fence[s]) param_slice_fence[s] = fence;
            }
        }
        param_slices_pending_mask = 0;

        for (std::uint32_t s = 0; s < kIndexSlices; ++s) {
            if ((idx_slices_pending_mask & (1u << s)) != 0) {
                if (fence > idx_slice_fence[s]) idx_slice_fence[s] = fence;
            }
        }
        idx_slices_pending_mask = 0;

        for (std::uint32_t s = 0; s < kIndexUploadSlices; ++s) {
            if ((idx_up_slices_pending_mask & (1u << s)) != 0) {
                if (fence > idx_up_slice_fence[s]) idx_up_slice_fence[s] = fence;
            }
        }
        idx_up_slices_pending_mask = 0;

        for (std::uint32_t s = 0; s < kIndirectSlices; ++s) {
            if ((indirect_slices_pending_mask & (1u << s)) != 0) {
                if (fence > indirect_slice_fence[s]) indirect_slice_fence[s] = fence;
            }
        }
        indirect_slices_pending_mask = 0;

        if (compute != nullptr) {
            compute->MarkSrvSnapshotsSubmitted(fence);
        }

        if (descriptor_ring != nullptr && !batch_retired_slices.empty()) {
            for (const d3d12::DescriptorSlice& slice : batch_retired_slices) {
                descriptor_ring->Release(slice, fence);
            }
        }
        batch_retired_slices.clear();

        if (!batch_retired_psos.empty()) {
            for (ID3D12PipelineState* pso : batch_retired_psos) {
                retired_psos.push_back({pso, fence});
            }
            batch_retired_psos.clear();
        }
    }

    void ReserveParamPair(std::uint32_t* out_vs_slot, std::uint32_t* out_ps_slot) {
        if (param_slot_in_slice + 2 > kParamSlotsPerSlice) {
            const std::uint32_t next = (param_slice + 1) % kParamSlices;
            if ((param_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = param_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            param_slice = next;
            param_slot_in_slice = 0;
        }
        const std::uint32_t base = param_slice * kParamSlotsPerSlice + param_slot_in_slice;
        param_slot_in_slice += 2;
        param_slices_pending_mask |= (1u << param_slice);
        *out_vs_slot = base;
        *out_ps_slot = base + 1;
    }

    bool ReserveIndexStaging(std::uint64_t bytes, std::uint64_t* out_offset) {
        if (index_staging == nullptr || bytes == 0 || bytes > kIndexStagingBytes) return false;
        const std::uint64_t aligned = (bytes + 15ull) & ~15ull;
        if (aligned > kIndexSliceBytes) {
            FlushBatch();
            std::uint64_t max_fence = 0;
            for (const std::uint64_t f : idx_slice_fence) {
                if (f > max_fence) max_fence = f;
            }
            if (max_fence > device->CompletedValue()) {
                (void)device->WaitFor(max_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            *out_offset = 0;
            const std::uint32_t covered =
                static_cast<std::uint32_t>((aligned + kIndexSliceBytes - 1ull) / kIndexSliceBytes);
            for (std::uint32_t s = 0; s < covered && s < kIndexSlices; ++s) {
                idx_slices_pending_mask |= (1u << s);
            }
            idx_slice = std::min<std::uint32_t>(covered > 0 ? covered - 1 : 0, kIndexSlices - 1);
            idx_slice_cursor = kIndexSliceBytes;
            return true;
        }
        if (idx_slice_cursor + aligned > kIndexSliceBytes) {
            const std::uint32_t next = (idx_slice + 1) % kIndexSlices;
            if ((idx_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = idx_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            idx_slice = next;
            idx_slice_cursor = 0;
        }
        *out_offset = static_cast<std::uint64_t>(idx_slice) * kIndexSliceBytes + idx_slice_cursor;
        idx_slice_cursor += aligned;
        idx_slices_pending_mask |= (1u << idx_slice);
        return true;
    }

    bool ReserveIndexUpload(std::uint64_t bytes, std::uint64_t* out_offset) {
        if (index_upload == nullptr || index_upload_mapped == nullptr || bytes == 0 || bytes > kIndexUploadBytes) {
            return false;
        }
        const std::uint64_t aligned = (bytes + 15ull) & ~15ull;
        if (aligned > kIndexUploadSliceBytes) {
            FlushBatch();
            std::uint64_t max_fence = 0;
            for (const std::uint64_t f : idx_up_slice_fence) {
                if (f > max_fence) max_fence = f;
            }
            if (max_fence > device->CompletedValue()) {
                (void)device->WaitFor(max_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            *out_offset = 0;
            const std::uint32_t covered =
                static_cast<std::uint32_t>((aligned + kIndexUploadSliceBytes - 1ull) / kIndexUploadSliceBytes);
            for (std::uint32_t s = 0; s < covered && s < kIndexUploadSlices; ++s) {
                idx_up_slices_pending_mask |= (1u << s);
            }
            idx_up_slice = std::min<std::uint32_t>(covered > 0 ? covered - 1 : 0, kIndexUploadSlices - 1);
            idx_up_slice_cursor = kIndexUploadSliceBytes;
            return true;
        }
        if (idx_up_slice_cursor + aligned > kIndexUploadSliceBytes) {
            const std::uint32_t next = (idx_up_slice + 1) % kIndexUploadSlices;
            if ((idx_up_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = idx_up_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            idx_up_slice = next;
            idx_up_slice_cursor = 0;
        }
        *out_offset = static_cast<std::uint64_t>(idx_up_slice) * kIndexUploadSliceBytes + idx_up_slice_cursor;
        idx_up_slice_cursor += aligned;
        idx_up_slices_pending_mask |= (1u << idx_up_slice);
        return true;
    }

    std::uint64_t ReserveIndirectOffset(std::uint64_t aligned_bytes) {
        if (indirect_slice_cursor + aligned_bytes > kIndirectSliceBytes) {
            const std::uint32_t next = (indirect_slice + 1) % kIndirectSlices;
            if ((indirect_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = indirect_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            indirect_slice = next;
            indirect_slice_cursor = 0;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(indirect_slice) * kIndirectSliceBytes + indirect_slice_cursor;
        indirect_slice_cursor += aligned_bytes;
        indirect_slices_pending_mask |= (1u << indirect_slice);
        return offset;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE VisibleCpu(std::uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = device->CbvSrvUavHeap()->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * cbv_stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(std::uint32_t slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * rtv_stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpu(std::uint32_t slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * dsv_stride;
        return handle;
    }

    void RetireSlice(d3d12::DescriptorSlice slice) {
        if (!slice.valid() || descriptor_ring == nullptr) return;
        if (batch_open) {
            batch_retired_slices.push_back(slice);
        } else {
            descriptor_ring->Release(slice, device->SubmittedValue());
        }
    }

    d3d12::DescriptorSlice AllocateSpace1Slice() {
        if (descriptor_ring == nullptr || compute == nullptr || compute->NullTemplateCpu().ptr == 0) return {};
        descriptor_ring->Collect(device->CompletedValue());
        std::optional<d3d12::DescriptorSlice> slice =
            descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        if (!slice.has_value() && batch_open) {
            FlushBatch();
            descriptor_ring->Collect(device->CompletedValue());
            slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        }
        if (!slice.has_value()) {
            const std::uint64_t oldest = descriptor_ring->OldestPendingFence();
            if (oldest != 0) {
                (void)device->WaitFor(oldest);
                const std::uint64_t done = device->CompletedValue();
                ReclaimCompleted(done);
                descriptor_ring->Collect(done);
                slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
            }
        }
        if (!slice.has_value()) {
            (void)device->WaitIdle();
            const std::uint64_t done = device->CompletedValue();
            ReclaimCompleted(done);
            descriptor_ring->Collect(done);
            slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        }
        if (!slice.has_value()) return {};
        device->Get()->CopyDescriptorsSimple(GuestHeapLayout::kSpace1Count, VisibleCpu(slice->offset),
                                             compute->NullTemplateCpu(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return *slice;
    }

    bool WriteBufferUav(std::uint32_t stage_base, std::uint32_t slot, const d3d12::BindingWrite& write) {
        if (memory->EnsureSlab(write.slab) != d3d12::Status::kOk) return false;
        ID3D12Resource* slab = memory->SlabBuffer(write.slab);
        if (slab == nullptr) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = static_cast<UINT>(write.offset / 4ull);
        uav.Buffer.NumElements = write.dwords;
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->Get()->CreateUnorderedAccessView(slab, nullptr, &uav, VisibleCpu(stage_base + slot));
        return true;
    }

    bool WriteBufferSrv(std::uint32_t stage_base, std::uint32_t slot, const d3d12::BindingWrite& write,
                        SrvCopy* out_copy) {
        if (memory->EnsureSlab(write.slab) != d3d12::Status::kOk) return false;
        if (memory->SlabBuffer(write.slab) == nullptr) return false;
        const std::uint64_t bytes = static_cast<std::uint64_t>(write.dwords) * 4ull;
        std::uint64_t ring_offset = 0;
        if (compute->SrvSnapshotRing() == nullptr ||
            !compute->ReserveSrvSnapshot(bytes, &ring_offset, &Impl::FlushThunk, this)) {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = static_cast<UINT>(ring_offset / 4ull);
        srv.Buffer.NumElements = write.dwords;
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->Get()->CreateShaderResourceView(compute->SrvSnapshotRing(), &srv, VisibleCpu(stage_base + slot));
        out_copy->slab = write.slab;
        out_copy->src_offset = write.offset;
        out_copy->dst_offset = ring_offset;
        out_copy->bytes = bytes;
        return true;
    }

    // Ring-buffered VS (stage 0) and PS (stage 1) space-1 resource binding:
    // reuses the active slice when `(stage_key, user_data, srv_ring_gen)` match;
    // otherwise allocates a fresh 191-slot slice and retires the previous one
    // with the batch fence, eliminating every per-draw GPU WaitFor stall.
    std::uint32_t BindStageResources(std::uint32_t stage_idx, std::uint64_t stage_key,
                                     const std::uint32_t user_data[16]) {
        const std::uint32_t static_null_offset = arena_base + GuestHeapLayout::StageRegion(stage_idx);
        if (compute->Paths().status() != d3d12::PathTableStatus::kOk) return static_null_offset;

        StageBindState& st = stage_state[stage_idx];
        const std::uint64_t current_gen = compute->SrvRingGeneration();
        const bool same_user = std::memcmp(st.user_data.data(), user_data, sizeof(st.user_data)) == 0;
        const bool srv_valid = st.srv_copies.empty() || st.srv_ring_gen == current_gen;
        if (st.valid && st.key == stage_key && same_user && srv_valid) {
            return st.slice.valid() ? st.slice.offset : static_null_offset;
        }

        const d3d12::BindingPlan* active_plans[GuestHeapLayout::kSpace1Count];
        std::uint32_t active_bindings[GuestHeapLayout::kSpace1Count];
        std::uint32_t planned = 0;
        for (std::uint32_t binding = 0; binding < GuestHeapLayout::kSpace1Count; ++binding) {
            const d3d12::BindingPlan* plan = compute->Paths().Find(stage_key, binding);
            if (plan == nullptr) continue;
            active_plans[planned] = plan;
            active_bindings[planned] = binding;
            ++planned;
        }

        if (st.slice.valid()) {
            RetireSlice(st.slice);
            st.slice = {};
        }
        st.srv_copies.clear();
        st.key = stage_key;
        std::memcpy(st.user_data.data(), user_data, sizeof(st.user_data));

        if (planned == 0) {
            st.srv_ring_gen = compute->SrvRingGeneration();
            st.valid = true;
            return static_null_offset;
        }

        const d3d12::DescriptorSlice new_slice = AllocateSpace1Slice();
        if (!new_slice.valid()) {
            st.valid = false;
            return static_null_offset;
        }

        for (std::uint32_t i = 0; i < planned; ++i) {
            const d3d12::BindingPlan* plan = active_plans[i];
            const std::uint32_t binding = active_bindings[i];
            d3d12::GuestHeapSlot slot{};
            const d3d12::GuestRangeKind kind =
                plan->kind == d3d12::BindingKind::kUav ? d3d12::GuestRangeKind::kUav : d3d12::GuestRangeKind::kSrv;
            if (!d3d12::GuestHeapSlotFor(GuestStage::kGraphics, kind, binding, d3d12::kGuestParamsSpace, &slot)) {
                ++counts.bindings_refused;
                continue;
            }
            const d3d12::BindingWrite write =
                d3d12::DecideBindingWrite(*plan, user_data, memory->Table(), ReadGuestMemory, nullptr);
            switch (write.verdict) {
            case d3d12::BindingVerdict::kNull:
                ++counts.bindings_null;
                break;
            case d3d12::BindingVerdict::kWrite:
                if (write.kind == d3d12::BindingKind::kUav) {
                    if (WriteBufferUav(new_slice.offset, slot.index, write)) {
                        ++counts.bindings_written;
                    } else {
                        ++counts.bindings_refused;
                    }
                } else {
                    SrvCopy copy{};
                    if (WriteBufferSrv(new_slice.offset, slot.index, write, &copy)) {
                        st.srv_copies.push_back(copy);
                        ++counts.bindings_written;
                        ++counts.bindings_srv;
                    } else {
                        ++counts.bindings_refused;
                    }
                }
                break;
            default:
                ++counts.bindings_refused;
                if (logged_binding_refusals.insert(stage_key ^ binding).second) {
                    LogMessage(Level::kWarn, "guest graphics: key %016llx binding %u refused: %s",
                               static_cast<unsigned long long>(stage_key), binding,
                               d3d12::BindingWriteLine(write).c_str());
                }
                break;
            }
        }
        st.slice = new_slice;
        st.srv_ring_gen = compute->SrvRingGeneration();
        st.valid = true;
        return st.slice.offset;
    }

    // Resolves or creates a native 2D RTV texture for a guest DrawTarget.
    bool EnsureRenderTarget(const DrawTarget& t, bool mark_rendered, D3D12_CPU_DESCRIPTOR_HANDLE* out_handle) {
        if (t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192 || t.format == 0) return false;
        const TargetKey key{t.base, t.format, t.width, t.height};
        if (const auto it = rtv_pool.find(key); it != rtv_pool.end()) {
            it->second.last_used_seq = ++target_use_seq;
            if (mark_rendered) it->second.gpu_rendered = true;
            *out_handle = RtvCpu(it->second.slot);
            return true;
        }
        // Evict LRU if all kMaxRtvs slots are occupied.
        if (free_rtv_slots.empty()) {
            auto victim = rtv_pool.end();
            for (auto it = rtv_pool.begin(); it != rtv_pool.end(); ++it) {
                if (victim == rtv_pool.end() || it->second.last_used_seq < victim->second.last_used_seq) {
                    victim = it;
                }
            }
            if (victim == rtv_pool.end()) return false;
            FlushBatch();
            if (last_draw_fence != 0 && device->CompletedValue() < last_draw_fence) {
                (void)device->WaitFor(last_draw_fence);
            }
            const std::uint32_t reused_slot = victim->second.slot;
            retired_targets.push_back({std::move(victim->second.resource), victim->second.bytes,
                                       device->SubmittedValue()});
            rtv_pool.erase(victim);
            free_rtv_slots.push_back(reused_slot);
        }
        const std::uint64_t bytes = EstimateTextureBytes(t.width, t.height, t.format);
        if (!device->Memory().Allocate(d3d12::Owner::kHeaps, bytes).ok) return false;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = t.width;
        desc.Height = t.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(t.format);
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = desc.Format;
        clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.0f;
        clear.Color[3] = 1.0f;

        ComPtr<ID3D12Resource> res;
        if (FAILED(device->Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                          IID_PPV_ARGS(res.GetAddressOf())))) {
            device->Memory().Free(d3d12::Owner::kHeaps, bytes);
            return false;
        }
        const std::uint32_t slot = free_rtv_slots.back();
        free_rtv_slots.pop_back();
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = desc.Format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->Get()->CreateRenderTargetView(res.Get(), &rtv, RtvCpu(slot));
        rtv_pool.emplace(key, TargetEntry{std::move(res), slot, bytes, ++target_use_seq, mark_rendered});
        ++counts.targets_created;
        *out_handle = RtvCpu(slot);
        return true;
    }

    // Resolves or creates a native 2D DSV texture for a guest depth target.
    bool EnsureDepthTarget(std::uint64_t depth_va, std::uint32_t format, std::uint32_t width, std::uint32_t height,
                           D3D12_CPU_DESCRIPTOR_HANDLE* out_handle) {
        if (width == 0 || height == 0 || width > 8192 || height > 8192 || format == 0) return false;
        const TargetKey key{depth_va, format, width, height};
        if (const auto it = dsv_pool.find(key); it != dsv_pool.end()) {
            it->second.last_used_seq = ++target_use_seq;
            *out_handle = DsvCpu(it->second.slot);
            return true;
        }
        if (free_dsv_slots.empty()) {
            auto victim = dsv_pool.end();
            for (auto it = dsv_pool.begin(); it != dsv_pool.end(); ++it) {
                if (victim == dsv_pool.end() || it->second.last_used_seq < victim->second.last_used_seq) {
                    victim = it;
                }
            }
            if (victim == dsv_pool.end()) return false;
            FlushBatch();
            if (last_draw_fence != 0 && device->CompletedValue() < last_draw_fence) {
                (void)device->WaitFor(last_draw_fence);
            }
            const std::uint32_t reused_slot = victim->second.slot;
            retired_targets.push_back({std::move(victim->second.resource), victim->second.bytes,
                                       device->SubmittedValue()});
            dsv_pool.erase(victim);
            free_dsv_slots.push_back(reused_slot);
        }
        const std::uint64_t bytes = EstimateTextureBytes(width, height, format);
        if (!device->Memory().Allocate(d3d12::Owner::kHeaps, bytes).ok) return false;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(format);
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = desc.Format;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        ComPtr<ID3D12Resource> res;
        if (FAILED(device->Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                          IID_PPV_ARGS(res.GetAddressOf())))) {
            device->Memory().Free(d3d12::Owner::kHeaps, bytes);
            return false;
        }
        const std::uint32_t slot = free_dsv_slots.back();
        free_dsv_slots.pop_back();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = desc.Format;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        device->Get()->CreateDepthStencilView(res.Get(), &dsv, DsvCpu(slot));
        dsv_pool.emplace(key, TargetEntry{std::move(res), slot, bytes, ++target_use_seq, false});
        ++counts.targets_created;
        *out_handle = DsvCpu(slot);
        return true;
    }

    // Fallback stage key computation when NoteShaderCreated did not observe the
    // Sony Shdr container before the first draw.
    std::uint64_t FallbackStageKey(int stage, const ExtractedProgram& prog_data, const DrawIdentity& id) {
        const std::uint32_t rsrc1 = stage == 0 ? id.vs.rsrc1 : id.ps.rsrc1;
        const std::uint32_t rsrc2 = stage == 0 ? id.vs.rsrc2 : id.ps.rsrc2;
        const std::uint32_t mask = stage == 0 ? id.vs_out_cntl : id.ps_input_ena;
        const FallbackKey cache_id{stage, prog_data.hash, rsrc1, rsrc2, mask};
        if (const auto it = fallback_keys.find(cache_id); it != fallback_keys.end()) return it->second;

        const gcn::Program decoded = gcn::decode(prog_data.words.data(), prog_data.words.size());
        if (!decoded.errors.empty()) {
            fallback_keys.emplace(cache_id, 0);
            return 0;
        }
        gcn::TranslateOptions opts;
        opts.stage = stage == 0 ? gcn::Stage::Vertex : gcn::Stage::Pixel;
        opts.rsrc1 = rsrc1;
        opts.rsrc2 = rsrc2;
        opts.vs_out_cntl = stage == 0 ? id.vs_out_cntl : 0;
        opts.ps_input_ena = stage == 1 ? id.ps_input_ena : 0;
        const gcn::TranslateResult tr = gcn::translate(decoded, opts);
        if (!tr.errors.empty()) {
            fallback_keys.emplace(cache_id, 0);
            return 0;
        }
        ConsoleStageKey key;
        key.stage = stage == 0 ? Stage::kVertex : Stage::kPixel;
        key.rsrc1 = rsrc1;
        key.rsrc2 = rsrc2;
        key.mask = mask;
        key.early_fragment_tests = false;
        for (const gcn::PredictedImage& img : gcn::predict_image_dims(decoded, tr)) {
            key.dims.push_back((img.dim << 1) | (img.arrayed ? 1u : 0u));
        }
        key.sampler_modes.assign(tr.samplers.size(), 0);
        if (stage == 1 && !tr.ps_inputs.empty()) {
            std::uint32_t max_in = 0;
            for (const std::uint32_t idx : tr.ps_inputs) {
                if (idx > max_in) max_in = idx;
            }
            key.input_map.resize(max_in + 1);
            for (std::uint32_t i = 0; i <= max_in; ++i) {
                const std::uint32_t cntl = i < 32 ? id.ps_input_cntl[i] : i;
                key.input_map[i] = static_cast<std::uint8_t>(cntl & 0x1fu);
                if ((cntl & (1u << 10)) != 0 && i < 32) key.flat_mask |= (1u << i);
            }
        }
        const std::uint64_t computed = d3d12::ConsoleStageKeyOf(prog_data.words, key);
        fallback_keys.emplace(cache_id, computed);
        return computed;
    }

    ID3D12PipelineState* EnsureGraphicsPso(const PsoKey& pso_key, const std::vector<std::uint8_t>& vs_dxil,
                                           const std::vector<std::uint8_t>& ps_dxil, std::uint32_t vs_elements,
                                           const DrawIdentity& id, const DrawTopologyInfo& topo) {
        const PsoMapKey map_key{pso_key.lo, pso_key.hi};
        if (const auto it = pso_objects.find(map_key); it != pso_objects.end()) {
            (void)psos->Acquire(pso_key, ++clock);
            return it->second;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.VS.pShaderBytecode = vs_dxil.data();
        desc.VS.BytecodeLength = vs_dxil.size();
        desc.PS.pShaderBytecode = ps_dxil.data();
        desc.PS.BytecodeLength = ps_dxil.size();

        // Vertex input elements match `gcn/hlsl.cpp`'s `uint4 ... : INn` semantics.
        std::array<D3D12_INPUT_ELEMENT_DESC, 16> input_elements{};
        const std::uint32_t elem_count = vs_elements > 16u ? 16u : vs_elements;
        for (std::uint32_t k = 0; k < elem_count; ++k) {
            input_elements[k].SemanticName = "IN";
            input_elements[k].SemanticIndex = k;
            input_elements[k].Format = DXGI_FORMAT_R32G32B32A32_UINT;
            input_elements[k].InputSlot = k;
            input_elements[k].AlignedByteOffset = 0;
            input_elements[k].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            input_elements[k].InstanceDataStepRate = 0;
        }
        desc.InputLayout.pInputElementDescs = elem_count > 0 ? input_elements.data() : nullptr;
        desc.InputLayout.NumElements = elem_count;

        // Blend state (Guard G4 compliant: DrawBlendFactor excludes constant factors).
        desc.BlendState.AlphaToCoverageEnable = FALSE;
        bool independent_blend = false;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const auto& b = id.blend[i];
            auto& rt = desc.BlendState.RenderTarget[i];
            rt.BlendEnable = b.enable ? TRUE : FALSE;
            rt.LogicOpEnable = FALSE;
            rt.SrcBlend = ToD3d12Blend(b.src_color);
            rt.DestBlend = ToD3d12Blend(b.dst_color);
            rt.BlendOp = ToD3d12BlendOp(b.color_op);
            rt.SrcBlendAlpha = ToD3d12Blend(b.src_alpha);
            rt.DestBlendAlpha = ToD3d12Blend(b.dst_alpha);
            rt.BlendOpAlpha = ToD3d12BlendOp(b.alpha_op);
            rt.RenderTargetWriteMask = b.write_mask;
            if (i > 0) {
                const auto& r0 = desc.BlendState.RenderTarget[0];
                if (rt.BlendEnable != r0.BlendEnable || rt.SrcBlend != r0.SrcBlend || rt.DestBlend != r0.DestBlend ||
                    rt.BlendOp != r0.BlendOp || rt.SrcBlendAlpha != r0.SrcBlendAlpha ||
                    rt.DestBlendAlpha != r0.DestBlendAlpha || rt.BlendOpAlpha != r0.BlendOpAlpha ||
                    rt.RenderTargetWriteMask != r0.RenderTargetWriteMask) {
                    independent_blend = true;
                }
            }
        }
        desc.BlendState.IndependentBlendEnable = independent_blend ? TRUE : FALSE;

        // Rasterizer state.
        desc.RasterizerState.FillMode = id.raster.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        if (id.raster.cull_front && id.raster.cull_back) {
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        } else if (id.raster.cull_front) {
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        } else if (id.raster.cull_back) {
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        } else {
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        }
        desc.RasterizerState.FrontCounterClockwise = id.raster.front_cw ? FALSE : TRUE;
        desc.RasterizerState.DepthClipEnable = id.raster.depth_clip ? TRUE : FALSE;

        // Depth-stencil state.
        desc.DepthStencilState.DepthEnable =
            (id.depth_format != 0 && id.depth_stencil.depth_test) ? TRUE : FALSE;
        desc.DepthStencilState.DepthWriteMask =
            (id.depth_format != 0 && id.depth_stencil.depth_write_enable) ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                          : D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = ToD3d12Compare(id.depth_stencil.depth_func);
        desc.DepthStencilState.StencilEnable =
            (id.depth_format != 0 && id.depth_stencil.stencil_enable) ? TRUE : FALSE;
        desc.DepthStencilState.StencilReadMask = id.depth_stencil.stencil_read_mask;
        desc.DepthStencilState.StencilWriteMask = id.depth_stencil.stencil_write_mask;
        desc.DepthStencilState.FrontFace = ToD3d12StencilFace(id.depth_stencil.front);
        desc.DepthStencilState.BackFace = ToD3d12StencilFace(
            id.depth_stencil.backface_enable ? id.depth_stencil.back : id.depth_stencil.front);

        // MRT formats (preserve slot indices; gaps remain DXGI_FORMAT_UNKNOWN).
        for (std::uint32_t i = 0; i < 8; ++i) desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        std::uint32_t num_rtvs = 0;
        for (std::uint32_t i = 0; i < id.target_count && i < 8; ++i) {
            const DrawTarget& t = id.targets[i];
            if (t.index < 8) {
                desc.RTVFormats[t.index] = static_cast<DXGI_FORMAT>(t.format);
                if (t.index + 1 > num_rtvs) num_rtvs = t.index + 1;
            }
        }
        desc.NumRenderTargets = num_rtvs;
        desc.DSVFormat = id.depth_format != 0 ? static_cast<DXGI_FORMAT>(id.depth_format) : DXGI_FORMAT_UNKNOWN;
        desc.PrimitiveTopologyType = ToD3d12TopologyType(topo.type);
        desc.SampleMask = 0xFFFFFFFFu;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        ID3D12PipelineState* pso = nullptr;
        const HRESULT hr = device->Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr) || pso == nullptr) {
            ++counts.bad_pso;
            if (logged_pso_failures.insert(pso_key.lo ^ pso_key.hi).second) {
                LogMessage(Level::kError, "guest graphics: CreateGraphicsPipelineState failed (hr=0x%08lx) for %016llx",
                           static_cast<unsigned long>(hr), static_cast<unsigned long long>(pso_key.lo));
            }
            return nullptr;
        }
        ++counts.created_psos;
        const std::uint64_t estimated_bytes = static_cast<std::uint64_t>(vs_dxil.size() + ps_dxil.size() + 65536);
        const std::vector<PsoKey> evicted = psos->Insert(pso_key, pso_key.lo ^ pso_key.hi, estimated_bytes, ++clock);
        const std::uint64_t retire_fence = device->SubmittedValue();
        for (const PsoKey& gone : evicted) {
            const PsoMapKey gone_key{gone.lo, gone.hi};
            if (const auto it = pso_objects.find(gone_key); it != pso_objects.end()) {
                if (batch_open) {
                    batch_retired_psos.push_back(it->second);
                } else {
                    retired_psos.push_back({it->second, retire_fence});
                }
                pso_objects.erase(it);
                ++counts.evicted_psos;
            }
        }
        pso_objects.emplace(map_key, pso);
        return pso;
    }
};

GuestGraphics::GuestGraphics() : impl_(std::make_unique<Impl>()) {}
GuestGraphics::~GuestGraphics() { Destroy(); }

d3d12::Status GuestGraphics::Create(d3d12::Device& device, d3d12::PipelineSet& pipelines,
                                    d3d12::GuestMemory& memory, GuestCompute& compute,
                                    d3d12::DescriptorRing* descriptor_ring,
                                    std::uint32_t arena_base, std::uint32_t sampler_base) {
    if (impl_ == nullptr) return d3d12::Status::kUnavailable;
    Impl& impl = *impl_;
    impl.device = &device;
    impl.pipelines = &pipelines;
    impl.memory = &memory;
    impl.compute = &compute;
    impl.descriptor_ring = descriptor_ring;
    impl.root = pipelines.GuestRootSignature(GuestStage::kGraphics);
    impl.arena_base = arena_base;
    impl.sampler_base = sampler_base;
    impl.cbv_stride = device.CbvSrvUavStride();
    impl.sampler_stride = device.SamplerStride();
    impl.rtv_stride = device.RtvStride();
    impl.dsv_stride = device.DsvStride();
    if (impl.root == nullptr) {
        LogMessage(Level::kWarn, "guest graphics: no graphics root signature; draws will be refused");
        return d3d12::Status::kUnavailable;
    }

    // Private RTV and DSV descriptor heaps for guest draw targets.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = Impl::kMaxRtvs;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device.Get()->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(impl.rtv_heap.GetAddressOf())))) {
        LogMessage(Level::kError, "guest graphics: RTV descriptor heap creation failed");
        return d3d12::Status::kOutOfMemory;
    }
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_desc.NumDescriptors = Impl::kMaxDsvs;
    dsv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device.Get()->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(impl.dsv_heap.GetAddressOf())))) {
        LogMessage(Level::kError, "guest graphics: DSV descriptor heap creation failed");
        return d3d12::Status::kOutOfMemory;
    }
    impl.free_rtv_slots.clear();
    for (std::uint32_t i = Impl::kMaxRtvs; i > 0; --i) impl.free_rtv_slots.push_back(i - 1);
    impl.free_dsv_slots.clear();
    for (std::uint32_t i = Impl::kMaxDsvs; i > 0; --i) impl.free_dsv_slots.push_back(i - 1);

    // StageParams 4-slice upload ring (VS + PS root CBVs, 2 MiB total).
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = static_cast<UINT64>(Impl::kParamSlots) * d3d12::kStageParamsBytes;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device.Get()->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(impl.params.GetAddressOf())))) {
        LogMessage(Level::kError, "guest graphics: StageParams upload ring creation failed");
        return d3d12::Status::kOutOfMemory;
    }
    if (FAILED(impl.params->Map(0, nullptr, &impl.params_mapped))) return d3d12::Status::kOutOfMemory;

    // DEFAULT-heap index staging ring (COPY_DEST resting state, 4 x 1 MiB).
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC ib_desc = buf_desc;
    ib_desc.Width = Impl::kIndexStagingBytes;
    if (device.Memory().Allocate(d3d12::Owner::kHeaps, Impl::kIndexStagingBytes).ok) {
        if (SUCCEEDED(device.Get()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(impl.index_staging.GetAddressOf())))) {
            impl.index_staging_budgeted = true;
        } else {
            device.Memory().Free(d3d12::Owner::kHeaps, Impl::kIndexStagingBytes);
        }
    }

    // UPLOAD-heap index ring (4 x 1 MiB) for CPU-mapped index buffers.
    D3D12_RESOURCE_DESC ib_up_desc = buf_desc;
    ib_up_desc.Width = Impl::kIndexUploadBytes;
    if (device.Memory().Allocate(d3d12::Owner::kHeaps, Impl::kIndexUploadBytes).ok) {
        if (SUCCEEDED(device.Get()->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ib_up_desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                            IID_PPV_ARGS(impl.index_upload.GetAddressOf()))) &&
            SUCCEEDED(impl.index_upload->Map(0, nullptr, &impl.index_upload_mapped))) {
            impl.index_upload_budgeted = true;
        } else {
            impl.index_upload.Reset();
            impl.index_upload_mapped = nullptr;
            device.Memory().Free(d3d12::Owner::kHeaps, Impl::kIndexUploadBytes);
        }
    }

    // Indirect draw staging ring (4 x 16 KiB) + command signatures.
    D3D12_RESOURCE_DESC ind_desc = buf_desc;
    ind_desc.Width = Impl::kIndirectStagingBytes;
    (void)device.Get()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ind_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(impl.indirect_staging.GetAddressOf()));

    D3D12_INDIRECT_ARGUMENT_DESC draw_arg{};
    draw_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC draw_sig{};
    draw_sig.ByteStride = 4 * sizeof(std::uint32_t);
    draw_sig.NumArgumentDescs = 1;
    draw_sig.pArgumentDescs = &draw_arg;
    (void)device.Get()->CreateCommandSignature(&draw_sig, nullptr,
                                               IID_PPV_ARGS(impl.indirect_draw_sig.GetAddressOf()));

    D3D12_INDIRECT_ARGUMENT_DESC draw_idx_arg{};
    draw_idx_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC draw_idx_sig{};
    draw_idx_sig.ByteStride = 5 * sizeof(std::uint32_t);
    draw_idx_sig.NumArgumentDescs = 1;
    draw_idx_sig.pArgumentDescs = &draw_idx_arg;
    (void)device.Get()->CreateCommandSignature(&draw_idx_sig, nullptr,
                                               IID_PPV_ARGS(impl.indirect_draw_indexed_sig.GetAddressOf()));

    impl.psos = std::make_unique<d3d12::PsoCache>(device.Memory().cap(d3d12::Owner::kShaderTools));
    LogMessage(Level::kInfo, "guest graphics: ready (%s)", d3d12::GuestRootDescription(GuestStage::kGraphics).c_str());
    return d3d12::Status::kOk;
}

void GuestGraphics::Destroy() {
    if (impl_ == nullptr) return;
    Impl& impl = *impl_;
    impl.ResetBatchState();
    impl.active_cmd = Impl::CommandSlot{};
    if (impl.device != nullptr) {
        (void)impl.device->WaitIdle();
    }
    for (ID3D12PipelineState* pso : impl.batch_retired_psos) {
        if (pso != nullptr) pso->Release();
    }
    impl.batch_retired_psos.clear();
    for (Impl::RetiredPso& r : impl.retired_psos) {
        if (r.pso != nullptr) r.pso->Release();
    }
    impl.retired_psos.clear();
    for (auto& [k, pso] : impl.pso_objects) {
        (void)k;
        if (pso != nullptr) pso->Release();
    }
    impl.pso_objects.clear();
    impl.psos.reset();

    for (Impl::RetiredTarget& r : impl.retired_targets) {
        if (impl.device != nullptr && r.bytes != 0) {
            impl.device->Memory().Free(d3d12::Owner::kHeaps, r.bytes);
        }
    }
    impl.retired_targets.clear();
    for (auto& [k, entry] : impl.rtv_pool) {
        (void)k;
        if (impl.device != nullptr && entry.bytes != 0) {
            impl.device->Memory().Free(d3d12::Owner::kHeaps, entry.bytes);
        }
    }
    impl.rtv_pool.clear();
    for (auto& [k, entry] : impl.dsv_pool) {
        (void)k;
        if (impl.device != nullptr && entry.bytes != 0) {
            impl.device->Memory().Free(d3d12::Owner::kHeaps, entry.bytes);
        }
    }
    impl.dsv_pool.clear();

    impl.in_flight.clear();
    impl.free_lists.clear();
    impl.last_draw_fence = 0;

    if (impl.index_staging_budgeted && impl.device != nullptr) {
        impl.device->Memory().Free(d3d12::Owner::kHeaps, Impl::kIndexStagingBytes);
        impl.index_staging_budgeted = false;
    }
    impl.index_staging.Reset();
    impl.idx_slice = 0;
    impl.idx_slice_cursor = 0;
    impl.idx_slice_fence.fill(0);
    impl.idx_slices_pending_mask = 0;

    if (impl.index_upload != nullptr && impl.index_upload_mapped != nullptr) {
        impl.index_upload->Unmap(0, nullptr);
    }
    impl.index_upload_mapped = nullptr;
    if (impl.index_upload_budgeted && impl.device != nullptr) {
        impl.device->Memory().Free(d3d12::Owner::kHeaps, Impl::kIndexUploadBytes);
        impl.index_upload_budgeted = false;
    }
    impl.index_upload.Reset();
    impl.idx_up_slice = 0;
    impl.idx_up_slice_cursor = 0;
    impl.idx_up_slice_fence.fill(0);
    impl.idx_up_slices_pending_mask = 0;

    impl.indirect_staging.Reset();
    impl.indirect_slice = 0;
    impl.indirect_slice_cursor = 0;
    impl.indirect_slice_fence.fill(0);
    impl.indirect_slices_pending_mask = 0;
    impl.indirect_draw_sig.Reset();
    impl.indirect_draw_indexed_sig.Reset();

    if (impl.params != nullptr && impl.params_mapped != nullptr) {
        impl.params->Unmap(0, nullptr);
    }
    impl.params_mapped = nullptr;
    impl.params.Reset();
    impl.param_slice = 0;
    impl.param_slot_in_slice = 0;
    impl.param_slice_fence.fill(0);
    impl.param_slices_pending_mask = 0;

    impl.rtv_heap.Reset();
    impl.dsv_heap.Reset();
    impl.noted_by_hash.clear();
    impl.programs.clear();
    impl.fallback_keys.clear();
    impl.batch_retired_slices.clear();
    for (Impl::StageBindState& st : impl.stage_state) {
        st = Impl::StageBindState{};
    }
    impl.device = nullptr;
    impl.pipelines = nullptr;
    impl.memory = nullptr;
    impl.compute = nullptr;
    impl.descriptor_ring = nullptr;
    impl.root = nullptr;
}

bool GuestGraphics::Ready() const { return impl_ != nullptr && impl_->root != nullptr; }

void GuestGraphics::NoteShaderCreated(int /*creator*/, const std::uint8_t* container, std::size_t size) {
    if (impl_ == nullptr || container == nullptr || size < 16) return;
    Impl& impl = *impl_;

    // gx_trace passes `creator = id - kCreateShader156`:
    //   0 = kCreateShader156 (types 1, 5, 6), 1 = kCreateShader15 (types 1, 5),
    //   2 = kCreatePixelShader (type 2), 3 = kCreateComputeShader (type 4).
    // Determine the graphics stage directly from the container's parsed type
    // so pixel shaders (creator 2) and kCreateShader15 vertex shaders (creator
    // 1) are both indexed alongside kCreateShader156 vertex shaders.
    std::vector<std::uint8_t> buf(container, container + size);
    gcn::ShaderCode code;
    if (!gcn::shader_code(buf, code) || code.words.empty()) return;
    int norm_stage = -1;
    if (code.type == 1u) {
        norm_stage = 0;  // VS
    } else if (code.type == 2u) {
        norm_stage = 1;  // PS
    } else {
        return;
    }

    const ConsoleContainerStage cs = d3d12::ReadConsoleContainerStage(buf, code.type);
    if (!cs.ok) return;

    std::vector<std::uint32_t> words = std::move(code.words);
    const std::string name = FindOrbShdrName(buf);
    if (!name.empty()) shader_patch_apply(name, words);
    const std::uint64_t prog_hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(words.data()), words.size() * sizeof(std::uint32_t)));

    const NotedKey hash_key{norm_stage, prog_hash};
    if (impl.noted_by_hash.find(hash_key) != impl.noted_by_hash.end()) return;

    const gcn::Program decoded = gcn::decode(words.data(), words.size());
    if (!decoded.errors.empty()) return;
    gcn::TranslateOptions opts;
    opts.stage = norm_stage == 0 ? gcn::Stage::Vertex : gcn::Stage::Pixel;
    opts.rsrc1 = cs.key.rsrc1;
    opts.rsrc2 = cs.key.rsrc2;
    opts.vs_out_cntl = norm_stage == 0 ? cs.key.mask : 0;
    opts.ps_input_ena = norm_stage == 1 ? cs.key.mask : 0;
    const gcn::TranslateResult tr = gcn::translate(decoded, opts);
    if (!tr.errors.empty()) return;

    ConsoleStageKey key = cs.key;
    for (const gcn::PredictedImage& p : gcn::predict_image_dims(decoded, tr)) {
        key.dims.push_back((p.dim << 1) | (p.arrayed ? 1u : 0u));
    }
    key.sampler_modes.assign(tr.samplers.size(), 0);
    if (norm_stage == 1) {
        std::string why;
        if (!d3d12::ConsolePsInputMap(cs, tr.ps_inputs, key.input_map, key.flat_mask, why)) return;
        key.early_fragment_tests = false;
    }

    const std::uint64_t stage_key = d3d12::ConsoleStageKeyOf(words, key);
    NotedShader noted{};
    noted.stage = norm_stage;
    noted.key = stage_key;
    noted.element_count = static_cast<std::uint32_t>(key.elements.size());
    noted.formats_from_params = key.formats_from_params;
    impl.noted_by_hash.emplace(hash_key, noted);
    ++impl.counts.shaders_noted;
}

bool GuestGraphics::Draw(const GpuDraw& d, const DrawIdentity& id) {
    if (impl_ == nullptr || !Ready()) return false;
    Impl& impl = *impl_;
    impl.compute->EnsureCacheLoaded();
    if (!impl.compute->CacheReady()) {
        ++impl.counts.no_cache;
        return false;
    }

    const DrawTopologyInfo topo = d3d12::DecodeTopology(id.prim);
    if (!topo.supported) {
        ++impl.counts.bad_topology;
        return false;
    }

    // Extract VS and PS program words from guest memory (cached by code VA).
    const auto get_program = [&impl](std::uint64_t code_va) -> const ExtractedProgram* {
        if (const auto it = impl.programs.find(code_va); it != impl.programs.end()) return &it->second;
        ExtractedProgram extracted;
        if (!ExtractProgram(code_va, &extracted)) return nullptr;
        return &impl.programs.emplace(code_va, std::move(extracted)).first->second;
    };
    const ExtractedProgram* vs_prog = get_program(id.vs.code_va);
    const ExtractedProgram* ps_prog = get_program(id.ps.code_va);
    if (vs_prog == nullptr || ps_prog == nullptr) {
        ++impl.counts.bad_program;
        return false;
    }

    // Resolve bake-identical VS/PS keys (prefer container-noted keys, fall back
    // to register-derived keys if the container wasn't observed at creation).
    std::uint64_t vs_key = 0;
    std::uint32_t vs_elements = 0;
    if (const auto it = impl.noted_by_hash.find(NotedKey{0, vs_prog->hash}); it != impl.noted_by_hash.end()) {
        vs_key = it->second.key;
        vs_elements = it->second.element_count;
    } else {
        vs_key = impl.FallbackStageKey(0, *vs_prog, id);
    }
    std::uint64_t ps_key = 0;
    if (const auto it = impl.noted_by_hash.find(NotedKey{1, ps_prog->hash}); it != impl.noted_by_hash.end()) {
        ps_key = it->second.key;
    } else {
        ps_key = impl.FallbackStageKey(1, *ps_prog, id);
    }
    if (vs_key == 0 || ps_key == 0) {
        ++impl.counts.bad_program;
        return false;
    }

    // Check the live PSO table FIRST so cached draws skip both bbx_shaders.cache
    // lookups and zlib decompression completely.
    const PsoKey pso_key = d3d12::GraphicsPsoKey(vs_key, ps_key, id);
    ID3D12PipelineState* pso = nullptr;
    const PsoMapKey pso_map_key{pso_key.lo, pso_key.hi};
    if (const auto it = impl.pso_objects.find(pso_map_key); it != impl.pso_objects.end()) {
        (void)impl.psos->Acquire(pso_key, ++impl.clock);
        pso = it->second;
    } else {
        d3d12::BakedBlob vs_blob{}, ps_blob{};
        if (!impl.compute->Cache().Lookup(vs_key, &vs_blob)) {
            ++impl.counts.miss_vs;
            if (impl.logged_misses.insert(vs_key).second) {
                LogMessage(Level::kWarn, "guest graphics: VS key %016llx (%s) not in bbx_shaders.cache",
                           static_cast<unsigned long long>(vs_key), vs_prog->name.c_str());
            }
            return false;
        }
        if (!impl.compute->Cache().Lookup(ps_key, &ps_blob)) {
            ++impl.counts.miss_ps;
            if (impl.logged_misses.insert(ps_key).second) {
                LogMessage(Level::kWarn, "guest graphics: PS key %016llx (%s) not in bbx_shaders.cache",
                           static_cast<unsigned long long>(ps_key), ps_prog->name.c_str());
            }
            return false;
        }
        std::vector<std::uint8_t> vs_dxil(vs_blob.raw_size);
        std::vector<std::uint8_t> ps_dxil(ps_blob.raw_size);
        if (!Inflate(vs_blob, &vs_dxil) || !Inflate(ps_blob, &ps_dxil)) {
            ++impl.counts.bad_blob;
            return false;
        }
        pso = impl.EnsureGraphicsPso(pso_key, vs_dxil, ps_dxil, vs_elements, id, topo);
        if (pso == nullptr) return false;
    }

    // Ensure native 2D render targets (marking them GPU-rendered for zero-copy
    // direct scanout) and optional depth-stencil target.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[8] = {};
    std::uint32_t num_rtvs = 0;
    std::uint32_t primary_w = 0, primary_h = 0;
    for (std::uint32_t i = 0; i < id.target_count && i < 8; ++i) {
        const DrawTarget& t = id.targets[i];
        if (t.index >= 8) continue;
        if (!impl.EnsureRenderTarget(t, true, &rtv_handles[t.index])) {
            ++impl.counts.bad_target;
            return false;
        }
        if (t.index + 1 > num_rtvs) num_rtvs = t.index + 1;
        if (primary_w == 0) {
            primary_w = t.width;
            primary_h = t.height;
        }
    }
    if (num_rtvs == 0) {
        ++impl.counts.bad_target;
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle{};
    bool has_dsv = false;
    if (id.depth_format != 0) {
        const std::uint32_t dw = id.depth_stencil.depth_width != 0 ? id.depth_stencil.depth_width : primary_w;
        const std::uint32_t dh = id.depth_stencil.depth_height != 0 ? id.depth_stencil.depth_height : primary_h;
        if (impl.EnsureDepthTarget(id.depth_stencil.depth_base, id.depth_format, dw, dh, &dsv_handle)) {
            has_dsv = true;
        }
    }

    // Resolve indexed and indirect draw buffers before recording.
    const bool indexed = d.index_va != 0;
    const bool indirect = d.indirect_va != 0;
    ID3D12Resource* ib_slab = nullptr;
    std::uint64_t ib_slab_off = 0;
    std::uint64_t ib_staged_off = 0;
    std::uint64_t ib_bytes = 0;
    bool ib_from_slab = false;
    D3D12_INDEX_BUFFER_VIEW ibv{};
    if (indexed) {
        const std::uint32_t idx_stride = d.index_type == 1 ? 4u : 2u;
        const std::uint32_t count = indirect ? 65536u : d.index_count;
        ib_bytes = static_cast<std::uint64_t>(count) * idx_stride;
        if (ib_bytes == 0 || ib_bytes > Impl::kIndexStagingBytes) {
            ++impl.counts.bad_index;
            return false;
        }
        std::uint32_t slab_idx = 0;
        if (impl.memory->Table().ContiguousSlab(d.index_va, ib_bytes, &slab_idx, &ib_slab_off) &&
            impl.memory->EnsureSlab(slab_idx) == d3d12::Status::kOk &&
            impl.memory->SlabBuffer(slab_idx) != nullptr &&
            impl.ReserveIndexStaging(ib_bytes, &ib_staged_off)) {
            ib_slab = impl.memory->SlabBuffer(slab_idx);
            ib_from_slab = true;
            ibv.BufferLocation = impl.index_staging->GetGPUVirtualAddress() + ib_staged_off;
            ibv.SizeInBytes = static_cast<UINT>(ib_bytes);
            ibv.Format = d.index_type == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        } else if (hle_kernel_va_mapped(d.index_va, static_cast<std::size_t>(ib_bytes)) &&
                   impl.ReserveIndexUpload(ib_bytes, &ib_staged_off)) {
            std::memcpy(static_cast<std::uint8_t*>(impl.index_upload_mapped) + ib_staged_off,
                        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(d.index_va)),
                        static_cast<std::size_t>(ib_bytes));
            ibv.BufferLocation = impl.index_upload->GetGPUVirtualAddress() + ib_staged_off;
            ibv.SizeInBytes = static_cast<UINT>(ib_bytes);
            ibv.Format = d.index_type == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        } else {
            ++impl.counts.bad_index;
            return false;
        }
    }

    ID3D12Resource* indirect_slab = nullptr;
    std::uint64_t indirect_src_off = 0;
    std::uint64_t indirect_dst_off = 0;
    const std::uint64_t indirect_bytes = indexed ? 5u * sizeof(std::uint32_t) : 4u * sizeof(std::uint32_t);
    ID3D12CommandSignature* indirect_sig =
        indexed ? impl.indirect_draw_indexed_sig.Get() : impl.indirect_draw_sig.Get();
    if (indirect) {
        std::uint32_t slab_idx = 0;
        if (indirect_sig == nullptr || impl.indirect_staging == nullptr ||
            !impl.memory->Table().ContiguousSlab(d.indirect_va, indirect_bytes, &slab_idx, &indirect_src_off) ||
            impl.memory->EnsureSlab(slab_idx) != d3d12::Status::kOk) {
            ++impl.counts.bad_index;
            return false;
        }
        indirect_slab = impl.memory->SlabBuffer(slab_idx);
        if (indirect_slab == nullptr) {
            ++impl.counts.bad_index;
            return false;
        }
        const std::uint64_t aligned = (indirect_bytes + 15ull) & ~15ull;
        indirect_dst_off = impl.ReserveIndirectOffset(aligned);
    }

    // Write VS and PS StageParams into the 4-slice upload ring.
    std::uint32_t vs_slot = 0, ps_slot = 0;
    impl.ReserveParamPair(&vs_slot, &ps_slot);
    auto* vs_params = static_cast<std::uint8_t*>(impl.params_mapped) +
                      static_cast<std::size_t>(vs_slot) * d3d12::kStageParamsBytes;
    auto* ps_params = static_cast<std::uint8_t*>(impl.params_mapped) +
                      static_cast<std::size_t>(ps_slot) * d3d12::kStageParamsBytes;
    std::memset(vs_params, 0, d3d12::kStageParamsBytes);
    std::memset(ps_params, 0, d3d12::kStageParamsBytes);
    std::memcpy(vs_params + 16, id.vs.user, sizeof(id.vs.user));
    std::memcpy(ps_params + 16, id.ps.user, sizeof(id.ps.user));

    // Ring-buffered VS (stage 0) and PS (stage 1) space-1 descriptor tables.
    const std::uint32_t vs_table_off = impl.BindStageResources(0, vs_key, id.vs.user);
    const std::uint32_t ps_table_off = impl.BindStageResources(1, ps_key, id.ps.user);

    // Ensure any SRV ring slices used by VS or PS are marked pending for this batch
    // (even if PS binding caused an intermediate flush).
    const auto& vs_copies = impl.stage_state[0].srv_copies;
    const auto& ps_copies = impl.stage_state[1].srv_copies;
    for (const Impl::SrvCopy& copy : vs_copies) impl.compute->TouchSrvSnapshotOffset(copy.dst_offset);
    for (const Impl::SrvCopy& copy : ps_copies) impl.compute->TouchSrvSnapshotOffset(copy.dst_offset);

    // Open or reuse the active batched command list.
    if (!impl.BeginBatch()) {
        ++impl.counts.record_failed;
        return false;
    }
    ID3D12GraphicsCommandList* list = impl.active_cmd.list.Get();

    // Pre-draw copies from space-3 slabs into SRV snapshot ring, IB staging, and
    // indirect staging using stack arrays (zero heap allocations per draw).
    ID3D12Resource* copy_slabs[34];
    std::uint32_t copy_slab_count = 0;
    const auto add_slab = [&copy_slabs, &copy_slab_count](ID3D12Resource* res) {
        if (res == nullptr) return;
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            if (copy_slabs[i] == res) return;
        }
        if (copy_slab_count < 34) copy_slabs[copy_slab_count++] = res;
    };
    const bool has_srv_copies =
        (!vs_copies.empty() || !ps_copies.empty()) && impl.compute->SrvSnapshotRing() != nullptr;
    if (has_srv_copies) {
        for (const Impl::SrvCopy& copy : vs_copies) add_slab(impl.memory->SlabBuffer(copy.slab));
        for (const Impl::SrvCopy& copy : ps_copies) add_slab(impl.memory->SlabBuffer(copy.slab));
    }
    if (ib_from_slab) add_slab(ib_slab);
    if (indirect) add_slab(indirect_slab);

    if (copy_slab_count != 0) {
        D3D12_RESOURCE_BARRIER pre[34];
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            pre[i] = TransitionBarrier(copy_slabs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        list->ResourceBarrier(copy_slab_count, pre);

        if (has_srv_copies) {
            ID3D12Resource* srv_ring = impl.compute->SrvSnapshotRing();
            for (const Impl::SrvCopy& copy : vs_copies) {
                if (ID3D12Resource* slab = impl.memory->SlabBuffer(copy.slab)) {
                    list->CopyBufferRegion(srv_ring, copy.dst_offset, slab, copy.src_offset, copy.bytes);
                }
            }
            for (const Impl::SrvCopy& copy : ps_copies) {
                if (ID3D12Resource* slab = impl.memory->SlabBuffer(copy.slab)) {
                    list->CopyBufferRegion(srv_ring, copy.dst_offset, slab, copy.src_offset, copy.bytes);
                }
            }
        }
        if (ib_from_slab) {
            list->CopyBufferRegion(impl.index_staging.Get(), ib_staged_off, ib_slab, ib_slab_off, ib_bytes);
        }
        if (indirect) {
            list->CopyBufferRegion(impl.indirect_staging.Get(), indirect_dst_off, indirect_slab, indirect_src_off,
                                   indirect_bytes);
        }

        D3D12_RESOURCE_BARRIER post[37];
        std::uint32_t post_count = 0;
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            post[post_count++] = TransitionBarrier(copy_slabs[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (has_srv_copies) {
            post[post_count++] = TransitionBarrier(impl.compute->SrvSnapshotRing(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        if (ib_from_slab) {
            post[post_count++] = TransitionBarrier(impl.index_staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
        if (indirect) {
            post[post_count++] = TransitionBarrier(impl.indirect_staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
        list->ResourceBarrier(post_count, post);
    }

    // Bind heaps, root signature, samplers, space3, PSO, and space1 tables only
    // when they actually changed within this command list batch.
    if (!impl.batch_heaps_bound) {
        ID3D12DescriptorHeap* heaps[2] = {impl.device->CbvSrvUavHeap(), impl.device->SamplerHeap()};
        list->SetDescriptorHeaps(2, heaps);
        impl.batch_heaps_bound = true;
    }
    if (!impl.batch_root_bound) {
        list->SetGraphicsRootSignature(impl.root);
        impl.batch_root_bound = true;
    }
    if (impl.batch_pso != pso) {
        list->SetPipelineState(pso);
        impl.batch_pso = pso;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS params_base = impl.params->GetGPUVirtualAddress();
    const D3D12_GPU_DESCRIPTOR_HANDLE cbv_start =
        impl.device->CbvSrvUavHeap()->GetGPUDescriptorHandleForHeapStart();

    list->SetGraphicsRootConstantBufferView(0, params_base + static_cast<UINT64>(vs_slot) * d3d12::kStageParamsBytes);
    if (impl.batch_vs_table != vs_table_off) {
        D3D12_GPU_DESCRIPTOR_HANDLE vs_res = cbv_start;
        vs_res.ptr += static_cast<UINT64>(vs_table_off) * impl.cbv_stride;
        list->SetGraphicsRootDescriptorTable(1, vs_res);
        impl.batch_vs_table = vs_table_off;
    }
    list->SetGraphicsRootConstantBufferView(3, params_base + static_cast<UINT64>(ps_slot) * d3d12::kStageParamsBytes);
    if (impl.batch_ps_table != ps_table_off) {
        D3D12_GPU_DESCRIPTOR_HANDLE ps_res = cbv_start;
        ps_res.ptr += static_cast<UINT64>(ps_table_off) * impl.cbv_stride;
        list->SetGraphicsRootDescriptorTable(4, ps_res);
        impl.batch_ps_table = ps_table_off;
    }
    if (!impl.batch_samplers_bound) {
        const D3D12_GPU_DESCRIPTOR_HANDLE smp_start =
            impl.device->SamplerHeap()->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE vs_smp = smp_start;
        vs_smp.ptr += static_cast<UINT64>(impl.sampler_base + 0 * GuestHeapLayout::kSamplerCount) * impl.sampler_stride;
        D3D12_GPU_DESCRIPTOR_HANDLE ps_smp = smp_start;
        ps_smp.ptr += static_cast<UINT64>(impl.sampler_base + 1 * GuestHeapLayout::kSamplerCount) * impl.sampler_stride;
        list->SetGraphicsRootDescriptorTable(2, vs_smp);
        list->SetGraphicsRootDescriptorTable(5, ps_smp);
        impl.batch_samplers_bound = true;
    }
    if (!impl.batch_space3_bound) {
        list->SetGraphicsRootDescriptorTable(6, impl.memory->TableHandle());
        impl.batch_space3_bound = true;
    }

    // Output-merger render targets and stencil ref (filtered for redundant sets).
    bool om_changed = !impl.batch_om_set || impl.batch_num_rtvs != num_rtvs || impl.batch_has_dsv != has_dsv ||
                      (has_dsv && impl.batch_dsv.ptr != dsv_handle.ptr);
    if (!om_changed) {
        for (std::uint32_t i = 0; i < num_rtvs; ++i) {
            if (impl.batch_rtvs[i].ptr != rtv_handles[i].ptr) {
                om_changed = true;
                break;
            }
        }
    }
    if (om_changed) {
        list->OMSetRenderTargets(num_rtvs, rtv_handles, FALSE, has_dsv ? &dsv_handle : nullptr);
        impl.batch_om_set = true;
        impl.batch_num_rtvs = num_rtvs;
        for (std::uint32_t i = 0; i < num_rtvs; ++i) impl.batch_rtvs[i] = rtv_handles[i];
        impl.batch_has_dsv = has_dsv;
        impl.batch_dsv = dsv_handle;
    }

    if (!impl.batch_stencil_ref_set || impl.batch_stencil_ref != id.depth_stencil.stencil_ref) {
        list->OMSetStencilRef(id.depth_stencil.stencil_ref);
        impl.batch_stencil_ref_set = true;
        impl.batch_stencil_ref = id.depth_stencil.stencil_ref;
    }

    D3D12_VIEWPORT vp{};
    if (id.viewport.valid) {
        vp.TopLeftX = id.viewport.x;
        vp.TopLeftY = id.viewport.y;
        vp.Width = id.viewport.width;
        vp.Height = id.viewport.height;
        vp.MinDepth = id.viewport.min_depth;
        vp.MaxDepth = id.viewport.max_depth;
    } else {
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(primary_w);
        vp.Height = static_cast<float>(primary_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
    }
    if (!impl.batch_vp_set || std::memcmp(&impl.batch_vp, &vp, sizeof(vp)) != 0) {
        list->RSSetViewports(1, &vp);
        impl.batch_vp = vp;
        impl.batch_vp_set = true;
    }

    D3D12_RECT scissor{};
    scissor.left = id.scissor[0];
    scissor.top = id.scissor[1];
    scissor.right = id.scissor[2] > id.scissor[0] ? id.scissor[2] : static_cast<LONG>(primary_w);
    scissor.bottom = id.scissor[3] > id.scissor[1] ? id.scissor[3] : static_cast<LONG>(primary_h);
    if (!impl.batch_scissor_set || std::memcmp(&impl.batch_scissor, &scissor, sizeof(scissor)) != 0) {
        list->RSSetScissorRects(1, &scissor);
        impl.batch_scissor = scissor;
        impl.batch_scissor_set = true;
    }

    const D3D12_PRIMITIVE_TOPOLOGY d3d_topo = ToD3d12Topology(topo.topology);
    if (impl.batch_topology != d3d_topo) {
        list->IASetPrimitiveTopology(d3d_topo);
        impl.batch_topology = d3d_topo;
    }
    if (indexed) list->IASetIndexBuffer(&ibv);

    // Issue the draw call.
    const std::uint32_t instances = d.instance_count > 0 ? d.instance_count : 1u;
    if (indirect) {
        list->ExecuteIndirect(indirect_sig, 1, impl.indirect_staging.Get(), indirect_dst_off, nullptr, 0);
        ++impl.counts.indirect;
    } else if (indexed) {
        list->DrawIndexedInstanced(d.index_count, instances, 0, id.base_vertex, 0);
        ++impl.counts.indexed;
    } else {
        const std::uint32_t start_vertex = id.base_vertex > 0 ? static_cast<std::uint32_t>(id.base_vertex) : 0u;
        list->DrawInstanced(d.index_count, instances, start_vertex, 0);
    }

    // Restore transient staging buffers back to COPY_DEST (stack array, zero alloc).
    D3D12_RESOURCE_BARRIER epilogue[3];
    std::uint32_t epilogue_count = 0;
    if (has_srv_copies) {
        epilogue[epilogue_count++] = TransitionBarrier(impl.compute->SrvSnapshotRing(),
                                                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (ib_from_slab) {
        epilogue[epilogue_count++] = TransitionBarrier(impl.index_staging.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (indirect) {
        epilogue[epilogue_count++] = TransitionBarrier(impl.indirect_staging.Get(),
                                                       D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (epilogue_count != 0) {
        list->ResourceBarrier(epilogue_count, epilogue);
    }

    ++impl.counts.drawn;
    ++impl.batch_ops;
    if (impl.batch_ops >= Impl::kMaxBatchOps) {
        impl.FlushBatch();
    }
    return true;
}

bool GuestGraphics::ClearView(const bbx::d3d12::DecodedClearView& view, bool apply, bool* out_created) {
    if (impl_ == nullptr || !Ready() || !view.ok) return false;
    Impl& impl = *impl_;

    if (!view.depth) {
        const TargetKey key{view.base, view.format, view.width, view.height};
        const bool existed = impl.rtv_pool.find(key) != impl.rtv_pool.end();
        DrawTarget t{};
        t.index = 0;
        t.base = view.base;
        t.width = view.width;
        t.height = view.height;
        t.format = view.format;
        t.srgb = view.srgb;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
        if (!impl.EnsureRenderTarget(t, apply, &rtv_handle)) {
            ++impl.counts.bad_target;
            return false;
        }
        if (out_created != nullptr) *out_created = !existed;
        if (!apply) return true;

        if (!impl.BeginBatch()) {
            ++impl.counts.record_failed;
            return false;
        }
        impl.active_cmd.list->ClearRenderTargetView(rtv_handle, view.rgba, 0, nullptr);
        ++impl.counts.target_clears;
        ++impl.batch_ops;
        if (impl.batch_ops >= Impl::kMaxBatchOps) impl.FlushBatch();
        return true;
    }

    const TargetKey key{view.base, view.format, view.width, view.height};
    const bool existed = impl.dsv_pool.find(key) != impl.dsv_pool.end();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle{};
    if (!impl.EnsureDepthTarget(view.base, view.format, view.width, view.height, &dsv_handle)) {
        ++impl.counts.bad_target;
        return false;
    }
    if (out_created != nullptr) *out_created = !existed;
    if (!apply) return true;

    D3D12_CLEAR_FLAGS flags = static_cast<D3D12_CLEAR_FLAGS>(0);
    if (view.clear_depth) flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_DEPTH);
    if (view.clear_stencil) flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_STENCIL);
    if (flags == 0) return true;

    if (!impl.BeginBatch()) {
        ++impl.counts.record_failed;
        return false;
    }
    impl.active_cmd.list->ClearDepthStencilView(dsv_handle, flags, view.depth_value, view.stencil_value, 0, nullptr);
    ++impl.counts.target_clears;
    ++impl.batch_ops;
    if (impl.batch_ops >= Impl::kMaxBatchOps) impl.FlushBatch();
    return true;
}

bool GuestGraphics::ClearTargetAt(std::uint64_t va, std::uint32_t value, std::size_t bytes) {
    if (impl_ == nullptr || !Ready() || va == 0 || bytes == 0) return false;
    Impl& impl = *impl_;
    const std::uint64_t hi = va + bytes;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[Impl::kMaxRtvs];
    std::uint32_t rtv_count = 0;
    struct DsvClearItem {
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        std::uint32_t format = 0;
    };
    DsvClearItem dsvs[Impl::kMaxDsvs];
    std::uint32_t dsv_count = 0;

    for (auto& [key, entry] : impl.rtv_pool) {
        const std::uint64_t entry_hi = key.va + (entry.bytes != 0 ? entry.bytes : 4ull);
        if (key.va < hi && entry_hi > va) {
            entry.last_used_seq = ++impl.target_use_seq;
            entry.gpu_rendered = true;
            if (rtv_count < Impl::kMaxRtvs) rtvs[rtv_count++] = impl.RtvCpu(entry.slot);
        }
    }
    for (auto& [key, entry] : impl.dsv_pool) {
        const std::uint64_t entry_hi = key.va + (entry.bytes != 0 ? entry.bytes : 4ull);
        if (key.va < hi && entry_hi > va) {
            entry.last_used_seq = ++impl.target_use_seq;
            if (dsv_count < Impl::kMaxDsvs) dsvs[dsv_count++] = {impl.DsvCpu(entry.slot), key.format};
        }
    }
    if (rtv_count == 0 && dsv_count == 0) return false;

    if (!impl.BeginBatch()) {
        ++impl.counts.record_failed;
        return false;
    }
    ID3D12GraphicsCommandList* list = impl.active_cmd.list.Get();
    const float rgba[4] = {
        static_cast<float>(value & 0xFFu) / 255.0f,
        static_cast<float>((value >> 8) & 0xFFu) / 255.0f,
        static_cast<float>((value >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((value >> 24) & 0xFFu) / 255.0f,
    };
    for (std::uint32_t i = 0; i < rtv_count; ++i) {
        list->ClearRenderTargetView(rtvs[i], rgba, 0, nullptr);
    }
    const float depth_val = (value == 0u) ? 0.0f : 1.0f;
    const UINT8 stencil_val = static_cast<UINT8>(value & 0xFFu);
    for (std::uint32_t i = 0; i < dsv_count; ++i) {
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (dsvs[i].format == static_cast<std::uint32_t>(bbx::d3d12::DxgiFormat::kD24UnormS8Uint) ||
            dsvs[i].format == static_cast<std::uint32_t>(bbx::d3d12::DxgiFormat::kD32FloatS8X24Uint)) {
            flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_STENCIL);
        }
        list->ClearDepthStencilView(dsvs[i].handle, flags, depth_val, stencil_val, 0, nullptr);
    }
    ++impl.counts.target_clears;
    ++impl.batch_ops;
    if (impl.batch_ops >= Impl::kMaxBatchOps) impl.FlushBatch();
    return true;
}

bool GuestGraphics::CopyTarget(std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) {
    if (impl_ == nullptr || !Ready() || dst_va == 0 || src_va == 0 || dst_va == src_va || bytes == 0) return false;
    Impl& impl = *impl_;

    // Check color RTV pool for a target rooted at `src_va`.
    auto best_rtv = impl.rtv_pool.end();
    for (auto it = impl.rtv_pool.begin(); it != impl.rtv_pool.end(); ++it) {
        if (it->first.va == src_va) {
            if (best_rtv == impl.rtv_pool.end() || it->second.last_used_seq > best_rtv->second.last_used_seq) {
                best_rtv = it;
            }
        }
    }
    if (best_rtv != impl.rtv_pool.end()) {
        const TargetKey src_key = best_rtv->first;
        const bool src_rendered = best_rtv->second.gpu_rendered;
        best_rtv->second.last_used_seq = ++impl.target_use_seq;
        DrawTarget dst_t{};
        dst_t.index = 0;
        dst_t.base = dst_va;
        dst_t.width = src_key.width;
        dst_t.height = src_key.height;
        dst_t.format = src_key.format;
        D3D12_CPU_DESCRIPTOR_HANDLE dst_handle{};
        if (!impl.EnsureRenderTarget(dst_t, src_rendered, &dst_handle)) return false;

        const TargetKey dst_key{dst_va, src_key.format, src_key.width, src_key.height};
        const auto src_it = impl.rtv_pool.find(src_key);
        const auto dst_it = impl.rtv_pool.find(dst_key);
        if (src_it == impl.rtv_pool.end() || dst_it == impl.rtv_pool.end() ||
            src_it->second.resource == nullptr || dst_it->second.resource == nullptr) {
            return false;
        }
        if (src_rendered) dst_it->second.gpu_rendered = true;

        if (!impl.BeginBatch()) {
            ++impl.counts.record_failed;
            return false;
        }
        ID3D12GraphicsCommandList* list = impl.active_cmd.list.Get();
        D3D12_RESOURCE_BARRIER pre[2] = {
            TransitionBarrier(src_it->second.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_SOURCE),
            TransitionBarrier(dst_it->second.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, pre);
        list->CopyResource(dst_it->second.resource.Get(), src_it->second.resource.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            TransitionBarrier(src_it->second.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET),
            TransitionBarrier(dst_it->second.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        list->ResourceBarrier(2, post);
        ++impl.counts.target_copies;
        ++impl.batch_ops;
        if (impl.batch_ops >= Impl::kMaxBatchOps) impl.FlushBatch();
        return true;
    }

    // Check depth DSV pool for a target rooted at `src_va`.
    auto best_dsv = impl.dsv_pool.end();
    for (auto it = impl.dsv_pool.begin(); it != impl.dsv_pool.end(); ++it) {
        if (it->first.va == src_va) {
            if (best_dsv == impl.dsv_pool.end() || it->second.last_used_seq > best_dsv->second.last_used_seq) {
                best_dsv = it;
            }
        }
    }
    if (best_dsv != impl.dsv_pool.end()) {
        const TargetKey src_key = best_dsv->first;
        best_dsv->second.last_used_seq = ++impl.target_use_seq;
        D3D12_CPU_DESCRIPTOR_HANDLE dst_handle{};
        if (!impl.EnsureDepthTarget(dst_va, src_key.format, src_key.width, src_key.height, &dst_handle)) return false;

        const TargetKey dst_key{dst_va, src_key.format, src_key.width, src_key.height};
        const auto src_it = impl.dsv_pool.find(src_key);
        const auto dst_it = impl.dsv_pool.find(dst_key);
        if (src_it == impl.dsv_pool.end() || dst_it == impl.dsv_pool.end() ||
            src_it->second.resource == nullptr || dst_it->second.resource == nullptr) {
            return false;
        }

        if (!impl.BeginBatch()) {
            ++impl.counts.record_failed;
            return false;
        }
        ID3D12GraphicsCommandList* list = impl.active_cmd.list.Get();
        D3D12_RESOURCE_BARRIER pre[2] = {
            TransitionBarrier(src_it->second.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE),
            TransitionBarrier(dst_it->second.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                              D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, pre);
        list->CopyResource(dst_it->second.resource.Get(), src_it->second.resource.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            TransitionBarrier(src_it->second.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE),
            TransitionBarrier(dst_it->second.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE),
        };
        list->ResourceBarrier(2, post);
        ++impl.counts.target_copies;
        ++impl.batch_ops;
        if (impl.batch_ops >= Impl::kMaxBatchOps) impl.FlushBatch();
        return true;
    }

    return false;
}

void GuestGraphics::ForgetRegion(std::uint64_t lo, std::uint64_t hi) {
    if (impl_ == nullptr || lo >= hi) return;
    Impl& impl = *impl_;

    bool flushed = false;
    std::uint64_t retire_fence = impl.device != nullptr ? impl.device->SubmittedValue() : 0;
    const auto ensure_flushed = [&impl, &flushed, &retire_fence]() {
        if (!flushed) {
            impl.FlushBatch();
            retire_fence = impl.device != nullptr ? impl.device->SubmittedValue() : 0;
            flushed = true;
        }
    };

    for (auto it = impl.rtv_pool.begin(); it != impl.rtv_pool.end();) {
        const std::uint64_t entry_lo = it->first.va;
        const std::uint64_t entry_hi = entry_lo + (it->second.bytes != 0 ? it->second.bytes : 4ull);
        if (entry_lo < hi && entry_hi > lo) {
            ensure_flushed();
            impl.free_rtv_slots.push_back(it->second.slot);
            impl.retired_targets.push_back({std::move(it->second.resource), it->second.bytes, retire_fence});
            it = impl.rtv_pool.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = impl.dsv_pool.begin(); it != impl.dsv_pool.end();) {
        const std::uint64_t entry_lo = it->first.va;
        const std::uint64_t entry_hi = entry_lo + (it->second.bytes != 0 ? it->second.bytes : 4ull);
        if (entry_lo < hi && entry_hi > lo) {
            ensure_flushed();
            impl.free_dsv_slots.push_back(it->second.slot);
            impl.retired_targets.push_back({std::move(it->second.resource), it->second.bytes, retire_fence});
            it = impl.dsv_pool.erase(it);
        } else {
            ++it;
        }
    }
    InvalidatePrograms(lo, hi);
}

void GuestGraphics::InvalidatePrograms(std::uint64_t lo, std::uint64_t hi) {
    if (impl_ == nullptr || lo >= hi) return;
    Impl& impl = *impl_;
    for (auto it = impl.programs.begin(); it != impl.programs.end();) {
        const std::uint64_t prog_lo = it->first;
        const std::uint64_t prog_hi =
            prog_lo + static_cast<std::uint64_t>(it->second.words.size()) * sizeof(std::uint32_t);
        if (prog_lo < hi && prog_hi > lo) {
            it = impl.programs.erase(it);
        } else {
            ++it;
        }
    }
}

void GuestGraphics::Flush() {
    if (impl_ == nullptr) return;
    impl_->FlushBatch();
}

bool GuestGraphics::HasPending() const {
    return impl_ != nullptr && impl_->batch_open;
}

bool GuestGraphics::HasRenderedTarget(std::uint64_t va, std::uint32_t width, std::uint32_t height) const {
    if (impl_ == nullptr || va == 0 || width == 0 || height == 0) return false;
    const Impl& impl = *impl_;
    for (const auto& [key, entry] : impl.rtv_pool) {
        if (key.va == va && key.width == width && key.height == height &&
            entry.gpu_rendered && entry.resource != nullptr && IsDirectPresentFormat(key.format)) {
            return true;
        }
    }
    return false;
}

bool GuestGraphics::TryAcquirePresentTarget(std::uint64_t va, std::uint32_t width, std::uint32_t height,
                                            bbx::d3d12::ComPtr<ID3D12Resource>* out_resource,
                                            DXGI_FORMAT* out_format) const {
    if (impl_ == nullptr || va == 0 || width == 0 || height == 0 ||
        out_resource == nullptr || out_format == nullptr) {
        return false;
    }
    const Impl& impl = *impl_;
    auto best = impl.rtv_pool.end();
    for (auto it = impl.rtv_pool.begin(); it != impl.rtv_pool.end(); ++it) {
        if (it->first.va == va && it->first.width == width && it->first.height == height &&
            it->second.gpu_rendered && it->second.resource != nullptr && IsDirectPresentFormat(it->first.format)) {
            if (best == impl.rtv_pool.end() || it->second.last_used_seq > best->second.last_used_seq) {
                best = it;
            }
        }
    }
    if (best == impl.rtv_pool.end()) return false;
    *out_resource = best->second.resource;
    *out_format = static_cast<DXGI_FORMAT>(best->first.format);
    return true;
}

std::string GuestGraphics::Report() const {
    if (impl_ == nullptr) return "guest graphics: none";
    const Impl& impl = *impl_;
    char line[576];
    std::snprintf(line, sizeof(line),
                  "guest graphics: %llu draw(s) in %llu batch(es) (%llu indexed, %llu indirect), "
                  "%llu clear(s), %llu copy(s), %llu shader(s) noted, %llu PSO(s) (%llu evicted, %llu failed), "
                  "%llu target(s), %llu miss(es) (vs %llu, ps %llu), %llu space-1 slot(s) (%llu srv, %llu refused)",
                  static_cast<unsigned long long>(impl.counts.drawn),
                  static_cast<unsigned long long>(impl.counts.batches),
                  static_cast<unsigned long long>(impl.counts.indexed),
                  static_cast<unsigned long long>(impl.counts.indirect),
                  static_cast<unsigned long long>(impl.counts.target_clears),
                  static_cast<unsigned long long>(impl.counts.target_copies),
                  static_cast<unsigned long long>(impl.counts.shaders_noted),
                  static_cast<unsigned long long>(impl.counts.created_psos),
                  static_cast<unsigned long long>(impl.counts.evicted_psos),
                  static_cast<unsigned long long>(impl.counts.bad_pso),
                  static_cast<unsigned long long>(impl.counts.targets_created),
                  static_cast<unsigned long long>(impl.counts.miss_vs + impl.counts.miss_ps),
                  static_cast<unsigned long long>(impl.counts.miss_vs),
                  static_cast<unsigned long long>(impl.counts.miss_ps),
                  static_cast<unsigned long long>(impl.counts.bindings_written),
                  static_cast<unsigned long long>(impl.counts.bindings_srv),
                  static_cast<unsigned long long>(impl.counts.bindings_refused));
    return line;
}

void GuestGraphics::OnFrame(std::uint64_t completed) {
    if (impl_ == nullptr) return;
    impl_->ReclaimCompleted(completed);
    if (impl_->psos != nullptr) {
        const std::uint64_t retire_fence = impl_->device != nullptr ? impl_->device->SubmittedValue() : completed;
        for (const PsoKey& evicted : impl_->psos->Trim(0)) {
            const PsoMapKey key{evicted.lo, evicted.hi};
            if (const auto it = impl_->pso_objects.find(key); it != impl_->pso_objects.end()) {
                if (impl_->batch_open) {
                    impl_->batch_retired_psos.push_back(it->second);
                } else {
                    impl_->retired_psos.push_back({it->second, retire_fence});
                }
                impl_->pso_objects.erase(it);
                ++impl_->counts.evicted_psos;
            }
        }
    }
    impl_->completed = completed;
}

}  // namespace bbx::platform
