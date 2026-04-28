#include "src/overlay/OverlayThrottle.h"

#include <algorithm>

namespace overlay {

OverlayThrottle::OverlayThrottle(std::chrono::milliseconds min_interval)
    : min_interval_(std::max(std::chrono::milliseconds::zero(), min_interval)) {}

bool OverlayThrottle::RecordOverlayInvalidation(uint64_t event_sequence) {
  if (event_sequence == 0) {
    return false;
  }

  const uint64_t floor = std::max(last_present_sequence_, pending_overlay_sequence_);
  if (event_sequence <= floor) {
    return false;
  }

  pending_overlay_sequence_ = event_sequence;
  return true;
}

OverlayThrottle::Decision OverlayThrottle::Evaluate(Clock::time_point now,
                                                    bool overlay_only_work,
                                                    bool bypass_overlay_throttle) const {
  Decision decision {};
  decision.has_pending_overlay = pending_overlay_sequence_ > 0;
  decision.coalesced_sequence = pending_overlay_sequence_;

  if (!decision.has_pending_overlay) {
    return decision;
  }

  if (bypass_overlay_throttle || !overlay_only_work) {
    decision.allow_present = true;
    decision.throttled = false;
    return decision;
  }

  if (!last_present_at_.has_value() || min_interval_ <= std::chrono::milliseconds::zero()) {
    decision.allow_present = true;
    decision.throttled = false;
    return decision;
  }

  const Clock::time_point deadline = *last_present_at_ + min_interval_;
  if (now >= deadline) {
    decision.allow_present = true;
    decision.throttled = false;
    return decision;
  }

  decision.allow_present = false;
  decision.throttled = true;
  decision.next_deadline = deadline;
  return decision;
}

void OverlayThrottle::OnPresented(Clock::time_point now, uint64_t presented_sequence) {
  if (presented_sequence == 0) {
    return;
  }

  last_present_sequence_ = std::max(last_present_sequence_, presented_sequence);
  last_present_at_ = now;

  if (pending_overlay_sequence_ <= last_present_sequence_) {
    pending_overlay_sequence_ = 0;
  }
}

}  // namespace overlay
