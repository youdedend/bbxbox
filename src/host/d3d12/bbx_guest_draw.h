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
    bool depth_write = false;         // DB_DEPTH_CONTROL's Z_WRITE
    std::uint32_t target_count = 0;
    DrawTarget targets[8] = {};
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

// A stable fingerprint of the identity, for "log this once": two draws that would
// build the same pipeline share it, and a draw whose target moved does not.
[[nodiscard]] std::uint64_t DrawFingerprint(const DrawIdentity& identity);

// "vs 0x1000 ps 0x2000, 1 target: 0 framebuffer 1920x1080 (format 28), depth
// format 0, scissor 0,0..1920,1080": the one line the console logs the first time
// it sees a draw, and the same line in the tests.
[[nodiscard]] std::string DrawIdentityLine(const DrawIdentity& identity);

}  // namespace bbx::d3d12
