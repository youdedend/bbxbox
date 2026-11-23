// SPDX-License-Identifier: GPL-3.0-or-later
// tests/path_table_test.cpp -- the bake's binding assignment (bbx_path_table).
//
// The bake pairs each binding the console's HLSL declares with the ResourcePath
// the translator found for it; the console may not work that pairing out itself.
// So the reader is the trust boundary: a file from another bake, a duplicate
// binding, or a line this format did not write has to be refused whole, because
// one accepted wrong plan binds another buffer's bytes and no frame explains it.

#include <cstdint>
#include <string>

#include "harness.h"
#include "host/d3d12/bbx_cache.h"
#include "host/d3d12/bbx_guest_path.h"
#include "host/d3d12/bbx_path_table.h"

using namespace bbx::d3d12;

namespace {

StampDigest DigestOf(std::uint8_t seed) {
    StampDigest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = static_cast<std::uint8_t>(seed + i);
    return digest;
}

BindingPlan MakePlan(std::uint32_t binding, bool uav, int user_sgpr, std::uint32_t offset_dw) {
    BindingPlan plan;
    plan.binding = binding;
    plan.kind = uav ? BindingKind::kUav : BindingKind::kSrv;
    plan.path.user_sgpr = user_sgpr;
    plan.path.final_offset_dw = static_cast<int>(offset_dw);
    return plan;
}

std::string HexOf(const StampDigest& digest) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

std::string File(const StampDigest& digest, const std::string& records) {
    return std::string("# bbx_shaders.paths v1\nstamp ") + HexOf(digest) + "\n" + records;
}

}  // namespace

TEST(PathTable_ABindingLineIsWrittenAndReadBack) {
    const StampDigest digest = DigestOf(1);
    const BindingPlan plan = MakePlan(7, true, 3, 5);
    const std::string line = PathTable::FormatBinding(0xfeed'face'0000'0001ull, 2, plan);
    CHECK(line == "bind feedface00000001 2 7 uav user=3;imm=0;fin=5;finv=0;loads=");
    PathTable table;
    CHECK(table.Load(File(digest, line + "\n"), digest) == PathTableStatus::kOk);
    const BindingPlan* found = table.Find(0xfeed'face'0000'0001ull, 7);
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->kind == BindingKind::kUav);
        CHECK_EQ(found->path.user_sgpr, 3);
        CHECK_EQ(found->path.final_offset_dw, 5);
    }
    CHECK_EQ(table.bindings(), 1u);
    CHECK_EQ(table.programs(), 1u);
    CHECK(table.Report().find("1 bindings on 1 programs") != std::string::npos);
}

TEST(PathTable_ABindingTheBakeDidNotWriteIsNotFoundNotDefaulted) {
    const StampDigest digest = DigestOf(2);
    PathTable table;
    CHECK(table.Load(File(digest, PathTable::FormatBinding(0x10, 2, MakePlan(1, false, 0, 0)) + "\n"), digest) ==
          PathTableStatus::kOk);
    CHECK(table.Find(0x10, 1) != nullptr);
    CHECK(table.Find(0x10, 2) == nullptr);   // that program declares no t2
    CHECK(table.Find(0x11, 1) == nullptr);   // no such program
    CHECK(table.Find(0x10, 64) == nullptr);  // no t64 exists
}

TEST(PathTable_ATableFromAnotherBakeIsRefusedWhole) {
    PathTable table;
    const std::string text = File(DigestOf(3), PathTable::FormatBinding(0x20, 2, MakePlan(0, false, 0, 0)) + "\n");
    CHECK(table.Load(text, DigestOf(4)) == PathTableStatus::kStampMismatch);
    CHECK_EQ(table.bindings(), 0u);
    CHECK(table.Find(0x20, 0) == nullptr);  // nothing survives a mismatch
    CHECK(table.Report().find("stamp mismatch") != std::string::npos);
}

TEST(PathTable_TextThatIsNotThisFileIsRefused) {
    PathTable table;
    CHECK(table.Load("key 0 0\n", DigestOf(1)) == PathTableStatus::kNotATable);
    CHECK(table.Load("# bbx_shaders.paths v2\nstamp 00\n", DigestOf(1)) == PathTableStatus::kNotATable);
    CHECK(table.Load("", DigestOf(1)) == PathTableStatus::kMissing);
}

TEST(PathTable_ALineThisFormatDidNotWriteIsRefused) {
    const StampDigest digest = DigestOf(5);
    PathTable table;
    // A record kind the console does not know: a newer bake's field must not be
    // ignored, because ignoring it is binding without it.
    CHECK(table.Load(File(digest, "sampler 0000000000000001 2 0 srv user=0;imm=0;fin=0;finv=0;loads=\n"), digest) ==
          PathTableStatus::kCorrupt);
    // The wrong number of fields.
    CHECK(table.Load(File(digest, "bind 0000000000000001 2 0 srv\n"), digest) == PathTableStatus::kCorrupt);
    // A key that is not hexadecimal.
    CHECK(table.Load(File(digest, "bind xyz 2 0 srv user=0;imm=0;fin=0;finv=0;loads=\n"), digest) ==
          PathTableStatus::kCorrupt);
    // A binding outside t0..t127.
    CHECK(table.Load(File(digest, "bind 0000000000000001 2 128 srv user=0;imm=0;fin=0;finv=0;loads=\n"), digest) ==
          PathTableStatus::kCorrupt);
    // A kind that is neither srv nor uav.
    CHECK(table.Load(File(digest, "bind 0000000000000001 2 0 cbv user=0;imm=0;fin=0;finv=0;loads=\n"), digest) ==
          PathTableStatus::kCorrupt);
    // A path this reader did not write.
    CHECK(table.Load(File(digest, "bind 0000000000000001 2 0 srv s0 -> [ptr+0x10]\n"), digest) ==
          PathTableStatus::kCorrupt);
    // No stamp at all: the file cannot be paired with a cache.
    CHECK(table.Load("# bbx_shaders.paths v1\n", digest) == PathTableStatus::kCorrupt);
}

TEST(PathTable_TwoPlansForOneBindingAreRefused) {
    const StampDigest digest = DigestOf(6);
    const std::string line = PathTable::FormatBinding(0x30, 2, MakePlan(4, false, 0, 0)) + "\n";
    PathTable table;
    CHECK(table.Load(File(digest, line + line), digest) == PathTableStatus::kCorrupt);
    CHECK(table.Report().find("written twice") != std::string::npos);
}

TEST(PathTable_CommentsAndBlankLinesAreFine) {
    const StampDigest digest = DigestOf(7);
    const std::string text = File(digest,
                                  "\n# the compute programs\n" +
                                      PathTable::FormatBinding(0x40, 2, MakePlan(0, false, 0, 0)) + "\n\n");
    PathTable table;
    CHECK(table.Load(text, digest) == PathTableStatus::kOk);
    CHECK_EQ(table.bindings(), 1u);
}

TEST(PathTable_ProgramsAreCountedOncePerKeyNotPerBinding) {
    const StampDigest digest = DigestOf(8);
    std::string records;
    for (std::uint32_t binding = 0; binding < 4; ++binding) {
        records += PathTable::FormatBinding(0x50, 2, MakePlan(binding, false, 0, binding * 4)) + "\n";
    }
    records += PathTable::FormatBinding(0x51, 2, MakePlan(0, false, 0, 0)) + "\n";
    PathTable table;
    CHECK(table.Load(File(digest, records), digest) == PathTableStatus::kOk);
    CHECK_EQ(table.bindings(), 5u);
    CHECK_EQ(table.programs(), 2u);
}
