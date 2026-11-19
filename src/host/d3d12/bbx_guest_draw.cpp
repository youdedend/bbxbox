// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_guest_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bbx::d3d12 {
namespace {

// The register offsets, each one copied from host/render.cpp's
// draw_inputs_from_registers rather than from a manual: the SH file is
// 0x2C00-based, so `sh[0x48]` is SPI_SHADER_PGM_LO_VS.
constexpr std::uint32_t kVsProgram = 0x48;      // LO, HI, RSRC1, RSRC2
constexpr std::uint32_t kVsUser = 0x4c;         // sixteen user SGPRs
constexpr std::uint32_t kPsProgram = 0x08;
constexpr std::uint32_t kPsUser = 0x0c;
constexpr std::uint32_t kPrimitiveType = 0x242;  // VGT_PRIMITIVE_TYPE, in the uconfig file
constexpr std::uint32_t kTargetMask = 0x8e;      // CB_TARGET_MASK
constexpr std::uint32_t kShaderMask = 0x8f;      // CB_SHADER_MASK
constexpr std::uint32_t kColFormat = 0x1c5;      // SPI_SHADER_COL_FORMAT
constexpr std::uint32_t kColor = 0x318;          // CB_COLOR0_BASE .. 0x0f words each
constexpr std::uint32_t kColorStride = 0x0f;
constexpr std::uint32_t kBlendControl = 0x1e0;   // CB_BLEND0_CONTROL..7
constexpr std::uint32_t kBlendConst = 0x105;     // CB_BLEND_RED..ALPHA (4 floats)
constexpr std::uint32_t kBaseVertex = 0x102;     // VGT_INDX_OFFSET
constexpr std::uint32_t kStencilControl = 0x10b; // DB_STENCIL_CONTROL
constexpr std::uint32_t kStencilRefMask = 0x10c; // DB_STENCILREFMASK
constexpr std::uint32_t kVport = 0x10f;          // PA_CL_VPORT_XSCALE..ZOFFSET (6 floats)
constexpr std::uint32_t kPsInputEna = 0x1b3;     // SPI_PS_INPUT_ENA
constexpr std::uint32_t kPsInControl = 0x1b6;    // SPI_PS_INPUT_CNTL_0's address select
constexpr std::uint32_t kPsInputCntl = 0x191;    // SPI_PS_INPUT_CNTL_0..31
constexpr std::uint32_t kClipCntl = 0x204;       // PA_CL_CLIP_CNTL
constexpr std::uint32_t kSuScMode = 0x205;       // PA_SU_SC_MODE_CNTL
constexpr std::uint32_t kVsOutCntl = 0x207;      // PA_CL_VS_OUT_CNTL
constexpr std::uint32_t kDepthControl = 0x200;   // DB_DEPTH_CONTROL
constexpr std::uint32_t kZInfo = 0x10;           // DB_Z_INFO
constexpr std::uint32_t kStencilInfo = 0x11;     // DB_STENCIL_INFO
constexpr std::uint32_t kZReadBase = 0x12;       // DB_Z_READ_BASE
constexpr std::uint32_t kDepthSize = 0x16;       // DB_DEPTH_SIZE
constexpr std::uint32_t kScreenScissor = 0x0c;   // TL, BR
constexpr std::uint32_t kGenericScissor = 0x90;  // TL, BR
constexpr std::uint32_t kVportScissor = 0x94;    // TL, BR
constexpr std::uint32_t kScModeCntl0 = 0x292;    // PA_SC_MODE_CNTL_0

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The address a stage's LO/HI pair names: bits 8..39 in HI, the top eight bits of
// the 40-bit guest space in LO's low byte - the same rule the compute stage's
// COMPUTE_PGM_LO/HI pair is read with (platform/guest_compute.cpp).
std::uint64_t CodeAddress(std::uint32_t lo, std::uint32_t hi) {
    return (static_cast<std::uint64_t>(lo & 0xffu) << 40) | (static_cast<std::uint64_t>(hi) << 8);
}

void DecodeProgram(const std::uint32_t* words, const std::uint32_t* user, DrawProgram* out) {
    out->code_va = CodeAddress(words[0], words[1]);
    out->rsrc1 = words[2];
    out->rsrc2 = words[3];
    for (int i = 0; i < 16; ++i) out->user[i] = user[i];
}

// GCN CB_BLENDn_CONTROL factor decoding matching host/render.cpp's blend_factor(),
// with Xbox Guard G4 applied: constant-alpha and blend-factor PSOs are rejected
// by Xbox consoles, so factors 13/14/19/20 clamp safely to kOne/kZero.
DrawBlendFactor DecodeBlendFactor(std::uint32_t f, bool* clamped) {
    switch (f & 0x1fu) {
    case 0: return DrawBlendFactor::kZero;
    case 1: return DrawBlendFactor::kOne;
    case 2: return DrawBlendFactor::kSrcColor;
    case 3: return DrawBlendFactor::kInvSrcColor;
    case 4: return DrawBlendFactor::kSrcAlpha;
    case 5: return DrawBlendFactor::kInvSrcAlpha;
    case 6: return DrawBlendFactor::kDestAlpha;
    case 7: return DrawBlendFactor::kInvDestAlpha;
    case 8: return DrawBlendFactor::kDestColor;
    case 9: return DrawBlendFactor::kInvDestColor;
    case 10: return DrawBlendFactor::kSrcAlphaSat;
    case 13:  // CONSTANT_COLOR (Guard G4)
    case 19:  // CONSTANT_ALPHA (Guard G4)
        if (clamped) *clamped = true;
        return DrawBlendFactor::kOne;
    case 14:  // ONE_MINUS_CONSTANT_COLOR (Guard G4)
    case 20:  // ONE_MINUS_CONSTANT_ALPHA (Guard G4)
        if (clamped) *clamped = true;
        return DrawBlendFactor::kZero;
    case 15: return DrawBlendFactor::kSrc1Color;
    case 16: return DrawBlendFactor::kInvSrc1Color;
    case 17: return DrawBlendFactor::kSrc1Alpha;
    case 18: return DrawBlendFactor::kInvSrc1Alpha;
    default: return DrawBlendFactor::kOne;
    }
}

DrawBlendOp DecodeBlendOp(std::uint32_t op) {
    switch (op & 7u) {
    case 0: return DrawBlendOp::kAdd;
    case 1: return DrawBlendOp::kSubtract;
    case 2: return DrawBlendOp::kMin;
    case 3: return DrawBlendOp::kMax;
    case 4: return DrawBlendOp::kRevSubtract;
    default: return DrawBlendOp::kAdd;
    }
}

DrawCompareOp DecodeCompareOp(std::uint32_t op) {
    switch (op & 7u) {
    case 0: return DrawCompareOp::kNever;
    case 1: return DrawCompareOp::kLess;
    case 2: return DrawCompareOp::kEqual;
    case 3: return DrawCompareOp::kLessEqual;
    case 4: return DrawCompareOp::kGreater;
    case 5: return DrawCompareOp::kNotEqual;
    case 6: return DrawCompareOp::kGreaterEqual;
    case 7: return DrawCompareOp::kAlways;
    default: return DrawCompareOp::kAlways;
    }
}

DrawStencilOp DecodeStencilOp(std::uint32_t op) {
    switch (op & 0xfu) {
    case 0: return DrawStencilOp::kKeep;
    case 1: return DrawStencilOp::kZero;
    case 2: return DrawStencilOp::kKeep;     // ONES -> keep (matches render.cpp)
    case 3: return DrawStencilOp::kReplace;  // REPLACE_TEST
    case 4: return DrawStencilOp::kReplace;  // REPLACE_OP
    case 5: return DrawStencilOp::kIncrSat;
    case 6: return DrawStencilOp::kDecrSat;
    case 7: return DrawStencilOp::kInvert;
    case 8: return DrawStencilOp::kIncr;
    case 9: return DrawStencilOp::kDecr;
    default: return DrawStencilOp::kKeep;
    }
}

// The scissor: the generic and screen scissors intersected (a zero bottom-right
// corner on one leaves the other alone), everything when that is empty - exactly
// host/render.cpp's draw_scissor - and viewport scissor 0 when PA_SC_MODE_CNTL_0
// enables it (bit 1).
void DecodeScissor(const std::uint32_t* ctx, std::int32_t out[4]) {
    const auto tl = [](std::uint32_t v) {
        return std::make_pair(static_cast<std::int32_t>(v & 0x7fffu), static_cast<std::int32_t>((v >> 16) & 0x7fffu));
    };
    const auto [sx0, sy0] = tl(ctx[kScreenScissor]);
    const auto [sx1, sy1] = tl(ctx[kScreenScissor + 1]);
    const auto [gx0, gy0] = tl(ctx[kGenericScissor]);
    const auto [gx1, gy1] = tl(ctx[kGenericScissor + 1]);
    std::int32_t x0 = std::max(sx0, gx0), y0 = std::max(sy0, gy0);
    std::int32_t x1 = sx1 && gx1 ? std::min(sx1, gx1) : std::max(sx1, gx1);
    std::int32_t y1 = sy1 && gy1 ? std::min(sy1, gy1) : std::max(sy1, gy1);
    if (x1 <= x0 || y1 <= y0) {
        x0 = 0;
        y0 = 0;
        x1 = 16384;
        y1 = 16384;
    }
    if ((ctx[kScModeCntl0] & 2u) != 0) {
        const auto [vx0, vy0] = tl(ctx[kVportScissor]);
        const auto [vx1, vy1] = tl(ctx[kVportScissor + 1]);
        x0 = std::max(x0, vx0);
        y0 = std::max(y0, vy0);
        x1 = std::max(x0, std::min(x1, vx1));
        y1 = std::max(y0, std::min(y1, vy1));
    }
    out[0] = x0;
    out[1] = y0;
    out[2] = x1;
    out[3] = y1;
}

void DecodeViewport(const std::uint32_t* ctx, DrawViewport* out) {
    const float xs = BitsToFloat(ctx[kVport + 0]);
    const float xo = BitsToFloat(ctx[kVport + 1]);
    const float ys = BitsToFloat(ctx[kVport + 2]);
    const float yo = BitsToFloat(ctx[kVport + 3]);
    const float zs = BitsToFloat(ctx[kVport + 4]);
    const float zo = BitsToFloat(ctx[kVport + 5]);
    const float w = 2.0f * std::fabs(xs);
    const float h = 2.0f * std::fabs(ys);
    if (std::isfinite(w) && std::isfinite(h) && w > 0.0f && h > 0.0f) {
        out->x = xo - std::fabs(xs);
        out->y = yo - std::fabs(ys);
        out->width = w;
        out->height = h;
        if (zs != 0.0f || zo != 0.0f) {
            const float z0 = zo;
            const float z1 = zo + zs;
            out->min_depth = std::clamp(std::min(z0, z1), 0.0f, 1.0f);
            out->max_depth = std::clamp(std::max(z0, z1), 0.0f, 1.0f);
        } else {
            out->min_depth = 0.0f;
            out->max_depth = 1.0f;
        }
        out->valid = true;
    } else {
        *out = DrawViewport{};
    }
}

std::uint32_t Fnv1a32(std::uint32_t h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (i * 8)) & 0xffu;
        h *= 16777619u;
    }
    return h;
}

DrawIdentity Refuse(const DrawIdentity& identity, DrawVerdict verdict, std::string why) {
    DrawIdentity out = identity;
    out.verdict = verdict;
    out.why = std::move(why);
    return out;
}

}  // namespace

const char* DrawVerdictName(DrawVerdict verdict) {
    switch (verdict) {
    case DrawVerdict::kOk: return "ok";
    case DrawVerdict::kNoVertexProgram: return "no vertex program";
    case DrawVerdict::kNoPixelProgram: return "no pixel program";
    case DrawVerdict::kNoTarget: return "no render target is enabled";
    case DrawVerdict::kUnknownTargetFormat: return "a render target's format is not one this port maps";
    case DrawVerdict::kNoTargetExtent: return "a render target's pitch and format give no pixels";
    case DrawVerdict::kExtraStage: return "the draw enables a stage this port does not draw";
    }
    return "?";
}

DrawTopologyInfo DecodeTopology(std::uint32_t prim) {
    switch (prim) {
    case 1: return {DrawTopologyType::kPoint, DrawTopology::kPointList, false, true};
    case 2: return {DrawTopologyType::kLine, DrawTopology::kLineList, false, true};
    case 3: return {DrawTopologyType::kLine, DrawTopology::kLineStrip, false, true};
    case 4: return {DrawTopologyType::kTriangle, DrawTopology::kTriangleList, false, true};
    case 5: return {DrawTopologyType::kTriangle, DrawTopology::kTriangleList, false, true};  // fan -> list fallback
    case 6: return {DrawTopologyType::kTriangle, DrawTopology::kTriangleStrip, false, true};
    case 9: return {DrawTopologyType::kTriangle, DrawTopology::kTriangleList, false, true};  // patch control points
    case 17: return {DrawTopologyType::kTriangle, DrawTopology::kTriangleList, true, true};  // rect list
    default: return {};
    }
}

DrawIdentity DecodeDraw(const std::uint32_t* sh, const std::uint32_t* ctx, const std::uint32_t* uconfig,
                        std::uint32_t stages) {
    DrawIdentity out;
    if (sh == nullptr || ctx == nullptr || uconfig == nullptr) {
        out.verdict = DrawVerdict::kNoVertexProgram;
        out.why = "the draw has no register file";
        return out;
    }
    // A stage other than the two this file decodes, first: drawing it as a VS/PS
    // draw would be a frame built from the wrong program, and the alternative -
    // skipping it quietly - is a frame with a hole nobody can attribute.
    if ((stages & 0x7u) != 0) {
        return Refuse(out, DrawVerdict::kExtraStage,
                      "VGT_SHADER_STAGES_EN " + Hex(stages) + " enables the ES/GS/HS/LS bits, and this port draws VS/PS");
    }
    // The two stage programs first: a draw names both, and one without the other
    // is not a draw this port can build a pipeline for.
    DecodeProgram(sh + kVsProgram, sh + kVsUser, &out.vs);
    DecodeProgram(sh + kPsProgram, sh + kPsUser, &out.ps);
    if (!out.vs.present()) {
        return Refuse(out, DrawVerdict::kNoVertexProgram,
                      "SPI_SHADER_PGM_LO/HI_VS are " + Hex(sh[kVsProgram]) + "/" + Hex(sh[kVsProgram + 1]));
    }
    if (!out.ps.present()) {
        return Refuse(out, DrawVerdict::kNoPixelProgram,
                      "SPI_SHADER_PGM_LO/HI_PS are " + Hex(sh[kPsProgram]) + "/" + Hex(sh[kPsProgram + 1]));
    }
    out.ps_input_ena = ctx[kPsInputEna];
    out.ps_in_control = ctx[kPsInControl];
    out.vs_out_cntl = ctx[kVsOutCntl];
    out.target_mask = ctx[kTargetMask];
    out.shader_mask = ctx[kShaderMask];
    out.col_format = ctx[kColFormat];
    out.z_info = ctx[kZInfo];
    out.prim = uconfig[kPrimitiveType];
    out.base_vertex = static_cast<std::int32_t>(ctx[kBaseVertex]);
    for (std::uint32_t i = 0; i < 32; ++i) out.ps_input_cntl[i] = ctx[kPsInputCntl + i];
    for (int i = 0; i < 4; ++i) out.blend_const[i] = BitsToFloat(ctx[kBlendConst + static_cast<std::uint32_t>(i)]);
    DecodeScissor(ctx, out.scissor);
    DecodeViewport(ctx, &out.viewport);

    // Rasterizer state: PA_SU_SC_MODE_CNTL & PA_CL_CLIP_CNTL (render.cpp:3874-3880).
    const std::uint32_t su_mode = ctx[kSuScMode];
    out.raster.cull_front = (su_mode & 1u) != 0;
    out.raster.cull_back = (su_mode & 2u) != 0;
    out.raster.front_cw = (su_mode & 4u) != 0;
    out.raster.wireframe = ((su_mode & 0x10u) != 0) && (((su_mode >> 5) & 7u) == 1u);
    const std::uint32_t clip = ctx[kClipCntl];
    out.raster.depth_clip = (clip & ((1u << 16) | (1u << 26) | (1u << 27))) == 0;

    // Blend state per target slot 0..7 (CB_BLENDn_CONTROL, render.cpp:3992-4005).
    for (std::uint32_t t = 0; t < 8; ++t) {
        DrawBlendTarget& b = out.blend[t];
        const std::uint32_t bc = ctx[kBlendControl + t];
        const std::uint32_t target_nibble = (out.target_mask >> (4 * t)) & 0xfu;
        const std::uint32_t shader_nibble = (out.shader_mask >> (4 * t)) & 0xfu;
        b.write_mask = static_cast<std::uint8_t>(target_nibble & shader_nibble);
        b.enable = (bc & (1u << 30)) != 0;
        if (b.enable) {
            bool clamped = false;
            b.src_color = DecodeBlendFactor(bc & 0x1fu, &clamped);
            b.color_op = DecodeBlendOp((bc >> 5) & 7u);
            b.dst_color = DecodeBlendFactor((bc >> 8) & 0x1fu, &clamped);
            if ((bc & (1u << 29)) != 0) {  // SEPARATE_ALPHA_BLEND
                b.src_alpha = DecodeBlendFactor((bc >> 16) & 0x1fu, &clamped);
                b.alpha_op = DecodeBlendOp((bc >> 21) & 7u);
                b.dst_alpha = DecodeBlendFactor((bc >> 24) & 0x1fu, &clamped);
            } else {
                b.src_alpha = b.src_color;
                b.alpha_op = b.color_op;
                b.dst_alpha = b.dst_color;
            }
            b.clamped_constant_factor = clamped;
            if (clamped) out.clamped_constant_blend = true;
        }
    }

    // The targets: a target exists when its nibble of CB_TARGET_MASK is non-zero
    // and its CB_COLORn_BASE is set, which is the pair host/render.cpp skips on.
    for (std::uint32_t t = 0; t < 8; ++t) {
        const std::uint32_t* c = ctx + kColor + t * kColorStride;
        const std::uint32_t mask = (out.target_mask >> (4 * t)) & 0xfu;
        if (mask == 0 || c[0] == 0) continue;
        // CB_COLORn_INFO: FORMAT [6:2], NUMBER_TYPE [10:8], COMP_SWAP [12:11] -
        // CbInfo::from in host/render.cpp. The FORMAT field is the same
        // enumeration a T#'s dfmt uses, so this port's own table decodes it; the
        // NUMBER_TYPE field differs from a T#'s nfmt in exactly one value, sRGB
        // (6 in a colour target, 9 in a T#), and that is written out rather than
        // passed through - a target that silently lost its sRGB twin would be a
        // frame that is right in the wrong colour space.
        const std::uint32_t info = c[4];
        const std::uint32_t number_type = (info >> 8) & 7u;
        const std::uint32_t nfmt = number_type == 6u ? 9u : number_type;
        const FormatInfo format = FromGcnFormat((info >> 2) & 0x1fu, nfmt, (info >> 11) & 3u);
        if (!format.valid()) {
            return Refuse(out, DrawVerdict::kUnknownTargetFormat,
                          "target " + std::to_string(t) + " has CB_COLOR" + std::to_string(t) + "_INFO format " +
                              std::to_string((info >> 2) & 0x1fu) + " number type " + std::to_string((info >> 8) & 7u) +
                              ", which this port does not map");
        }
        const std::uint32_t pitch = ((c[1] & 0x7ffu) + 1u) * 8u;
        const std::uint32_t slice_px = ((c[2] & 0x3fffffu) + 1u) * 64u;
        const std::uint32_t padded_h = slice_px / pitch;
        if (padded_h == 0) {
            // A slice shorter than one row: the registers name a target with no
            // rows in it, which is a view of zero pixels - refused rather than
            // rounded up to one.
            return Refuse(out, DrawVerdict::kNoTargetExtent,
                          "target " + std::to_string(t) + " has a pitch of " + std::to_string(pitch) +
                              " bytes and a slice of " + std::to_string(slice_px) + " bytes");
        }
        DrawTarget& target = out.targets[out.target_count++];
        target.index = t;
        target.base = static_cast<std::uint64_t>(c[0]) << 8;
        target.format = static_cast<std::uint32_t>(format.format);
        target.srgb = format.srgb;
        // The width is the pitch over the format's bytes per pixel-block, and the
        // height is the padded slice's: the extent the view is made with.
        const std::uint32_t bytes = format.bytes_per_block != 0 ? format.bytes_per_block : 4u;
        target.width = pitch / bytes;
        target.height = padded_h;
        target.set_mask = mask;
    }
    if (out.target_count == 0 || out.shader_mask == 0) {
        return Refuse(out, DrawVerdict::kNoTarget,
                      "CB_TARGET_MASK " + Hex(out.target_mask) + " and CB_SHADER_MASK " + Hex(out.shader_mask) +
                          " select no target");
    }

    // The depth target: DB_Z_INFO's format field (its two low bits), with the
    // stencil plane when DB_STENCIL_INFO says one is there - the rule
    // host/render.cpp's db_format uses, decided by the port's own table.
    const std::uint32_t z_format = out.z_info & 3u;
    const bool stencil = (ctx[kStencilInfo] & 1u) != 0;
    const std::uint32_t dc = ctx[kDepthControl];
    const std::uint32_t sc = ctx[kStencilControl];
    const std::uint32_t sref = ctx[kStencilRefMask];
    out.depth_write = (dc & 2u) != 0;  // preserves existing contract for DB_DEPTH_CONTROL Z enable
    out.depth_stencil.depth_test = (dc & 2u) != 0;
    out.depth_stencil.depth_write_enable = (dc & 4u) != 0;
    out.depth_stencil.depth_func = DecodeCompareOp((dc >> 4) & 7u);
    out.depth_stencil.stencil_enable = (dc & 1u) != 0;
    out.depth_stencil.backface_enable = (dc & (1u << 7)) != 0;
    out.depth_stencil.stencil_ref = static_cast<std::uint8_t>(sref & 0xffu);
    out.depth_stencil.stencil_read_mask = static_cast<std::uint8_t>((sref >> 8) & 0xffu);
    out.depth_stencil.stencil_write_mask = static_cast<std::uint8_t>((sref >> 16) & 0xffu);
    out.depth_stencil.front.fail_op = DecodeStencilOp(sc & 0xfu);
    out.depth_stencil.front.pass_op = DecodeStencilOp((sc >> 4) & 0xfu);
    out.depth_stencil.front.depth_fail_op = DecodeStencilOp((sc >> 8) & 0xfu);
    out.depth_stencil.front.func = DecodeCompareOp((dc >> 8) & 7u);
    out.depth_stencil.back = out.depth_stencil.front;
    if (out.depth_stencil.backface_enable) {
        out.depth_stencil.back.fail_op = DecodeStencilOp((sc >> 12) & 0xfu);
        out.depth_stencil.back.pass_op = DecodeStencilOp((sc >> 16) & 0xfu);
        out.depth_stencil.back.depth_fail_op = DecodeStencilOp((sc >> 20) & 0xfu);
        out.depth_stencil.back.func = DecodeCompareOp((dc >> 20) & 7u);
    }
    out.depth_stencil.depth_base = static_cast<std::uint64_t>(ctx[kZReadBase]) << 8;
    if (ctx[kDepthSize] != 0) {
        out.depth_stencil.depth_width = ((ctx[kDepthSize] & 0x7ffu) + 1u) * 8u;
        out.depth_stencil.depth_height = (((ctx[kDepthSize] >> 11) & 0x7ffu) + 1u) * 8u;
    }
    if (z_format != 0 || stencil) {
        out.depth_format = static_cast<std::uint32_t>(FromDepthFormat(z_format, stencil).format);
    }
    out.verdict = DrawVerdict::kOk;
    return out;
}

PsoKey GraphicsPsoKey(std::uint64_t vs_key, std::uint64_t ps_key, const DrawIdentity& identity) {
    PsoKeyInput in{};
    in.vs = vs_key;
    in.ps = ps_key;
    // Attachment N is CB slot N, gaps included (matches render.cpp:3963).
    std::uint8_t num_rtvs = 0;
    for (std::uint32_t i = 0; i < identity.target_count; ++i) {
        const DrawTarget& t = identity.targets[i];
        if (t.index < 8) {
            in.rtv_formats[t.index] = t.format;
            if (static_cast<std::uint8_t>(t.index + 1) > num_rtvs) {
                num_rtvs = static_cast<std::uint8_t>(t.index + 1);
            }
        }
    }
    in.rtv_count = num_rtvs;
    in.dsv_format = identity.depth_format;
    in.sample_count = 1;

    std::uint32_t blend_hash = 2166136261u;
    for (std::uint8_t t = 0; t < num_rtvs; ++t) {
        const DrawBlendTarget& b = identity.blend[t];
        const std::uint32_t packed = (b.enable ? 1u : 0u) | (static_cast<std::uint32_t>(b.src_color) << 1) |
                                     (static_cast<std::uint32_t>(b.dst_color) << 5) |
                                     (static_cast<std::uint32_t>(b.color_op) << 9) |
                                     (static_cast<std::uint32_t>(b.src_alpha) << 12) |
                                     (static_cast<std::uint32_t>(b.dst_alpha) << 16) |
                                     (static_cast<std::uint32_t>(b.alpha_op) << 20) |
                                     (static_cast<std::uint32_t>(b.write_mask) << 24);
        blend_hash = Fnv1a32(blend_hash, packed);
    }
    in.blend = blend_hash;

    const DrawDepthStencil& ds = identity.depth_stencil;
    std::uint32_t ds_hash = 2166136261u;
    ds_hash = Fnv1a32(ds_hash, (ds.depth_test ? 1u : 0u) | (ds.depth_write_enable ? 2u : 0u) |
                                   (static_cast<std::uint32_t>(ds.depth_func) << 2) | (ds.stencil_enable ? 32u : 0u) |
                                   (ds.backface_enable ? 64u : 0u) |
                                   (static_cast<std::uint32_t>(ds.stencil_read_mask) << 8) |
                                   (static_cast<std::uint32_t>(ds.stencil_write_mask) << 16));
    ds_hash = Fnv1a32(ds_hash, static_cast<std::uint32_t>(ds.front.fail_op) |
                                   (static_cast<std::uint32_t>(ds.front.depth_fail_op) << 4) |
                                   (static_cast<std::uint32_t>(ds.front.pass_op) << 8) |
                                   (static_cast<std::uint32_t>(ds.front.func) << 12) |
                                   (static_cast<std::uint32_t>(ds.back.fail_op) << 16) |
                                   (static_cast<std::uint32_t>(ds.back.depth_fail_op) << 20) |
                                   (static_cast<std::uint32_t>(ds.back.pass_op) << 24) |
                                   (static_cast<std::uint32_t>(ds.back.func) << 28));
    in.depth_stencil = ds_hash;

    const DrawRasterizer& r = identity.raster;
    in.rasterizer = (r.cull_front ? 1u : 0u) | (r.cull_back ? 2u : 0u) | (r.front_cw ? 4u : 0u) |
                    (r.wireframe ? 8u : 0u) | (r.depth_clip ? 16u : 0u);
    in.root_signature = 1;  // rs-1.0 guest graphics
    in.bindless = 0;        // descriptor tables
    in.feature_level = 0x1100;
    in.pso_library = 0;     // console does not use a pipeline library
    in.topology = static_cast<std::uint32_t>(DecodeTopology(identity.prim).type);
    return MakePsoKey(in);
}

std::uint64_t DrawFingerprint(const DrawIdentity& identity) {
    // FNV-1a over the fields that select the pipeline and the targets it writes:
    // the two programs, the masks and formats, the depth format, the scissor. It
    // is a log-deduplication key, not an identity anything is built from.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix(static_cast<std::uint64_t>(identity.verdict));
    mix(identity.vs.code_va);
    mix(identity.ps.code_va);
    mix(identity.vs.rsrc1);
    mix(identity.ps.rsrc1);
    mix(identity.ps_input_ena);
    mix(identity.vs_out_cntl);
    mix(identity.depth_format);
    mix(identity.target_count);
    for (std::uint32_t i = 0; i < identity.target_count; ++i) {
        mix(identity.targets[i].base);
        mix(identity.targets[i].format);
        mix(identity.targets[i].width);
        mix(identity.targets[i].height);
    }
    for (int i = 0; i < 4; ++i) mix(static_cast<std::uint64_t>(identity.scissor[i]));
    return hash;
}

std::string DrawIdentityLine(const DrawIdentity& identity) {
    if (identity.verdict != DrawVerdict::kOk) {
        return std::string("draw refused (") + DrawVerdictName(identity.verdict) + "): " + identity.why;
    }
    std::string line = "vs " + Hex(identity.vs.code_va) + " ps " + Hex(identity.ps.code_va) + ", " +
                       std::to_string(identity.target_count) + " target(s):";
    for (std::uint32_t i = 0; i < identity.target_count; ++i) {
        const DrawTarget& t = identity.targets[i];
        line += " [" + std::to_string(t.index) + "] " + Hex(t.base) + " " + std::to_string(t.width) + "x" +
                std::to_string(t.height) + " format " + std::to_string(t.format) + (t.srgb ? " srgb" : "");
    }
    line += ", depth format " + std::to_string(identity.depth_format) + (identity.depth_write ? " (written)" : "");
    line += ", scissor " + std::to_string(identity.scissor[0]) + "," + std::to_string(identity.scissor[1]) + ".." +
            std::to_string(identity.scissor[2]) + "," + std::to_string(identity.scissor[3]);
    return line;
}

}  // namespace bbx::d3d12
