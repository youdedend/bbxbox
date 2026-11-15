// SPDX-License-Identifier: GPL-3.0-or-later
// Root signatures and PSO creation for the port's own passes (see pipeline.h).
#include "src/host/d3d12/pipeline.h"

#include <cstdio>

#include "src/host/d3d12/device.h"

namespace bbx::d3d12 {
namespace {

// The overlay's vertex, as a console reads it: 32 bytes, matching
// xbox/shaders/overlay.hlsl's OverlayVertex.
struct OverlayVertexLayout {
    static constexpr D3D12_INPUT_ELEMENT_DESC kElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
};

D3D12_SHADER_BYTECODE Bytecode(const shaders::Blob& blob) {
    D3D12_SHADER_BYTECODE code{};
    code.pShaderBytecode = blob.data;
    code.BytecodeLength = blob.size;
    return code;
}

// Straight alpha, written the one way the console accepts it: SRC_ALPHA /
// INV_SRC_ALPHA. The constant-alpha blend factors (BLEND_FACTOR and
// INV_BLEND_FACTOR) fail PSO creation on a console (guard G4), so they are not
// spelled here at all and precheck greps for them.
D3D12_BLEND_DESC StraightAlphaBlend() {
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.RenderTargetWriteMask = 0x0F;
    return blend;
}

D3D12_RASTERIZER_DESC FullscreenRasterizer() {
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    // The fullscreen triangle's winding is fixed by its vertex order; culling is
    // off so a driver that disagrees cannot delete it.
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    return raster;
}

D3D12_DEPTH_STENCIL_DESC NoDepth() {
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilEnable = FALSE;
    return depth;
}

}  // namespace

const char* PassName(PassKind kind) {
    switch (kind) {
        case PassKind::kPresent: return "present";
        case PassKind::kOverlay: return "overlay";
        case PassKind::kUntile: return "untile";
        case PassKind::kSelftest: return "stream_selftest";
        case PassKind::kCount: break;
    }
    return "unknown";
}

std::uint32_t PipelineSet::RootConstants(PassKind kind) {
    switch (kind) {
        // PresentParams and OverlayParams are 16 bytes each (a float4 / two
        // float2s and padding): 4 dwords.
        case PassKind::kPresent: return 4;
        case PassKind::kOverlay: return 4;
        // UntileParams: 8 dwords (two uint2s and four uints). SelftestParams: 4.
        case PassKind::kUntile: return 8;
        case PassKind::kSelftest: return 4;
        case PassKind::kCount: break;
    }
    return 0;
}

PipelineSet::~PipelineSet() { Destroy(); }

Status PipelineSet::Create(Device& device) {
    Destroy();
    device_ = &device;
    // PSOs are charged to Owner::kShaderTools, the owner that already holds the
    // DXIL cache and the compile arena (there is no separate pipeline owner in
    // bbx_budget's plan); the object cap is the measured console limit.
    cache_ = std::make_unique<PsoCache>(device.Memory().cap(Owner::kShaderTools));
    for (std::size_t index = 0; index < roots_.size(); ++index) {
        const Status status = CreateRootSignature(static_cast<PassKind>(index));
        if (status != Status::kOk) { return status; }
    }
    Log("pipelines: root signatures ready (%s)", Describe().c_str());
    return Status::kOk;
}

Status PipelineSet::CreateRootSignature(PassKind kind) {
    // Descriptor tables, one per kind, one space: never heap indexing (SM 6.6 is
    // not on the console). The compute passes read t0 and write u0, so the untile
    // signature carries two tables; the graphics passes read t0 only.
    const bool compute = kind == PassKind::kUntile || kind == PassKind::kSelftest;
    const bool reads_texture = kind == PassKind::kUntile || kind == PassKind::kPresent ||
                               kind == PassKind::kOverlay;
    const bool writes_uav = kind == PassKind::kUntile || kind == PassKind::kSelftest;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;  // t0; a guest's arrays are P3's business
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_APPEND_ALIGNED_ELEMENT;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;  // u0
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_APPEND_ALIGNED_ELEMENT;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;  // b0
    parameters[0].Constants.RegisterSpace = 0;
    parameters[0].Constants.Num32BitValues = RootConstants(kind);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1 is always the read table (t0) where the pass reads one, else the
    // write table (u0); parameter 2, when present, is the write table.
    std::uint32_t count = 1;
    auto add_table = [&](D3D12_DESCRIPTOR_RANGE* range) {
        parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[count].DescriptorTable.NumDescriptorRanges = 1;
        parameters[count].DescriptorTable.pDescriptorRanges = range;
        parameters[count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++count;
    };
    if (reads_texture) { add_table(&ranges[0]); }
    if (writes_uav) { add_table(&ranges[1]); }

    D3D12_STATIC_SAMPLER_DESC samplers[1]{};
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = 0.0f;  // no mips in the port's own passes
    // present.hlsl samples the frame image bilinearly; overlay.hlsl samples the
    // glyph atlas point-wise or glyphs blur.
    samplers[0].Filter = kind == PassKind::kPresent ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                                                    : D3D12_FILTER_MIN_MAG_MIP_POINT;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = count;
    desc.pParameters = parameters;
    desc.NumStaticSamplers = compute ? 0u : 1u;
    desc.pStaticSamplers = compute ? nullptr : samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (kind == PassKind::kOverlay) {
        // The overlay draws from a vertex buffer, so it needs the input assembler.
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr)) {
        if (error) {
            Log("root signature (%s) refused: %s", PassName(kind),
                static_cast<const char*>(error->GetBufferPointer()));
        } else {
            Log("D3D12SerializeRootSignature (%s) failed: 0x%08lX", PassName(kind),
                static_cast<unsigned long>(hr));
        }
        return FromHresult(hr);
    }
    const HRESULT created = device_->Get()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(roots_[static_cast<std::size_t>(kind)].GetAddressOf()));
    if (FAILED(created)) {
        Log("CreateRootSignature (%s) failed: 0x%08lX", PassName(kind),
            static_cast<unsigned long>(created));
        return FromHresult(created);
    }
    Log("root signature %s: %u parameter(s), %s", PassName(kind), count,
        compute ? "compute" : "graphics");
    return Status::kOk;
}

ID3D12PipelineState* PipelineSet::Ensure(PassKind kind) {
    if (kind >= PassKind::kCount || device_ == nullptr) { return nullptr; }
    const std::size_t index = static_cast<std::size_t>(kind);
    if (psos_[index] != nullptr) { return psos_[index].Get(); }

    Status status = Status::kUnavailable;
    switch (kind) {
        case PassKind::kPresent:
            status = CreateGraphicsPso(kind, shaders::PresentVs(), shaders::PresentPs());
            break;
        case PassKind::kOverlay:
            status = CreateGraphicsPso(kind, shaders::OverlayVs(), shaders::OverlayPs());
            break;
        case PassKind::kUntile: status = CreateComputePso(kind, shaders::UntileCs()); break;
        case PassKind::kSelftest: status = CreateComputePso(kind, shaders::SelftestCs()); break;
        case PassKind::kCount: break;
    }
    if (status != Status::kOk) {
        Log("pipeline: %s is unavailable (%s); the pass will be skipped", PassName(kind),
            StatusName(status));
        return nullptr;
    }
    return psos_[index].Get();
}

Status PipelineSet::CreateGraphicsPso(PassKind kind, const shaders::Blob& vs,
                                      const shaders::Blob& ps) {
    if (vs.data == nullptr || ps.data == nullptr) {
        // The generated header was not linked in: a build fault, said plainly.
        Log("pipeline: %s has no DXIL (was bbx_shaders_generated.h linked?)", PassName(kind));
        return Status::kUnavailable;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = roots_[static_cast<std::size_t>(kind)].Get();
    desc.VS = Bytecode(vs);
    desc.PS = Bytecode(ps);
    desc.BlendState = StraightAlphaBlend();
    desc.SampleMask = 0xFFFFFFFF;
    desc.RasterizerState = FullscreenRasterizer();
    desc.DepthStencilState = NoDepth();
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    if (kind == PassKind::kOverlay) {
        desc.InputLayout.pInputElementDescs = OverlayVertexLayout::kElements;
        desc.InputLayout.NumElements =
            static_cast<UINT>(sizeof(OverlayVertexLayout::kElements) /
                              sizeof(OverlayVertexLayout::kElements[0]));
    }
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType =
        kind == PassKind::kOverlay ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                   : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    // The swapchain's format, which is what both passes write into.
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->Get()->CreateGraphicsPipelineState(
        &desc, __uuidof(ID3D12PipelineState),
        reinterpret_cast<void**>(pso.GetAddressOf()));
    if (FAILED(hr)) {
        Log("CreateGraphicsPipelineState (%s, %zu + %zu bytes of DXIL) failed: 0x%08lX",
            PassName(kind), vs.size, ps.size, static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    psos_[static_cast<std::size_t>(kind)] = pso;
    // Register with the cache: it owns the accounting (object cap, 192 MiB) and
    // remembers the eviction order, even though these four are held directly.
    PsoKeyInput input{};
    input.vs = Fnv1a64(std::string_view(static_cast<const char*>(vs.data), vs.size));
    input.ps = Fnv1a64(std::string_view(static_cast<const char*>(ps.data), ps.size));
    input.rtv_formats[0] = static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    input.rtv_count = 1;
    input.sample_count = 1;
    input.topology = static_cast<std::uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    input.feature_level = 11;
    const PsoKey key = MakePsoKey(input);
    (void)cache_->Insert(key, Fnv1a64(std::string_view(PassName(kind))), vs.size + ps.size, ++clock_);
    Log("pipeline %s: %zu + %zu bytes of DXIL, PSO ready", PassName(kind), vs.size, ps.size);
    return Status::kOk;
}

Status PipelineSet::CreateComputePso(PassKind kind, const shaders::Blob& cs) {
    if (cs.data == nullptr) {
        Log("pipeline: %s has no DXIL (was bbx_shaders_generated.h linked?)", PassName(kind));
        return Status::kUnavailable;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = roots_[static_cast<std::size_t>(kind)].Get();
    desc.CS = Bytecode(cs);
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->Get()->CreateComputePipelineState(
        &desc, __uuidof(ID3D12PipelineState),
        reinterpret_cast<void**>(pso.GetAddressOf()));
    if (FAILED(hr)) {
        Log("CreateComputePipelineState (%s) failed: 0x%08lX", PassName(kind),
            static_cast<unsigned long>(hr));
        return FromHresult(hr);
    }
    psos_[static_cast<std::size_t>(kind)] = pso;
    PsoKeyInput input{};
    input.vs = Fnv1a64(std::string_view(static_cast<const char*>(cs.data), cs.size));
    input.feature_level = 11;
    const PsoKey key = MakePsoKey(input);
    (void)cache_->Insert(key, Fnv1a64(std::string_view(PassName(kind))), cs.size, ++clock_);
    Log("pipeline %s: %zu bytes of DXIL, compute PSO ready", PassName(kind), cs.size);
    return Status::kOk;
}

void PipelineSet::Collect(std::uint64_t completed) {
    if (cache_ == nullptr) { return; }
    // The cache's own clock is the frame tick; a PSO released here is one no
    // submission at or before `completed` can still reference.
    for (const PsoKey& key : cache_->Trim(0)) {
        Log("pipeline: retired a PSO whose fence %llu has passed",
            static_cast<unsigned long long>(completed));
        (void)key;
    }
}

std::string PipelineSet::Describe() const {
    if (cache_ == nullptr) { return "pipelines: none"; }
    const PsoStats& stats = cache_->Stats();
    char text[192];
    std::snprintf(text, sizeof(text), "PSOs: %u live, %llu hits, %llu misses, %llu evictions",
                  stats.live, static_cast<unsigned long long>(stats.hits),
                  static_cast<unsigned long long>(stats.misses),
                  static_cast<unsigned long long>(stats.evictions));
    return text;
}

void PipelineSet::Destroy() {
    for (auto& pso : psos_) { pso.Reset(); }
    for (auto& root : roots_) { root.Reset(); }
    cache_.reset();
    device_ = nullptr;
}

}  // namespace bbx::d3d12
