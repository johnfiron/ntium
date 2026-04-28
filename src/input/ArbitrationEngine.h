#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ntium::input {

enum class ArbitrationSource : std::uint8_t {
  kUserInput = 0U,
  kIpc = 1U,
};

enum class ArbitrationChannel : std::uint8_t {
  kCameraTranslate = 0U,
  kCameraRotate = 1U,
  kCameraZoom = 2U,
  kCameraMode = 3U,
  kOverlayStyle = 4U,
  kSystemControl = 5U,
};

inline constexpr std::size_t kArbitrationChannelCount = 6U;

enum class ArbitrationDisposition : std::uint8_t {
  kApplied = 0U,
  kDeferred = 1U,
  kCoalesced = 2U,
  kRejected = 3U,
};

enum class ArbitrationRejectReason : std::uint8_t {
  kNone = 0U,
  kUserLeaseActive = 1U,
  kNonCoalescibleWhileLeased = 2U,
  kInvalidRequest = 3U,
};

enum class ArbitrationEventKind : std::uint8_t {
  kLeaseAcquired = 0U,
  kLeaseRenewed = 1U,
  kLeaseReleased = 2U,
  kLeaseExpired = 3U,
  kDeferredQueued = 4U,
  kDeferredCoalesced = 5U,
  kDeferredDrained = 6U,
  kEmergencyPreempted = 7U,
};

struct ArbitrationRequest {
  ArbitrationSource source = ArbitrationSource::kUserInput;
  ArbitrationChannel channel = ArbitrationChannel::kCameraRotate;
  std::uint64_t source_sequence = 0U;
  std::uint64_t monotonic_time_ms = 0U;

  // For user-originated camera manipulation, this should match
  // InputRouterSnapshot::interaction_epoch. It is used to guard stale
  // release/acquire notifications.
  std::uint64_t user_interaction_epoch = 0U;

  // Set true for hard-stop controls that must preempt both lease and
  // deferred queues (for example, an emergency stop command).
  bool emergency = false;

  // IPC requests can opt into defer+coalesce while user lease is active.
  bool allow_deferred_coalescing = true;

  // Optional payload identity used for deterministic latest-wins coalescing.
  std::uint64_t payload_fingerprint = 0U;
};

struct ArbitrationDecision {
  ArbitrationDisposition disposition = ArbitrationDisposition::kApplied;
  ArbitrationRejectReason reject_reason = ArbitrationRejectReason::kNone;
  bool should_execute_now = true;
  bool held_by_user_lease = false;
  std::uint64_t decision_sequence = 0U;
  std::size_t deferred_count = 0U;
};

struct DeferredArbitrationCommand {
  ArbitrationRequest request{};
  std::uint64_t deferred_at_ms = 0U;
  std::uint32_t coalesce_count = 0U;
};

struct ArbitrationSnapshot {
  bool user_lease_active = false;
  std::uint64_t lease_interaction_epoch = 0U;
  std::uint64_t lease_expires_at_ms = 0U;
  std::uint64_t decision_sequence = 0U;
  std::size_t deferred_count = 0U;
  std::uint64_t total_deferred = 0U;
  std::uint64_t total_coalesced = 0U;
};

struct ArbitrationEvent {
  ArbitrationEventKind kind = ArbitrationEventKind::kLeaseAcquired;
  ArbitrationChannel channel = ArbitrationChannel::kCameraRotate;
  ArbitrationSnapshot snapshot{};
  std::uint64_t now_ms = 0U;
};

using ArbitrationEventCallback = std::function<void(const ArbitrationEvent&)>;

struct ArbitrationConfig {
  // Duration that camera channels remain leased to user input after each
  // interaction heartbeat.
  std::uint64_t user_lease_duration_ms = 180U;

  // If true, a deferred slot with the same fingerprint is still treated as
  // coalesced and replaced by the newest request deterministically.
  bool replace_even_on_matching_fingerprint = true;

  ArbitrationEventCallback event_callback;
};

class ArbitrationEngine {
 public:
  explicit ArbitrationEngine(ArbitrationConfig config = {});

  const ArbitrationSnapshot& snapshot() const { return snapshot_; }

  // Hooks for InputRouter transitions.
  void AcquireOrRenewUserLease(std::uint64_t interaction_epoch, std::uint64_t now_ms);
  bool ReleaseUserLease(std::uint64_t interaction_epoch, std::uint64_t now_ms);

  // Win32 hook: call on WM_KILLFOCUS / WM_CAPTURECHANGED to guarantee command
  // arbitration falls back to IPC if interaction is interrupted.
  void CancelUserLeaseForSystem(std::uint64_t now_ms);

  ArbitrationDecision Evaluate(const ArbitrationRequest& request);

  // Returns deferred IPC commands in deterministic channel order and clears
  // deferred slots.
  std::vector<DeferredArbitrationCommand> DrainDeferred(std::uint64_t now_ms);

  void ClearDeferred();

 private:
  struct DeferredSlot {
    bool occupied = false;
    DeferredArbitrationCommand command{};
  };

  static std::size_t ChannelIndex(ArbitrationChannel channel);
  static bool ChannelUsesUserLease(ArbitrationChannel channel);

  bool HasActiveUserLease(std::uint64_t now_ms) const;
  void ExpireLeaseIfNeeded(std::uint64_t now_ms);
  void Emit(ArbitrationEventKind kind, ArbitrationChannel channel, std::uint64_t now_ms);

  ArbitrationConfig config_;
  ArbitrationSnapshot snapshot_{};
  std::array<DeferredSlot, kArbitrationChannelCount> deferred_{};
};

}  // namespace ntium::input
