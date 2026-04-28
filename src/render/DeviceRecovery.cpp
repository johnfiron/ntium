#include "src/render/DeviceRecovery.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace render {

namespace {

void MaybeEmitTransition(
    const AdapterIdentity& adapter,
    DeviceRecoveryState from,
    DeviceRecoveryState to,
    uint64_t event_sequence,
    const IDeviceRecoveryStateMachine::TransitionCallback& callback) {
  if (from == to || !callback) {
    return;
  }
  callback(adapter, from, to, event_sequence);
}

class DeviceRecoveryStateMachine final : public IDeviceRecoveryStateMachine {
 public:
  explicit DeviceRecoveryStateMachine(DeviceRecoveryConfig config)
      : config_(std::move(config)) {}

  RenderStatus EnsureTrackedAdapter(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) override {
    auto [it, inserted] = snapshots_.try_emplace(adapter, DeviceRecoverySnapshot{});
    if (inserted) {
      it->second.adapter = adapter;
      it->second.state = DeviceRecoveryState::kReady;
      it->second.last_status = RenderStatus::kOk;
      it->second.retry_count = 0;
      it->second.recovery_generation = 0;
    }
    it->second.last_event_sequence =
        std::max(it->second.last_event_sequence, event_sequence);
    return RenderStatus::kOk;
  }

  RenderStatus RemoveAdapter(const AdapterIdentity& adapter) override {
    const auto erased = snapshots_.erase(adapter);
    return erased == 0 ? RenderStatus::kNotFound : RenderStatus::kOk;
  }

  RenderStatus MarkDeviceLost(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) override {
    auto it = snapshots_.find(adapter);
    if (it == snapshots_.end()) {
      return RenderStatus::kNotFound;
    }
    DeviceRecoverySnapshot& snapshot = it->second;
    const DeviceRecoveryState from = snapshot.state;
    snapshot.state = DeviceRecoveryState::kDeviceLost;
    snapshot.failure_reason = DeviceRecoveryFailureReason::kNone;
    snapshot.last_status = RenderStatus::kDeviceLost;
    snapshot.last_event_sequence = event_sequence;
    MaybeEmitTransition(
        adapter, from, snapshot.state, event_sequence, transition_callback_);
    return RenderStatus::kOk;
  }

  RenderStatus RequestRecovery(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) override {
    auto it = snapshots_.find(adapter);
    if (it == snapshots_.end()) {
      return RenderStatus::kNotFound;
    }
    DeviceRecoverySnapshot& snapshot = it->second;
    if (snapshot.state == DeviceRecoveryState::kReady) {
      return RenderStatus::kOk;
    }

    const DeviceRecoveryState from = snapshot.state;
    snapshot.state = DeviceRecoveryState::kRecoveryScheduled;
    snapshot.last_event_sequence = event_sequence;
    MaybeEmitTransition(
        adapter, from, snapshot.state, event_sequence, transition_callback_);
    return RenderStatus::kOk;
  }

  RenderStatus RunRecoveryStep(
      const AdapterIdentity& adapter,
      uint64_t event_sequence,
      DeviceRecoveryStepResult* out_result) override {
    if (out_result == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    auto it = snapshots_.find(adapter);
    if (it == snapshots_.end()) {
      return RenderStatus::kNotFound;
    }
    DeviceRecoverySnapshot& snapshot = it->second;
    DeviceRecoveryStepResult result{};
    result.state = snapshot.state;
    result.failure_reason = snapshot.failure_reason;
    result.retry_count = snapshot.retry_count;

    if (snapshot.state == DeviceRecoveryState::kReady) {
      result.status = RenderStatus::kOk;
      *out_result = result;
      return result.status;
    }
    if (snapshot.state == DeviceRecoveryState::kFailed) {
      result.status = snapshot.last_status;
      *out_result = result;
      return result.status;
    }

    if (snapshot.state == DeviceRecoveryState::kDeviceLost) {
      const DeviceRecoveryState from = snapshot.state;
      snapshot.state = DeviceRecoveryState::kRecoveryScheduled;
      snapshot.last_event_sequence = event_sequence;
      result.transition_performed = true;
      MaybeEmitTransition(
          adapter, from, snapshot.state, event_sequence, transition_callback_);
    }

    if (snapshot.state != DeviceRecoveryState::kRecoveryScheduled) {
      result.status = snapshot.last_status;
      result.state = snapshot.state;
      result.failure_reason = snapshot.failure_reason;
      result.retry_count = snapshot.retry_count;
      *out_result = result;
      return result.status;
    }

    const DeviceRecoveryState from_scheduled = snapshot.state;
    snapshot.state = DeviceRecoveryState::kRecoveringDevice;
    result.transition_performed = true;
    MaybeEmitTransition(
        adapter,
        from_scheduled,
        snapshot.state,
        event_sequence,
        transition_callback_);

    RenderStatus recover_status = RenderStatus::kUnsupportedPlatform;
    if (hooks_.recover_device) {
      recover_status = hooks_.recover_device(adapter, event_sequence);
    }
    if (recover_status != RenderStatus::kOk) {
      snapshot.last_status = recover_status;
      snapshot.failure_reason = DeviceRecoveryFailureReason::kRecoverDeviceFailed;
      AdvanceFailureState(adapter, event_sequence, &snapshot);

      result.status = snapshot.last_status;
      result.state = snapshot.state;
      result.failure_reason = snapshot.failure_reason;
      result.retry_count = snapshot.retry_count;
      result.retry_scheduled =
          snapshot.state == DeviceRecoveryState::kRecoveryScheduled;
      *out_result = result;
      return result.status;
    }

    const DeviceRecoveryState from_recovering = snapshot.state;
    snapshot.state = DeviceRecoveryState::kRebindingSwapchains;
    MaybeEmitTransition(
        adapter,
        from_recovering,
        snapshot.state,
        event_sequence,
        transition_callback_);

    RenderStatus rebind_status = RenderStatus::kUnsupportedPlatform;
    if (hooks_.rebind_swapchains) {
      rebind_status = hooks_.rebind_swapchains(adapter, event_sequence);
    }
    if (rebind_status != RenderStatus::kOk) {
      snapshot.last_status = rebind_status;
      snapshot.failure_reason = DeviceRecoveryFailureReason::kSwapchainRebindFailed;
      AdvanceFailureState(adapter, event_sequence, &snapshot);

      result.status = snapshot.last_status;
      result.state = snapshot.state;
      result.failure_reason = snapshot.failure_reason;
      result.retry_count = snapshot.retry_count;
      result.retry_scheduled =
          snapshot.state == DeviceRecoveryState::kRecoveryScheduled;
      *out_result = result;
      return result.status;
    }

    const DeviceRecoveryState from_rebinding = snapshot.state;
    snapshot.state = DeviceRecoveryState::kReady;
    snapshot.last_status = RenderStatus::kOk;
    snapshot.failure_reason = DeviceRecoveryFailureReason::kNone;
    snapshot.retry_count = 0;
    ++snapshot.recovery_generation;
    snapshot.last_event_sequence = event_sequence;
    result.recovery_completed = true;
    MaybeEmitTransition(
        adapter,
        from_rebinding,
        snapshot.state,
        event_sequence,
        transition_callback_);
    if (hooks_.on_recovery_succeeded) {
      hooks_.on_recovery_succeeded(
          adapter, event_sequence, snapshot.recovery_generation);
    }

    result.status = snapshot.last_status;
    result.state = snapshot.state;
    result.failure_reason = snapshot.failure_reason;
    result.retry_count = snapshot.retry_count;
    *out_result = result;
    return result.status;
  }

  RenderStatus GetSnapshot(
      const AdapterIdentity& adapter,
      DeviceRecoverySnapshot* out_snapshot) const override {
    if (out_snapshot == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    const auto it = snapshots_.find(adapter);
    if (it == snapshots_.end()) {
      return RenderStatus::kNotFound;
    }
    *out_snapshot = it->second;
    return RenderStatus::kOk;
  }

  std::vector<AdapterIdentity> ListTrackedAdapters() const override {
    std::vector<AdapterIdentity> adapters;
    adapters.reserve(snapshots_.size());
    for (const auto& [adapter, _] : snapshots_) {
      (void)_;
      adapters.push_back(adapter);
    }
    return adapters;
  }

  void SetHooks(DeviceRecoveryHooks hooks) override { hooks_ = std::move(hooks); }

  void SetTransitionCallback(TransitionCallback callback) override {
    transition_callback_ = std::move(callback);
  }

 private:
  void AdvanceFailureState(
      const AdapterIdentity& adapter,
      uint64_t event_sequence,
      DeviceRecoverySnapshot* snapshot) {
    if (snapshot == nullptr) {
      return;
    }

    ++snapshot->retry_count;
    snapshot->last_event_sequence = event_sequence;
    const DeviceRecoveryState from = snapshot->state;
    if (snapshot->retry_count > config_.max_retries) {
      snapshot->state = DeviceRecoveryState::kFailed;
      snapshot->failure_reason = DeviceRecoveryFailureReason::kRetryBudgetExhausted;
    } else {
      snapshot->state = DeviceRecoveryState::kRecoveryScheduled;
    }
    MaybeEmitTransition(
        adapter, from, snapshot->state, event_sequence, transition_callback_);
  }

  DeviceRecoveryConfig config_;
  DeviceRecoveryHooks hooks_;
  TransitionCallback transition_callback_;
  std::unordered_map<AdapterIdentity, DeviceRecoverySnapshot, AdapterIdentityHash>
      snapshots_;
};

}  // namespace

std::unique_ptr<IDeviceRecoveryStateMachine> CreateDeviceRecoveryStateMachine(
    DeviceRecoveryConfig config) {
  return std::make_unique<DeviceRecoveryStateMachine>(std::move(config));
}

}  // namespace render
