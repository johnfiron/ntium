#pragma once

#include <cstdint>
#include <functional>
#include <cstddef>

namespace ntium::runtime {

enum class SessionRuntimeState : std::uint8_t {
  kStopped = 0U,
  kActive = 1U,
  kLocked = 2U,
  kSuspended = 3U,
  kLogoffPending = 4U,
  kShutdownPending = 5U,
};

enum class SessionTransitionTrigger : std::uint8_t {
  kUnknown = 0U,
  kProcessStart = 1U,
  kSessionLogon = 2U,
  kSessionLogoff = 3U,
  kSessionLock = 4U,
  kSessionUnlock = 5U,
  kConsoleConnect = 6U,
  kConsoleDisconnect = 7U,
  kRemoteConnect = 8U,
  kRemoteDisconnect = 9U,
  kSuspend = 10U,
  kResume = 11U,
  kShutdown = 12U,
};

enum class SessionTransitionStatus : std::uint8_t {
  kApplied = 0U,
  kIgnored = 1U,
  kInvalidArgument = 2U,
  kHookFailed = 3U,
  kUnsupportedPlatform = 4U,
};

enum class SessionTransitionAction : std::uint32_t {
  kNone = 0U,
  kPauseRuntime = 1U << 0U,
  kResumeRuntime = 1U << 1U,
  kPersistState = 1U << 2U,
  kRestoreState = 1U << 3U,
  kRebindDesktop = 1U << 4U,
};

using SessionActionHook = std::function<bool(SessionTransitionTrigger, std::uint64_t)>;

struct SessionTransitionHooks {
  SessionActionHook pause_runtime;
  SessionActionHook resume_runtime;
  SessionActionHook persist_state;
  SessionActionHook restore_state;
  SessionActionHook rebind_desktop;
};

struct SessionRuntimeSnapshot {
  SessionRuntimeState state = SessionRuntimeState::kStopped;
  SessionRuntimeState previous_non_suspended_state = SessionRuntimeState::kStopped;
  SessionTransitionTrigger last_trigger = SessionTransitionTrigger::kUnknown;
  std::uint64_t transition_sequence = 0U;
  std::uint64_t last_transition_ms = 0U;
};

struct SessionTransitionResult {
  SessionTransitionStatus status = SessionTransitionStatus::kIgnored;
  SessionTransitionTrigger trigger = SessionTransitionTrigger::kUnknown;
  SessionRuntimeState from = SessionRuntimeState::kStopped;
  SessionRuntimeState to = SessionRuntimeState::kStopped;
  std::uint32_t action_mask = 0U;
  bool transition_performed = false;
  bool hook_failed = false;
  std::uint64_t transition_sequence = 0U;
};

class SessionManager {
 public:
  explicit SessionManager(SessionTransitionHooks hooks = {});

  const SessionRuntimeSnapshot& snapshot() const { return snapshot_; }
  void SetHooks(SessionTransitionHooks hooks);

  SessionTransitionResult ProcessTrigger(
      SessionTransitionTrigger trigger,
      std::uint64_t now_ms);

  SessionTransitionResult HandleSessionChange(
      std::uint32_t session_event,
      std::intptr_t session_id,
      std::uint64_t now_ms);

  SessionTransitionResult HandlePowerBroadcast(
      std::uint32_t power_event_code,
      std::uint64_t now_ms);

  SessionTransitionResult HandleEndSession(
      bool ending_session,
      std::uint64_t now_ms);

  static bool HasAction(
      std::uint32_t action_mask,
      SessionTransitionAction action);

 private:
  struct TransitionPlan {
    bool valid = false;
    SessionRuntimeState to = SessionRuntimeState::kStopped;
    std::uint32_t action_mask = 0U;
  };

  static TransitionPlan DeterminePlan(
      SessionRuntimeState current,
      SessionTransitionTrigger trigger,
      SessionRuntimeState resume_target);

  static bool MapSessionChangeEvent(
      std::uint32_t session_event,
      SessionTransitionTrigger* out_trigger);

  static bool IsSuspendPowerEvent(std::uint32_t power_event_code);
  static bool IsResumePowerEvent(std::uint32_t power_event_code);

  bool ApplyActions(
      std::uint32_t action_mask,
      SessionTransitionTrigger trigger,
      std::uint64_t now_ms) const;

  static std::uint32_t ActionMask(
      SessionTransitionAction first,
      SessionTransitionAction second = SessionTransitionAction::kNone,
      SessionTransitionAction third = SessionTransitionAction::kNone);

  SessionTransitionHooks hooks_;
  SessionRuntimeSnapshot snapshot_ {};
};

}  // namespace ntium::runtime
