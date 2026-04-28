#include "src/ipc/PipeSecurity.h"

#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>

#include <sddl.h>

#include <vector>
#endif

namespace ntium::ipc {

PipeSecurityDecision AllowSecurityDecision() {
  PipeSecurityDecision decision;
  decision.verdict = PipeSecurityVerdict::kAllow;
  decision.error_code = PipeErrorCode::kInternal;
  decision.reason = "allowed";
  return decision;
}

PipeSecurityDecision UnsupportedPlatformSecurityDecision() {
  PipeSecurityDecision decision;
  decision.verdict = PipeSecurityVerdict::kUnsupportedPlatform;
  decision.error_code = PipeErrorCode::kInternal;
  decision.reason = "security validation is unsupported on this platform";
  return decision;
}

PipeSecurityDecision InternalSecurityErrorDecision(std::string reason) {
  PipeSecurityDecision decision;
  decision.verdict = PipeSecurityVerdict::kInternalError;
  decision.error_code = PipeErrorCode::kInternal;
  decision.reason = std::move(reason);
  return decision;
}

#ifdef _WIN32
namespace {

bool QueryTokenInfo(HANDLE token,
                    TOKEN_INFORMATION_CLASS info_class,
                    std::vector<std::uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }

  DWORD bytes_needed = 0;
  if (GetTokenInformation(token, info_class, nullptr, 0, &bytes_needed) != FALSE) {
    return false;
  }
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
    return false;
  }

  out->assign(bytes_needed, 0U);
  if (GetTokenInformation(token, info_class, out->data(), bytes_needed, &bytes_needed) ==
      FALSE) {
    return false;
  }
  return true;
}

bool TryGetTokenUserSid(HANDLE token,
                        std::vector<std::uint8_t>* token_user_buffer,
                        PSID* sid_out) {
  if (token_user_buffer == nullptr || sid_out == nullptr) {
    return false;
  }
  if (!QueryTokenInfo(token, TokenUser, token_user_buffer)) {
    return false;
  }
  auto* token_user = reinterpret_cast<TOKEN_USER*>(token_user_buffer->data());
  if (token_user->User.Sid == nullptr || IsValidSid(token_user->User.Sid) == FALSE) {
    return false;
  }
  *sid_out = token_user->User.Sid;
  return true;
}

bool TryGetTokenSessionId(HANDLE token, DWORD* session_id_out) {
  if (session_id_out == nullptr) {
    return false;
  }
  DWORD session_id = 0;
  DWORD bytes_returned = 0;
  if (GetTokenInformation(token, TokenSessionId, &session_id, sizeof(session_id),
                          &bytes_returned) == FALSE) {
    return false;
  }
  if (bytes_returned < sizeof(session_id)) {
    return false;
  }
  *session_id_out = session_id;
  return true;
}

class ScopedHandle final {
 public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

std::string SidToString(PSID sid) {
  if (sid == nullptr) {
    return {};
  }
  LPSTR sid_string = nullptr;
  if (ConvertSidToStringSidA(sid, &sid_string) == FALSE || sid_string == nullptr) {
    return {};
  }
  std::string value(sid_string);
  LocalFree(sid_string);
  return value;
}

PipeSecurityDecision Denied(PipeErrorCode error_code,
                            std::uint32_t detail_a,
                            std::uint32_t detail_b,
                            const char* reason) {
  PipeSecurityDecision decision;
  decision.verdict = PipeSecurityVerdict::kDeny;
  decision.error_code = error_code;
  decision.detail_a = detail_a;
  decision.detail_b = detail_b;
  decision.reason = reason;
  return decision;
}

}  // namespace

bool SameUserSameSessionSecurityPolicy::EnsureServerIdentity(std::string* error_reason) {
  if (server_identity_initialized_) {
    return true;
  }

  HANDLE server_token_raw = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &server_token_raw) == FALSE) {
    if (error_reason != nullptr) {
      *error_reason = "failed to open server token";
    }
    return false;
  }
  ScopedHandle server_token(server_token_raw);

  std::vector<std::uint8_t> token_user_buffer;
  PSID server_sid = nullptr;
  if (!TryGetTokenUserSid(server_token.get(), &token_user_buffer, &server_sid)) {
    if (error_reason != nullptr) {
      *error_reason = "failed to resolve server SID";
    }
    return false;
  }

  DWORD session_id = 0;
  if (!TryGetTokenSessionId(server_token.get(), &session_id)) {
    if (error_reason != nullptr) {
      *error_reason = "failed to resolve server session id";
    }
    return false;
  }

  server_user_sid_string_ = SidToString(server_sid);
  server_session_id_ = static_cast<std::uint32_t>(session_id);
  server_identity_initialized_ = !server_user_sid_string_.empty();
  if (!server_identity_initialized_ && error_reason != nullptr) {
    *error_reason = "failed to encode server SID";
  }
  return server_identity_initialized_;
}

PipeSecurityDecision SameUserSameSessionSecurityPolicy::ValidateConnection(
    const PipeSecurityContext& context) {
  std::string error_reason;
  if (!EnsureServerIdentity(&error_reason)) {
    return InternalSecurityErrorDecision(error_reason);
  }

  HANDLE pipe_handle = reinterpret_cast<HANDLE>(context.native_pipe_handle);
  if (pipe_handle == nullptr || pipe_handle == INVALID_HANDLE_VALUE) {
    return InternalSecurityErrorDecision("invalid pipe handle");
  }

  if (context.reject_remote_clients) {
    BOOL is_remote_client = FALSE;
    if (GetNamedPipeHandleStateW(pipe_handle, nullptr, nullptr, nullptr, nullptr, nullptr, 0) ==
        FALSE) {
      return InternalSecurityErrorDecision("failed to query named pipe handle state");
    }
    if (GetNamedPipeClientComputerNameW(pipe_handle, nullptr, 0) != FALSE) {
      is_remote_client = TRUE;
    } else {
      const DWORD last_error = GetLastError();
      if (last_error != ERROR_MORE_DATA && last_error != ERROR_INSUFFICIENT_BUFFER &&
          last_error != ERROR_INVALID_PARAMETER && last_error != ERROR_NOT_SUPPORTED) {
        return InternalSecurityErrorDecision("failed while checking client origin");
      }
    }

    if (is_remote_client != FALSE) {
      return Denied(PipeErrorCode::kRemoteClientRejected, 0U, 0U,
                    "remote client rejected");
    }
  }

  ULONG client_pid = 0;
  if (GetNamedPipeClientProcessId(pipe_handle, &client_pid) == FALSE || client_pid == 0U) {
    return InternalSecurityErrorDecision("failed to resolve client process id");
  }

  ScopedHandle client_process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                          static_cast<DWORD>(client_pid)));
  if (client_process.get() == nullptr) {
    return InternalSecurityErrorDecision("failed to open client process");
  }

  HANDLE client_token_raw = nullptr;
  if (OpenProcessToken(client_process.get(), TOKEN_QUERY, &client_token_raw) == FALSE) {
    return InternalSecurityErrorDecision("failed to open client token");
  }
  ScopedHandle client_token(client_token_raw);

  std::vector<std::uint8_t> client_token_user_buffer;
  PSID client_sid = nullptr;
  if (!TryGetTokenUserSid(client_token.get(), &client_token_user_buffer, &client_sid)) {
    return InternalSecurityErrorDecision("failed to query client SID");
  }

  const std::string client_sid_string = SidToString(client_sid);
  if (client_sid_string.empty()) {
    return InternalSecurityErrorDecision("failed to encode client SID");
  }

  if (client_sid_string != server_user_sid_string_) {
    return Denied(PipeErrorCode::kAuthSidMismatch, 0U, 0U, "client SID mismatch");
  }

  DWORD client_session_id = 0;
  if (!TryGetTokenSessionId(client_token.get(), &client_session_id)) {
    return InternalSecurityErrorDecision("failed to query client session id");
  }

  if (static_cast<std::uint32_t>(client_session_id) != server_session_id_) {
    return Denied(PipeErrorCode::kAuthSessionMismatch,
                  static_cast<std::uint32_t>(client_session_id),
                  server_session_id_,
                  "client session mismatch");
  }

  return AllowSecurityDecision();
}

#else

PipeSecurityDecision SameUserSameSessionSecurityPolicy::ValidateConnection(
    const PipeSecurityContext& /*context*/) {
  return UnsupportedPlatformSecurityDecision();
}

#endif

}  // namespace ntium::ipc
