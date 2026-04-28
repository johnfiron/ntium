#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ntium::ipc {

inline constexpr std::uint32_t kPipeProtocolMagic = 0x31435049U;  // "IPC1"
inline constexpr std::string_view kPipeNameV1 =
    R"(\\.\pipe\ntium.wallpaper.ctrl.v1)";
inline constexpr std::uint8_t kPipeProtocolMajor = 1U;
inline constexpr std::uint8_t kPipeProtocolMinor = 0U;
inline constexpr std::uint8_t kPipeHeaderBytes = 32U;
inline constexpr std::uint32_t kMaxPayloadBytes = 65536U;
inline constexpr std::uint32_t kMaxFrameBytes = 65568U;
inline constexpr std::uint32_t kMaxInboundFramesPerSec = 120U;
inline constexpr std::uint32_t kMaxInboundPayloadBytesPerSec = 524288U;
inline constexpr std::uint32_t kMaxInFlightCommands = 64U;

enum class PipeHeaderFlags : std::uint8_t {
  kAckRequired = 0x01U,
  kUrgent = 0x02U,
};

enum class PipeMessageType : std::uint8_t {
  kHello = 0x01U,
  kHelloAck = 0x02U,
  kPing = 0x03U,
  kPong = 0x04U,
  kCommand = 0x10U,
  kAck = 0x11U,
  kError = 0x12U,
};

enum class PipeCommandId : std::uint16_t {
  kZoom = 0x0001U,
  kRotate = 0x0002U,
  kStyle = 0x0003U,
  kSpan = 0x0004U,
  kMirror = 0x0005U,
  kCameraMove = 0x0006U,
};

enum class PipeErrorCode : std::uint32_t {
  kUnsupportedVersion = 0x00000001U,
  kBadHeaderLen = 0x00000002U,
  kBadHeaderCrc = 0x00000003U,
  kBadPayloadCrc = 0x00000004U,
  kPayloadTooLarge = 0x00000005U,
  kUnknownMsgType = 0x00000006U,
  kProtocolState = 0x00000007U,
  kUnknownCommand = 0x00000008U,
  kUnsupportedCommandVersion = 0x00000009U,
  kMalformedPayload = 0x0000000AU,
  kRateLimited = 0x0000000BU,
  kTooManyInFlight = 0x0000000CU,
  kAuthSidMismatch = 0x0000000DU,
  kAuthSessionMismatch = 0x0000000EU,
  kRemoteClientRejected = 0x0000000FU,
  kInternal = 0x00000010U,
};

enum class PipeErrorHandling : std::uint8_t {
  kContinue = 0U,
  kCloseAfterSend = 1U,
};

enum class PipeProtocolStateId : std::uint32_t {
  kConnectedWaitHello = 1U,
  kActive = 2U,
  kClosing = 3U,
};

enum class PipeFieldId : std::uint32_t {
  kHeaderFlagsReservedBits = 1U,
  kHeaderReserved0 = 2U,
  kHeaderReserved1 = 3U,
  kHelloReserved = 4U,
  kHelloAckReserved = 5U,
  kCommandEnvelopeReserved = 6U,
  kCommandEnvelopeCommandVersion = 7U,
  kZoomAnchorXNorm = 8U,
  kZoomAnchorYNorm = 9U,
  kZoomMode = 10U,
  kZoomClampToSurface = 11U,
  kRotateSpace = 12U,
  kStyleStyleId = 13U,
  kStyleTransitionSec = 14U,
  kSpanSpanMode = 15U,
  kSpanMonitorCount = 16U,
  kSpanPrimaryMonitorId = 17U,
  kMirrorEnabled = 18U,
  kMirrorTargetCount = 19U,
  kMirrorSourceMonitorId = 20U,
  kCameraMoveMoveKind = 21U,
  kCameraMoveInterpolation = 22U,
  kCameraMoveDurationMs = 23U,
  kCameraMoveCollisionPolicy = 24U,
};

enum class PipeRuleId : std::uint32_t {
  kNotFinite = 1U,
  kOutOfRangeInclusive = 2U,
  kEnumUnknown = 3U,
  kReservedFieldNonZero = 4U,
  kCountArrayMismatch = 5U,
  kDuplicateMonitorId = 6U,
  kSourceIdInTargetList = 7U,
  kRequiredIdMissingFromList = 8U,
  kInvalidBodySize = 9U,
};

struct PipeFrameHeader {
  std::uint32_t magic = kPipeProtocolMagic;
  std::uint8_t version_major = kPipeProtocolMajor;
  std::uint8_t version_minor = kPipeProtocolMinor;
  std::uint8_t header_len = kPipeHeaderBytes;
  PipeMessageType msg_type = PipeMessageType::kHello;
  std::uint8_t flags = 0U;
  std::uint8_t reserved0 = 0U;
  std::uint16_t reserved1 = 0U;
  std::uint32_t sequence = 0U;
  std::uint32_t ack_sequence = 0U;
  std::uint32_t payload_len = 0U;
  std::uint32_t payload_crc32c = 0U;
  std::uint32_t header_crc32c = 0U;
};

struct PipeFrame {
  PipeFrameHeader header;
  std::vector<std::uint8_t> payload;
};

struct PipeErrorFramePayload {
  std::uint32_t failed_sequence = 0U;
  std::uint16_t failed_msg_type = 0U;
  std::uint16_t failed_command_id = 0U;
  PipeErrorCode error_code = PipeErrorCode::kInternal;
  std::uint32_t detail_a = 0U;
  std::uint32_t detail_b = 0U;
  PipeErrorHandling handling = PipeErrorHandling::kContinue;
};

struct PipeValidationIssue {
  PipeErrorCode code = PipeErrorCode::kInternal;
  std::uint32_t detail_a = 0U;
  std::uint32_t detail_b = 0U;
  PipeErrorHandling handling = PipeErrorHandling::kCloseAfterSend;
  bool send_error_frame = true;
};

struct PipeValidationResult {
  bool ok = false;
  std::optional<PipeValidationIssue> issue;
};

struct PipeProtocolLimits {
  std::uint32_t max_payload_bytes = kMaxPayloadBytes;
  std::uint32_t max_frame_bytes = kMaxFrameBytes;
};

struct PipeCommandEnvelopeV1 {
  std::uint16_t command_id = 0U;
  std::uint8_t command_version = 0U;
  std::uint8_t command_flags = 0U;
  std::uint32_t reserved = 0U;
};

std::uint32_t ReadU32Le(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint16_t ReadU16Le(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint8_t ReadU8(std::span<const std::uint8_t> bytes, std::size_t offset);
float ReadF32Le(std::span<const std::uint8_t> bytes, std::size_t offset);
double ReadF64Le(std::span<const std::uint8_t> bytes, std::size_t offset);

void WriteU32Le(std::uint8_t* out, std::size_t offset, std::uint32_t value);
void WriteU16Le(std::uint8_t* out, std::size_t offset, std::uint16_t value);
void WriteU8(std::uint8_t* out, std::size_t offset, std::uint8_t value);

std::uint32_t Crc32c(std::span<const std::uint8_t> bytes);
std::array<std::uint8_t, kPipeHeaderBytes> EncodeFrameHeader(const PipeFrameHeader& header);
bool TryDecodeFrameHeader(std::span<const std::uint8_t> bytes, PipeFrameHeader* header);
bool IsKnownMessageType(std::uint8_t value);
bool IsKnownCommandId(std::uint16_t value);
bool IsAckRequired(std::uint8_t flags);
std::size_t MinimumPayloadSizeForMessage(PipeMessageType type);
std::optional<std::size_t> FixedPayloadSizeForMessage(PipeMessageType type);
bool HeaderHasReservedFlagBits(std::uint8_t flags);

PipeValidationResult ValidateFrameHeader(const PipeFrameHeader& header,
                                         const PipeProtocolLimits& limits);
PipeValidationResult ValidatePayloadCrc(const PipeFrameHeader& header,
                                        std::span<const std::uint8_t> payload);
PipeValidationResult ValidateMessagePayload(const PipeFrameHeader& header,
                                            std::span<const std::uint8_t> payload);
PipeValidationResult ValidateCommandPayload(std::span<const std::uint8_t> payload,
                                            PipeCommandEnvelopeV1* envelope_out = nullptr);

bool TryParseFrame(std::span<const std::uint8_t> frame_bytes,
                   PipeFrame* frame_out,
                   PipeValidationIssue* issue_out = nullptr,
                   const PipeProtocolLimits& limits = {});

std::string MessageTypeToString(PipeMessageType type);
std::string ErrorCodeToString(PipeErrorCode code);

}  // namespace ntium::ipc
