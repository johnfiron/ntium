#include "src/runtime/SessionManager.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace ntium::runtime {
namespace {

constexpr std::uint32_t kActionPause =
    static_cast<std::uint32_t>(SessionTransitionAction::kPauseRuntime);
constexpr std::uint32_t kActionResume =
    static_cast<std::uint32_t>(SessionTransitionAction::kResumeRuntime);
constexpr std::uint32_t kActionPersist =
    static_cast<std::uint32_t>(SessionTransitionAction::kPersistState);
constexpr std::uint32_t kActionRestore =
    static_cast<std::uint32_t>(SessionTransitionAction::kRestoreState);
constexpr std::uint32_t kActionRebind =
    static_cast<std::uint32_t>(SessionTransitionAction::kRebindDesktop);

}  // namespace

SessionManager::SessionManager(SessionTransitionHooks hooks)
    : hooks_(std::move(hooks)) {}

void SessionManager::SetHooks(SessionTransitionHooks hooks) {
  hooks_ = std::move(hooks);
}

SessionTransitionResult SessionManager::ProcessTrigger(
    SessionTransitionTrigger trigger,
    std::uint64_t now_ms) {
  SessionTransitionResult result;
  result.trigger = trigger;
  result.from = snapshot_.state;
  result.to = snapshot_.state;
  result.transition_sequence = snapshot_.transition_sequence;

  if (trigger == SessionTransitionTrigger::kUnknown) {
    result.status = SessionTransitionStatus::kInvalidArgument;
    return result;
  }

  const TransitionPlan plan = DeterminePlan(
      snapshot_.state, trigger, snapshot_.previous_non_suspended_state);
  if (!plan.valid || plan.to == snapshot_.state) {
    result.status = SessionTransitionStatus::kIgnored;
    return result;
  }

  if (!ApplyActions(plan.action_mask, trigger, now_ms)) {
    result.status = SessionTransitionStatus::kHookFailed;
    result.action_mask = plan.action_mask;
    result.hook_failed = true;
    return result;
  }

  if (snapshot_.state != SessionRuntimeState::kSuspended) {
    snapshot_.previous_non_suspended_state = snapshot_.state;
  }

  snapshot_.state = plan.to;
  snapshot_.last_trigger = trigger;
  snapshot_.last_transition_ms = now_ms;
  snapshot_.transition_sequence += 1U;
  if (snapshot_.state != SessionRuntimeState::kSuspended) {
    snapshot_.previous_non_suspended_state = snapshot_.state;
  }

  result.status = SessionTransitionStatus::kApplied;
  result.to = snapshot_.state;
  result.action_mask = plan.action_mask;
  result.transition_performed = true;
  result.transition_sequence = snapshot_.transition_sequence;
  return result;
}

SessionTransitionResult SessionManager::HandleSessionChange(
    std::uint32_t session_event,
    std::intptr_t session_id,
    std::uint64_t now_ms) {
  (void)session_id;

  SessionTransitionTrigger trigger = SessionTransitionTrigger::kUnknown;
  if (!MapSessionChangeEvent(session_event, &trigger)) {
    SessionTransitionResult result;
    result.status = SessionTransitionStatus::kIgnored;
    result.from = snapshot_.state;
    result.to = snapshot_.state;
    result.transition_sequence = snapshot_.transition_sequence;
    return result;
  }
  return ProcessTrigger(trigger, now_ms);
}

SessionTransitionResult SessionManager::HandlePowerBroadcast(
    std::uint32_t power_event_code,
    std::uint64_t now_ms) {
  if (IsSuspendPowerEvent(power_event_code)) {
    return ProcessTrigger(SessionTransitionTrigger::kSuspend, now_ms);
  }
  if (IsResumePowerEvent(power_event_code)) {
    return ProcessTrigger(SessionTransitionTrigger::kResume, now_ms);
  }

  SessionTransitionResult result;
  result.status = SessionTransitionStatus::kIgnored;
  result.from = snapshot_.state;
  result.to = snapshot_.state;
  result.transition_sequence = snapshot_.transition_sequence;
  return result;
}

SessionTransitionResult SessionManager::HandleEndSession(
    bool ending_session,
    std::uint64_t now_ms) {
  if (!ending_session) {
    SessionTransitionResult result;
    result.status = SessionTransitionStatus::kIgnored;
    result.from = snapshot_.state;
    result.to = snapshot_.state;
    result.transition_sequence = snapshot_.transition_sequence;
    return result;
  }
  return ProcessTrigger(SessionTransitionTrigger::kShutdown, now_ms);
}

bool SessionManager::HasAction(
    std::uint32_t action_mask,
    SessionTransitionAction action) {
  return (action_mask & static_cast<std::uint32_t>(action)) != 0U;
}

SessionManager::TransitionPlan SessionManager::DeterminePlan(
    SessionRuntimeState current,
    SessionTransitionTrigger trigger,
    SessionRuntimeState resume_target) {
  TransitionPlan plan;
  plan.to = current;
  plan.action_mask = 0U;

  switch (trigger) {
    case SessionTransitionTrigger::kProcessStart:
    case SessionTransitionTrigger::kSessionLogon:
      if (current == SessionRuntimeState::kStopped ||
          current == SessionRuntimeState::kLogoffPending) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kActive;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kRestoreState,
            SessionTransitionAction::kRebindDesktop,
            SessionTransitionAction::kResumeRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kSessionLock:
    case SessionTransitionTrigger::kConsoleDisconnect:
    case SessionTransitionTrigger::kRemoteDisconnect:
      if (current == SessionRuntimeState::kActive) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kLocked;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kPersistState,
            SessionTransitionAction::kPauseRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kSessionUnlock:
    case SessionTransitionTrigger::kConsoleConnect:
    case SessionTransitionTrigger::kRemoteConnect:
      if (current == SessionRuntimeState::kLocked) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kActive;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kRebindDesktop,
            SessionTransitionAction::kRestoreState,
            SessionTransitionAction::kResumeRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kSuspend:
      if (current != SessionRuntimeState::kSuspended &&
          current != SessionRuntimeState::kStopped &&
          current != SessionRuntimeState::kShutdownPending &&
          current != SessionRuntimeState::kLogoffPending) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kSuspended;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kPersistState,
            SessionTransitionAction::kPauseRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kResume:
      if (current == SessionRuntimeState::kSuspended) {
        plan.valid = true;
        plan.to = (resume_target == SessionRuntimeState::kLocked)
                      ? SessionRuntimeState::kLocked
                      : SessionRuntimeState::kActive;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kRebindDesktop,
            SessionTransitionAction::kRestoreState,
            SessionTransitionAction::kResumeRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kSessionLogoff:
      if (current != SessionRuntimeState::kStopped &&
          current != SessionRuntimeState::kShutdownPending) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kLogoffPending;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kPersistState,
            SessionTransitionAction::kPauseRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kShutdown:
      if (current != SessionRuntimeState::kStopped) {
        plan.valid = true;
        plan.to = SessionRuntimeState::kShutdownPending;
        plan.action_mask = ActionMask(
            SessionTransitionAction::kPersistState,
            SessionTransitionAction::kPauseRuntime);
      }
      return plan;

    case SessionTransitionTrigger::kUnknown:
      return plan;
  }

  return plan;
}

bool SessionManager::MapSessionChangeEvent(
    std::uint32_t session_event,
    SessionTransitionTrigger* out_trigger) {
  if (out_trigger == nullptr) {
    return false;
  }

#ifdef _WIN32
  switch (session_event) {
    case WTS_SESSION_LOGON:
      *out_trigger = SessionTransitionTrigger::kSessionLogon;
      return true;
    case WTS_SESSION_LOGOFF:
      *out_trigger = SessionTransitionTrigger::kSessionLogoff;
      return true;
    case WTS_SESSION_LOCK:
      *out_trigger = SessionTransitionTrigger::kSessionLock;
      return true;
    case WTS_SESSION_UNLOCK:
      *out_trigger = SessionTransitionTrigger::kSessionUnlock;
      return true;
    case WTS_CONSOLE_CONNECT:
      *out_trigger = SessionTransitionTrigger::kConsoleConnect;
      return true;
    case WTS_CONSOLE_DISCONNECT:
      *out_trigger = SessionTransitionTrigger::kConsoleDisconnect;
      return true;
    case WTS_REMOTE_CONNECT:
      *out_trigger = SessionTransitionTrigger::kRemoteConnect;
      return true;
    case WTS_REMOTE_DISCONNECT:
      *out_trigger = SessionTransitionTrigger::kRemoteDisconnect;
      return true;
    default:
      return false;
  }
#else
  constexpr std::uint32_t kWtsConsoleConnect = 1U;
  constexpr std::uint32_t kWtsConsoleDisconnect = 2U;
  constexpr std::uint32_t kWtsRemoteConnect = 3U;
  constexpr std::uint32_t kWtsRemoteDisconnect = 4U;
  constexpr std::uint32_t kWtsSessionLogon = 5U;
  constexpr std::uint32_t kWtsSessionLogoff = 6U;
  constexpr std::uint32_t kWtsSessionLock = 7U;
  constexpr std::uint32_t kWtsSessionUnlock = 8U;

  switch (session_event) {
    case kWtsSessionLogon:
      *out_trigger = SessionTransitionTrigger::kSessionLogon;
      return true;
    case kWtsSessionLogoff:
      *out_trigger = SessionTransitionTrigger::kSessionLogoff;
      return true;
    case kWtsSessionLock:
      *out_trigger = SessionTransitionTrigger::kSessionLock;
      return true;
    case kWtsSessionUnlock:
      *out_trigger = SessionTransitionTrigger::kSessionUnlock;
      return true;
    case kWtsConsoleConnect:
      *out_trigger = SessionTransitionTrigger::kConsoleConnect;
      return true;
    case kWtsConsoleDisconnect:
      *out_trigger = SessionTransitionTrigger::kConsoleDisconnect;
      return true;
    case kWtsRemoteConnect:
      *out_trigger = SessionTransitionTrigger::kRemoteConnect;
      return true;
    case kWtsRemoteDisconnect:
      *out_trigger = SessionTransitionTrigger::kRemoteDisconnect;
      return true;
    default:
      return false;
  }
#endif
}

bool SessionManager::IsSuspendPowerEvent(std::uint32_t power_event_code) {
#ifdef _WIN32
  return power_event_code == PBT_APMSUSPEND;
#else
  constexpr std::uint32_t kPbtApmSuspend = 0x0004U;
  return power_event_code == kPbtApmSuspend;
#endif
}

bool SessionManager::IsResumePowerEvent(std::uint32_t power_event_code) {
#ifdef _WIN32
  return power_event_code == PBT_APMRESUMEAUTOMATIC ||
         power_event_code == PBT_APMRESUMESUSPEND;
#else
  constexpr std::uint32_t kPbtApmResumeSuspend = 0x0007U;
  constexpr std::uint32_t kPbtApmResumeAutomatic = 0x0012U;
  return power_event_code == kPbtApmResumeSuspend ||
         power_event_code == kPbtApmResumeAutomatic;
#endif
}

bool SessionManager::ApplyActions(
    std::uint32_t action_mask,
    SessionTransitionTrigger trigger,
    std::uint64_t now_ms) const {
  if ((action_mask & kActionPersist) != 0U && hooks_.persist_state &&
      !hooks_.persist_state(trigger, now_ms)) {
    return false;
  }
  if ((action_mask & kActionPause) != 0U && hooks_.pause_runtime &&
      !hooks_.pause_runtime(trigger, now_ms)) {
    return false;
  }
  if ((action_mask & kActionRebind) != 0U && hooks_.rebind_desktop &&
      !hooks_.rebind_desktop(trigger, now_ms)) {
    return false;
  }
  if ((action_mask & kActionRestore) != 0U && hooks_.restore_state &&
      !hooks_.restore_state(trigger, now_ms)) {
    return false;
  }
  if ((action_mask & kActionResume) != 0U && hooks_.resume_runtime &&
      !hooks_.resume_runtime(trigger, now_ms)) {
    return false;
  }
  return true;
}

std::uint32_t SessionManager::ActionMask(
    SessionTransitionAction first,
    SessionTransitionAction second,
    SessionTransitionAction third) {
  return static_cast<std::uint32_t>(first) |
         static_cast<std::uint32_t>(second) |
         static_cast<std::uint32_t>(third);
}

}  // namespace ntium::runtime
