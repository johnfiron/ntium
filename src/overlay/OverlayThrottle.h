#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace overlay {

class OverlayThrottle {
 public:
  using Clock = std::chrono::steady_clock;

  struct Decision {
    bool allow_present = false;
    bool throttled = false;
    bool has_pending_overlay = false;
    uint64_t coalesced_sequence = 0;
    std::optional<Clock::time_point> next_deadline;
  };

  explicit OverlayThrottle(
      std::chrono::milliseconds min_interval = std::chrono::milliseconds(100));

  // Records an overlay invalidation sequence. Returns false when the sequence is
  // stale versus already pending/presented work.
  bool RecordOverlayInvalidation(uint64_t event_sequence);

  // Evaluates whether present should execute at `now`.
  // - overlay_only_work: scheduler currently has only overlay work pending.
  // - bypass_overlay_throttle: urgent ingest/input/runtime work is pending and
  //   may piggyback overlay updates immediately.
  Decision Evaluate(Clock::time_point now,
                    bool overlay_only_work,
                    bool bypass_overlay_throttle) const;

  // Marks a completed present decision.
  void OnPresented(Clock::time_point now, uint64_t presented_sequence);

  bool has_pending_overlay() const noexcept { return pending_overlay_sequence_ > 0; }
  uint64_t latest_pending_overlay_sequence() const noexcept {
    return pending_overlay_sequence_;
  }
  uint64_t last_present_sequence() const noexcept { return last_present_sequence_; }
  std::chrono::milliseconds min_interval() const noexcept { return min_interval_; }

 private:
  std::chrono::milliseconds min_interval_ {};
  uint64_t pending_overlay_sequence_ = 0;
  uint64_t last_present_sequence_ = 0;
  std::optional<Clock::time_point> last_present_at_;
};

}  // namespace overlay
