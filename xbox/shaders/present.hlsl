// SPDX-License-Identifier: GPL-3.0-or-later
// The present pass: a fullscreen triangle that samples the frame's final image
// during gameplay, or renders the procedural Bloodborne Yharnam Blood Moon &
// Hunter's Mark gothic backdrop while in the pre-boot launcher / shader baker.
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

cbuffer PresentParams : register(b0) {
    float4 tint;          // rgb multiplier, a = transfer selector (1 = passthrough)
    float4 source_size;   // xy = texel size (1/w, 1/h), z = RCAS sharpness (0 = off), w = backdrop aspect (> 0.5 = Yharnam backdrop)
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

float3 ApplyRcas(float2 uv, float3 center, float2 texel, float sharpness) {
    const float3 n = source.Sample(source_sampler, uv + float2(0.0, -texel.y)).rgb;
    const float3 s = source.Sample(source_sampler, uv + float2(0.0, texel.y)).rgb;
    const float3 w = source.Sample(source_sampler, uv + float2(-texel.x, 0.0)).rgb;
    const float3 e = source.Sample(source_sampler, uv + float2(texel.x, 0.0)).rgb;

    const float3 mn = min(center, min(min(n, s), min(w, e)));
    const float3 mx = max(center, max(max(n, s), max(w, e)));
    const float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-5));
    const float3 weight = -sqrt(amp) * (saturate(sharpness) * 0.22);
    const float3 denom = max(1.0 + 4.0 * weight, 0.12);
    return saturate((center + weight * (n + s + w + e)) / denom);
}

float SdSegment(float2 p, float2 a, float2 b) {
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float denom = max(dot(ba, ba), 1e-6);
    const float h = saturate(dot(pa, ba) / denom);
    return length(pa - ba * h);
}

float HuntersMarkSdf(float2 p) {
    const float2 q = float2(abs(p.x), p.y);
    float d = SdSegment(q, float2(0.0, -0.78), float2(0.0, 0.45));
    d = min(d, SdSegment(q, float2(0.0, 0.45), float2(0.0, 0.77)));
    d = min(d, SdSegment(q, float2(0.0, -0.57), float2(0.135, -0.57)));
    d = min(d, SdSegment(q, float2(0.0, 0.43), float2(0.095, 0.43)));
    d = min(d, SdSegment(q, float2(0.0, -0.43), float2(0.34, -0.05)));
    d = min(d, SdSegment(q, float2(0.34, -0.05), float2(0.175, 0.35)));
    d = min(d, SdSegment(q, float2(0.175, 0.35), float2(0.0, 0.45)));
    d = min(d, SdSegment(q, float2(0.255, -0.155), float2(0.105, -0.045)));
    d = min(d, SdSegment(q, float2(0.245, 0.145), float2(0.095, 0.055)));
    d = min(d, SdSegment(q, float2(0.02, 0.44), float2(0.085, 0.63)));
    const float etch = 0.003 * sin(q.y * 28.0 + q.x * 16.0);
    return d + etch;
}

float3 RenderYharnamBackdrop(float2 uv, float aspect) {
    const float2 p = float2((uv.x - 0.5) * 2.0 * aspect, (uv.y - 0.5) * 2.0);

    // 1. Yharnam midnight sky gradient and drifting blood-mist clouds.
    const float sky_t = saturate((p.y + 1.0) * 0.5);
    float3 rgb = lerp(float3(0.024, 0.015, 0.032), float3(0.105, 0.026, 0.042), sky_t * sky_t);
    const float mist = 0.5 + 0.5 * sin(p.x * 2.4 + cos(p.y * 3.1)) * cos(p.y * 2.2 - p.x * 1.3);
    rgb = lerp(rgb, float3(0.20, 0.038, 0.055), mist * 0.25);

    // 2. The Paleblood / Crimson Red Moon hanging over upper-right Yharnam.
    const float2 moon_center = float2(0.46 * aspect, -0.18);
    const float2 mp = p - moon_center;
    const float moon_r = length(mp);
    const float moon_radius = 0.50;

    const float corona = exp(-max(0.0, moon_r - 0.12) * 2.1);
    rgb = lerp(rgb, float3(0.48, 0.065, 0.090), corona * 0.58);

    if (moon_r < moon_radius + 0.03) {
        const float disc = 1.0 - smoothstep(moon_radius - 0.02, moon_radius + 0.015, moon_r);
        const float crater = 0.5 + 0.28 * sin(mp.x * 9.0 + mp.y * 6.0) +
                             0.22 * cos(mp.x * 16.0 - mp.y * 13.0);
        const float limb = saturate(moon_r / moon_radius);
        const float3 moon_core = lerp(float3(0.52, 0.08, 0.09), float3(0.30, 0.04, 0.06), crater * 0.65);
        const float3 moon_col = lerp(moon_core, float3(0.82, 0.24, 0.14), limb * limb);
        rgb = lerp(rgb, moon_col, disc * 0.72);
    }

    // Astrolabe tarnished-brass ring around the Blood Moon.
    const float ring_d = abs(moon_r - 0.57);
    if (ring_d < 0.016) {
        const float rt = 1.0 - smoothstep(0.003, 0.015, ring_d);
        rgb = lerp(rgb, float3(0.68, 0.51, 0.28), rt * 0.45);
    }

    // 3. The Hunter's Mark (Caryll Rune) glowing inside the Paleblood Moon.
    const float d_rune = HuntersMarkSdf(mp / 0.44);
    if (d_rune < 0.20) {
        const float aura = exp(-max(0.0, d_rune) * 18.0);
        rgb = lerp(rgb, float3(0.78, 0.11, 0.12), aura * 0.65);
    }
    if (d_rune < 0.036) {
        const float core = 1.0 - smoothstep(0.008, 0.034, d_rune);
        const float hot = 1.0 - smoothstep(0.0, 0.014, d_rune);
        const float3 rune_col = lerp(float3(0.84, 0.64, 0.34), float3(0.96, 0.89, 0.74), hot);
        rgb = lerp(rgb, rune_col, core * 0.88);
    }

    // 4. Yharnam Gothic Cathedral Spires silhouette along the horizon.
    const float spire_wave = abs(frac(p.x * 1.35 + 0.25) - 0.5) * 2.0;
    const float spire_arch = abs(frac(p.x * 4.05) - 0.5) * 2.0;
    const float horizon_y = 0.62 + 0.18 * sqrt(saturate(spire_wave)) + 0.06 * spire_arch;
    const float spire_mask = smoothstep(horizon_y - 0.015, horizon_y + 0.015, p.y);
    const float rim_mask = 1.0 - smoothstep(0.0, 0.03, abs(p.y - horizon_y));
    rgb = lerp(rgb, float3(0.45, 0.08, 0.10), rim_mask * 0.35);
    rgb = lerp(rgb, float3(0.022, 0.015, 0.026), spire_mask * 0.92);

    // 5. Cinematic Victorian vignette for crisp HUD contrast.
    const float2 vig_uv = uv * (1.0 - uv);
    const float vig = saturate(vig_uv.x * vig_uv.y * 14.0);
    return rgb * lerp(0.35, 1.0, vig);
}

float4 PresentPs(PresentOut input) : SV_Target {
    if (source_size.w > 0.5) {
        return float4(RenderYharnamBackdrop(input.uv, source_size.w), 1.0);
    }
    const float4 sampled = source.Sample(source_sampler, input.uv);
    float3 rgb = sampled.rgb;
    if (source_size.z > 0.001) {
        rgb = ApplyRcas(input.uv, rgb, source_size.xy, source_size.z);
    }
    return float4(rgb * tint.rgb, sampled.a * tint.a);
}
