// SPDX-License-Identifier: GPL-3.0-or-later
// tests/guest_draw_test.cpp -- what a draw is, from the guest's own registers.
//
// Slice 5B-2b, the graphics half's first step. `host_gpu_draw` counted every draw
// and dropped it; before it can build a pipeline it has to answer "which pipeline
// is this, and can this port build it at all", and the answer is in registers the
// guest wrote. Every offset here is one host/render.cpp's
// draw_inputs_from_registers reads, so the test is also the record of that
// agreement: the SH file is 0x2C00-based, the context file 0xA000-based, the
// user-config file 0xC000-based.
//
// The refusals are cases for the same reason the descriptor refusals are: a draw
// built for the wrong target is a frame that is wrong in no log, and a draw
// silently skipped is a frame that is wrong in no log either.

#include <cstdint>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_format.h"
#include "host/d3d12/bbx_guest_draw.h"

using namespace bbx::d3d12;

namespace {

constexpr std::uint32_t kSh = 0x400;      // the SH file's entries
constexpr std::uint32_t kCtx = 0x1000;    // the context file's
constexpr std::uint32_t kUconfig = 0x400; // the user-config file's

// One draw's register files, with the draw a title makes most of the time: a
// vertex program at 0x1000, a pixel program at 0x2000, one R8G8B8A8_UNORM target
// of 1920x1080 at 0x40000000 and a 32-bit float depth target written.
struct Registers {
    std::vector<std::uint32_t> sh = std::vector<std::uint32_t>(kSh, 0);
    std::vector<std::uint32_t> ctx = std::vector<std::uint32_t>(kCtx, 0);
    std::vector<std::uint32_t> uconfig = std::vector<std::uint32_t>(kUconfig, 0);

    void Program(std::uint32_t at, std::uint64_t va, std::uint32_t rsrc1, std::uint32_t rsrc2) {
        sh[at + 0] = static_cast<std::uint32_t>((va >> 40) & 0xffu);  // PGM_LO's low byte carries bits 39:32
        sh[at + 1] = static_cast<std::uint32_t>(va >> 8);             // PGM_HI
        sh[at + 2] = rsrc1;
        sh[at + 3] = rsrc2;
    }
    void User(std::uint32_t at, std::uint32_t first) {
        for (std::uint32_t i = 0; i < 16; ++i) sh[at + i] = first + i;
    }
    // CB_COLORn_BASE, PITCH, SLICE, VIEW, INFO.
    void Color(std::uint32_t t, std::uint64_t base, std::uint32_t pitch_bytes, std::uint32_t height_px,
               std::uint32_t format, std::uint32_t number_type, std::uint32_t comp_swap) {
        std::uint32_t* c = ctx.data() + 0x318 + t * 0x0f;
        c[0] = static_cast<std::uint32_t>(base >> 8);
        c[1] = pitch_bytes / 8 - 1;
        // CB_COLORn_SLICE counts 64-byte units of the padded slice, and the padded
        // height is that slice over the row's pitch - host/render.cpp's two lines.
        c[2] = pitch_bytes * height_px / 64 - 1;
        c[3] = 0;
        c[4] = ((format & 0x1fu) << 2) | ((number_type & 7u) << 8) | ((comp_swap & 3u) << 11);
    }
    void Scissor(std::uint32_t right, std::uint32_t bottom) {
        ctx[0x0c] = 0;
        ctx[0x0d] = right | (bottom << 16);
    }
};

Registers OneDraw() {
    Registers r;
    r.Program(0x48, 0x1000, 0x0000c1c0u, 0x0000000au);  // VS
    r.Program(0x08, 0x2000, 0x0000c3c0u, 0x0000000bu);  // PS
    r.User(0x4c, 0x1100);
    r.User(0x0c, 0x2200);
    r.Color(0, 0x4000'0000ull, 7680, 1080, /*format=*/10, /*unorm=*/0, /*swap=*/0);
    r.ctx[0x8e] = 0x1;  // CB_TARGET_MASK: target 0, one channel
    r.ctx[0x8f] = 0xf;  // CB_SHADER_MASK: all four components exported
    r.ctx[0x1c5] = 0;
    r.ctx[0x10] = 3;    // DB_Z_INFO: Z_32
    r.ctx[0x11] = 0;    // DB_STENCIL_INFO: no stencil plane
    r.ctx[0x200] = 2;   // DB_DEPTH_CONTROL: Z_WRITE
    r.Scissor(1920, 1080);
    r.uconfig[0x242] = 4;  // VGT_PRIMITIVE_TYPE: triangles
    return r;
}

}  // namespace

TEST(GuestDraw_DecodesBothProgramsFromTheRegisterFile) {
    const Registers r = OneDraw();
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kOk);
    CHECK_EQ(id.vs.code_va, 0x1000ull);
    CHECK_EQ(id.ps.code_va, 0x2000ull);
    CHECK_EQ(id.vs.rsrc1, 0x0000c1c0u);
    CHECK_EQ(id.ps.rsrc2, 0x0000000bu);
    // The user data is the draw's, not the previous draw's: sixteen dwords each.
    CHECK_EQ(id.vs.user[0], 0x1100u);
    CHECK_EQ(id.vs.user[15], 0x110fu);
    CHECK_EQ(id.ps.user[0], 0x2200u);
    CHECK_EQ(id.prim, 4u);
}

TEST(GuestDraw_TheProgramAddressIsTheComputeStageFormOfLoAndHi) {
    // A guest VA above 4 GiB keeps its top eight bits in PGM_LO's low byte, which
    // is the rule the compute stage's COMPUTE_PGM_LO/HI pair is read with.
    Registers r = OneDraw();
    r.Program(0x48, 0x01'2345'6800ull, 0, 0);
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kOk);
    CHECK_EQ(id.vs.code_va, 0x01'2345'6800ull);
}

TEST(GuestDraw_DecodesTheTargetsExtentFormatAndAddress) {
    const Registers r = OneDraw();
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK_EQ(id.target_count, 1u);
    const DrawTarget& t = id.targets[0];
    CHECK_EQ(t.index, 0u);
    CHECK_EQ(t.base, 0x4000'0000ull);
    CHECK_EQ(t.width, 1920u);   // 7680 bytes of row over four bytes a pixel
    CHECK_EQ(t.height, 1080u);
    CHECK_EQ(t.format, static_cast<std::uint32_t>(DxgiFormat::kR8G8B8A8Unorm));
    CHECK(!t.srgb);
    CHECK_EQ(t.set_mask, 0x1u);
}

TEST(GuestDraw_SrgbAndTheScissorComeThroughToo) {
    Registers r = OneDraw();
    r.Color(0, 0x4000'0000ull, 7680, 1080, 10, /*number_type=*/6, 0);  // 6 = srgb in a CB target
    r.Scissor(1280, 720);
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kOk);
    CHECK(id.targets[0].srgb);
    CHECK_EQ(id.scissor[0], 0);
    CHECK_EQ(id.scissor[1], 0);
    CHECK_EQ(id.scissor[2], 1280);
    CHECK_EQ(id.scissor[3], 720);
}

TEST(GuestDraw_TheDepthTargetIsDecidedByItsTwoBitsAndTheStencilPlane) {
    Registers r = OneDraw();
    DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK_EQ(id.depth_format, static_cast<std::uint32_t>(DxgiFormat::kD32Float));
    CHECK(id.depth_write);
    r.ctx[0x10] = 1;  // Z_16
    id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK_EQ(id.depth_format, static_cast<std::uint32_t>(DxgiFormat::kD16Unorm));
    r.ctx[0x11] = 1;  // and a stencil plane
    id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK_EQ(id.depth_format, static_cast<std::uint32_t>(DxgiFormat::kD24UnormS8Uint));
    r.ctx[0x10] = 0;  // no Z format, stencil plane only
    id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK_EQ(id.depth_format, static_cast<std::uint32_t>(DxgiFormat::kD32FloatS8X24Uint));
}

TEST(GuestDraw_ADrawWithoutItsDepthTargetSaysSoWithoutRefusing) {
    Registers r = OneDraw();
    r.ctx[0x10] = 0;  // no Z format
    r.ctx[0x11] = 0;  // and no stencil plane
    r.ctx[0x200] = 0;
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kOk);  // a colour-only draw is a draw
    CHECK_EQ(id.depth_format, 0u);
    CHECK(!id.depth_write);
}

TEST(GuestDraw_ADrawWithoutAVertexProgramIsRefusedByName) {
    Registers r = OneDraw();
    r.Program(0x48, 0, 0, 0);
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kNoVertexProgram);
    CHECK(DrawIdentityLine(id).find("no vertex program") != std::string::npos);
    CHECK(DrawIdentityLine(id).find("SPI_SHADER_PGM_LO/HI_VS") != std::string::npos);
}

TEST(GuestDraw_ADrawWithoutAPixelProgramIsRefusedByName) {
    Registers r = OneDraw();
    r.Program(0x08, 0, 0, 0);
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kNoPixelProgram);
    CHECK(DrawIdentityLine(id).find("no pixel program") != std::string::npos);
}

TEST(GuestDraw_ADrawThatEnablesNoTargetIsRefused) {
    Registers r = OneDraw();
    r.ctx[0x8e] = 0;
    CHECK(DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data()).verdict == DrawVerdict::kNoTarget);
    // A mask that names target 0 with a base of zero is the same thing: there is
    // nothing to render into, and the console must not invent a target.
    r = OneDraw();
    r.ctx[0x318] = 0;
    CHECK(DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data()).verdict == DrawVerdict::kNoTarget);
    // So is a shader mask of zero: nothing is exported.
    r = OneDraw();
    r.ctx[0x8f] = 0;
    CHECK(DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data()).verdict == DrawVerdict::kNoTarget);
}

TEST(GuestDraw_ATargetFormatThisPortDoesNotMapIsRefusedWithItsFields) {
    Registers r = OneDraw();
    r.Color(0, 0x4000'0000ull, 7680, 1080, /*format=*/31, 0, 0);
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kUnknownTargetFormat);
    const std::string line = DrawIdentityLine(id);
    CHECK(line.find("target 0") != std::string::npos);
    CHECK(line.find("format 31") != std::string::npos);
}

TEST(GuestDraw_APitchTooNarrowForTheFormatIsRefusedNotRoundedUp) {
    // CB_COLORn_SLICE counts 64-byte units of the slice, and the padded height is
    // that slice over the row's pitch. A slice of one unit with a 16 KiB row is
    // zero rows: a target with no pixels in it, refused rather than rounded up.
    Registers r = OneDraw();
    r.Color(0, 0x4000'0000ull, 7680, 1080, /*format=*/10, 0, 0);
    r.ctx[0x318 + 1] = 2047;  // CB_COLOR0_PITCH: (2047 + 1) * 8 = a 16 KiB row
    r.ctx[0x318 + 2] = 0;     // CB_COLOR0_SLICE: (0 + 1) * 64 = one 64-byte unit
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kNoTargetExtent);
    CHECK(DrawIdentityLine(id).find("a slice of", std::string::npos) == std::string::npos);
    CHECK(DrawIdentityLine(id).find("slice of 64 bytes") != std::string::npos);
}

TEST(GuestDraw_TheSecondTargetIsADifferentEntryNotADifferentDraw) {
    Registers r = OneDraw();
    r.Color(1, 0x5000'0000ull, 3840, 1080, 10, 0, 0);
    r.ctx[0x8e] = 0x11;  // targets 0 and 1
    const DrawIdentity id = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    CHECK(id.verdict == DrawVerdict::kOk);
    CHECK_EQ(id.target_count, 2u);
    CHECK_EQ(id.targets[1].index, 1u);
    CHECK_EQ(id.targets[1].base, 0x5000'0000ull);
    CHECK_EQ(id.targets[1].width, 960u);
    CHECK(DrawIdentityLine(id).find("[1] 0x50000000 960x1080") != std::string::npos);
}

TEST(GuestDraw_TheLineNamesTheProgramsTheTargetsAndTheScissor) {
    const Registers r = OneDraw();
    const std::string line = DrawIdentityLine(DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data()));
    CHECK(line.rfind("vs 0x1000 ps 0x2000, 1 target(s):", 0) == 0);
    CHECK(line.find("[0] 0x40000000 1920x1080 format") != std::string::npos);
    CHECK(line.find("scissor 0,0..1920,1080") != std::string::npos);
}

TEST(GuestDraw_TheFingerprintIsTheSameDrawTwiceAndNotADifferentOne) {
    const Registers r = OneDraw();
    const DrawIdentity a = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    Registers same = OneDraw();
    CHECK_EQ(DrawFingerprint(a), DrawFingerprint(DecodeDraw(same.sh.data(), same.ctx.data(), same.uconfig.data())));
    // The same programs into another target is another pipeline configuration, and
    // the log has to say so once rather than once per target.
    Registers moved = OneDraw();
    moved.Color(0, 0x5000'0000ull, 7680, 1080, 10, 0, 0);
    CHECK(DrawFingerprint(a) != DrawFingerprint(DecodeDraw(moved.sh.data(), moved.ctx.data(), moved.uconfig.data())));
    // So is another pixel program, and so is another scissor.
    Registers other_ps = OneDraw();
    other_ps.Program(0x08, 0x3000, 0x0000c3c0u, 0x0000000bu);
    CHECK(DrawFingerprint(a) !=
          DrawFingerprint(DecodeDraw(other_ps.sh.data(), other_ps.ctx.data(), other_ps.uconfig.data())));
    Registers smaller = OneDraw();
    smaller.Scissor(1280, 720);
    CHECK(DrawFingerprint(a) !=
          DrawFingerprint(DecodeDraw(smaller.sh.data(), smaller.ctx.data(), smaller.uconfig.data())));
}

TEST(GuestDraw_ARefusedDrawStillHasAFingerprint) {
    // A refused draw is logged once too, so the two refusals of one broken draw do
    // not each take a line.
    Registers r = OneDraw();
    r.Program(0x48, 0, 0, 0);
    const DrawIdentity refused = DecodeDraw(r.sh.data(), r.ctx.data(), r.uconfig.data());
    Registers same = OneDraw();
    same.Program(0x48, 0, 0, 0);
    CHECK_EQ(DrawFingerprint(refused), DrawFingerprint(DecodeDraw(same.sh.data(), same.ctx.data(), same.uconfig.data())));
    const Registers good = OneDraw();
    CHECK(DrawFingerprint(refused) !=
          DrawFingerprint(DecodeDraw(good.sh.data(), good.ctx.data(), good.uconfig.data())));
}
