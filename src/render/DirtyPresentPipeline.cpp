#include "src/render/DirtyPresentPipeline.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace render {

namespace {

struct PendingMonitorState {
  uint64_t latest_event_sequence = 0;
  bool force_full_frame = false;
  PresentReason reason = PresentReason::kInvalidation;
  bool bypass_overlay_throttle = false;
  DirtyFullFrameFallbackReason fallback_reason =
      DirtyFullFrameFallbackReason::kNone;
  std::vector<DirtyRectPx> dirty_rects;
};

bool IsMonitorIdentityLess(
    const MonitorIdentity& lhs,
    const MonitorIdentity& rhs) noexcept {
  if (lhs.adapter.luid_high_part != rhs.adapter.luid_high_part) {
    return lhs.adapter.luid_high_part < rhs.adapter.luid_high_part;
  }
  if (lhs.adapter.luid_low_part != rhs.adapter.luid_low_part) {
    return lhs.adapter.luid_low_part < rhs.adapter.luid_low_part;
  }
  if (lhs.target_id != rhs.target_id) {
    return lhs.target_id < rhs.target_id;
  }
  if (lhs.adapter.adapter_key != rhs.adapter.adapter_key) {
    return lhs.adapter.adapter_key < rhs.adapter.adapter_key;
  }
  return lhs.monitor_key < rhs.monitor_key;
}

bool IsValidRect(const DirtyRectPx& rect) noexcept {
  return rect.left < rect.right && rect.top < rect.bottom;
}

void MergeRectSets(std::vector<DirtyRectPx>* destination,
                   const std::vector<DirtyRectPx>& source) {
  if (destination == nullptr || source.empty()) {
    return;
  }
  destination->reserve(destination->size() + source.size());
  for (const DirtyRectPx& rect : source) {
    if (!IsValidRect(rect)) {
      continue;
    }
    destination->push_back(rect);
  }
}

class DirtyPresentPipeline final : public IDirtyPresentPipeline {
 public:
  explicit DirtyPresentPipeline(DirtyPresentPipelineConfig config)
      : config_(std::move(config)),
        coalescer_(CreateDirtyRectCoalescer(config_.coalescer)) {}

  RenderStatus SetActiveMonitors(
      const std::vector<MonitorLayout>& active_monitors,
      uint64_t event_sequence) override {
    std::vector<MonitorLayout> normalized;
    normalized.reserve(active_monitors.size());
    for (const MonitorLayout& monitor : active_monitors) {
      if (!monitor.IsValid()) {
        continue;
      }
      normalized.push_back(monitor);
    }

    std::sort(
        normalized.begin(),
        normalized.end(),
        [](const MonitorLayout& lhs, const MonitorLayout& rhs) {
          return IsMonitorIdentityLess(lhs.monitor, rhs.monitor);
        });
    normalized.erase(
        std::unique(
            normalized.begin(),
            normalized.end(),
            [](const MonitorLayout& lhs, const MonitorLayout& rhs) {
              return lhs.monitor == rhs.monitor;
            }),
        normalized.end());
    active_monitors_ = std::move(normalized);

    // Drop stale pending entries if monitor disappeared from topology.
    for (auto it = pending_by_monitor_.begin();
         it != pending_by_monitor_.end();) {
      const bool active = std::any_of(
          active_monitors_.begin(),
          active_monitors_.end(),
          [&it](const MonitorLayout& layout) {
            return layout.monitor == it->first;
          });
      if (!active) {
        it = pending_by_monitor_.erase(it);
      } else {
        ++it;
      }
    }
    last_topology_event_sequence_ = event_sequence;
    return RenderStatus::kOk;
  }

  RenderStatus QueueDirtyEvent(
      const DirtyPresentEvent& event,
      DirtyPresentPipelineUpdate* out_update) override {
    if (out_update == nullptr) {
      return RenderStatus::kInvalidArgument;
    }

    DirtyPresentPipelineUpdate update{};
    update.event_sequence = event.event_sequence;
    const DirtyRectCoalescerResult coalesced =
        coalescer_->Coalesce(event, active_monitors_);

    update.has_effective_work = coalesced.has_effective_work;
    update.batch_fallback_reason = coalesced.batch_fallback_reason;
    if (!coalesced.has_effective_work &&
        coalesced.batch_fallback_reason ==
            DirtyFullFrameFallbackReason::kNoActiveTargetMonitors) {
      update.status = RenderStatus::kNotFound;
      *out_update = update;
      return update.status;
    }

    for (const MonitorDirtyRects& monitor_dirty : coalesced.per_monitor) {
      if (monitor_dirty.dirty_rects.empty()) {
        continue;
      }
      PendingMonitorState& pending = pending_by_monitor_[monitor_dirty.monitor];
      pending.latest_event_sequence =
          std::max(pending.latest_event_sequence, event.event_sequence);
      pending.bypass_overlay_throttle =
          pending.bypass_overlay_throttle || event.bypass_overlay_throttle;
      pending.reason = event.reason;
      pending.fallback_reason = monitor_dirty.fallback_reason;
      if (monitor_dirty.force_full_frame) {
        pending.force_full_frame = true;
        pending.dirty_rects = monitor_dirty.dirty_rects;
      } else if (!pending.force_full_frame) {
        MergeRectSets(&pending.dirty_rects, monitor_dirty.dirty_rects);
      }
      update.touched_monitors.push_back(monitor_dirty.monitor);
    }

    std::sort(
        update.touched_monitors.begin(),
        update.touched_monitors.end(),
        IsMonitorIdentityLess);
    update.touched_monitors.erase(
        std::unique(
            update.touched_monitors.begin(),
            update.touched_monitors.end()),
        update.touched_monitors.end());

    update.status = RenderStatus::kOk;
    *out_update = update;
    return update.status;
  }

  RenderStatus BuildPendingPresentIntents(
      std::vector<PresentIntent>* out_intents) const override {
    if (out_intents == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    std::vector<std::pair<MonitorIdentity, PendingMonitorState>> ordered;
    ordered.reserve(pending_by_monitor_.size());
    for (const auto& [monitor, pending] : pending_by_monitor_) {
      if (pending.dirty_rects.empty()) {
        continue;
      }
      ordered.emplace_back(monitor, pending);
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const std::pair<MonitorIdentity, PendingMonitorState>& lhs,
           const std::pair<MonitorIdentity, PendingMonitorState>& rhs) {
          return IsMonitorIdentityLess(lhs.first, rhs.first);
        });

    out_intents->clear();
    out_intents->reserve(ordered.size());
    for (const auto& [monitor, pending] : ordered) {
      PresentIntent intent{};
      intent.monitor = monitor;
      intent.event_sequence = pending.latest_event_sequence;
      intent.force_full_frame = pending.force_full_frame;
      intent.bypass_overlay_throttle = pending.bypass_overlay_throttle;
      intent.reason = pending.reason;
      intent.dirty_rects = pending.dirty_rects;
      out_intents->push_back(std::move(intent));
    }
    return RenderStatus::kOk;
  }

  RenderStatus ConsumePresentIntent(
      const MonitorIdentity& monitor,
      PresentIntent* out_intent) override {
    if (out_intent == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    const auto it = pending_by_monitor_.find(monitor);
    if (it == pending_by_monitor_.end() || it->second.dirty_rects.empty()) {
      return RenderStatus::kNotFound;
    }
    PresentIntent intent{};
    intent.monitor = monitor;
    intent.event_sequence = it->second.latest_event_sequence;
    intent.force_full_frame = it->second.force_full_frame;
    intent.bypass_overlay_throttle = it->second.bypass_overlay_throttle;
    intent.reason = it->second.reason;
    intent.dirty_rects = it->second.dirty_rects;
    *out_intent = std::move(intent);
    pending_by_monitor_.erase(it);
    return RenderStatus::kOk;
  }

  bool HasPendingWork() const override {
    for (const auto& [_, pending] : pending_by_monitor_) {
      (void)_;
      if (!pending.dirty_rects.empty()) {
        return true;
      }
    }
    return false;
  }

  std::vector<MonitorIdentity> ListPendingMonitors() const override {
    std::vector<MonitorIdentity> monitors;
    monitors.reserve(pending_by_monitor_.size());
    for (const auto& [monitor, pending] : pending_by_monitor_) {
      if (pending.dirty_rects.empty()) {
        continue;
      }
      monitors.push_back(monitor);
    }
    std::sort(monitors.begin(), monitors.end(), IsMonitorIdentityLess);
    return monitors;
  }

  DirtyRectCoalescerConfig coalescer_config() const override {
    return coalescer_->config();
  }

 private:
  DirtyPresentPipelineConfig config_;
  std::unique_ptr<IDirtyRectCoalescer> coalescer_;
  std::vector<MonitorLayout> active_monitors_;
  std::unordered_map<MonitorIdentity, PendingMonitorState, MonitorIdentityHash>
      pending_by_monitor_;
  uint64_t last_topology_event_sequence_ = 0;
};

}  // namespace

std::unique_ptr<IDirtyPresentPipeline> CreateDirtyPresentPipeline(
    DirtyPresentPipelineConfig config) {
  return std::make_unique<DirtyPresentPipeline>(std::move(config));
}

}  // namespace render
