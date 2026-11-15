// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_quarantine.h"

#include <string>

using namespace bbx::d3d12;

namespace {
Unit MakeUnit(std::uint64_t hash, std::uint64_t partner = 0) {
    Unit u;
    u.hash = hash;
    u.partner = partner;
    u.kind = ShaderKind::kPixel;
    u.bundle = "GXShader.shaderbnd.dcx";
    u.entry = "GXFlver_Gbuf_[A].ppo";
    if (partner) u.partner_entry = "GXFlver_Gbuf_[A].vso";
    return u;
}
}  // namespace

TEST(InflightRoundTripsSingleAndPair) {
    const Unit unit = MakeUnit(0x1122334455667788ull);
    const std::string text = SerializeInflight(unit, 12, 3209);
    const Inflight parsed = ParseInflight(text);
    CHECK(parsed.present);
    CHECK_EQ(parsed.unit.hash, unit.hash);
    CHECK_EQ(parsed.unit.partner, 0ull);
    CHECK_EQ(parsed.index, 12u);
    CHECK_EQ(parsed.total, 3209u);
    CHECK(parsed.unit.bundle == unit.bundle);
    CHECK(parsed.unit.entry == unit.entry);

    const Unit pair = MakeUnit(0xabc, 0xdef);
    const Inflight parsed_pair = ParseInflight(SerializeInflight(pair, 1, 2));
    CHECK(parsed_pair.present);
    CHECK_EQ(parsed_pair.unit.partner, 0xdefull);
    CHECK(parsed_pair.unit.is_pair());
    CHECK(parsed_pair.unit.kind == ShaderKind::kPixel);
}

TEST(InflightRejectsGarbage) {
    CHECK(!ParseInflight("").present);
    CHECK(!ParseInflight("0\n").present);
    CHECK(!ParseInflight("hello\n").present);
    CHECK(!ParseInflight("1\nhash=zzzz\n").present);
    CHECK(!ParseInflight("1\nhash=0000000000000000\n").present);
    // A v1 record with a hash but no other fields is still usable.
    const Inflight minimal = ParseInflight("1\nhash=00000000000000ff\n");
    CHECK(minimal.present);
    CHECK_EQ(minimal.unit.hash, 0xffull);
    CHECK(minimal.unit.bundle.empty());
}

TEST(RecoveryPromotesToQuarantine) {
    Quarantine quarantine;
    const Unit pair = MakeUnit(0x1234, 0x5678);
    const CrashRecovery recovery =
        RecoverFromInflight(SerializeInflight(pair, 7, 100), quarantine);
    CHECK(recovery.found);
    CHECK(recovery.promoted);
    CHECK(quarantine.Contains(0x1234ull));
    CHECK(quarantine.Contains(0x5678ull));
    CHECK_EQ(quarantine.size(), 2u);
    CHECK(recovery.message.find("quarantined") != std::string::npos);
    CHECK(recovery.message.find("7/100") != std::string::npos);
}

TEST(RecoveryWithoutBreadcrumbDoesNothing) {
    Quarantine quarantine;
    const CrashRecovery recovery = RecoverFromInflight("", quarantine);
    CHECK(!recovery.found);
    CHECK(!recovery.promoted);
    CHECK_EQ(quarantine.size(), 0u);
}

TEST(QuarantineParsesTolerantly) {
    const Quarantine q = Quarantine::Parse(
        "0000000000000abc\r\n"
        "\n"
        "not a hash\n"
        "0000000000000abc\n"          // duplicate
        "0x0000000000000def trailing note\n"
        "0000000000000000\n"          // zero is not a shader
        "0000000000000abc");
    CHECK_EQ(q.size(), 2u);
    CHECK(q.Contains(0xabcull));
    CHECK(q.Contains(0xdefull));
    CHECK(!q.Contains(0ull));
    CHECK(!q.Contains(0x999ull));
}

TEST(QuarantineSerializesSortedAndUnique) {
    Quarantine q;
    q.Add(0x30ull);
    q.Add(0x10ull);
    q.Add(0x30ull);
    q.Add(MakeUnit(0x20, 0x40));
    const std::string text = q.Serialize();
    CHECK(text == "0000000000000010\n0000000000000020\n0000000000000030\n0000000000000040\n");
    CHECK_EQ(Quarantine::Parse(text).size(), 4u);
    CHECK(q.Summary(2).find("4 entries") != std::string::npos);
    CHECK(q.Summary(2).find("…") != std::string::npos);
}

TEST(QuarantineMatchesEitherSideOfAPair) {
    Quarantine q;
    const Unit pair = MakeUnit(0x1, 0x2);
    q.Add(pair);
    CHECK(q.Contains(pair));
    Unit only_vs;
    only_vs.hash = 0x2;
    CHECK(q.Contains(only_vs));
    Unit unrelated;
    unrelated.hash = 0x3;
    CHECK(!q.Contains(unrelated));
}

TEST(StageHashIsStableAndKindSensitive) {
    const std::uint64_t a = StageHash(0x1234, ShaderKind::kPixel, "stamp");
    const std::uint64_t b = StageHash(0x1234, ShaderKind::kPixel, "stamp");
    const std::uint64_t c = StageHash(0x1234, ShaderKind::kVertex, "stamp");
    const std::uint64_t d = StageHash(0x1234, ShaderKind::kPixel, "other");
    CHECK_EQ(a, b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a != 0);
}
