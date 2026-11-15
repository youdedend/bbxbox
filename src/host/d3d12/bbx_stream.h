// SPDX-License-Identifier: GPL-3.0-or-later
// Walking bbhost's typed command stream without trusting it.
//
// `src/host/stream_ops.h` defines records as an 8-byte header (kind, site,
// bytes) followed by a plain struct and its arrays, every record a multiple of
// eight bytes. The replay reads that memory; a bug in the recorder, a mismatched
// upstream commit, or a partially written block would otherwise be a wild read
// in the GPU thread. The walker validates structure first, and the replay only
// sees records that fit.
//
// The record kinds are mirrored here in upstream order, without Vulkan types,
// so this compiles and tests on a machine with no Vulkan headers. The sizes of
// the payloads are the replay's business; the walker's contract is the header.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace bbx::d3d12 {

// Mirrors gpu::stream::Op order exactly (stream_ops.h). `kCount` last.
enum class RecordKind : std::uint16_t {
    kBegin = 0,
    kEndSubmit,
    kForeignSubmit,
    kDrawState,
    kBarrier,
    kTransferBarrier,
    kCopyOrderBarrier,
    kPassBarrier,
    kEndRendering,
    kBeginRendering,
    kFill,
    kCopyBuffer,
    kCopyBufferToImage,
    kCopyImageToBuffer,
    kClearColor,
    kResetQueries,
    kBeginQuery,
    kEndQuery,
    kCopyQueryResults,
    kTimestamp,
    kBindPipeline,
    kBindSets,
    kPushConstants,
    kUpdateSets,
    kDispatch,
    kClearDepth,
    kCopyImage,
    kDispatchIndirect,
    kMarker,
    kPushBuffers,
    kListDraw,
    kExecuteCommands,
    kListRun,
    kUpdateBuffer,
    kMarkerWindow,
    kCount,
};

struct RecordHeader {
    RecordKind kind = RecordKind::kBegin;
    std::uint16_t site = 0;
    std::uint32_t bytes = 0;
};

[[nodiscard]] const char* RecordKindName(RecordKind kind);
[[nodiscard]] bool IsKnownKind(RecordKind kind);

enum class WalkError : std::uint8_t {
    kNone = 0,
    kMisalignedRecord,
    kHeaderTooSmall,
    kRecordOutOfBounds,
    kUnknownKind,
    kTrailingBytes,
};

[[nodiscard]] const char* WalkErrorName(WalkError error);

struct WalkResult {
    WalkError error = WalkError::kNone;
    std::size_t records = 0;
    std::size_t bytes = 0;
    std::size_t bad_offset = 0;   // where the walk stopped
    RecordKind last_kind = RecordKind::kBegin;

    [[nodiscard]] bool ok() const { return error == WalkError::kNone; }
    [[nodiscard]] std::string_view Describe() const;
};

// Calls `on_record` for each structurally valid record. Stops at the first
// structural error; records already visited are counted, so a partial block is
// reportable ("the recorder wrote 812 records, then garbage").
[[nodiscard]] WalkResult WalkStream(
    std::span<const std::uint8_t> block,
    const std::function<void(const RecordHeader&, std::span<const std::uint8_t>)>& on_record);

// The statistics the exit report prints.
struct StreamStats {
    std::uint64_t records = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t barriers = 0;
    std::uint64_t copies = 0;
    std::uint64_t markers = 0;
    std::uint64_t bytes = 0;
    std::uint64_t walk_errors = 0;
};

void Accumulate(StreamStats& stats, RecordKind kind, std::uint32_t bytes);
[[nodiscard]] std::string DescribeStream(const StreamStats& stats);

}  // namespace bbx::d3d12
