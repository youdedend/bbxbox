// SPDX-License-Identifier: GPL-3.0-or-later
// What a draw is, read from the guest's own registers (Slice 5B-2b, the graphics half).
//
// `host_gpu_draw` was a counter: every draw was counted and dropped. The first
// thing it needs is not a device call but an answer to "which pipeline is this?",
// and the guest already states it - the two stage programs' addresses and their
// register blocks, the render targets, the formats they are written in, the
// scissor. So this file decodes exactly those, at the register offsets
// `host/render.cpp`'s `draw_inputs_from_registers` reads them at (the SH file is
// 0x2C00-based, the context file 0xA000-based), and refuses what a pipeline
// cannot be built for.
//
// The rule this file is written under: no field is guessed. Every value comes
// from a register the guest wrote, every offset is one upstream reads, and every
// thing that cannot be built is a named refusal instead of a default. That is
// also why the tessellated and geometry-shader cases are not "detected" here: the
// stage-enable register's bits are not re-derived from a comment, so a draw that
// carries one is caught by the file that knows the stages (the console's own
// session) rather than by a guess here.
//
// Device-free: `tests/guest_draw_test.cpp` pins every verdict on the build
// machine, where a wrong offset costs a second instead of a console round trip.
#pragma once

#include <cstdint>
#include <string>

#include "src/host/d3d12/bbx_cache.h"
#include "src/host/d3d12/bbx_format.h"

namespace bbx::d3d12 {

// One stage's program as the draw names it: the address pair the command
// processor would fetch from (SPI_SHADER_PGM_LO/HI), the register block that
// states the stage's facts (RSRC1/RSRC2) and the sixteen user SGPRs the draw
// passes. A stage whose address is zero is not present.
struct DrawProgram {
    std::uint64_t code_va = 0;  // (lo & 0xff) << 40 | hi << 8, as the compute stage's
    std::uint32_t rsrc1 = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t user[16] = {};
    [[nodiscard]] bool present() const { return code_va != 0; }
};

// One render target, as CB_COLORn states it: where it is, what format it is
// written in, and the extent its PITCH and SLICE give (the same two words
// host/render.cpp reads: pitch = (PITCH[10:0] + 1) * 8 bytes, a slice is
// (SLICE[21:0] + 1) * 64 pixels, and the height is the slice over the pitch).
struct DrawTarget {
    std::uint32_t index = 0;     // 0..7
    std::uint64_t base = 0;      // guest VA of the target's slice
    std::uint32_t format = 0;    // DxgiFormat, as bbx_format spells it
    bool srgb = false;
    std::uint32_t width = 0;     // pitch / the format's bytes per pixel-block
    std::uint32_t height = 0;
    std::uint32_t set_mask = 0;  // the nibble of CB_TARGET_MASK that named it
};

// Portable fixed-function enums decoded from GCN context registers. Kept
// Windows-free so every mapping rule (including Xbox Guard G4 constant-alpha
// blend clamping) is pinned by host unit tests.
enum class DrawBlendFactor : std::uint8_t {
    kZero = 0,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestAlpha,
    kInvDestAlpha,
    kDestColor,
    kInvDestColor,
    kSrcAlphaSat,
    kSrc1Color,
    kInvSrc1Color,
    kSrc1Alpha,
    kInvSrc1Alpha,
};

enum class DrawBlendOp : std::uint8_t {
    kAdd = 0,
    kSubtract,
    kRevSubtract,
    kMin,
    kMax,
};

enum class DrawCompareOp : std::uint8_t {
    kNever = 0,
    kLess,
    kEqual,
    kLessEqual,
    kGreater,
    kNotEqual,
    kGreaterEqual,
    kAlways,
};

enum class DrawStencilOp : std::uint8_t {
    kKeep = 0,
    kZero,
    kReplace,
    kIncrSat,
    kDecrSat,
    kInvert,
    kIncr,
    kDecr,
};

enum class DrawTopologyType : std::uint8_t {
    kUndefined = 0,
    kPoint,
    kLine,
    kTriangle,
};

enum class DrawTopology : std::uint8_t {
    kUndefined = 0,
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
};

struct DrawTopologyInfo {
    DrawTopologyType type = DrawTopologyType::kUndefined;
    DrawTopology topology = DrawTopology::kUndefined;
    bool rect_list = false;
    bool supported = false;
};

struct DrawBlendTarget {
    bool enable = false;
    DrawBlendFactor src_color = DrawBlendFactor::kOne;
    DrawBlendFactor dst_color = DrawBlendFactor::kZero;
    DrawBlendOp color_op = DrawBlendOp::kAdd;
    DrawBlendFactor src_alpha = DrawBlendFactor::kOne;
    DrawBlendFactor dst_alpha = DrawBlendFactor::kZero;
    DrawBlendOp alpha_op = DrawBlendOp::kAdd;
    std::uint8_t write_mask = 0x0f;
    // Guard G4: Xbox consoles reject constant-alpha / blend-factor PSOs, so
    // GCN factors 13/14/19/20 clamp safely to kOne/kZero and raise this flag.
    bool clamped_constant_factor = false;
};

struct DrawStencilFace {
    DrawStencilOp fail_op = DrawStencilOp::kKeep;
    DrawStencilOp depth_fail_op = DrawStencilOp::kKeep;
    DrawStencilOp pass_op = DrawStencilOp::kKeep;
    DrawCompareOp func = DrawCompareOp::kAlways;
};

struct DrawDepthStencil {
    bool depth_test = false;          // DB_DEPTH_CONTROL bit 1 (Z_ENABLE)
    bool depth_write_enable = false;  // DB_DEPTH_CONTROL bit 2 (Z_WRITE_ENABLE)
    DrawCompareOp depth_func = DrawCompareOp::kLess;
    bool stencil_enable = false;      // DB_DEPTH_CONTROL bit 0
    bool backface_enable = false;     // DB_DEPTH_CONTROL bit 7
    std::uint8_t stencil_ref = 0;
    std::uint8_t stencil_read_mask = 0xff;
    std::uint8_t stencil_write_mask = 0xff;
    DrawStencilFace front;
    DrawStencilFace back;
    std::uint64_t depth_base = 0;     // DB_Z_READ_BASE << 8
    std::uint32_t depth_width = 0;    // decoded from DB_DEPTH_SIZE when set
    std::uint32_t depth_height = 0;
};

struct DrawRasterizer {
    bool cull_front = false;
    bool cull_back = false;
    bool front_cw = false;
    bool wireframe = false;
    bool depth_clip = true;
};

struct DrawViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
    bool valid = false;
};

enum class DrawVerdict : std::uint8_t {
    kOk = 0,
    kNoVertexProgram,    // SPI_SHADER_PGM_LO/HI_VS are zero
    kNoPixelProgram,     // ..._PS are zero
    kNoTarget,           // CB_TARGET_MASK or CB_SHADER_MASK selects nothing
    kUnknownTargetFormat, // a target's CB_COLORn_INFO FORMAT is not one this port maps
    kNoTargetExtent,     // a target's slice is shorter than one row: no pixels in it
    kExtraStage,         // VGT_SHADER_STAGES_EN names a stage other than VS/PS
};

[[nodiscard]] const char* DrawVerdictName(DrawVerdict verdict);

// The pipeline-selecting state, decoded. `targets` is `target_count` entries in
// target order; `depth_format` is 0 when the draw has no depth target.
struct DrawIdentity {
    DrawVerdict verdict = DrawVerdict::kNoVertexProgram;
    std::string why;  // the refusal, in words: which register said what
    DrawProgram vs;
    DrawProgram ps;
    std::uint32_t ps_input_ena = 0;   // SPI_PS_INPUT_ENA: the PS's input mask
    std::uint32_t ps_in_control = 0;  // SPI_PS_INPUT_CNTL_0's address select
    std::uint32_t ps_input_cntl[32] = {};  // SPI_PS_INPUT_CNTL_0..31
    std::uint32_t vs_out_cntl = 0;    // PA_CL_VS_OUT_CNTL: the VS's output mask
    std::uint32_t target_mask = 0;    // CB_TARGET_MASK
    std::uint32_t shader_mask = 0;    // CB_SHADER_MASK
    std::uint32_t col_format = 0;     // SPI_SHADER_COL_FORMAT
    std::uint32_t z_info = 0;         // DB_Z_INFO
    std::uint32_t depth_format = 0;   // DxgiFormat, 0 when there is no depth target
    bool depth_write = false;         // DB_DEPTH_CONTROL Z enable/write flag
    std::uint32_t target_count = 0;
    DrawTarget targets[8] = {};
    // Full fixed-function state decoded from the same context file for D3D12 PSO creation.
    DrawBlendTarget blend[8] = {};
    float blend_const[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool clamped_constant_blend = false;
    DrawDepthStencil depth_stencil;
    DrawRasterizer raster;
    DrawViewport viewport;
    std::int32_t base_vertex = 0;     // VGT_INDX_OFFSET (ctx[0x102])
    // The rectangle the draw writes: the generic scissor intersected with the
    // screen scissor (zero bottom-right on one leaves the other), and the viewport
    // scissor when PA_SC_MODE_CNTL_0 enables it. `x1`/`y1` are exclusive.
    std::int32_t scissor[4] = {0, 0, 0, 0};
    std::uint32_t prim = 0;  // VGT_PRIMITIVE_TYPE
};

// `sh` is the draw's SH register file (0x2C00-based, 0x400 entries), `ctx` its
// context file (0xA000-based, 0x1000 entries), `uconfig` the user-config file
// (0xC000-based). Offsets are the ones `host/render.cpp`'s
// `draw_inputs_from_registers` uses; nothing else is read.
// `stages` is VGT_SHADER_STAGES_EN as the caller knows it (0 when it does not):
// a draw that enables a stage other than the vertex and pixel programs this file
// decodes - tessellation's LS/HS or a geometry shader - is refused by name rather
// than drawn as if it were a VS/PS draw. `host/render.cpp` reads the same word's
// low three bits for the same question ("a stage other than VS/PS"); the port does
// not decode the stage list on its own, because which stages a draw has is the
// session's answer and guessing it here would be a silent fallthrough.
[[nodiscard]] DrawIdentity DecodeDraw(const std::uint32_t* sh, const std::uint32_t* ctx,
                                     const std::uint32_t* uconfig, std::uint32_t stages = 0);

// Maps VGT_PRIMITIVE_TYPE into D3D12-compatible topology class and primitive topology.
[[nodiscard]] DrawTopologyInfo DecodeTopology(std::uint32_t prim);

// Builds the canonical 128-bit PSO LRU key for a VS/PS draw + fixed-function state.
// Always uses pso_library = 0 (console has no ID3D12PipelineLibrary).
[[nodiscard]] PsoKey GraphicsPsoKey(std::uint64_t vs_key, std::uint64_t ps_key,
                                    const DrawIdentity& identity);

// A stable fingerprint of the identity, for "log this once": two draws that would
// build the same pipeline share it, and a draw whose target moved does not.
[[nodiscard]] std::uint64_t DrawFingerprint(const DrawIdentity& identity);

// "vs 0x1000 ps 0x2000, 1 target: 0 framebuffer 1920x1080 (format 28), depth
// format 0, scissor 0,0..1920,1080": the one line the console logs the first time
// it sees a draw, and the same line in the tests.
[[nodiscard]] std::string DrawIdentityLine(const DrawIdentity& identity);

// Portable input for GX ClearRenderTargetView / ClearDepthStencilView tokens
// (`GpuClearView` in host/gpu.h), kept header-independent so host tests can
// verify format/extent/plane resolution without pulling in bbhost headers.
struct ClearViewInput {
    bool depth = false;
    std::uint32_t extent = 0;       // (width << 16) | height, 0 when unknown
    std::uint32_t color[5] = {};    // CB_COLOR BASE, PITCH, SLICE, VIEW, INFO
    float rgba[4] = {};
    std::uint32_t z_info = 0;       // DB_Z_INFO
    std::uint32_t stencil_info = 0; // DB_STENCIL_INFO
    std::uint32_t z_read_base = 0;  // DB_Z_READ_BASE
    std::uint32_t depth_size = 0;   // DB_DEPTH_SIZE
    std::uint32_t flags = 0;        // bit 0 = depth, bit 1 = stencil (0 = default both)
    float depth_value = 1.0f;
    std::uint32_t stencil = 0;
};

struct DecodedClearView {
    bool ok = false;
    const char* why = "invalid view";
    bool depth = false;
    std::uint64_t base = 0;
    std::uint32_t format = 0;
    bool srgb = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float rgba[4] = {};
    bool clear_depth = false;
    bool clear_stencil = false;
    float depth_value = 1.0f;
    std::uint8_t stencil_value = 0;
};

[[nodiscard]] DecodedClearView DecodeClearView(const ClearViewInput& in);

}  // namespace bbx::d3d12
