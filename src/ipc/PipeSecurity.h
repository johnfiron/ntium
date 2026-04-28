#pragma once

#include <cstdint>
#include <string>

#include "src/ipc/PipeProtocol.h"

namespace ntium::ipc {

enum class PipeSecurityVerdict : std::uint8_t {
  kAllow = 0U,
  kDeny = 1U,
  kUnsupportedPlatform = 2U,
  kInternalError = 3U,
};

struct PipeSecurityContext {
  void* native_pipe_handle = nullptr;
  std::uint64_t connection_id = 0U;
  bool reject_remote_clients = true;
};

struct PipeSecurityDecision {
  PipeSecurityVerdict verdict = PipeSecurityVerdict::kDeny;
  PipeErrorCode error_code = PipeErrorCode::kInternal;
  std::uint32_t detail_a = 0U;
  std::uint32_t detail_b = 0U;
  std::string reason;
};

class PipeSecurityPolicy {
 public:
  virtual ~PipeSecurityPolicy() = default;
  virtual PipeSecurityDecision ValidateConnection(
      const PipeSecurityContext& context) = 0;
};

class SameUserSameSessionSecurityPolicy : public PipeSecurityPolicy {
 public:
  PipeSecurityDecision ValidateConnection(
      const PipeSecurityContext& context) override;

 private:
#ifdef _WIN32
  bool EnsureServerIdentity(std::string* error_reason);

  std::string server_user_sid_string_;
  std::uint32_t server_session_id_ = 0U;
  bool server_identity_initialized_ = false;
#endif
};

PipeSecurityDecision AllowSecurityDecision();
PipeSecurityDecision UnsupportedPlatformSecurityDecision();
PipeSecurityDecision InternalSecurityErrorDecision(std::string reason);

}  // namespace ntium::ipc
