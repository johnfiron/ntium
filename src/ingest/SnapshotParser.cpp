#include "SnapshotParser.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ntium::ingest {

namespace {

template <typename T>
const T* ReadUnaligned(const std::uint8_t* bytes) noexcept {
  return reinterpret_cast<const T*>(bytes);
}

bool ValidateCoord(std::uint16_t coord, std::uint16_t extent) noexcept {
  return coord < extent;
}

bool IsValidClearTargetType(std::uint8_t record_type) noexcept {
  return IsKnownRecordType(record_type);
}

SnapshotParseResult MakeInitialResult() {
  SnapshotParseResult result{};
  result.disposition = SnapshotDisposition::kRejectSnapshot;
  result.error = SnapshotParseError::kFrameTooSmall;
  result.failure_record_index = 0;
  return result;
}

}  // namespace

std::uint32_t ComputeCrc32c(const std::uint8_t* bytes, std::size_t size) noexcept {
  // CRC-32C (Castagnoli), reflected, init/xorout = 0xFFFFFFFF.
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<std::uint32_t>(bytes[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t lsb = crc & 1u;
      crc >>= 1u;
      if (lsb != 0u) {
        crc ^= 0x82F63B78u;
      }
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

SnapshotParseResult ParseSnapshotV1(const std::uint8_t* snapshot_bytes,
                                    std::size_t snapshot_size,
                                    const SnapshotParseContext& context) noexcept {
  SnapshotParseResult result = MakeInitialResult();
  if (snapshot_bytes == nullptr) {
    return result;
  }

  // 1) Frame bounds.
  if (snapshot_size < kSnapshotHeaderSize) {
    result.error = SnapshotParseError::kFrameTooSmall;
    return result;
  }
  if (snapshot_size > kMaxSnapshotBytes) {
    result.error = SnapshotParseError::kFrameTooLarge;
    return result;
  }

  const auto* header = ReadUnaligned<SnapshotHeaderWire>(snapshot_bytes);
  result.header = *header;

  // 2) Header decode.
  if (header->magic != kSnapshotMagic) {
    result.error = SnapshotParseError::kInvalidMagic;
    return result;
  }
  if (header->format_version != kSnapshotFormatVersion) {
    result.error = SnapshotParseError::kUnsupportedFormatVersion;
    return result;
  }
  if (header->header_size != kSnapshotHeaderSize) {
    result.error = SnapshotParseError::kInvalidHeaderSize;
    return result;
  }
  if (header->snapshot_kind != static_cast<std::uint8_t>(SnapshotKind::kFullState) &&
      header->snapshot_kind != static_cast<std::uint8_t>(SnapshotKind::kDelta)) {
    result.error = SnapshotParseError::kInvalidSnapshotKind;
    return result;
  }
  if (header->header_flags != 0) {
    result.error = SnapshotParseError::kNonZeroHeaderFlags;
    return result;
  }
  if (header->reserved0 != 0) {
    result.error = SnapshotParseError::kNonZeroHeaderReserved0;
    return result;
  }
  if (header->reserved1 != 0) {
    result.error = SnapshotParseError::kNonZeroHeaderReserved1;
    return result;
  }

  // 3) Header bounds.
  if (header->canvas_width_px < kMinCanvasExtentPx ||
      header->canvas_width_px > kMaxCanvasExtentPx) {
    result.error = SnapshotParseError::kCanvasWidthOutOfRange;
    return result;
  }
  if (header->canvas_height_px < kMinCanvasExtentPx ||
      header->canvas_height_px > kMaxCanvasExtentPx) {
    result.error = SnapshotParseError::kCanvasHeightOutOfRange;
    return result;
  }
  if (header->record_count > kMaxRecordCount) {
    result.error = SnapshotParseError::kRecordCountOutOfRange;
    return result;
  }
  if (header->records_bytes > kMaxSnapshotBytes - kSnapshotHeaderSize) {
    result.error = SnapshotParseError::kRecordsBytesOutOfRange;
    return result;
  }
  const std::size_t expected_total_bytes =
      static_cast<std::size_t>(header->header_size) +
      static_cast<std::size_t>(header->records_bytes);
  if (expected_total_bytes != snapshot_size) {
    result.error = SnapshotParseError::kFrameSizeMismatch;
    return result;
  }

  // 4) Header CRC then snapshot CRC.
  std::array<std::uint8_t, kSnapshotHeaderSize> header_copy{};
  std::memcpy(header_copy.data(), snapshot_bytes, kSnapshotHeaderSize);
  for (std::size_t i = 48; i <= 55; ++i) {
    header_copy[i] = 0;
  }
  if (ComputeCrc32c(header_copy.data(), header_copy.size()) !=
      header->header_crc32c) {
    result.error = SnapshotParseError::kHeaderCrcMismatch;
    return result;
  }

  std::vector<std::uint8_t> snapshot_copy(snapshot_bytes,
                                          snapshot_bytes + snapshot_size);
  for (std::size_t i = 52; i <= 55; ++i) {
    snapshot_copy[i] = 0;
  }
  if (ComputeCrc32c(snapshot_copy.data(), snapshot_copy.size()) !=
      header->snapshot_crc32c) {
    result.error = SnapshotParseError::kSnapshotCrcMismatch;
    return result;
  }

  // 5) Sequence gate.
  if (header->sequence <= context.last_applied_sequence) {
    result.disposition = SnapshotDisposition::kIgnoreSnapshot;
    result.error = SnapshotParseError::kStaleSequence;
    return result;
  }
  if (header->snapshot_kind == static_cast<std::uint8_t>(SnapshotKind::kFullState)) {
    if (header->base_sequence != 0) {
      result.disposition = SnapshotDisposition::kRejectSnapshot;
      result.error = SnapshotParseError::kInvalidFullStateBaseSequence;
      return result;
    }
  } else if (header->base_sequence != context.last_applied_sequence) {
    result.disposition = SnapshotDisposition::kRejectDelta;
    result.error = SnapshotParseError::kBaseSequenceMismatch;
    return result;
  }

  // 6) Record walk.
  std::size_t offset = kSnapshotHeaderSize;
  std::size_t parsed_record_bytes = 0;
  result.records.clear();
  result.records.reserve(header->record_count);

  for (std::uint32_t record_index = 0; record_index < header->record_count;
       ++record_index) {
    result.failure_record_index = record_index;
    if (offset + kRecordEnvelopeSize > snapshot_size) {
      result.error = SnapshotParseError::kRecordTruncated;
      return result;
    }

    const auto* envelope = ReadUnaligned<RecordEnvelopeWire>(snapshot_bytes + offset);
    const std::uint16_t record_size = envelope->record_size;
    const std::uint16_t payload_size = envelope->payload_size;
    const std::size_t record_end = offset + static_cast<std::size_t>(record_size);

    if (record_size < kRecordEnvelopeSize || record_size > kMaxRecordBytes) {
      result.error = SnapshotParseError::kRecordSizeOutOfRange;
      return result;
    }
    if (payload_size > kMaxPayloadBytes) {
      result.error = SnapshotParseError::kPayloadSizeOutOfRange;
      return result;
    }
    if (record_size != static_cast<std::uint16_t>(kRecordEnvelopeSize + payload_size)) {
      result.error = SnapshotParseError::kRecordSizePayloadMismatch;
      return result;
    }
    if (record_end > snapshot_size) {
      result.error = SnapshotParseError::kRecordOutOfBounds;
      return result;
    }
    if ((envelope->record_flags & ~kRecordFlagOptional) != 0u) {
      result.error = SnapshotParseError::kRecordFlagsInvalid;
      return result;
    }
    if (envelope->schema_version != 1u) {
      result.error = SnapshotParseError::kRecordSchemaVersionInvalid;
      return result;
    }
    if (envelope->reserved0 != 0u) {
      result.error = SnapshotParseError::kRecordReserved0NonZero;
      return result;
    }
    if (envelope->reserved1 != 0u) {
      result.error = SnapshotParseError::kRecordReserved1NonZero;
      return result;
    }
    if (envelope->reserved2 != 0u) {
      result.error = SnapshotParseError::kRecordReserved2NonZero;
      return result;
    }

    const std::uint8_t* payload = snapshot_bytes + offset + kRecordEnvelopeSize;
    if (ComputeCrc32c(payload, payload_size) != envelope->payload_crc32c) {
      result.error = SnapshotParseError::kPayloadCrcMismatch;
      return result;
    }

    bool skipped_unknown_optional = false;
    if (!IsKnownRecordType(envelope->record_type)) {
      if ((envelope->record_flags & kRecordFlagOptional) == 0u) {
        result.error = SnapshotParseError::kUnknownMandatoryRecordType;
        return result;
      }
      skipped_unknown_optional = true;
    } else {
      const PayloadSizeRule payload_rule = PayloadRuleFor(envelope->record_type);
      if (payload_size < payload_rule.min_size_bytes) {
        result.error = SnapshotParseError::kPayloadSizeTooSmallForType;
        return result;
      }

      const RecordType record_type = static_cast<RecordType>(envelope->record_type);
      if (record_type != RecordType::kClearEvent) {
        if (envelope->ttl_ms < 1 || envelope->ttl_ms > kMaxTtlMs) {
          result.error = SnapshotParseError::kRecordTtlOutOfRange;
          return result;
        }
        if (envelope->event_id == 0) {
          result.error = SnapshotParseError::kRecordEventIdInvalid;
          return result;
        }
      }

      switch (record_type) {
        case RecordType::kScanRing: {
          const auto* p = ReadUnaligned<ScanRingPayloadPrefixWire>(payload);
          if (!ValidateCoord(p->center_x_px, header->canvas_width_px) ||
              !ValidateCoord(p->center_y_px, header->canvas_height_px)) {
            result.error = SnapshotParseError::kScanRingCenterOutOfRange;
            return result;
          }
          if (p->radius_px < 1 || p->radius_px > 16384) {
            result.error = SnapshotParseError::kScanRingRadiusOutOfRange;
            return result;
          }
          if (p->stroke_px < 1 || p->stroke_px > 1024) {
            result.error = SnapshotParseError::kScanRingStrokeOutOfRange;
            return result;
          }
          if (p->phase_deg_x100 > 35999) {
            result.error = SnapshotParseError::kScanRingPhaseOutOfRange;
            return result;
          }
          const auto angular_velocity =
              static_cast<std::int32_t>(p->angular_velocity_deg_x100);
          if (angular_velocity < -36000 || angular_velocity > 36000) {
            result.error = SnapshotParseError::kScanRingAngularVelocityOutOfRange;
            return result;
          }
          break;
        }
        case RecordType::kScanArc: {
          const auto* p = ReadUnaligned<ScanArcPayloadPrefixWire>(payload);
          if (!ValidateCoord(p->center_x_px, header->canvas_width_px) ||
              !ValidateCoord(p->center_y_px, header->canvas_height_px)) {
            result.error = SnapshotParseError::kScanArcCenterOutOfRange;
            return result;
          }
          if (p->radius_px < 1 || p->radius_px > 16384) {
            result.error = SnapshotParseError::kScanArcRadiusOutOfRange;
            return result;
          }
          if (p->stroke_px < 1 || p->stroke_px > 1024) {
            result.error = SnapshotParseError::kScanArcStrokeOutOfRange;
            return result;
          }
          if (p->start_deg_x100 > 35999) {
            result.error = SnapshotParseError::kScanArcStartOutOfRange;
            return result;
          }
          if (p->sweep_deg_x100 < 1 || p->sweep_deg_x100 > 36000) {
            result.error = SnapshotParseError::kScanArcSweepOutOfRange;
            return result;
          }
          const auto angular_velocity =
              static_cast<std::int32_t>(p->angular_velocity_deg_x100);
          if (angular_velocity < -36000 || angular_velocity > 36000) {
            result.error = SnapshotParseError::kScanArcAngularVelocityOutOfRange;
            return result;
          }
          if (p->reserved != 0) {
            result.error = SnapshotParseError::kScanArcReservedNonZero;
            return result;
          }
          break;
        }
        case RecordType::kPointAlert: {
          const auto* p = ReadUnaligned<PointAlertPayloadPrefixWire>(payload);
          if (!ValidateCoord(p->x_px, header->canvas_width_px) ||
              !ValidateCoord(p->y_px, header->canvas_height_px)) {
            result.error = SnapshotParseError::kPointAlertPositionOutOfRange;
            return result;
          }
          if (p->size_px < 1 || p->size_px > 1024) {
            result.error = SnapshotParseError::kPointAlertSizeOutOfRange;
            return result;
          }
          if (p->pulse_period_ms < 16 || p->pulse_period_ms > 60000) {
            result.error = SnapshotParseError::kPointAlertPulseOutOfRange;
            return result;
          }
          if (p->severity > 5) {
            result.error = SnapshotParseError::kPointAlertSeverityOutOfRange;
            return result;
          }
          if (p->z_index > 31) {
            result.error = SnapshotParseError::kPointAlertZIndexOutOfRange;
            return result;
          }
          break;
        }
        case RecordType::kAreaAlert: {
          const auto* p = ReadUnaligned<AreaAlertPayloadPrefixWire>(payload);
          if (!(p->x_min_px < p->x_max_px) || !(p->y_min_px < p->y_max_px) ||
              p->x_max_px > header->canvas_width_px ||
              p->y_max_px > header->canvas_height_px) {
            result.error = SnapshotParseError::kAreaAlertBoundsInvalid;
            return result;
          }
          if (p->border_px > 1024) {
            result.error = SnapshotParseError::kAreaAlertBorderOutOfRange;
            return result;
          }
          if (p->severity > 5) {
            result.error = SnapshotParseError::kAreaAlertSeverityOutOfRange;
            return result;
          }
          break;
        }
        case RecordType::kClearEvent: {
          const auto* p = ReadUnaligned<ClearEventPayloadPrefixWire>(payload);
          if (p->reserved != 0) {
            result.error = SnapshotParseError::kClearEventReservedNonZero;
            return result;
          }
          if (envelope->event_id != 0) {
            result.error = SnapshotParseError::kClearEventEnvelopeEventIdNonZero;
            return result;
          }
          if (envelope->ttl_ms != 1) {
            result.error = SnapshotParseError::kClearEventEnvelopeTtlInvalid;
            return result;
          }
          if (envelope->created_unix_ms != header->generated_unix_ms) {
            result.error = SnapshotParseError::kClearEventCreatedTimestampInvalid;
            return result;
          }
          switch (static_cast<ClearScope>(p->clear_scope)) {
            case ClearScope::kAll:
              if (p->target_type != 0 || p->target_event_id != 0) {
                result.error = SnapshotParseError::kClearEventTargetEventIdInvalid;
                return result;
              }
              break;
            case ClearScope::kByType:
              if (!IsValidClearTargetType(p->target_type)) {
                result.error = SnapshotParseError::kClearEventTargetTypeInvalid;
                return result;
              }
              if (p->target_event_id != 0) {
                result.error = SnapshotParseError::kClearEventTargetEventIdInvalid;
                return result;
              }
              break;
            case ClearScope::kByEventId:
              if (p->target_type != 0) {
                result.error = SnapshotParseError::kClearEventTargetTypeInvalid;
                return result;
              }
              if (p->target_event_id == 0) {
                result.error = SnapshotParseError::kClearEventTargetEventIdInvalid;
                return result;
              }
              break;
            default:
              result.error = SnapshotParseError::kClearEventScopeInvalid;
              return result;
          }
          break;
        }
      }
    }

    SnapshotRecordView view{};
    view.envelope = *envelope;
    view.record_offset_bytes = offset;
    view.payload_offset_bytes = offset + kRecordEnvelopeSize;
    view.skipped_unknown_optional = skipped_unknown_optional;
    result.records.push_back(view);

    parsed_record_bytes += record_size;
    offset = record_end;
  }

  if (result.records.size() != header->record_count) {
    result.error = SnapshotParseError::kRecordCountMismatch;
    return result;
  }
  if (parsed_record_bytes != header->records_bytes) {
    result.error = SnapshotParseError::kRecordBytesMismatch;
    return result;
  }
  if (offset != snapshot_size) {
    result.error = SnapshotParseError::kRecordOutOfBounds;
    return result;
  }

  // 7) Atomic apply (caller-owned), parse succeeded.
  result.disposition = SnapshotDisposition::kApply;
  result.error = SnapshotParseError::kNone;
  result.failure_record_index = result.records.size();
  return result;
}

SnapshotParseResult ParseSnapshotV1(const std::vector<std::uint8_t>& snapshot_bytes,
                                    const SnapshotParseContext& context) noexcept {
  return ParseSnapshotV1(snapshot_bytes.data(), snapshot_bytes.size(), context);
}

const char* ToString(SnapshotParseError error) noexcept {
  switch (error) {
    case SnapshotParseError::kNone:
      return "none";
    case SnapshotParseError::kFrameTooSmall:
      return "frame_too_small";
    case SnapshotParseError::kFrameTooLarge:
      return "frame_too_large";
    case SnapshotParseError::kInvalidMagic:
      return "invalid_magic";
    case SnapshotParseError::kUnsupportedFormatVersion:
      return "unsupported_format_version";
    case SnapshotParseError::kInvalidHeaderSize:
      return "invalid_header_size";
    case SnapshotParseError::kInvalidSnapshotKind:
      return "invalid_snapshot_kind";
    case SnapshotParseError::kNonZeroHeaderFlags:
      return "non_zero_header_flags";
    case SnapshotParseError::kNonZeroHeaderReserved0:
      return "non_zero_header_reserved0";
    case SnapshotParseError::kNonZeroHeaderReserved1:
      return "non_zero_header_reserved1";
    case SnapshotParseError::kCanvasWidthOutOfRange:
      return "canvas_width_out_of_range";
    case SnapshotParseError::kCanvasHeightOutOfRange:
      return "canvas_height_out_of_range";
    case SnapshotParseError::kRecordCountOutOfRange:
      return "record_count_out_of_range";
    case SnapshotParseError::kRecordsBytesOutOfRange:
      return "records_bytes_out_of_range";
    case SnapshotParseError::kFrameSizeMismatch:
      return "frame_size_mismatch";
    case SnapshotParseError::kHeaderCrcMismatch:
      return "header_crc_mismatch";
    case SnapshotParseError::kSnapshotCrcMismatch:
      return "snapshot_crc_mismatch";
    case SnapshotParseError::kStaleSequence:
      return "stale_sequence";
    case SnapshotParseError::kInvalidFullStateBaseSequence:
      return "invalid_full_state_base_sequence";
    case SnapshotParseError::kBaseSequenceMismatch:
      return "base_sequence_mismatch";
    case SnapshotParseError::kRecordTruncated:
      return "record_truncated";
    case SnapshotParseError::kRecordSizeOutOfRange:
      return "record_size_out_of_range";
    case SnapshotParseError::kRecordSizePayloadMismatch:
      return "record_size_payload_mismatch";
    case SnapshotParseError::kRecordOutOfBounds:
      return "record_out_of_bounds";
    case SnapshotParseError::kRecordFlagsInvalid:
      return "record_flags_invalid";
    case SnapshotParseError::kRecordSchemaVersionInvalid:
      return "record_schema_version_invalid";
    case SnapshotParseError::kRecordReserved0NonZero:
      return "record_reserved0_non_zero";
    case SnapshotParseError::kRecordReserved1NonZero:
      return "record_reserved1_non_zero";
    case SnapshotParseError::kRecordReserved2NonZero:
      return "record_reserved2_non_zero";
    case SnapshotParseError::kPayloadSizeOutOfRange:
      return "payload_size_out_of_range";
    case SnapshotParseError::kPayloadSizeTooSmallForType:
      return "payload_size_too_small_for_type";
    case SnapshotParseError::kPayloadCrcMismatch:
      return "payload_crc_mismatch";
    case SnapshotParseError::kUnknownMandatoryRecordType:
      return "unknown_mandatory_record_type";
    case SnapshotParseError::kRecordTtlOutOfRange:
      return "record_ttl_out_of_range";
    case SnapshotParseError::kRecordCountMismatch:
      return "record_count_mismatch";
    case SnapshotParseError::kRecordBytesMismatch:
      return "record_bytes_mismatch";
    case SnapshotParseError::kRecordEventIdInvalid:
      return "record_event_id_invalid";
    case SnapshotParseError::kScanRingCenterOutOfRange:
      return "scan_ring_center_out_of_range";
    case SnapshotParseError::kScanRingRadiusOutOfRange:
      return "scan_ring_radius_out_of_range";
    case SnapshotParseError::kScanRingStrokeOutOfRange:
      return "scan_ring_stroke_out_of_range";
    case SnapshotParseError::kScanRingPhaseOutOfRange:
      return "scan_ring_phase_out_of_range";
    case SnapshotParseError::kScanRingAngularVelocityOutOfRange:
      return "scan_ring_angular_velocity_out_of_range";
    case SnapshotParseError::kScanArcCenterOutOfRange:
      return "scan_arc_center_out_of_range";
    case SnapshotParseError::kScanArcRadiusOutOfRange:
      return "scan_arc_radius_out_of_range";
    case SnapshotParseError::kScanArcStrokeOutOfRange:
      return "scan_arc_stroke_out_of_range";
    case SnapshotParseError::kScanArcStartOutOfRange:
      return "scan_arc_start_out_of_range";
    case SnapshotParseError::kScanArcSweepOutOfRange:
      return "scan_arc_sweep_out_of_range";
    case SnapshotParseError::kScanArcAngularVelocityOutOfRange:
      return "scan_arc_angular_velocity_out_of_range";
    case SnapshotParseError::kScanArcReservedNonZero:
      return "scan_arc_reserved_non_zero";
    case SnapshotParseError::kPointAlertPositionOutOfRange:
      return "point_alert_position_out_of_range";
    case SnapshotParseError::kPointAlertSizeOutOfRange:
      return "point_alert_size_out_of_range";
    case SnapshotParseError::kPointAlertPulseOutOfRange:
      return "point_alert_pulse_out_of_range";
    case SnapshotParseError::kPointAlertSeverityOutOfRange:
      return "point_alert_severity_out_of_range";
    case SnapshotParseError::kPointAlertZIndexOutOfRange:
      return "point_alert_z_index_out_of_range";
    case SnapshotParseError::kAreaAlertBoundsInvalid:
      return "area_alert_bounds_invalid";
    case SnapshotParseError::kAreaAlertBorderOutOfRange:
      return "area_alert_border_out_of_range";
    case SnapshotParseError::kAreaAlertSeverityOutOfRange:
      return "area_alert_severity_out_of_range";
    case SnapshotParseError::kClearEventScopeInvalid:
      return "clear_event_scope_invalid";
    case SnapshotParseError::kClearEventTargetTypeInvalid:
      return "clear_event_target_type_invalid";
    case SnapshotParseError::kClearEventTargetEventIdInvalid:
      return "clear_event_target_event_id_invalid";
    case SnapshotParseError::kClearEventReservedNonZero:
      return "clear_event_reserved_non_zero";
    case SnapshotParseError::kClearEventEnvelopeEventIdNonZero:
      return "clear_event_envelope_event_id_non_zero";
    case SnapshotParseError::kClearEventEnvelopeTtlInvalid:
      return "clear_event_envelope_ttl_invalid";
    case SnapshotParseError::kClearEventCreatedTimestampInvalid:
      return "clear_event_created_timestamp_invalid";
  }
  return "unknown_error";
}

}  // namespace ntium::ingest
