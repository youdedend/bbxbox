// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_stream.h"

#include <cstdio>
#include <string>

namespace bbx::d3d12 {
namespace {
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kAlignment = 8;
}  // namespace

const char* RecordKindName(RecordKind kind) {
    switch (kind) {
    case RecordKind::kBegin: return "Begin";
    case RecordKind::kEndSubmit: return "EndSubmit";
    case RecordKind::kForeignSubmit: return "ForeignSubmit";
    case RecordKind::kDrawState: return "DrawState";
    case RecordKind::kBarrier: return "Barrier";
    case RecordKind::kTransferBarrier: return "TransferBarrier";
    case RecordKind::kCopyOrderBarrier: return "CopyOrderBarrier";
    case RecordKind::kPassBarrier: return "PassBarrier";
    case RecordKind::kEndRendering: return "EndRendering";
    case RecordKind::kBeginRendering: return "BeginRendering";
    case RecordKind::kFill: return "Fill";
    case RecordKind::kCopyBuffer: return "CopyBuffer";
    case RecordKind::kCopyBufferToImage: return "CopyBufferToImage";
    case RecordKind::kCopyImageToBuffer: return "CopyImageToBuffer";
    case RecordKind::kClearColor: return "ClearColor";
    case RecordKind::kResetQueries: return "ResetQueries";
    case RecordKind::kBeginQuery: return "BeginQuery";
    case RecordKind::kEndQuery: return "EndQuery";
    case RecordKind::kCopyQueryResults: return "CopyQueryResults";
    case RecordKind::kTimestamp: return "Timestamp";
    case RecordKind::kBindPipeline: return "BindPipeline";
    case RecordKind::kBindSets: return "BindSets";
    case RecordKind::kPushConstants: return "PushConstants";
    case RecordKind::kUpdateSets: return "UpdateSets";
    case RecordKind::kDispatch: return "Dispatch";
    case RecordKind::kClearDepth: return "ClearDepth";
    case RecordKind::kCopyImage: return "CopyImage";
    case RecordKind::kDispatchIndirect: return "DispatchIndirect";
    case RecordKind::kMarker: return "Marker";
    case RecordKind::kPushBuffers: return "PushBuffers";
    case RecordKind::kListDraw: return "ListDraw";
    case RecordKind::kExecuteCommands: return "ExecuteCommands";
    case RecordKind::kListRun: return "ListRun";
    case RecordKind::kUpdateBuffer: return "UpdateBuffer";
    case RecordKind::kMarkerWindow: return "MarkerWindow";
    case RecordKind::kCount: break;
    }
    return "?";
}

bool IsKnownKind(RecordKind kind) {
    return static_cast<std::uint16_t>(kind) < static_cast<std::uint16_t>(RecordKind::kCount);
}

const char* WalkErrorName(WalkError error) {
    switch (error) {
    case WalkError::kNone: return "ok";
    case WalkError::kMisalignedRecord: return "record is not 8-byte aligned";
    case WalkError::kHeaderTooSmall: return "record header is smaller than 8 bytes";
    case WalkError::kRecordOutOfBounds: return "record extends past the block";
    case WalkError::kUnknownKind: return "record has an unknown opcode";
    case WalkError::kTrailingBytes: return "block ends with bytes that are not a record";
    }
    return "?";
}

std::string_view WalkResult::Describe() const {
    static thread_local char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "walk %s at byte %zu after %zu records (%zu bytes), last %s",
                  WalkErrorName(error), bad_offset, records, bytes, RecordKindName(last_kind));
    return buf;
}

WalkResult WalkStream(
    std::span<const std::uint8_t> block,
    const std::function<void(const RecordHeader&, std::span<const std::uint8_t>)>& on_record) {
    WalkResult result;
    std::size_t at = 0;
    while (at < block.size()) {
        if (block.size() - at < kHeaderSize) {
            result.error = WalkError::kTrailingBytes;
            result.bad_offset = at;
            return result;
        }
        if (at % kAlignment != 0) {
            result.error = WalkError::kMisalignedRecord;
            result.bad_offset = at;
            return result;
        }
        RecordHeader header;
        header.kind = static_cast<RecordKind>(block[at] | (block[at + 1] << 8));
        header.site = static_cast<std::uint16_t>(block[at + 2] | (block[at + 3] << 8));
        header.bytes = static_cast<std::uint32_t>(block[at + 4]) |
                       (static_cast<std::uint32_t>(block[at + 5]) << 8) |
                       (static_cast<std::uint32_t>(block[at + 6]) << 16) |
                       (static_cast<std::uint32_t>(block[at + 7]) << 24);
        if (!IsKnownKind(header.kind)) {
            result.error = WalkError::kUnknownKind;
            result.bad_offset = at;
            return result;
        }
        if (header.bytes < kHeaderSize || header.bytes % kAlignment != 0) {
            result.error = WalkError::kHeaderTooSmall;
            result.bad_offset = at;
            return result;
        }
        if (static_cast<std::size_t>(header.bytes) > block.size() - at) {
            result.error = WalkError::kRecordOutOfBounds;
            result.bad_offset = at;
            return result;
        }
        if (on_record) {
            on_record(header,
                      block.subspan(at + kHeaderSize, static_cast<std::size_t>(header.bytes) - kHeaderSize));
        }
        result.last_kind = header.kind;
        ++result.records;
        result.bytes += header.bytes;
        at += header.bytes;
    }
    return result;
}

void Accumulate(StreamStats& stats, RecordKind kind, std::uint32_t bytes) {
    ++stats.records;
    stats.bytes += bytes;
    switch (kind) {
    case RecordKind::kDrawState:
    case RecordKind::kListDraw:
    case RecordKind::kListRun:
        ++stats.draws;
        break;
    case RecordKind::kDispatch:
    case RecordKind::kDispatchIndirect:
        ++stats.dispatches;
        break;
    case RecordKind::kBarrier:
    case RecordKind::kTransferBarrier:
    case RecordKind::kCopyOrderBarrier:
    case RecordKind::kPassBarrier:
        ++stats.barriers;
        break;
    case RecordKind::kFill:
    case RecordKind::kCopyBuffer:
    case RecordKind::kCopyBufferToImage:
    case RecordKind::kCopyImageToBuffer:
    case RecordKind::kCopyImage:
    case RecordKind::kUpdateBuffer:
        ++stats.copies;
        break;
    case RecordKind::kMarker:
    case RecordKind::kMarkerWindow:
        ++stats.markers;
        break;
    default:
        break;
    }
}

std::string DescribeStream(const StreamStats& stats) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "stream: %llu records (%.1f MiB), %llu draws, %llu dispatches, %llu barriers, "
                  "%llu copies, %llu markers, %llu walk errors",
                  static_cast<unsigned long long>(stats.records),
                  static_cast<double>(stats.bytes) / 1048576.0,
                  static_cast<unsigned long long>(stats.draws),
                  static_cast<unsigned long long>(stats.dispatches),
                  static_cast<unsigned long long>(stats.barriers),
                  static_cast<unsigned long long>(stats.copies),
                  static_cast<unsigned long long>(stats.markers),
                  static_cast<unsigned long long>(stats.walk_errors));
    return buf;
}

}  // namespace bbx::d3d12
