#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace overlay {

struct OverlayBounds {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  bool IsValid() const noexcept { return left < right && top < bottom; }

  bool operator==(const OverlayBounds& other) const noexcept {
    return left == other.left && top == other.top &&
           right == other.right && bottom == other.bottom;
  }
};

enum class EventStoreResult : uint8_t {
  kApplied = 0,
  kIgnoredStale,
  kInvalidArgument
};

struct EventStoreOptions {
  // Upper bound for per-key clear/expiry tombstones used to reject stale writes.
  std::size_t max_tombstones = 4096;
};

class EventStore {
 public:
  using Clock = std::chrono::steady_clock;

  struct EventSnapshot {
    std::string event_id;
    OverlayBounds bounds;
    uint64_t event_sequence = 0;
    Clock::time_point expires_at {};
  };

  EventStore();
  explicit EventStore(EventStoreOptions options);

  EventStoreResult Upsert(std::string_view event_id,
                          const OverlayBounds& bounds,
                          uint64_t event_sequence,
                          std::chrono::milliseconds ttl,
                          Clock::time_point now = Clock::now());

  EventStoreResult Clear(std::string_view event_id, uint64_t event_sequence);
  std::size_t ClearAll(uint64_t event_sequence);

  // Removes expired events at or before `now` and returns them in deterministic
  // order (by event_sequence, then event_id).
  std::vector<EventSnapshot> Expire(Clock::time_point now);

  std::optional<EventSnapshot> Get(std::string_view event_id) const;
  std::vector<EventSnapshot> SnapshotOrderedBySequence() const;

  std::size_t size() const noexcept { return records_.size(); }
  bool empty() const noexcept { return records_.empty(); }
  uint64_t latest_sequence() const noexcept { return latest_sequence_; }
  uint64_t global_clear_sequence() const noexcept { return global_clear_sequence_; }

 private:
  struct EventRecord {
    OverlayBounds bounds;
    uint64_t sequence = 0;
    Clock::time_point expires_at {};
  };

  struct ExpiryNode {
    Clock::time_point expires_at {};
    std::string event_id;
    uint64_t sequence = 0;
  };

  struct ExpiryNodeCompare {
    bool operator()(const ExpiryNode& lhs, const ExpiryNode& rhs) const noexcept;
  };

  static std::string ToKey(std::string_view event_id);
  uint64_t LastAppliedSequenceForKey(std::string_view event_id) const;
  bool IsStaleForKey(std::string_view event_id, uint64_t event_sequence) const;
  void RememberTombstone(const std::string& event_id, uint64_t event_sequence);
  void PruneTombstonesIfNeeded();

  EventStoreOptions options_ {};
  uint64_t latest_sequence_ = 0;
  uint64_t global_clear_sequence_ = 0;

  std::unordered_map<std::string, EventRecord> records_;
  std::unordered_map<std::string, uint64_t> tombstone_sequences_;
  std::deque<std::pair<std::string, uint64_t>> tombstone_order_;

  std::priority_queue<ExpiryNode, std::vector<ExpiryNode>, ExpiryNodeCompare>
      expiry_heap_;
};

}  // namespace overlay
