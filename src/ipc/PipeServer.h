#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/ipc/PipeProtocol.h"
#include "src/ipc/PipeSecurity.h"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace ntium::ipc {

enum class PipeServerRunState : std::uint8_t {
  kStopped = 0U,
  kStarting = 1U,
  kListening = 2U,
  kConnected = 3U,
  kStopping = 4U,
};

enum class PipeServerError : std::uint8_t {
  kNone = 0U,
  kInvalidState = 1U,
  kInvalidConfig = 2U,
  kPlatformUnsupported = 3U,
  kCreatePipeFailed = 4U,
  kIoFailure = 5U,
  kSecurityFailure = 6U,
  kProtocolViolation = 7U,
  kInternalError = 8U,
};

enum class PipeServerEventKind : std::uint8_t {
  kListening = 0U,
  kClientConnected = 1U,
  kClientDisconnected = 2U,
  kFrameReceived = 3U,
  kFrameQueued = 4U,
  kProtocolError = 5U,
  kSecurityViolation = 6U,
  kInternalError = 7U,
};

enum class PipeIoOperationType : std::uint8_t {
  kConnect = 0U,
  kReadHeader = 1U,
  kReadPayload = 2U,
  kWriteFrame = 3U,
};

struct PipeServerConnectionSnapshot {
  std::uint64_t connection_id = 0U;
  PipeProtocolStateId protocol_state = PipeProtocolStateId::kConnectedWaitHello;
  std::uint32_t last_inbound_sequence = 0U;
  std::uint32_t last_outbound_sequence = 0U;
  bool authenticated = false;
  std::size_t pending_write_frames = 0U;
};

struct PipeOverlappedOperation {
  PipeIoOperationType operation = PipeIoOperationType::kConnect;
  std::uint64_t connection_id = 0U;
  bool pending = false;
  std::vector<std::uint8_t> buffer;

#ifdef _WIN32
  OVERLAPPED overlapped{};
  HANDLE event_handle = nullptr;
#else
  void* event_handle = nullptr;
#endif
};

struct PipeServerEvent {
  PipeServerEventKind kind = PipeServerEventKind::kInternalError;
  PipeServerError error = PipeServerError::kNone;
  std::uint64_t connection_id = 0U;
  std::optional<PipeFrame> frame;
  std::optional<PipeValidationIssue> validation_issue;
  std::optional<PipeSecurityDecision> security_decision;
  std::string message;
};

using PipeServerEventCallback = std::function<void(const PipeServerEvent&)>;

struct PipeServerConfig {
  std::string pipe_name = std::string(kPipeNameV1);
  PipeProtocolLimits protocol_limits{};
  std::uint32_t max_instances = 1U;
  std::uint32_t inbound_frames_per_second_limit = kMaxInboundFramesPerSec;
  std::uint32_t inbound_payload_bytes_per_second_limit =
      kMaxInboundPayloadBytesPerSec;
  std::uint32_t max_in_flight_commands = kMaxInFlightCommands;
  std::shared_ptr<PipeSecurityPolicy> security_policy;
  PipeServerEventCallback event_callback;
};

class PipeServer {
 public:
  virtual ~PipeServer() = default;

  virtual PipeServerRunState state() const = 0;
  virtual PipeServerError Start() = 0;
  virtual void Stop() = 0;
  virtual PipeServerError Poll(std::chrono::milliseconds timeout) = 0;
  virtual PipeServerError QueueFrame(std::uint64_t connection_id,
                                     const PipeFrame& frame) = 0;
  virtual std::optional<PipeServerConnectionSnapshot> connection_snapshot(
      std::uint64_t connection_id) const = 0;

  static std::unique_ptr<PipeServer> Create(PipeServerConfig config);
};

}  // namespace ntium::ipc
