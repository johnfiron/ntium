#include "src/ipc/PipeProtocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace ntium::ipc {
namespace {

constexpr std::size_t kHeaderCrcCoveredBytes = 28U;
constexpr std::size_t kHelloPayloadBytes = 24U;
constexpr std::size_t kHelloAckPayloadBytes = 24U;
constexpr std::size_t kCommandEnvelopeBytes = 8U;
constexpr std::size_t kAckPayloadBytes = 24U;
constexpr std::size_t kErrorPayloadBytes = 24U;

constexpr std::uint8_t kAckRequiredBit =
    static_cast<std::uint8_t>(PipeHeaderFlags::kAckRequired);
constexpr std::uint8_t kAllowedHeaderBits = kAckRequiredBit;

PipeValidationResult OkResult() {
  PipeValidationResult result;
  result.ok = true;
  return result;
}

PipeValidationResult ErrorResult(PipeErrorCode code,
                                 std::uint32_t detail_a,
                                 std::uint32_t detail_b,
                                 PipeErrorHandling handling,
                                 bool send_error_frame = true) {
  PipeValidationResult result;
  result.ok = false;
  result.issue = PipeValidationIssue{
      .code = code,
      .detail_a = detail_a,
      .detail_b = detail_b,
      .handling = handling,
      .send_error_frame = send_error_frame,
  };
  return result;
}

PipeValidationResult MalformedResult(PipeFieldId field_id, PipeRuleId rule_id) {
  return ErrorResult(PipeErrorCode::kMalformedPayload,
                     static_cast<std::uint32_t>(field_id),
                     static_cast<std::uint32_t>(rule_id),
                     PipeErrorHandling::kContinue);
}

std::uint32_t ComputeHeaderCrc(const PipeFrameHeader& header) {
  std::array<std::uint8_t, kHeaderCrcCoveredBytes> bytes{};
  WriteU32Le(bytes.data(), 0, header.magic);
  WriteU8(bytes.data(), 4, header.version_major);
  WriteU8(bytes.data(), 5, header.version_minor);
  WriteU8(bytes.data(), 6, header.header_len);
  WriteU8(bytes.data(), 7, static_cast<std::uint8_t>(header.msg_type));
  WriteU8(bytes.data(), 8, header.flags);
  WriteU8(bytes.data(), 9, header.reserved0);
  WriteU16Le(bytes.data(), 10, header.reserved1);
  WriteU32Le(bytes.data(), 12, header.sequence);
  WriteU32Le(bytes.data(), 16, header.ack_sequence);
  WriteU32Le(bytes.data(), 20, header.payload_len);
  WriteU32Le(bytes.data(), 24, header.payload_crc32c);
  return Crc32c(bytes);
}

bool IsFinite(float value) {
  return std::isfinite(value);
}

bool IsFinite(double value) {
  return std::isfinite(value);
}

PipeValidationResult ValidateZoomBody(std::span<const std::uint8_t> body) {
  if (body.size() < 16U) {
    return MalformedResult(PipeFieldId::kZoomAnchorXNorm,
                           PipeRuleId::kInvalidBodySize);
  }

  const float anchor_x = ReadF32Le(body, 4U);
  if (!IsFinite(anchor_x)) {
    return MalformedResult(PipeFieldId::kZoomAnchorXNorm, PipeRuleId::kNotFinite);
  }
  if (anchor_x < 0.0F || anchor_x > 1.0F) {
    return MalformedResult(PipeFieldId::kZoomAnchorXNorm,
                           PipeRuleId::kOutOfRangeInclusive);
  }

  const float anchor_y = ReadF32Le(body, 8U);
  if (!IsFinite(anchor_y)) {
    return MalformedResult(PipeFieldId::kZoomAnchorYNorm, PipeRuleId::kNotFinite);
  }
  if (anchor_y < 0.0F || anchor_y > 1.0F) {
    return MalformedResult(PipeFieldId::kZoomAnchorYNorm,
                           PipeRuleId::kOutOfRangeInclusive);
  }

  const std::uint8_t mode = ReadU8(body, 12U);
  if (mode > 1U) {
    return MalformedResult(PipeFieldId::kZoomMode, PipeRuleId::kEnumUnknown);
  }

  const std::uint8_t clamp_to_surface = ReadU8(body, 13U);
  if (clamp_to_surface > 1U) {
    return MalformedResult(PipeFieldId::kZoomClampToSurface,
                           PipeRuleId::kEnumUnknown);
  }

  if (ReadU16Le(body, 14U) != 0U) {
    return MalformedResult(PipeFieldId::kZoomClampToSurface,
                           PipeRuleId::kReservedFieldNonZero);
  }

  return OkResult();
}

PipeValidationResult ValidateRotateBody(std::span<const std::uint8_t> body) {
  if (body.size() < 16U) {
    return MalformedResult(PipeFieldId::kRotateSpace, PipeRuleId::kInvalidBodySize);
  }

  if (!IsFinite(ReadF32Le(body, 0U)) || !IsFinite(ReadF32Le(body, 4U)) ||
      !IsFinite(ReadF32Le(body, 8U))) {
    return MalformedResult(PipeFieldId::kRotateSpace, PipeRuleId::kNotFinite);
  }

  const std::uint8_t space = ReadU8(body, 12U);
  if (space > 1U) {
    return MalformedResult(PipeFieldId::kRotateSpace, PipeRuleId::kEnumUnknown);
  }

  if (ReadU8(body, 13U) != 0U || ReadU8(body, 14U) != 0U ||
      ReadU8(body, 15U) != 0U) {
    return MalformedResult(PipeFieldId::kRotateSpace,
                           PipeRuleId::kReservedFieldNonZero);
  }

  return OkResult();
}

PipeValidationResult ValidateStyleBody(std::span<const std::uint8_t> body) {
  if (body.size() < 12U) {
    return MalformedResult(PipeFieldId::kStyleStyleId, PipeRuleId::kInvalidBodySize);
  }

  const std::uint32_t style_id = ReadU32Le(body, 0U);
  if (style_id < 1U || style_id > 4U) {
    return MalformedResult(PipeFieldId::kStyleStyleId, PipeRuleId::kEnumUnknown);
  }

  const float transition_sec = ReadF32Le(body, 4U);
  if (!IsFinite(transition_sec)) {
    return MalformedResult(PipeFieldId::kStyleTransitionSec, PipeRuleId::kNotFinite);
  }
  if (transition_sec < 0.0F) {
    return MalformedResult(PipeFieldId::kStyleTransitionSec,
                           PipeRuleId::kOutOfRangeInclusive);
  }

  return OkResult();
}

PipeValidationResult ValidateSpanBody(std::span<const std::uint8_t> body) {
  if (body.size() < 8U) {
    return MalformedResult(PipeFieldId::kSpanSpanMode, PipeRuleId::kInvalidBodySize);
  }
  const std::size_t trailing_bytes = body.size() - 8U;
  if ((trailing_bytes % 4U) != 0U) {
    return MalformedResult(PipeFieldId::kSpanMonitorCount,
                           PipeRuleId::kCountArrayMismatch);
  }

  const std::uint8_t span_mode = ReadU8(body, 0U);
  const std::uint8_t monitor_count = ReadU8(body, 1U);
  const std::uint16_t reserved = ReadU16Le(body, 2U);
  const std::uint32_t primary_monitor_id = ReadU32Le(body, 4U);
  const std::size_t monitor_id_count = trailing_bytes / 4U;

  if (span_mode > 2U) {
    return MalformedResult(PipeFieldId::kSpanSpanMode, PipeRuleId::kEnumUnknown);
  }
  if (reserved != 0U) {
    return MalformedResult(PipeFieldId::kSpanMonitorCount,
                           PipeRuleId::kReservedFieldNonZero);
  }

  if (span_mode == 0U || span_mode == 1U) {
    if (monitor_count != 0U || monitor_id_count != 0U) {
      return MalformedResult(PipeFieldId::kSpanMonitorCount,
                             PipeRuleId::kCountArrayMismatch);
    }
    return OkResult();
  }

  if (monitor_count < 1U || monitor_count > 16U) {
    return MalformedResult(PipeFieldId::kSpanMonitorCount,
                           PipeRuleId::kOutOfRangeInclusive);
  }
  if (monitor_id_count != monitor_count) {
    return MalformedResult(PipeFieldId::kSpanMonitorCount,
                           PipeRuleId::kCountArrayMismatch);
  }

  bool primary_found = false;
  for (std::size_t index = 0; index < monitor_id_count; ++index) {
    if (ReadU32Le(body, 8U + (index * 4U)) == primary_monitor_id) {
      primary_found = true;
      break;
    }
  }
  if (!primary_found) {
    return MalformedResult(PipeFieldId::kSpanPrimaryMonitorId,
                           PipeRuleId::kRequiredIdMissingFromList);
  }

  return OkResult();
}

PipeValidationResult ValidateMirrorBody(std::span<const std::uint8_t> body) {
  if (body.size() < 8U) {
    return MalformedResult(PipeFieldId::kMirrorEnabled, PipeRuleId::kInvalidBodySize);
  }
  const std::size_t trailing_bytes = body.size() - 8U;
  if ((trailing_bytes % 4U) != 0U) {
    return MalformedResult(PipeFieldId::kMirrorTargetCount,
                           PipeRuleId::kCountArrayMismatch);
  }

  const std::uint8_t enabled = ReadU8(body, 0U);
  const std::uint8_t target_count = ReadU8(body, 1U);
  const std::uint16_t reserved = ReadU16Le(body, 2U);
  const std::uint32_t source_monitor_id = ReadU32Le(body, 4U);
  const std::size_t target_id_count = trailing_bytes / 4U;

  if (enabled > 1U) {
    return MalformedResult(PipeFieldId::kMirrorEnabled, PipeRuleId::kEnumUnknown);
  }
  if (reserved != 0U) {
    return MalformedResult(PipeFieldId::kMirrorTargetCount,
                           PipeRuleId::kReservedFieldNonZero);
  }

  if (enabled == 0U) {
    if (target_count != 0U || target_id_count != 0U) {
      return MalformedResult(PipeFieldId::kMirrorTargetCount,
                             PipeRuleId::kCountArrayMismatch);
    }
    return OkResult();
  }

  if (target_count < 1U || target_count > 15U) {
    return MalformedResult(PipeFieldId::kMirrorTargetCount,
                           PipeRuleId::kOutOfRangeInclusive);
  }
  if (target_id_count != target_count) {
    return MalformedResult(PipeFieldId::kMirrorTargetCount,
                           PipeRuleId::kCountArrayMismatch);
  }

  std::unordered_set<std::uint32_t> seen_targets;
  seen_targets.reserve(target_id_count);
  for (std::size_t index = 0; index < target_id_count; ++index) {
    const std::uint32_t target_id = ReadU32Le(body, 8U + (index * 4U));
    if (target_id == source_monitor_id) {
      return MalformedResult(PipeFieldId::kMirrorSourceMonitorId,
                             PipeRuleId::kSourceIdInTargetList);
    }
    if (!seen_targets.insert(target_id).second) {
      return MalformedResult(PipeFieldId::kMirrorTargetCount,
                             PipeRuleId::kDuplicateMonitorId);
    }
  }

  return OkResult();
}

PipeValidationResult ValidateCameraMoveBody(std::span<const std::uint8_t> body) {
  if (body.size() < 36U) {
    return MalformedResult(PipeFieldId::kCameraMoveMoveKind,
                           PipeRuleId::kInvalidBodySize);
  }

  const std::uint8_t move_kind = ReadU8(body, 0U);
  const std::uint8_t interpolation = ReadU8(body, 1U);
  const std::uint16_t reserved0 = ReadU16Le(body, 2U);
  const double x = ReadF64Le(body, 4U);
  const double y = ReadF64Le(body, 12U);
  const double z = ReadF64Le(body, 20U);
  const float duration_ms = ReadF32Le(body, 28U);
  const std::uint8_t collision_policy = ReadU8(body, 32U);

  if (move_kind > 2U) {
    return MalformedResult(PipeFieldId::kCameraMoveMoveKind,
                           PipeRuleId::kEnumUnknown);
  }
  if (interpolation > 2U) {
    return MalformedResult(PipeFieldId::kCameraMoveInterpolation,
                           PipeRuleId::kEnumUnknown);
  }
  if (reserved0 != 0U) {
    return MalformedResult(PipeFieldId::kCameraMoveInterpolation,
                           PipeRuleId::kReservedFieldNonZero);
  }
  if (!IsFinite(x) || !IsFinite(y) || !IsFinite(z)) {
    return MalformedResult(PipeFieldId::kCameraMoveMoveKind, PipeRuleId::kNotFinite);
  }
  if (!IsFinite(duration_ms)) {
    return MalformedResult(PipeFieldId::kCameraMoveDurationMs, PipeRuleId::kNotFinite);
  }
  if ((interpolation == 0U && duration_ms != 0.0F) ||
      (interpolation != 0U && duration_ms <= 0.0F)) {
    return MalformedResult(PipeFieldId::kCameraMoveDurationMs,
                           PipeRuleId::kOutOfRangeInclusive);
  }
  if (collision_policy > 1U) {
    return MalformedResult(PipeFieldId::kCameraMoveCollisionPolicy,
                           PipeRuleId::kEnumUnknown);
  }
  if (ReadU8(body, 33U) != 0U || ReadU8(body, 34U) != 0U ||
      ReadU8(body, 35U) != 0U) {
    return MalformedResult(PipeFieldId::kCameraMoveCollisionPolicy,
                           PipeRuleId::kReservedFieldNonZero);
  }

  return OkResult();
}

}  // namespace

std::uint32_t ReadU32Le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint16_t ReadU16Le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

std::uint8_t ReadU8(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return bytes[offset];
}

float ReadF32Le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  const std::uint32_t bits = ReadU32Le(bytes, offset);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double ReadF64Le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  const std::uint64_t low = ReadU32Le(bytes, offset);
  const std::uint64_t high = ReadU32Le(bytes, offset + 4U);
  const std::uint64_t bits = low | (high << 32U);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteU32Le(std::uint8_t* out, std::size_t offset, std::uint32_t value) {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  out[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  out[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

void WriteU16Le(std::uint8_t* out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void WriteU8(std::uint8_t* out, std::size_t offset, std::uint8_t value) {
  out[offset] = value;
}

std::uint32_t Crc32c(std::span<const std::uint8_t> bytes) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> generated{};
    for (std::uint32_t i = 0U; i < generated.size(); ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        if ((value & 1U) != 0U) {
          value = (value >> 1U) ^ 0x82F63B78U;
        } else {
          value >>= 1U;
        }
      }
      generated[i] = value;
    }
    return generated;
  }();

  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : bytes) {
    const std::uint8_t index = static_cast<std::uint8_t>((crc ^ byte) & 0xFFU);
    crc = table[index] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::array<std::uint8_t, kPipeHeaderBytes> EncodeFrameHeader(
    const PipeFrameHeader& header) {
  std::array<std::uint8_t, kPipeHeaderBytes> bytes{};
  WriteU32Le(bytes.data(), 0U, header.magic);
  WriteU8(bytes.data(), 4U, header.version_major);
  WriteU8(bytes.data(), 5U, header.version_minor);
  WriteU8(bytes.data(), 6U, header.header_len);
  WriteU8(bytes.data(), 7U, static_cast<std::uint8_t>(header.msg_type));
  WriteU8(bytes.data(), 8U, header.flags);
  WriteU8(bytes.data(), 9U, header.reserved0);
  WriteU16Le(bytes.data(), 10U, header.reserved1);
  WriteU32Le(bytes.data(), 12U, header.sequence);
  WriteU32Le(bytes.data(), 16U, header.ack_sequence);
  WriteU32Le(bytes.data(), 20U, header.payload_len);
  WriteU32Le(bytes.data(), 24U, header.payload_crc32c);
  const std::uint32_t computed_header_crc =
      Crc32c(std::span<const std::uint8_t>(bytes.data(), kHeaderCrcCoveredBytes));
  WriteU32Le(bytes.data(), 28U, computed_header_crc);
  return bytes;
}

bool TryDecodeFrameHeader(std::span<const std::uint8_t> bytes,
                          PipeFrameHeader* header) {
  if (header == nullptr || bytes.size() < kPipeHeaderBytes) {
    return false;
  }

  header->magic = ReadU32Le(bytes, 0U);
  header->version_major = ReadU8(bytes, 4U);
  header->version_minor = ReadU8(bytes, 5U);
  header->header_len = ReadU8(bytes, 6U);
  header->msg_type = static_cast<PipeMessageType>(ReadU8(bytes, 7U));
  header->flags = ReadU8(bytes, 8U);
  header->reserved0 = ReadU8(bytes, 9U);
  header->reserved1 = ReadU16Le(bytes, 10U);
  header->sequence = ReadU32Le(bytes, 12U);
  header->ack_sequence = ReadU32Le(bytes, 16U);
  header->payload_len = ReadU32Le(bytes, 20U);
  header->payload_crc32c = ReadU32Le(bytes, 24U);
  header->header_crc32c = ReadU32Le(bytes, 28U);
  return true;
}

bool IsKnownMessageType(std::uint8_t value) {
  switch (static_cast<PipeMessageType>(value)) {
    case PipeMessageType::kHello:
    case PipeMessageType::kHelloAck:
    case PipeMessageType::kPing:
    case PipeMessageType::kPong:
    case PipeMessageType::kCommand:
    case PipeMessageType::kAck:
    case PipeMessageType::kError:
      return true;
    default:
      return false;
  }
}

bool IsKnownCommandId(std::uint16_t value) {
  switch (static_cast<PipeCommandId>(value)) {
    case PipeCommandId::kZoom:
    case PipeCommandId::kRotate:
    case PipeCommandId::kStyle:
    case PipeCommandId::kSpan:
    case PipeCommandId::kMirror:
    case PipeCommandId::kCameraMove:
      return true;
    default:
      return false;
  }
}

bool IsAckRequired(std::uint8_t flags) {
  return (flags & kAckRequiredBit) != 0U;
}

std::size_t MinimumPayloadSizeForMessage(PipeMessageType type) {
  switch (type) {
    case PipeMessageType::kHello:
      return kHelloPayloadBytes;
    case PipeMessageType::kHelloAck:
      return kHelloAckPayloadBytes;
    case PipeMessageType::kPing:
    case PipeMessageType::kPong:
      return 0U;
    case PipeMessageType::kCommand:
      return kCommandEnvelopeBytes;
    case PipeMessageType::kAck:
      return kAckPayloadBytes;
    case PipeMessageType::kError:
      return kErrorPayloadBytes;
    default:
      return 0U;
  }
}

std::optional<std::size_t> FixedPayloadSizeForMessage(PipeMessageType type) {
  switch (type) {
    case PipeMessageType::kHello:
      return kHelloPayloadBytes;
    case PipeMessageType::kHelloAck:
      return kHelloAckPayloadBytes;
    case PipeMessageType::kPing:
    case PipeMessageType::kPong:
      return 0U;
    case PipeMessageType::kAck:
      return kAckPayloadBytes;
    case PipeMessageType::kError:
      return kErrorPayloadBytes;
    case PipeMessageType::kCommand:
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

bool HeaderHasReservedFlagBits(std::uint8_t flags) {
  return (flags & ~kAllowedHeaderBits) != 0U;
}

PipeValidationResult ValidateFrameHeader(const PipeFrameHeader& header,
                                         const PipeProtocolLimits& limits) {
  if (header.magic != kPipeProtocolMagic) {
    return ErrorResult(PipeErrorCode::kUnsupportedVersion,
                       header.version_major,
                       header.version_minor,
                       PipeErrorHandling::kCloseAfterSend);
  }

  if (header.header_len != kPipeHeaderBytes) {
    return ErrorResult(PipeErrorCode::kBadHeaderLen,
                       header.header_len,
                       kPipeHeaderBytes,
                       PipeErrorHandling::kCloseAfterSend);
  }

  if (header.version_major != kPipeProtocolMajor ||
      header.version_minor != kPipeProtocolMinor) {
    return ErrorResult(PipeErrorCode::kUnsupportedVersion,
                       header.version_major,
                       header.version_minor,
                       PipeErrorHandling::kCloseAfterSend);
  }

  const std::uint32_t expected_crc = ComputeHeaderCrc(header);
  if (expected_crc != header.header_crc32c) {
    return ErrorResult(PipeErrorCode::kBadHeaderCrc,
                       expected_crc,
                       header.header_crc32c,
                       PipeErrorHandling::kCloseAfterSend,
                       false);
  }

  if (HeaderHasReservedFlagBits(header.flags)) {
    return MalformedResult(PipeFieldId::kHeaderFlagsReservedBits,
                           PipeRuleId::kReservedFieldNonZero);
  }
  if (header.reserved0 != 0U) {
    return MalformedResult(PipeFieldId::kHeaderReserved0,
                           PipeRuleId::kReservedFieldNonZero);
  }
  if (header.reserved1 != 0U) {
    return MalformedResult(PipeFieldId::kHeaderReserved1,
                           PipeRuleId::kReservedFieldNonZero);
  }

  if (!IsKnownMessageType(static_cast<std::uint8_t>(header.msg_type))) {
    return ErrorResult(PipeErrorCode::kUnknownMsgType,
                       static_cast<std::uint8_t>(header.msg_type),
                       0U,
                       PipeErrorHandling::kContinue);
  }

  if (header.payload_len > limits.max_payload_bytes) {
    return ErrorResult(PipeErrorCode::kPayloadTooLarge,
                       header.payload_len,
                       limits.max_payload_bytes,
                       PipeErrorHandling::kCloseAfterSend);
  }

  const std::uint64_t frame_bytes =
      static_cast<std::uint64_t>(header.header_len) +
      static_cast<std::uint64_t>(header.payload_len);
  if (frame_bytes > limits.max_frame_bytes) {
    return ErrorResult(PipeErrorCode::kPayloadTooLarge,
                       static_cast<std::uint32_t>(
                           std::min<std::uint64_t>(frame_bytes,
                                                   std::numeric_limits<std::uint32_t>::max())),
                       limits.max_frame_bytes,
                       PipeErrorHandling::kCloseAfterSend);
  }

  if (header.payload_len == 0U && header.payload_crc32c != 0U) {
    return ErrorResult(PipeErrorCode::kBadPayloadCrc,
                       0U,
                       header.payload_crc32c,
                       PipeErrorHandling::kContinue);
  }

  return OkResult();
}

PipeValidationResult ValidatePayloadCrc(const PipeFrameHeader& header,
                                        std::span<const std::uint8_t> payload) {
  if (payload.size() != header.payload_len) {
    return ErrorResult(PipeErrorCode::kMalformedPayload,
                       0U,
                       static_cast<std::uint32_t>(PipeRuleId::kInvalidBodySize),
                       PipeErrorHandling::kContinue);
  }

  if (payload.empty()) {
    if (header.payload_crc32c == 0U) {
      return OkResult();
    }
    return ErrorResult(PipeErrorCode::kBadPayloadCrc,
                       0U,
                       header.payload_crc32c,
                       PipeErrorHandling::kContinue);
  }

  const std::uint32_t expected_crc = Crc32c(payload);
  if (expected_crc != header.payload_crc32c) {
    return ErrorResult(PipeErrorCode::kBadPayloadCrc,
                       expected_crc,
                       header.payload_crc32c,
                       PipeErrorHandling::kContinue);
  }

  return OkResult();
}

PipeValidationResult ValidateMessagePayload(const PipeFrameHeader& header,
                                            std::span<const std::uint8_t> payload) {
  const PipeMessageType type = header.msg_type;
  const std::size_t min_payload_size = MinimumPayloadSizeForMessage(type);
  if (payload.size() < min_payload_size) {
    PipeFieldId field_id = PipeFieldId::kCommandEnvelopeReserved;
    switch (type) {
      case PipeMessageType::kHello:
        field_id = PipeFieldId::kHelloReserved;
        break;
      case PipeMessageType::kHelloAck:
        field_id = PipeFieldId::kHelloAckReserved;
        break;
      case PipeMessageType::kCommand:
        field_id = PipeFieldId::kCommandEnvelopeReserved;
        break;
      default:
        break;
    }
    return MalformedResult(field_id, PipeRuleId::kInvalidBodySize);
  }

  const std::optional<std::size_t> fixed_payload_size =
      FixedPayloadSizeForMessage(type);
  if (fixed_payload_size.has_value() && payload.size() != *fixed_payload_size) {
    PipeFieldId field_id = PipeFieldId::kCommandEnvelopeReserved;
    if (type == PipeMessageType::kHello) {
      field_id = PipeFieldId::kHelloReserved;
    } else if (type == PipeMessageType::kHelloAck) {
      field_id = PipeFieldId::kHelloAckReserved;
    }
    return MalformedResult(field_id, PipeRuleId::kInvalidBodySize);
  }

  if (type == PipeMessageType::kHello && payload.size() >= kHelloPayloadBytes) {
    if (ReadU32Le(payload, 20U) != 0U) {
      return MalformedResult(PipeFieldId::kHelloReserved,
                             PipeRuleId::kReservedFieldNonZero);
    }
  }

  if (type == PipeMessageType::kHelloAck && payload.size() >= kHelloAckPayloadBytes) {
    if (ReadU32Le(payload, 20U) != 0U) {
      return MalformedResult(PipeFieldId::kHelloAckReserved,
                             PipeRuleId::kReservedFieldNonZero);
    }
  }

  if (type == PipeMessageType::kCommand) {
    return ValidateCommandPayload(payload);
  }

  return OkResult();
}

PipeValidationResult ValidateCommandPayload(std::span<const std::uint8_t> payload,
                                            PipeCommandEnvelopeV1* envelope_out) {
  if (payload.size() < kCommandEnvelopeBytes) {
    return MalformedResult(PipeFieldId::kCommandEnvelopeReserved,
                           PipeRuleId::kInvalidBodySize);
  }

  PipeCommandEnvelopeV1 envelope;
  envelope.command_id = ReadU16Le(payload, 0U);
  envelope.command_version = ReadU8(payload, 2U);
  envelope.command_flags = ReadU8(payload, 3U);
  envelope.reserved = ReadU32Le(payload, 4U);

  if (envelope_out != nullptr) {
    *envelope_out = envelope;
  }

  if (!IsKnownCommandId(envelope.command_id)) {
    return ErrorResult(PipeErrorCode::kUnknownCommand,
                       envelope.command_id,
                       0U,
                       PipeErrorHandling::kContinue);
  }

  if (envelope.command_version != 1U) {
    return ErrorResult(PipeErrorCode::kUnsupportedCommandVersion,
                       envelope.command_id,
                       envelope.command_version,
                       PipeErrorHandling::kContinue);
  }

  if (envelope.reserved != 0U) {
    return MalformedResult(PipeFieldId::kCommandEnvelopeReserved,
                           PipeRuleId::kReservedFieldNonZero);
  }

  const std::span<const std::uint8_t> body = payload.subspan(kCommandEnvelopeBytes);
  switch (static_cast<PipeCommandId>(envelope.command_id)) {
    case PipeCommandId::kZoom:
      return ValidateZoomBody(body);
    case PipeCommandId::kRotate:
      return ValidateRotateBody(body);
    case PipeCommandId::kStyle:
      return ValidateStyleBody(body);
    case PipeCommandId::kSpan:
      return ValidateSpanBody(body);
    case PipeCommandId::kMirror:
      return ValidateMirrorBody(body);
    case PipeCommandId::kCameraMove:
      return ValidateCameraMoveBody(body);
    default:
      return ErrorResult(PipeErrorCode::kUnknownCommand,
                         envelope.command_id,
                         0U,
                         PipeErrorHandling::kContinue);
  }
}

bool TryParseFrame(std::span<const std::uint8_t> frame_bytes,
                   PipeFrame* frame_out,
                   PipeValidationIssue* issue_out,
                   const PipeProtocolLimits& limits) {
  if (frame_out == nullptr) {
    return false;
  }

  if (frame_bytes.size() < kPipeHeaderBytes) {
    if (issue_out != nullptr) {
      *issue_out = PipeValidationIssue{
          .code = PipeErrorCode::kBadHeaderLen,
          .detail_a = static_cast<std::uint32_t>(frame_bytes.size()),
          .detail_b = kPipeHeaderBytes,
          .handling = PipeErrorHandling::kCloseAfterSend,
          .send_error_frame = true,
      };
    }
    return false;
  }

  PipeFrameHeader header;
  if (!TryDecodeFrameHeader(frame_bytes, &header)) {
    return false;
  }

  const PipeValidationResult header_validation = ValidateFrameHeader(header, limits);
  if (!header_validation.ok) {
    if (issue_out != nullptr && header_validation.issue.has_value()) {
      *issue_out = *header_validation.issue;
    }
    return false;
  }

  const std::uint64_t expected_frame_size =
      static_cast<std::uint64_t>(header.header_len) +
      static_cast<std::uint64_t>(header.payload_len);
  if (frame_bytes.size() < expected_frame_size) {
    if (issue_out != nullptr) {
      *issue_out = PipeValidationIssue{
          .code = PipeErrorCode::kMalformedPayload,
          .detail_a = 0U,
          .detail_b = static_cast<std::uint32_t>(PipeRuleId::kInvalidBodySize),
          .handling = PipeErrorHandling::kContinue,
          .send_error_frame = true,
      };
    }
    return false;
  }

  const std::span<const std::uint8_t> payload =
      frame_bytes.subspan(header.header_len, header.payload_len);

  const PipeValidationResult payload_crc_validation =
      ValidatePayloadCrc(header, payload);
  if (!payload_crc_validation.ok) {
    if (issue_out != nullptr && payload_crc_validation.issue.has_value()) {
      *issue_out = *payload_crc_validation.issue;
    }
    return false;
  }

  const PipeValidationResult payload_validation =
      ValidateMessagePayload(header, payload);
  if (!payload_validation.ok) {
    if (issue_out != nullptr && payload_validation.issue.has_value()) {
      *issue_out = *payload_validation.issue;
    }
    return false;
  }

  frame_out->header = header;
  frame_out->payload.assign(payload.begin(), payload.end());
  return true;
}

std::string MessageTypeToString(PipeMessageType type) {
  switch (type) {
    case PipeMessageType::kHello:
      return "HELLO";
    case PipeMessageType::kHelloAck:
      return "HELLO_ACK";
    case PipeMessageType::kPing:
      return "PING";
    case PipeMessageType::kPong:
      return "PONG";
    case PipeMessageType::kCommand:
      return "COMMAND";
    case PipeMessageType::kAck:
      return "ACK";
    case PipeMessageType::kError:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

std::string ErrorCodeToString(PipeErrorCode code) {
  switch (code) {
    case PipeErrorCode::kUnsupportedVersion:
      return "ERR_UNSUPPORTED_VERSION";
    case PipeErrorCode::kBadHeaderLen:
      return "ERR_BAD_HEADER_LEN";
    case PipeErrorCode::kBadHeaderCrc:
      return "ERR_BAD_HEADER_CRC";
    case PipeErrorCode::kBadPayloadCrc:
      return "ERR_BAD_PAYLOAD_CRC";
    case PipeErrorCode::kPayloadTooLarge:
      return "ERR_PAYLOAD_TOO_LARGE";
    case PipeErrorCode::kUnknownMsgType:
      return "ERR_UNKNOWN_MSG_TYPE";
    case PipeErrorCode::kProtocolState:
      return "ERR_PROTOCOL_STATE";
    case PipeErrorCode::kUnknownCommand:
      return "ERR_UNKNOWN_COMMAND";
    case PipeErrorCode::kUnsupportedCommandVersion:
      return "ERR_UNSUPPORTED_COMMAND_VERSION";
    case PipeErrorCode::kMalformedPayload:
      return "ERR_MALFORMED_PAYLOAD";
    case PipeErrorCode::kRateLimited:
      return "ERR_RATE_LIMITED";
    case PipeErrorCode::kTooManyInFlight:
      return "ERR_TOO_MANY_IN_FLIGHT";
    case PipeErrorCode::kAuthSidMismatch:
      return "ERR_AUTH_SID_MISMATCH";
    case PipeErrorCode::kAuthSessionMismatch:
      return "ERR_AUTH_SESSION_MISMATCH";
    case PipeErrorCode::kRemoteClientRejected:
      return "ERR_REMOTE_CLIENT_REJECTED";
    case PipeErrorCode::kInternal:
      return "ERR_INTERNAL";
    default:
      return "ERR_UNKNOWN";
  }
}

}  // namespace ntium::ipc
