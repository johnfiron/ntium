#include "src/input/ArbitrationEngine.h"

#include <algorithm>
#include <utility>

namespace ntium::input {
namespace {

constexpr ArbitrationChannel kDrainOrder[] = {
    ArbitrationChannel::kSystemControl,   ArbitrationChannel::kCameraMode,
    ArbitrationChannel::kCameraTranslate, ArbitrationChannel::kCameraRotate,
    ArbitrationChannel::kCameraZoom,      ArbitrationChannel::kOverlayStyle,
};

}  // namespace

ArbitrationEngine::ArbitrationEngine(ArbitrationConfig config)
    : config_(std::move(config)) {}

void ArbitrationEngine::AcquireOrRenewUserLease(std::uint64_t interaction_epoch,
                                                std::uint64_t now_ms) {
  if (interaction_epoch == 0U) {
    return;
  }
  ExpireLeaseIfNeeded(now_ms);

  const bool was_active = snapshot_.user_lease_active;
  snapshot_.user_lease_active = true;
  snapshot_.lease_interaction_epoch = interaction_epoch;
  snapshot_.lease_expires_at_ms = now_ms + config_.user_lease_duration_ms;

  Emit(was_active ? ArbitrationEventKind::kLeaseRenewed
                  : ArbitrationEventKind::kLeaseAcquired,
       ArbitrationChannel::kCameraRotate, now_ms);
}

bool ArbitrationEngine::ReleaseUserLease(std::uint64_t interaction_epoch,
                                         std::uint64_t now_ms) {
  ExpireLeaseIfNeeded(now_ms);
  if (!snapshot_.user_lease_active) {
    return false;
  }
  if (interaction_epoch != 0U &&
      interaction_epoch != snapshot_.lease_interaction_epoch) {
    return false;
  }
  snapshot_.user_lease_active = false;
  snapshot_.lease_expires_at_ms = 0U;
  Emit(ArbitrationEventKind::kLeaseReleased, ArbitrationChannel::kCameraRotate,
       now_ms);
  return true;
}

void ArbitrationEngine::CancelUserLeaseForSystem(std::uint64_t now_ms) {
  ExpireLeaseIfNeeded(now_ms);
  if (!snapshot_.user_lease_active) {
    return;
  }
  snapshot_.user_lease_active = false;
  snapshot_.lease_expires_at_ms = 0U;
  Emit(ArbitrationEventKind::kLeaseReleased, ArbitrationChannel::kCameraRotate,
       now_ms);
}

ArbitrationDecision ArbitrationEngine::Evaluate(
    const ArbitrationRequest& request) {
  ArbitrationDecision decision;
  if (request.source_sequence == 0U) {
    decision.disposition = ArbitrationDisposition::kRejected;
    decision.reject_reason = ArbitrationRejectReason::kInvalidRequest;
    decision.should_execute_now = false;
    decision.held_by_user_lease = snapshot_.user_lease_active;
    decision.decision_sequence = snapshot_.decision_sequence;
    decision.deferred_count = snapshot_.deferred_count;
    return decision;
  }

  ExpireLeaseIfNeeded(request.monotonic_time_ms);
  snapshot_.decision_sequence += 1U;
  decision.decision_sequence = snapshot_.decision_sequence;

  if (request.emergency) {
    snapshot_.user_lease_active = false;
    snapshot_.lease_expires_at_ms = 0U;
    ClearDeferred();
    Emit(ArbitrationEventKind::kEmergencyPreempted, request.channel,
         request.monotonic_time_ms);
    decision.disposition = ArbitrationDisposition::kApplied;
    decision.reject_reason = ArbitrationRejectReason::kNone;
    decision.should_execute_now = true;
    decision.held_by_user_lease = false;
    decision.deferred_count = snapshot_.deferred_count;
    return decision;
  }

  if (request.source == ArbitrationSource::kUserInput &&
      ChannelUsesUserLease(request.channel)) {
    AcquireOrRenewUserLease(request.user_interaction_epoch == 0U
                                ? request.source_sequence
                                : request.user_interaction_epoch,
                            request.monotonic_time_ms);
    decision.disposition = ArbitrationDisposition::kApplied;
    decision.reject_reason = ArbitrationRejectReason::kNone;
    decision.should_execute_now = true;
    decision.held_by_user_lease = false;
    decision.deferred_count = snapshot_.deferred_count;
    return decision;
  }

  const bool user_lease_blocks =
      request.source == ArbitrationSource::kIpc &&
      ChannelUsesUserLease(request.channel) &&
      HasActiveUserLease(request.monotonic_time_ms);
  if (!user_lease_blocks) {
    decision.disposition = ArbitrationDisposition::kApplied;
    decision.reject_reason = ArbitrationRejectReason::kNone;
    decision.should_execute_now = true;
    decision.held_by_user_lease = false;
    decision.deferred_count = snapshot_.deferred_count;
    return decision;
  }

  if (!request.allow_deferred_coalescing) {
    decision.disposition = ArbitrationDisposition::kRejected;
    decision.reject_reason = ArbitrationRejectReason::kNonCoalescibleWhileLeased;
    decision.should_execute_now = false;
    decision.held_by_user_lease = true;
    decision.deferred_count = snapshot_.deferred_count;
    return decision;
  }

  DeferredSlot& slot = deferred_[ChannelIndex(request.channel)];
  if (!slot.occupied) {
    slot.occupied = true;
    slot.command.request = request;
    slot.command.deferred_at_ms = request.monotonic_time_ms;
    slot.command.coalesce_count = 0U;
    snapshot_.deferred_count += 1U;
    snapshot_.total_deferred += 1U;
    Emit(ArbitrationEventKind::kDeferredQueued, request.channel,
         request.monotonic_time_ms);
    decision.disposition = ArbitrationDisposition::kDeferred;
  } else {
    const bool should_replace =
        config_.replace_even_on_matching_fingerprint ||
        slot.command.request.payload_fingerprint != request.payload_fingerprint;
    if (should_replace) {
      slot.command.request = request;
      slot.command.coalesce_count += 1U;
      slot.command.deferred_at_ms = request.monotonic_time_ms;
      snapshot_.total_coalesced += 1U;
      Emit(ArbitrationEventKind::kDeferredCoalesced, request.channel,
           request.monotonic_time_ms);
    }
    decision.disposition = ArbitrationDisposition::kCoalesced;
  }

  decision.reject_reason = ArbitrationRejectReason::kUserLeaseActive;
  decision.should_execute_now = false;
  decision.held_by_user_lease = true;
  decision.deferred_count = snapshot_.deferred_count;
  return decision;
}

std::vector<DeferredArbitrationCommand> ArbitrationEngine::DrainDeferred(
    std::uint64_t now_ms) {
  ExpireLeaseIfNeeded(now_ms);
  std::vector<DeferredArbitrationCommand> drained;
  if (snapshot_.deferred_count == 0U) {
    return drained;
  }

  drained.reserve(snapshot_.deferred_count);
  for (const ArbitrationChannel channel : kDrainOrder) {
    DeferredSlot& slot = deferred_[ChannelIndex(channel)];
    if (!slot.occupied) {
      continue;
    }
    drained.push_back(slot.command);
    slot.occupied = false;
  }
  const std::size_t old_count = snapshot_.deferred_count;
  snapshot_.deferred_count = 0U;
  if (old_count > 0U) {
    Emit(ArbitrationEventKind::kDeferredDrained, ArbitrationChannel::kCameraRotate,
         now_ms);
  }
  return drained;
}

void ArbitrationEngine::ClearDeferred() {
  for (DeferredSlot& slot : deferred_) {
    slot.occupied = false;
  }
  snapshot_.deferred_count = 0U;
}

std::size_t ArbitrationEngine::ChannelIndex(ArbitrationChannel channel) {
  return static_cast<std::size_t>(channel);
}

bool ArbitrationEngine::ChannelUsesUserLease(ArbitrationChannel channel) {
  switch (channel) {
    case ArbitrationChannel::kCameraTranslate:
    case ArbitrationChannel::kCameraRotate:
    case ArbitrationChannel::kCameraZoom:
    case ArbitrationChannel::kCameraMode:
      return true;
    case ArbitrationChannel::kOverlayStyle:
    case ArbitrationChannel::kSystemControl:
      return false;
  }
  return false;
}

bool ArbitrationEngine::HasActiveUserLease(std::uint64_t now_ms) const {
  return snapshot_.user_lease_active && now_ms < snapshot_.lease_expires_at_ms;
}

void ArbitrationEngine::ExpireLeaseIfNeeded(std::uint64_t now_ms) {
  if (!snapshot_.user_lease_active) {
    return;
  }
  if (now_ms < snapshot_.lease_expires_at_ms) {
    return;
  }
  snapshot_.user_lease_active = false;
  snapshot_.lease_expires_at_ms = 0U;
  Emit(ArbitrationEventKind::kLeaseExpired, ArbitrationChannel::kCameraRotate,
       now_ms);
}

void ArbitrationEngine::Emit(ArbitrationEventKind kind,
                             ArbitrationChannel channel,
                             std::uint64_t now_ms) {
  if (!config_.event_callback) {
    return;
  }
  ArbitrationEvent event;
  event.kind = kind;
  event.channel = channel;
  event.snapshot = snapshot_;
  event.now_ms = now_ms;
  config_.event_callback(event);
}

}  // namespace ntium::input
