#include "src/runtime/StartupManager.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace ntium::runtime {
namespace {

class PlatformSingleInstanceGate final : public ISingleInstanceGate {
 public:
  PlatformSingleInstanceGate() = default;
  ~PlatformSingleInstanceGate() override { Release(); }

  SingleInstanceResult TryAcquire(const std::string& instance_key) override {
    if (owns_instance_) {
      return SingleInstanceResult::kAcquired;
    }
    if (instance_key.empty()) {
      return SingleInstanceResult::kUnavailable;
    }

#ifdef _WIN32
    held_instance_key_ = "Local\\" + instance_key;
    mutex_handle_ = CreateMutexA(nullptr, FALSE, held_instance_key_.c_str());
    if (mutex_handle_ == nullptr) {
      held_instance_key_.clear();
      return SingleInstanceResult::kUnavailable;
    }

    const DWORD wait_result = WaitForSingleObject(mutex_handle_, 0);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
      owns_instance_ = true;
      return SingleInstanceResult::kAcquired;
    }

    CloseHandle(mutex_handle_);
    mutex_handle_ = nullptr;
    held_instance_key_.clear();
    if (wait_result == WAIT_TIMEOUT) {
      return SingleInstanceResult::kAlreadyRunning;
    }
    return SingleInstanceResult::kUnavailable;
#else
    std::lock_guard<std::mutex> lock(GlobalMutex());
    auto& global_leases = GlobalLeases();
    if (global_leases.find(instance_key) != global_leases.end()) {
      return SingleInstanceResult::kAlreadyRunning;
    }
    global_leases.insert(instance_key);
    held_instance_key_ = instance_key;
    owns_instance_ = true;
    return SingleInstanceResult::kAcquired;
#endif
  }

  void Release() override {
    if (!owns_instance_) {
      return;
    }

#ifdef _WIN32
    if (mutex_handle_ != nullptr) {
      (void)ReleaseMutex(mutex_handle_);
      CloseHandle(mutex_handle_);
      mutex_handle_ = nullptr;
    }
#else
    std::lock_guard<std::mutex> lock(GlobalMutex());
    if (!held_instance_key_.empty()) {
      GlobalLeases().erase(held_instance_key_);
    }
#endif

    held_instance_key_.clear();
    owns_instance_ = false;
  }

  bool IsHeld() const override { return owns_instance_; }

 private:
#ifndef _WIN32
  static std::unordered_set<std::string>& GlobalLeases() {
    static std::unordered_set<std::string> leases;
    return leases;
  }

  static std::mutex& GlobalMutex() {
    static std::mutex gate_mutex;
    return gate_mutex;
  }
#endif

  std::string held_instance_key_;
  bool owns_instance_ = false;
#ifdef _WIN32
  HANDLE mutex_handle_ = nullptr;
#endif
};

bool IsRestartCandidate(StartupExitReason reason) {
  switch (reason) {
    case StartupExitReason::kCrash:
    case StartupExitReason::kWatchdogTermination:
    case StartupExitReason::kPolicyRestart:
      return true;
    case StartupExitReason::kUnknown:
    case StartupExitReason::kGracefulShutdown:
    case StartupExitReason::kOsShutdown:
      return false;
  }
  return false;
}

bool IsRestartLaunch(StartupLaunchReason launch_reason) {
  return launch_reason == StartupLaunchReason::kScheduledRestart ||
         launch_reason == StartupLaunchReason::kCrashRecovery ||
         launch_reason == StartupLaunchReason::kManualRestart;
}

}  // namespace

std::unique_ptr<ISingleInstanceGate> CreatePlatformSingleInstanceGate() {
  return std::make_unique<PlatformSingleInstanceGate>();
}

StartupManager::StartupManager(
    StartupPolicy policy,
    std::unique_ptr<ISingleInstanceGate> single_instance_gate)
    : policy_(std::move(policy)),
      single_instance_gate_(std::move(single_instance_gate)) {
  if (policy_.enforce_single_instance && single_instance_gate_ == nullptr) {
    single_instance_gate_ = CreatePlatformSingleInstanceGate();
  }
}

StartupDecision StartupManager::EvaluateStartup(const StartupContext& context) {
  StartupDecision decision;
  decision.resulting_state = lifecycle_state_;
  decision.single_instance_result = SingleInstanceResult::kAcquired;

  PruneRestartWindow(context.now_ms);

  if (policy_.enforce_single_instance) {
    if (single_instance_gate_ == nullptr) {
      decision.disposition = StartupDecisionDisposition::kDenyPolicySuppressed;
      decision.single_instance_result = SingleInstanceResult::kUnavailable;
      lifecycle_state_ = StartupLifecycleState::kRestartSuppressed;
      decision.resulting_state = lifecycle_state_;
      return decision;
    }

    const SingleInstanceResult lock_result =
        single_instance_gate_->TryAcquire(policy_.instance_key);
    decision.single_instance_result = lock_result;
    if (lock_result == SingleInstanceResult::kAlreadyRunning) {
      decision.disposition = StartupDecisionDisposition::kDenyDuplicateInstance;
      lifecycle_state_ = StartupLifecycleState::kRestartSuppressed;
      decision.resulting_state = lifecycle_state_;
      return decision;
    }
    if (lock_result == SingleInstanceResult::kUnavailable) {
      decision.disposition = StartupDecisionDisposition::kDenyPolicySuppressed;
      lifecycle_state_ = StartupLifecycleState::kRestartSuppressed;
      decision.resulting_state = lifecycle_state_;
      return decision;
    }
  }

  decision.disposition = StartupDecisionDisposition::kAllowLaunch;
  if (IsRestartLaunch(context.launch_reason) &&
      policy_.restart_policy.enable_restart) {
    if (restart_attempt_times_ms_.size() >=
        policy_.restart_policy.max_restarts_per_window) {
      decision.disposition = StartupDecisionDisposition::kDenyPolicySuppressed;
      lifecycle_state_ = StartupLifecycleState::kRestartSuppressed;
      decision.resulting_state = lifecycle_state_;
      return decision;
    }

    const std::uint32_t restart_attempt =
        static_cast<std::uint32_t>(restart_attempt_times_ms_.size() + 1U);
    decision.restart_attempt = restart_attempt;
    decision.retry_backoff_ms = ComputeBackoffMs(restart_attempt);
    if (decision.retry_backoff_ms > 0U) {
      decision.disposition = StartupDecisionDisposition::kDelayWithBackoff;
      lifecycle_state_ = StartupLifecycleState::kRestartPending;
      decision.resulting_state = lifecycle_state_;
      return decision;
    }
  }

  lifecycle_state_ = StartupLifecycleState::kLaunchApproved;
  decision.resulting_state = lifecycle_state_;
  return decision;
}

RestartDecision StartupManager::OnProcessExit(const ExitContext& context) {
  RestartDecision decision;
  decision.resulting_state = lifecycle_state_;

  running_ = false;
  last_exit_ms_ = context.now_ms;
  PruneRestartWindow(context.now_ms);

  if (!policy_.restart_policy.enable_restart || !IsRestartCandidate(context.reason)) {
    lifecycle_state_ = StartupLifecycleState::kIdle;
    ReleaseSingleInstance();
    decision.resulting_state = lifecycle_state_;
    return decision;
  }

  if (restart_attempt_times_ms_.size() >=
      policy_.restart_policy.max_restarts_per_window) {
    lifecycle_state_ = StartupLifecycleState::kRestartSuppressed;
    ReleaseSingleInstance();
    decision.resulting_state = lifecycle_state_;
    return decision;
  }

  restart_attempt_times_ms_.push_back(context.now_ms);
  const std::uint32_t restart_attempt =
      static_cast<std::uint32_t>(restart_attempt_times_ms_.size());

  decision.should_restart = true;
  decision.restart_attempt = restart_attempt;
  decision.delay_ms = ComputeBackoffMs(restart_attempt);

  lifecycle_state_ = StartupLifecycleState::kRestartPending;
  ReleaseSingleInstance();
  decision.resulting_state = lifecycle_state_;
  return decision;
}

void StartupManager::MarkProcessRunning(std::uint64_t now_ms) {
  running_ = true;
  started_at_ms_ = now_ms;
  lifecycle_state_ = StartupLifecycleState::kRunning;
}

void StartupManager::ResetRestartHistory() { restart_attempt_times_ms_.clear(); }

void StartupManager::ReleaseSingleInstance() {
  if (single_instance_gate_ != nullptr) {
    single_instance_gate_->Release();
  }
}

std::uint64_t StartupManager::ComputeBackoffMs(std::uint32_t restart_attempt) const {
  const RestartPolicy& restart_policy = policy_.restart_policy;
  if (restart_attempt == 0U) {
    return 0U;
  }

  double delay = static_cast<double>(restart_policy.min_backoff_ms);
  for (std::uint32_t i = 1U; i < restart_attempt; ++i) {
    delay *= restart_policy.backoff_multiplier;
    if (delay >= static_cast<double>(restart_policy.max_backoff_ms)) {
      delay = static_cast<double>(restart_policy.max_backoff_ms);
      break;
    }
  }

  const auto clamped_delay = static_cast<std::uint64_t>(std::llround(delay));
  return std::clamp(
      clamped_delay, restart_policy.min_backoff_ms, restart_policy.max_backoff_ms);
}

void StartupManager::PruneRestartWindow(std::uint64_t now_ms) {
  const std::uint64_t window_ms = policy_.restart_policy.restart_window_ms;
  if (window_ms == 0U) {
    restart_attempt_times_ms_.clear();
    return;
  }

  const std::uint64_t floor_time =
      now_ms > window_ms ? now_ms - window_ms : 0U;
  auto new_begin = restart_attempt_times_ms_.begin();
  while (new_begin != restart_attempt_times_ms_.end() && *new_begin < floor_time) {
    ++new_begin;
  }
  restart_attempt_times_ms_.erase(restart_attempt_times_ms_.begin(), new_begin);
}

}  // namespace ntium::runtime
