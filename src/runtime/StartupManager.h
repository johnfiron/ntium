#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ntium::runtime {

enum class StartupLaunchReason : std::uint8_t {
  kUnknown = 0U,
  kProcessStart = 1U,
  kUserLogon = 2U,
  kScheduledRestart = 3U,
  kCrashRecovery = 4U,
  kManualRestart = 5U,
};

enum class StartupExitReason : std::uint8_t {
  kUnknown = 0U,
  kGracefulShutdown = 1U,
  kCrash = 2U,
  kWatchdogTermination = 3U,
  kPolicyRestart = 4U,
  kOsShutdown = 5U,
};

enum class StartupLifecycleState : std::uint8_t {
  kIdle = 0U,
  kLaunchApproved = 1U,
  kRunning = 2U,
  kRestartPending = 3U,
  kRestartSuppressed = 4U,
};

enum class StartupDecisionDisposition : std::uint8_t {
  kAllowLaunch = 0U,
  kDenyDuplicateInstance = 1U,
  kDenyPolicySuppressed = 2U,
  kDelayWithBackoff = 3U,
};

enum class SingleInstanceResult : std::uint8_t {
  kAcquired = 0U,
  kAlreadyRunning = 1U,
  kUnavailable = 2U,
};

struct RestartPolicy {
  bool enable_restart = true;
  std::uint32_t max_restarts_per_window = 3U;
  std::uint64_t restart_window_ms = 5U * 60U * 1000U;
  std::uint64_t min_backoff_ms = 1000U;
  std::uint64_t max_backoff_ms = 30000U;
  double backoff_multiplier = 2.0;
};

struct StartupPolicy {
  bool enforce_single_instance = true;
  std::string instance_key = "ntium.runtime.startup";
  RestartPolicy restart_policy {};
};

struct StartupContext {
  StartupLaunchReason launch_reason = StartupLaunchReason::kUnknown;
  std::uint64_t now_ms = 0U;
};

struct ExitContext {
  StartupExitReason reason = StartupExitReason::kUnknown;
  std::uint64_t now_ms = 0U;
};

struct StartupDecision {
  StartupDecisionDisposition disposition =
      StartupDecisionDisposition::kAllowLaunch;
  StartupLifecycleState resulting_state = StartupLifecycleState::kIdle;
  SingleInstanceResult single_instance_result = SingleInstanceResult::kAcquired;
  std::uint64_t retry_backoff_ms = 0U;
  std::uint32_t restart_attempt = 0U;
};

struct RestartDecision {
  bool should_restart = false;
  std::uint64_t delay_ms = 0U;
  std::uint32_t restart_attempt = 0U;
  StartupLifecycleState resulting_state = StartupLifecycleState::kIdle;
};

class ISingleInstanceGate {
 public:
  virtual ~ISingleInstanceGate() = default;

  virtual SingleInstanceResult TryAcquire(const std::string& instance_key) = 0;
  virtual void Release() = 0;
  virtual bool IsHeld() const = 0;
};

// Uses Win32 global/local named mutexes on Windows and an in-process stub on
// non-Windows hosts for deterministic tests.
std::unique_ptr<ISingleInstanceGate> CreatePlatformSingleInstanceGate();

class StartupManager {
 public:
  explicit StartupManager(
      StartupPolicy policy = {},
      std::unique_ptr<ISingleInstanceGate> single_instance_gate = {});

  const StartupPolicy& policy() const { return policy_; }
  StartupLifecycleState lifecycle_state() const { return lifecycle_state_; }
  std::uint32_t restart_count_in_window() const {
    return static_cast<std::uint32_t>(restart_attempt_times_ms_.size());
  }

  StartupDecision EvaluateStartup(const StartupContext& context);
  RestartDecision OnProcessExit(const ExitContext& context);

  void MarkProcessRunning(std::uint64_t now_ms);
  void ResetRestartHistory();
  void ReleaseSingleInstance();

 private:
  std::uint64_t ComputeBackoffMs(std::uint32_t restart_attempt) const;
  void PruneRestartWindow(std::uint64_t now_ms);

  StartupPolicy policy_ {};
  std::unique_ptr<ISingleInstanceGate> single_instance_gate_;
  StartupLifecycleState lifecycle_state_ = StartupLifecycleState::kIdle;
  bool running_ = false;
  std::uint64_t started_at_ms_ = 0U;
  std::uint64_t last_exit_ms_ = 0U;
  std::vector<std::uint64_t> restart_attempt_times_ms_;
};

}  // namespace ntium::runtime
