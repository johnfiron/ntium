#include "src/ipc/PipeServer.h"

#include <algorithm>
#include <utility>

namespace ntium::ipc {
namespace {

PipeServerError ValidateConfig(const PipeServerConfig& config) {
  if (config.pipe_name.empty()) {
    return PipeServerError::kInvalidConfig;
  }
  if (config.max_instances == 0U) {
    return PipeServerError::kInvalidConfig;
  }
  if (config.protocol_limits.max_payload_bytes == 0U ||
      config.protocol_limits.max_frame_bytes < kPipeHeaderBytes) {
    return PipeServerError::kInvalidConfig;
  }
  if (config.max_in_flight_commands == 0U) {
    return PipeServerError::kInvalidConfig;
  }
  return PipeServerError::kNone;
}

void InvokeCallback(const PipeServerConfig& config, const PipeServerEvent& event) {
  if (config.event_callback) {
    config.event_callback(event);
  }
}

}  // namespace

#ifdef _WIN32
namespace {

class Win32PipeServer final : public PipeServer {
 public:
  explicit Win32PipeServer(PipeServerConfig config) : config_(std::move(config)) {
    connection_.protocol_state = PipeProtocolStateId::kConnectedWaitHello;
  }

  ~Win32PipeServer() override { Stop(); }

  PipeServerRunState state() const override { return state_; }

  PipeServerError Start() override {
    if (state_ != PipeServerRunState::kStopped) {
      return PipeServerError::kInvalidState;
    }
    const PipeServerError validation_error = ValidateConfig(config_);
    if (validation_error != PipeServerError::kNone) {
      return validation_error;
    }
    state_ = PipeServerRunState::kStarting;

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = FALSE;
    security_attributes.lpSecurityDescriptor = nullptr;

    pipe_handle_ = CreateNamedPipeA(
        config_.pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        config_.max_instances,
        config_.protocol_limits.max_frame_bytes,
        config_.protocol_limits.max_frame_bytes,
        0,
        &security_attributes);

    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
      state_ = PipeServerRunState::kStopped;
      PipeServerEvent event;
      event.kind = PipeServerEventKind::kInternalError;
      event.error = PipeServerError::kCreatePipeFailed;
      event.message = "CreateNamedPipeA failed";
      InvokeCallback(config_, event);
      return PipeServerError::kCreatePipeFailed;
    }

    connection_id_ += 1U;
    connection_.connection_id = connection_id_;
    connection_.authenticated = false;
    connection_.pending_write_frames = 0U;
    connection_.last_inbound_sequence = 0U;
    connection_.last_outbound_sequence = 0U;
    connection_.protocol_state = PipeProtocolStateId::kConnectedWaitHello;

    state_ = PipeServerRunState::kListening;

    PipeServerEvent listening_event;
    listening_event.kind = PipeServerEventKind::kListening;
    listening_event.connection_id = connection_id_;
    listening_event.message = "Pipe server listening";
    InvokeCallback(config_, listening_event);
    return PipeServerError::kNone;
  }

  void Stop() override {
    if (state_ == PipeServerRunState::kStopped) {
      return;
    }
    state_ = PipeServerRunState::kStopping;
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(pipe_handle_);
      CloseHandle(pipe_handle_);
      pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    state_ = PipeServerRunState::kStopped;

    PipeServerEvent event;
    event.kind = PipeServerEventKind::kClientDisconnected;
    event.connection_id = connection_id_;
    event.message = "Pipe server stopped";
    InvokeCallback(config_, event);
  }

  PipeServerError Poll(std::chrono::milliseconds timeout) override {
    if (state_ == PipeServerRunState::kStopped) {
      return PipeServerError::kInvalidState;
    }
    if (state_ == PipeServerRunState::kListening) {
      const BOOL connected = ConnectNamedPipe(pipe_handle_, nullptr);
      if (connected == FALSE) {
        const DWORD last_error = GetLastError();
        if (last_error != ERROR_PIPE_CONNECTED && last_error != ERROR_NO_DATA &&
            last_error != ERROR_IO_PENDING) {
          PipeServerEvent error_event;
          error_event.kind = PipeServerEventKind::kInternalError;
          error_event.error = PipeServerError::kIoFailure;
          error_event.connection_id = connection_id_;
          error_event.message = "ConnectNamedPipe failed";
          InvokeCallback(config_, error_event);
          return PipeServerError::kIoFailure;
        }
      }

      PipeServerEvent connect_event;
      connect_event.kind = PipeServerEventKind::kClientConnected;
      connect_event.connection_id = connection_id_;
      connect_event.message = "Client connected";
      InvokeCallback(config_, connect_event);

      if (config_.security_policy) {
        PipeSecurityContext security_context;
        security_context.native_pipe_handle = pipe_handle_;
        security_context.connection_id = connection_id_;
        security_context.reject_remote_clients = true;
        const PipeSecurityDecision decision =
            config_.security_policy->ValidateConnection(security_context);
        if (decision.verdict != PipeSecurityVerdict::kAllow) {
          PipeServerEvent security_event;
          security_event.kind = PipeServerEventKind::kSecurityViolation;
          security_event.error = PipeServerError::kSecurityFailure;
          security_event.connection_id = connection_id_;
          security_event.security_decision = decision;
          security_event.message = decision.reason;
          InvokeCallback(config_, security_event);
          DisconnectNamedPipe(pipe_handle_);
          state_ = PipeServerRunState::kListening;
          return PipeServerError::kSecurityFailure;
        }
      }

      connection_.authenticated = true;
      connection_.protocol_state = PipeProtocolStateId::kConnectedWaitHello;
      state_ = PipeServerRunState::kConnected;
      return PipeServerError::kNone;
    }

    if (state_ != PipeServerRunState::kConnected) {
      return PipeServerError::kInvalidState;
    }

    if (timeout.count() > 0) {
      Sleep(static_cast<DWORD>(timeout.count()));
    }
    return PipeServerError::kNone;
  }

  PipeServerError QueueFrame(std::uint64_t connection_id,
                             const PipeFrame& frame) override {
    if (state_ != PipeServerRunState::kConnected) {
      return PipeServerError::kInvalidState;
    }
    if (connection_id != connection_id_) {
      return PipeServerError::kInvalidConfig;
    }

    PipeFrame frame_copy = frame;
    frame_copy.header.header_crc32c = 0U;
    frame_copy.header.payload_len =
        static_cast<std::uint32_t>(frame_copy.payload.size());
    frame_copy.header.payload_crc32c =
        frame_copy.payload.empty() ? 0U : Crc32c(frame_copy.payload);
    const auto encoded_header = EncodeFrameHeader(frame_copy.header);
    frame_copy.header.header_crc32c = ReadU32Le(encoded_header, 28U);

    write_queue_.push_back(std::move(frame_copy));
    connection_.pending_write_frames = write_queue_.size();

    PipeServerEvent event;
    event.kind = PipeServerEventKind::kFrameQueued;
    event.connection_id = connection_id_;
    event.message = "Frame queued for write";
    InvokeCallback(config_, event);

    return PipeServerError::kNone;
  }

  std::optional<PipeServerConnectionSnapshot> connection_snapshot(
      std::uint64_t connection_id) const override {
    if (connection_id != connection_id_) {
      return std::nullopt;
    }
    return connection_;
  }

 private:
  PipeServerConfig config_;
  PipeServerRunState state_ = PipeServerRunState::kStopped;
  std::uint64_t connection_id_ = 0U;
  PipeServerConnectionSnapshot connection_{};
  HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
  std::deque<PipeFrame> write_queue_;
};

}  // namespace
#else
namespace {

class StubPipeServer final : public PipeServer {
 public:
  explicit StubPipeServer(PipeServerConfig config) : config_(std::move(config)) {}

  PipeServerRunState state() const override { return PipeServerRunState::kStopped; }

  PipeServerError Start() override {
    PipeServerEvent event;
    event.kind = PipeServerEventKind::kInternalError;
    event.error = PipeServerError::kPlatformUnsupported;
    event.message = "Named pipe server requires Windows";
    InvokeCallback(config_, event);
    return PipeServerError::kPlatformUnsupported;
  }

  void Stop() override {}

  PipeServerError Poll(std::chrono::milliseconds /*timeout*/) override {
    return PipeServerError::kPlatformUnsupported;
  }

  PipeServerError QueueFrame(std::uint64_t /*connection_id*/,
                             const PipeFrame& /*frame*/) override {
    return PipeServerError::kPlatformUnsupported;
  }

  std::optional<PipeServerConnectionSnapshot> connection_snapshot(
      std::uint64_t /*connection_id*/) const override {
    return std::nullopt;
  }

 private:
  PipeServerConfig config_;
};

}  // namespace
#endif

std::unique_ptr<PipeServer> PipeServer::Create(PipeServerConfig config) {
  if (!config.security_policy) {
    config.security_policy = std::make_shared<SameUserSameSessionSecurityPolicy>();
  }
#ifdef _WIN32
  return std::make_unique<Win32PipeServer>(std::move(config));
#else
  return std::make_unique<StubPipeServer>(std::move(config));
#endif
}

}  // namespace ntium::ipc
