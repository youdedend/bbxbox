// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_stream.h"

#include <vector>

using namespace bbx::d3d12;

namespace {
void Append(std::vector<std::uint8_t>& out, RecordKind kind, std::uint32_t bytes,
            std::uint16_t site = 0) {
    out.push_back(static_cast<std::uint8_t>(kind));
    out.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(kind) >> 8));
    out.push_back(static_cast<std::uint8_t>(site));
    out.push_back(static_cast<std::uint8_t>(site >> 8));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bytes >> (8 * i)));
    for (std::uint32_t i = 8; i < bytes; ++i) out.push_back(0xcd);
}
}  // namespace

TEST(WalkAcceptsTheRecorderForm) {
    std::vector<std::uint8_t> block;
    Append(block, RecordKind::kBegin, 16);
    Append(block, RecordKind::kDrawState, 16);
    Append(block, RecordKind::kEndSubmit, 64);
    std::vector<RecordKind> seen;
    StreamStats stats;
    const WalkResult result = WalkStream(
        block, [&](const RecordHeader& h, std::span<const std::uint8_t> payload) {
            seen.push_back(h.kind);
            CHECK_EQ(payload.size(), h.bytes - 8);
            Accumulate(stats, h.kind, h.bytes);
        });
    CHECK(result.ok());
    CHECK_EQ(result.records, 3u);
    CHECK_EQ(result.bytes, block.size());
    CHECK_EQ(seen.size(), 3u);
    CHECK(seen[1] == RecordKind::kDrawState);
    CHECK_EQ(stats.draws, 1u);
    CHECK_EQ(stats.records, 3u);
}

TEST(WalkRejectsTruncation) {
    std::vector<std::uint8_t> block;
    Append(block, RecordKind::kBegin, 16);
    Append(block, RecordKind::kDispatch, 32);
    block.resize(block.size() - 8);  // the last record no longer fits
    const WalkResult result =
        WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
    CHECK(!result.ok());
    CHECK(result.error == WalkError::kRecordOutOfBounds);
    CHECK_EQ(result.records, 1u);  // the first record was fine and is counted
    CHECK_EQ(result.bad_offset, 16u);
}

TEST(WalkRejectsBadHeaders) {
    {
        std::vector<std::uint8_t> block;
        Append(block, RecordKind::kBegin, 12);  // not a multiple of 8
        const WalkResult r =
            WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
        CHECK(r.error == WalkError::kHeaderTooSmall);
    }
    {
        std::vector<std::uint8_t> block;
        Append(block, RecordKind::kBegin, 4);  // smaller than a header
        const WalkResult r =
            WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
        CHECK(r.error == WalkError::kHeaderTooSmall);
    }
    {
        std::vector<std::uint8_t> block;
        Append(block, RecordKind::kCount, 8);  // a kind that is not a record
        const WalkResult r =
            WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
        CHECK(r.error == WalkError::kUnknownKind);
    }
    {
        std::vector<std::uint8_t> block;
        Append(block, RecordKind::kBegin, 8);
        block.push_back(0);  // 4 trailing bytes: not a record
        block.push_back(0);
        block.push_back(0);
        block.push_back(0);
        const WalkResult r =
            WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
        CHECK(r.error == WalkError::kTrailingBytes);
    }
}

TEST(EmptyStreamIsValid) {
    const std::vector<std::uint8_t> block;
    const WalkResult r =
        WalkStream(block, [](const RecordHeader&, std::span<const std::uint8_t>) {});
    CHECK(r.ok());
    CHECK_EQ(r.records, 0u);
}

TEST(StatsClassifyOps) {
    StreamStats stats;
    Accumulate(stats, RecordKind::kDrawState, 16);
    Accumulate(stats, RecordKind::kListDraw, 16);
    Accumulate(stats, RecordKind::kDispatch, 16);
    Accumulate(stats, RecordKind::kBarrier, 32);
    Accumulate(stats, RecordKind::kCopyBuffer, 48);
    Accumulate(stats, RecordKind::kMarker, 16);
    Accumulate(stats, RecordKind::kMarkerWindow, 16);
    CHECK_EQ(stats.draws, 2u);
    CHECK_EQ(stats.dispatches, 1u);
    CHECK_EQ(stats.barriers, 1u);
    CHECK_EQ(stats.copies, 1u);
    CHECK_EQ(stats.markers, 2u);
    CHECK(DescribeStream(stats).find("7 records") != std::string::npos);
}

TEST(KindNamesCoverTheWholeUpstreamRange) {
    // stream_ops.h ends at kUpdateBuffer (index 34) with kMarkerWindow after
    // it; a gap would mean the mirror drifted from upstream.
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(RecordKind::kCount); ++i) {
        const RecordKind kind = static_cast<RecordKind>(i);
        CHECK(IsKnownKind(kind));
        CHECK(std::string(RecordKindName(kind)) != "?");
    }
    CHECK_EQ(static_cast<std::uint16_t>(RecordKind::kMarkerWindow), 34);
    CHECK_EQ(static_cast<std::uint16_t>(RecordKind::kCount), 35);
}
