#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "src/render/RenderTypes.h"

namespace render {

enum class DeviceRecoveryState : uint8_t {
  kReady = 0,
  kDeviceLost,
  kRecoveryScheduled,
  kRecoveringDevice,
  kRebindingSwapchains,
  kFailed
};

enum class DeviceRecoveryFailureReason : uint8_t {
  kNone = 0,
  kRecoverDeviceFailed,
  kSwapchainRebindFailed,
  kRetryBudgetExhausted
};

struct DeviceRecoveryConfig {
  uint32_t max_retries = 3;
};

struct DeviceRecoverySnapshot {
  AdapterIdentity adapter;
  DeviceRecoveryState state = DeviceRecoveryState::kReady;
  DeviceRecoveryFailureReason failure_reason = DeviceRecoveryFailureReason::kNone;
  RenderStatus last_status = RenderStatus::kOk;
  uint32_t retry_count = 0;
  uint64_t recovery_generation = 0;
  uint64_t last_event_sequence = 0;
};

// Recovery state machine hooks. Host runtime owns scheduling and invokes
// RunRecoveryStep only when lifecycle events request progression.
struct DeviceRecoveryHooks {
  // Recreate adapter-scoped device resources.
  std::function<RenderStatus(const AdapterIdentity&, uint64_t event_sequence)>
      recover_device;

  // Mark/rebind swapchains for monitors attached to the recovered adapter.
  std::function<RenderStatus(const AdapterIdentity&, uint64_t event_sequence)>
      rebind_swapchains;

  // Optional callback after successful transition back to Ready.
  std::function<void(
      const AdapterIdentity&,
      uint64_t event_sequence,
      uint64_t recovery_generation)>
      on_recovery_succeeded;
};

struct DeviceRecoveryStepResult {
  RenderStatus status = RenderStatus::kOk;
  DeviceRecoveryState state = DeviceRecoveryState::kReady;
  DeviceRecoveryFailureReason failure_reason = DeviceRecoveryFailureReason::kNone;
  bool transition_performed = false;
  bool recovery_completed = false;
  bool retry_scheduled = false;
  uint32_t retry_count = 0;
};

class IDeviceRecoveryStateMachine {
 public:
  using TransitionCallback = std::function<void(
      const AdapterIdentity& adapter,
      DeviceRecoveryState from,
      DeviceRecoveryState to,
      uint64_t event_sequence)>;

  virtual ~IDeviceRecoveryStateMachine() = default;

  virtual RenderStatus EnsureTrackedAdapter(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) = 0;
  virtual RenderStatus RemoveAdapter(const AdapterIdentity& adapter) = 0;

  virtual RenderStatus MarkDeviceLost(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) = 0;

  // Schedules deterministic recovery progression. No implicit render loop
  // exists; runtime must call RunRecoveryStep in response to events/timers.
  virtual RenderStatus RequestRecovery(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) = 0;

  // Executes at most one full deterministic recovery attempt:
  // RecoveryScheduled -> RecoveringDevice -> RebindingSwapchains -> Ready
  // or transitions back to RecoveryScheduled/Failed on failures.
  virtual RenderStatus RunRecoveryStep(
      const AdapterIdentity& adapter,
      uint64_t event_sequence,
      DeviceRecoveryStepResult* out_result) = 0;

  virtual RenderStatus GetSnapshot(
      const AdapterIdentity& adapter,
      DeviceRecoverySnapshot* out_snapshot) const = 0;

  virtual std::vector<AdapterIdentity> ListTrackedAdapters() const = 0;

  virtual void SetHooks(DeviceRecoveryHooks hooks) = 0;
  virtual void SetTransitionCallback(TransitionCallback callback) = 0;
};

std::unique_ptr<IDeviceRecoveryStateMachine> CreateDeviceRecoveryStateMachine(
    DeviceRecoveryConfig config = {});

}  // namespace render
