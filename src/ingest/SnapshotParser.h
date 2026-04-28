#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "SnapshotSchema.h"

namespace ntium::ingest {

enum class SnapshotParseError : std::uint16_t {
  kNone = 0,

  // Step 1: frame bounds.
  kFrameTooSmall = 100,
  kFrameTooLarge = 101,

  // Step 2: header decode.
  kInvalidMagic = 200,
  kUnsupportedFormatVersion = 201,
  kInvalidHeaderSize = 202,
  kInvalidSnapshotKind = 203,
  kNonZeroHeaderFlags = 204,
  kNonZeroHeaderReserved0 = 205,
  kNonZeroHeaderReserved1 = 206,

  // Step 3: header bounds.
  kCanvasWidthOutOfRange = 300,
  kCanvasHeightOutOfRange = 301,
  kRecordCountOutOfRange = 302,
  kRecordsBytesOutOfRange = 303,
  kFrameSizeMismatch = 304,

  // Step 4: CRC checks.
  kHeaderCrcMismatch = 400,
  kSnapshotCrcMismatch = 401,

  // Step 5: sequence gate.
  kStaleSequence = 500,
  kInvalidFullStateBaseSequence = 501,
  kBaseSequenceMismatch = 502,

  // Step 6: record walk (generic).
  kRecordTruncated = 600,
  kRecordSizeOutOfRange = 601,
  kRecordSizePayloadMismatch = 602,
  kRecordOutOfBounds = 603,
  kRecordFlagsInvalid = 604,
  kRecordSchemaVersionInvalid = 605,
  kRecordReserved0NonZero = 606,
  kRecordReserved1NonZero = 607,
  kRecordReserved2NonZero = 608,
  kPayloadSizeOutOfRange = 609,
  kPayloadSizeTooSmallForType = 610,
  kPayloadCrcMismatch = 611,
  kUnknownMandatoryRecordType = 612,
  kRecordTtlOutOfRange = 613,
  kRecordCountMismatch = 614,
  kRecordBytesMismatch = 615,
  kRecordEventIdInvalid = 616,

  // Step 6: type-specific payload validation.
  kScanRingCenterOutOfRange = 700,
  kScanRingRadiusOutOfRange = 701,
  kScanRingStrokeOutOfRange = 702,
  kScanRingPhaseOutOfRange = 703,
  kScanRingAngularVelocityOutOfRange = 704,

  kScanArcCenterOutOfRange = 720,
  kScanArcRadiusOutOfRange = 721,
  kScanArcStrokeOutOfRange = 722,
  kScanArcStartOutOfRange = 723,
  kScanArcSweepOutOfRange = 724,
  kScanArcAngularVelocityOutOfRange = 725,
  kScanArcReservedNonZero = 726,

  kPointAlertPositionOutOfRange = 740,
  kPointAlertSizeOutOfRange = 741,
  kPointAlertPulseOutOfRange = 742,
  kPointAlertSeverityOutOfRange = 743,
  kPointAlertZIndexOutOfRange = 744,

  kAreaAlertBoundsInvalid = 760,
  kAreaAlertBorderOutOfRange = 761,
  kAreaAlertSeverityOutOfRange = 762,

  kClearEventScopeInvalid = 780,
  kClearEventTargetTypeInvalid = 781,
  kClearEventTargetEventIdInvalid = 782,
  kClearEventReservedNonZero = 783,
  kClearEventEnvelopeEventIdNonZero = 784,
  kClearEventEnvelopeTtlInvalid = 785,
  kClearEventCreatedTimestampInvalid = 786,
};

struct SnapshotParseContext {
  // Last applied snapshot sequence for sequence gating semantics.
  std::uint64_t last_applied_sequence = 0;
};

struct SnapshotRecordView {
  RecordEnvelopeWire envelope{};
  std::size_t record_offset_bytes = 0;
  std::size_t payload_offset_bytes = 0;
  bool skipped_unknown_optional = false;
};

struct SnapshotParseResult {
  SnapshotDisposition disposition = SnapshotDisposition::kRejectSnapshot;
  SnapshotParseError error = SnapshotParseError::kFrameTooSmall;
  std::size_t failure_record_index = 0;
  SnapshotHeaderWire header{};
  std::vector<SnapshotRecordView> records;
};

SnapshotParseResult ParseSnapshotV1(const std::uint8_t* snapshot_bytes,
                                    std::size_t snapshot_size,
                                    const SnapshotParseContext& context) noexcept;

SnapshotParseResult ParseSnapshotV1(const std::vector<std::uint8_t>& snapshot_bytes,
                                    const SnapshotParseContext& context) noexcept;

const char* ToString(SnapshotParseError error) noexcept;

std::uint32_t ComputeCrc32c(const std::uint8_t* bytes, std::size_t size) noexcept;

}  // namespace ntium::ingest
