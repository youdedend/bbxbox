// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay pass: the launcher's, the boot report's and the frame telemetry's
// text, drawn into the same backbuffer the game will draw into.
//
// Text is a texture atlas of 1-bit glyphs uploaded once (see
// xbox/platform/font8.h) and instanced quads written into a dynamic vertex
// buffer. The pass deliberately knows nothing about fonts: it takes a position in
// pixels, a uv into the atlas and a colour per vertex, which is also what the
// solid-colour panels (status bars, memory gauge) use.
//
// Compiled at build time: dxc -T vs_6_4 -E OverlayVs / ps_6_4 -E OverlayPs.
// No [[vk::*]], no ResourceDescriptorHeap[], no 16-bit types.

cbuffer OverlayParams : register(b0) {
    float2 viewport;   // pixels; the CoreWindow's bounds, not the TV's mode
    float2 _padding;
};

struct OverlayVertex {
    float2 position : POSITION;   // pixels, origin top-left
    float2 uv : TEXCOORD0;        // atlas coordinates
    float4 color : COLOR0;        // straight alpha
};

struct OverlayOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

OverlayOut OverlayVs(OverlayVertex input) {
    OverlayOut output;
    output.position = float4(input.position.x / viewport.x * 2.0 - 1.0,
                             1.0 - input.position.y / viewport.y * 2.0,
                             0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

// One channel: the glyph atlas is coverage, and the vertex colour is the ink.
Texture2D<float> atlas : register(t0);
SamplerState atlas_sampler : register(s0);

float4 OverlayPs(OverlayOut input) : SV_Target {
    const float coverage = atlas.Sample(atlas_sampler, input.uv);
    return float4(input.color.rgb, input.color.a * coverage);
}
