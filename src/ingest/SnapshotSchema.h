#pragma once

#include <cstddef>
#include <cstdint>

namespace ntium::ingest {

constexpr std::uint32_t kSnapshotMagic = 0x31535645u;  // "EVS1"
constexpr std::uint16_t kSnapshotFormatVersion = 1;
constexpr std::uint16_t kSnapshotHeaderSize = 64;
constexpr std::uint16_t kRecordEnvelopeSize = 36;

constexpr std::size_t kMaxSnapshotBytes = 1'048'576;
constexpr std::uint32_t kMaxRecordCount = 4096;
constexpr std::uint16_t kMaxRecordBytes = 4096;
constexpr std::uint16_t kMaxPayloadBytes = 4060;
constexpr std::uint32_t kMaxTtlMs = 86'400'000;
constexpr std::uint16_t kMinCanvasExtentPx = 1;
constexpr std::uint16_t kMaxCanvasExtentPx = 16'384;

enum class SnapshotKind : std::uint8_t {
  kFullState = 1,
  kDelta = 2,
};

enum class RecordType : std::uint8_t {
  kScanRing = 0x01,
  kScanArc = 0x02,
  kPointAlert = 0x03,
  kAreaAlert = 0x04,
  kClearEvent = 0x05,
};

enum class ClearScope : std::uint8_t {
  kAll = 0,
  kByType = 1,
  kByEventId = 2,
};

enum class SnapshotDisposition : std::uint8_t {
  kApply = 0,
  kIgnoreSnapshot = 1,
  kRejectSnapshot = 2,
  kRejectDelta = 3,
};

constexpr std::uint8_t kRecordFlagOptional = 0x01;

struct PayloadSizeRule {
  std::uint16_t min_size_bytes;
  bool allow_trailing_bytes;
};

#pragma pack(push, 1)

struct SnapshotHeaderWire {
  std::uint32_t magic;
  std::uint16_t format_version;
  std::uint16_t header_size;
  std::uint8_t snapshot_kind;
  std::uint8_t header_flags;
  std::uint16_t reserved0;
  std::uint64_t sequence;
  std::uint64_t base_sequence;
  std::uint64_t generated_unix_ms;
  std::uint16_t canvas_width_px;
  std::uint16_t canvas_height_px;
  std::uint32_t record_count;
  std::uint32_t records_bytes;
  std::uint32_t header_crc32c;
  std::uint32_t snapshot_crc32c;
  std::uint64_t reserved1;
};

struct RecordEnvelopeWire {
  std::uint16_t record_size;
  std::uint8_t record_type;
  std::uint8_t record_flags;
  std::uint8_t schema_version;
  std::uint8_t reserved0;
  std::uint16_t reserved1;
  std::uint64_t event_id;
  std::uint64_t created_unix_ms;
  std::uint32_t ttl_ms;
  std::uint16_t payload_size;
  std::uint16_t reserved2;
  std::uint32_t payload_crc32c;
};

struct ScanRingPayloadPrefixWire {
  std::uint16_t center_x_px;
  std::uint16_t center_y_px;
  std::uint16_t radius_px;
  std::uint16_t stroke_px;
  std::uint16_t phase_deg_x100;
  std::int16_t angular_velocity_deg_x100;
  std::uint32_t color_argb;
};

struct ScanArcPayloadPrefixWire {
  std::uint16_t center_x_px;
  std::uint16_t center_y_px;
  std::uint16_t radius_px;
  std::uint16_t stroke_px;
  std::uint16_t start_deg_x100;
  std::uint16_t sweep_deg_x100;
  std::int16_t angular_velocity_deg_x100;
  std::uint16_t reserved;
  std::uint32_t color_argb;
};

struct PointAlertPayloadPrefixWire {
  std::uint16_t x_px;
  std::uint16_t y_px;
  std::uint16_t size_px;
  std::uint16_t style_id;
  std::uint16_t pulse_period_ms;
  std::uint8_t severity;
  std::uint8_t z_index;
  std::uint32_t color_argb;
};

struct AreaAlertPayloadPrefixWire {
  std::uint16_t x_min_px;
  std::uint16_t y_min_px;
  std::uint16_t x_max_px;
  std::uint16_t y_max_px;
  std::uint16_t border_px;
  std::uint8_t fill_alpha;
  std::uint8_t severity;
  std::uint32_t color_argb;
};

struct ClearEventPayloadPrefixWire {
  std::uint8_t clear_scope;
  std::uint8_t target_type;
  std::uint16_t reserved;
  std::uint64_t target_event_id;
};

#pragma pack(pop)

static_assert(sizeof(SnapshotHeaderWire) == kSnapshotHeaderSize,
              "Snapshot header must stay 64 bytes.");
static_assert(sizeof(RecordEnvelopeWire) == kRecordEnvelopeSize,
              "Record envelope must stay 36 bytes.");
static_assert(sizeof(ScanRingPayloadPrefixWire) == 16,
              "SCAN_RING payload prefix must stay 16 bytes.");
static_assert(sizeof(ScanArcPayloadPrefixWire) == 20,
              "SCAN_ARC payload prefix must stay 20 bytes.");
static_assert(sizeof(PointAlertPayloadPrefixWire) == 16,
              "POINT_ALERT payload prefix must stay 16 bytes.");
static_assert(sizeof(AreaAlertPayloadPrefixWire) == 16,
              "AREA_ALERT payload prefix must stay 16 bytes.");
static_assert(sizeof(ClearEventPayloadPrefixWire) == 12,
              "CLEAR_EVENT payload prefix must stay 12 bytes.");

constexpr bool IsKnownRecordType(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(RecordType::kScanRing) &&
         value <= static_cast<std::uint8_t>(RecordType::kClearEvent);
}

constexpr PayloadSizeRule PayloadRuleFor(std::uint8_t record_type) noexcept {
  switch (static_cast<RecordType>(record_type)) {
    case RecordType::kScanRing:
      return {16, true};
    case RecordType::kScanArc:
      return {20, true};
    case RecordType::kPointAlert:
      return {16, true};
    case RecordType::kAreaAlert:
      return {16, true};
    case RecordType::kClearEvent:
      return {12, true};
    default:
      break;
  }
  return {0, false};
}

}  // namespace ntium::ingest
