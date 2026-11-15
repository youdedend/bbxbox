// SPDX-License-Identifier: GPL-3.0-or-later
// The present pass: a fullscreen triangle that samples the frame's final image.
//
// Why this exists at all: bbhost's stream ends a frame with a blit to the
// swapchain. On the console the swapchain image is a FLIP_DISCARD backbuffer and
// the guest's "scanout" is an ordinary texture in the guest's own tiling, so the
// last step is always a sample-and-write. Doing it in a shader rather than a copy
// is what lets the port apply the SDR/HDR transfer and the 4K inverse scale in
// one place (guard G8: CoreWindow is 1080p, the TV may be 4K).
//
// Compiled at build time with dxc -T vs_6_4 / ps_6_4 (see cmake/embed_shaders.cmake
// and xbox/CMakeLists.txt). This file must not use anything SM 6.4 lacks on the
// console: no ResourceDescriptorHeap[], no 16-bit types, no mesh shaders.
// It is also not bbhost's `gcn/hlsl.cpp` output: that emitter produces SPIR-V
// flavoured HLSL ([[vk::*]] attributes) for dxc -spirv and cannot be fed to a
// DXIL profile. See docs/ARCHITECTURE_XBOX_D3D12.md §7.

cbuffer PresentParams : register(b0) {
    float4 tint;          // rgb multiplier, a = transfer selector (1 = passthrough)
    float4 source_size;   // xy = texel size of the sampled image
};

struct PresentOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// The fullscreen triangle: three vertices, no vertex buffer, no index buffer.
// (-1,-1) (3,-1) (-1,3) covers the viewport exactly once, which is faster than
// two triangles and has no diagonal seam.
PresentOut PresentVs(uint vertex_id : SV_VertexID) {
    PresentOut output;
    const float2 corner = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);
    output.position = float4(corner, 0.0, 1.0);
    // Direct3D's texture origin is the top-left; NDC's y points up.
    output.uv = float2((corner.x + 1.0) * 0.5, 1.0 - (corner.y + 1.0) * 0.5);
    return output;
}

Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);

float4 PresentPs(PresentOut input) : SV_Target {
    const float4 sampled = source.Sample(source_sampler, input.uv);
    return float4(sampled.rgb * tint.rgb, sampled.a * tint.a);
}
