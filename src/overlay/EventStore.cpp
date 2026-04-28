#include "src/overlay/EventStore.h"

#include <algorithm>
#include <utility>

namespace overlay {

namespace {

bool SnapshotSequenceOrder(const EventStore::EventSnapshot& lhs,
                           const EventStore::EventSnapshot& rhs) {
  if (lhs.event_sequence != rhs.event_sequence) {
    return lhs.event_sequence < rhs.event_sequence;
  }
  return lhs.event_id < rhs.event_id;
}

}  // namespace

bool EventStore::ExpiryNodeCompare::operator()(const ExpiryNode& lhs,
                                               const ExpiryNode& rhs) const noexcept {
  if (lhs.expires_at != rhs.expires_at) {
    // std::priority_queue is max-heap by default; reverse comparison for min-heap.
    return lhs.expires_at > rhs.expires_at;
  }
  if (lhs.sequence != rhs.sequence) {
    return lhs.sequence > rhs.sequence;
  }
  return lhs.event_id > rhs.event_id;
}

EventStore::EventStore() = default;

EventStore::EventStore(EventStoreOptions options) : options_(options) {}

EventStoreResult EventStore::Upsert(std::string_view event_id,
                                    const OverlayBounds& bounds,
                                    uint64_t event_sequence,
                                    std::chrono::milliseconds ttl,
                                    Clock::time_point now) {
  if (event_id.empty() || !bounds.IsValid() || ttl.count() < 0) {
    return EventStoreResult::kInvalidArgument;
  }
  if (IsStaleForKey(event_id, event_sequence)) {
    return EventStoreResult::kIgnoredStale;
  }

  const std::string key = ToKey(event_id);
  const Clock::time_point expires_at = now + ttl;

  records_[key] = EventRecord {
      bounds,
      event_sequence,
      expires_at,
  };
  expiry_heap_.push(ExpiryNode {
      expires_at,
      key,
      event_sequence,
  });

  latest_sequence_ = std::max(latest_sequence_, event_sequence);
  return EventStoreResult::kApplied;
}

EventStoreResult EventStore::Clear(std::string_view event_id, uint64_t event_sequence) {
  if (event_id.empty()) {
    return EventStoreResult::kInvalidArgument;
  }
  if (IsStaleForKey(event_id, event_sequence)) {
    return EventStoreResult::kIgnoredStale;
  }

  const std::string key = ToKey(event_id);
  records_.erase(key);
  RememberTombstone(key, event_sequence);
  latest_sequence_ = std::max(latest_sequence_, event_sequence);
  return EventStoreResult::kApplied;
}

std::size_t EventStore::ClearAll(uint64_t event_sequence) {
  if (event_sequence <= global_clear_sequence_) {
    return 0;
  }

  const std::size_t cleared = records_.size();
  records_.clear();
  tombstone_sequences_.clear();
  tombstone_order_.clear();

  global_clear_sequence_ = event_sequence;
  latest_sequence_ = std::max(latest_sequence_, event_sequence);
  return cleared;
}

std::vector<EventStore::EventSnapshot> EventStore::Expire(Clock::time_point now) {
  std::vector<EventSnapshot> expired;

  while (!expiry_heap_.empty()) {
    const ExpiryNode node = expiry_heap_.top();
    if (node.expires_at > now) {
      break;
    }
    expiry_heap_.pop();

    const auto record_it = records_.find(node.event_id);
    if (record_it == records_.end()) {
      continue;
    }
    if (record_it->second.sequence != node.sequence) {
      continue;
    }
    if (record_it->second.expires_at != node.expires_at) {
      continue;
    }
    if (record_it->second.expires_at > now) {
      continue;
    }

    expired.push_back(EventSnapshot {
        node.event_id,
        record_it->second.bounds,
        record_it->second.sequence,
        record_it->second.expires_at,
    });
    records_.erase(record_it);
    RememberTombstone(node.event_id, node.sequence);
  }

  std::sort(expired.begin(), expired.end(), SnapshotSequenceOrder);
  return expired;
}

std::optional<EventStore::EventSnapshot> EventStore::Get(std::string_view event_id) const {
  const auto it = records_.find(ToKey(event_id));
  if (it == records_.end()) {
    return std::nullopt;
  }

  return EventSnapshot {
      it->first,
      it->second.bounds,
      it->second.sequence,
      it->second.expires_at,
  };
}

std::vector<EventStore::EventSnapshot> EventStore::SnapshotOrderedBySequence() const {
  std::vector<EventSnapshot> snapshot;
  snapshot.reserve(records_.size());
  for (const auto& [event_id, record] : records_) {
    snapshot.push_back(EventSnapshot {
        event_id,
        record.bounds,
        record.sequence,
        record.expires_at,
    });
  }
  std::sort(snapshot.begin(), snapshot.end(), SnapshotSequenceOrder);
  return snapshot;
}

std::string EventStore::ToKey(std::string_view event_id) {
  return std::string(event_id);
}

uint64_t EventStore::LastAppliedSequenceForKey(std::string_view event_id) const {
  const std::string key = ToKey(event_id);
  uint64_t sequence_floor = global_clear_sequence_;

  const auto record_it = records_.find(key);
  if (record_it != records_.end()) {
    sequence_floor = std::max(sequence_floor, record_it->second.sequence);
  }

  const auto tombstone_it = tombstone_sequences_.find(key);
  if (tombstone_it != tombstone_sequences_.end()) {
    sequence_floor = std::max(sequence_floor, tombstone_it->second);
  }
  return sequence_floor;
}

bool EventStore::IsStaleForKey(std::string_view event_id, uint64_t event_sequence) const {
  return event_sequence <= LastAppliedSequenceForKey(event_id);
}

void EventStore::RememberTombstone(const std::string& event_id, uint64_t event_sequence) {
  auto [it, inserted] = tombstone_sequences_.try_emplace(event_id, event_sequence);
  if (!inserted && event_sequence <= it->second) {
    return;
  }
  it->second = event_sequence;
  tombstone_order_.emplace_back(event_id, event_sequence);
  PruneTombstonesIfNeeded();
}

void EventStore::PruneTombstonesIfNeeded() {
  const std::size_t cap = options_.max_tombstones;
  if (cap == 0) {
    tombstone_sequences_.clear();
    tombstone_order_.clear();
    return;
  }

  while (tombstone_sequences_.size() > cap && !tombstone_order_.empty()) {
    const auto [event_id, event_sequence] = tombstone_order_.front();
    tombstone_order_.pop_front();

    const auto it = tombstone_sequences_.find(event_id);
    if (it == tombstone_sequences_.end()) {
      continue;
    }
    if (it->second != event_sequence) {
      continue;
    }
    tombstone_sequences_.erase(it);
  }
}

}  // namespace overlay
