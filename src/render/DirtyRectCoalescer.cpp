#include "src/render/DirtyRectCoalescer.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace render {

namespace {

struct LocalRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

bool IsValidRect(const LocalRect& rect) noexcept {
  return rect.left < rect.right && rect.top < rect.bottom;
}

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

bool RectsOverlapOrTouch(const LocalRect& lhs, const LocalRect& rhs) noexcept {
  return lhs.left <= rhs.right && rhs.left <= lhs.right &&
         lhs.top <= rhs.bottom && rhs.top <= lhs.bottom;
}

bool RectsOverlapOnly(const LocalRect& lhs, const LocalRect& rhs) noexcept {
  return lhs.left < rhs.right && rhs.left < lhs.right && lhs.top < rhs.bottom &&
         rhs.top < lhs.bottom;
}

LocalRect UnionRect(const LocalRect& lhs, const LocalRect& rhs) noexcept {
  return LocalRect{
      std::min(lhs.left, rhs.left),
      std::min(lhs.top, rhs.top),
      std::max(lhs.right, rhs.right),
      std::max(lhs.bottom, rhs.bottom)};
}

int64_t RectArea(const LocalRect& rect) noexcept {
  if (!IsValidRect(rect)) {
    return 0;
  }
  const int64_t width = static_cast<int64_t>(rect.right) - rect.left;
  const int64_t height = static_cast<int64_t>(rect.bottom) - rect.top;
  return width * height;
}

LocalRect ClipToMonitorLocal(
    const DirtyRectPx& rect,
    const MonitorLayout& monitor) noexcept {
  const int32_t max_right = static_cast<int32_t>(monitor.width);
  const int32_t max_bottom = static_cast<int32_t>(monitor.height);
  return LocalRect{std::max(0, rect.left), std::max(0, rect.top),
                   std::min(max_right, rect.right),
                   std::min(max_bottom, rect.bottom)};
}

LocalRect MapDesktopRectToMonitorLocal(
    const DirtyRectPx& desktop_rect,
    const MonitorLayout& monitor) noexcept {
  const int32_t monitor_right =
      monitor.desktop_left + static_cast<int32_t>(monitor.width);
  const int32_t monitor_bottom =
      monitor.desktop_top + static_cast<int32_t>(monitor.height);
  const LocalRect clipped = LocalRect{
      std::max(desktop_rect.left, monitor.desktop_left),
      std::max(desktop_rect.top, monitor.desktop_top),
      std::min(desktop_rect.right, monitor_right),
      std::min(desktop_rect.bottom, monitor_bottom)};

  if (!IsValidRect(clipped)) {
    return {};
  }

  return LocalRect{
      clipped.left - monitor.desktop_left,
      clipped.top - monitor.desktop_top,
      clipped.right - monitor.desktop_left,
      clipped.bottom - monitor.desktop_top};
}

bool IsReasonForcedFullFrame(PresentReason reason) noexcept {
  switch (reason) {
    case PresentReason::kTopologyChanged:
    case PresentReason::kDeviceRecovery:
      return true;
    case PresentReason::kInvalidation:
      return false;
  }
  return false;
}

DirtyRectPx ToDirtyRectPx(const LocalRect& rect) noexcept {
  return DirtyRectPx{rect.left, rect.top, rect.right, rect.bottom};
}

void CoalesceRects(
    std::vector<LocalRect>* rects,
    bool merge_touching_edges) {
  if (rects == nullptr || rects->empty()) {
    return;
  }

  std::sort(
      rects->begin(),
      rects->end(),
      [](const LocalRect& lhs, const LocalRect& rhs) {
        if (lhs.top != rhs.top) {
          return lhs.top < rhs.top;
        }
        if (lhs.left != rhs.left) {
          return lhs.left < rhs.left;
        }
        if (lhs.bottom != rhs.bottom) {
          return lhs.bottom < rhs.bottom;
        }
        return lhs.right < rhs.right;
      });

  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<LocalRect> next;
    next.reserve(rects->size());
    for (const LocalRect& candidate : *rects) {
      LocalRect merged = candidate;
      bool merged_any = false;
      for (std::size_t index = 0; index < next.size(); ++index) {
        const bool intersects = merge_touching_edges
                                    ? RectsOverlapOrTouch(merged, next[index])
                                    : RectsOverlapOnly(merged, next[index]);
        if (!intersects) {
          continue;
        }
        merged = UnionRect(merged, next[index]);
        next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
        merged_any = true;
        changed = true;
        break;
      }
      next.push_back(merged);
      if (merged_any) {
        // A merge occurred; the outer fixed-point loop will continue.
      }
    }
    rects->swap(next);
  }
}

std::vector<MonitorLayout> SelectTargetMonitors(
    const DirtyPresentEvent& event,
    const std::vector<MonitorLayout>& active_monitors) {
  std::vector<MonitorLayout> selected;
  if (active_monitors.empty()) {
    return selected;
  }

  if (event.monitor_target == DirtyMonitorTarget::kAllActiveMonitors) {
    selected = active_monitors;
  } else if (event.monitor_target == DirtyMonitorTarget::kPrimaryOnly) {
    auto it = std::find_if(
        active_monitors.begin(),
        active_monitors.end(),
        [](const MonitorLayout& layout) { return layout.is_primary; });
    if (it != active_monitors.end()) {
      selected.push_back(*it);
    } else {
      selected.push_back(active_monitors.front());
    }
  } else {
    for (const MonitorIdentity& explicit_monitor : event.explicit_monitors) {
      auto it = std::find_if(
          active_monitors.begin(),
          active_monitors.end(),
          [&explicit_monitor](const MonitorLayout& layout) {
            return layout.monitor == explicit_monitor;
          });
      if (it != active_monitors.end()) {
        selected.push_back(*it);
      }
    }
  }

  std::sort(
      selected.begin(),
      selected.end(),
      [](const MonitorLayout& lhs, const MonitorLayout& rhs) {
        return IsMonitorIdentityLess(lhs.monitor, rhs.monitor);
      });
  selected.erase(
      std::unique(
          selected.begin(),
          selected.end(),
          [](const MonitorLayout& lhs, const MonitorLayout& rhs) {
            return lhs.monitor == rhs.monitor;
          }),
      selected.end());
  return selected;
}

}  // namespace

class DirtyRectCoalescer final : public IDirtyRectCoalescer {
 public:
  explicit DirtyRectCoalescer(DirtyRectCoalescerConfig config)
      : config_(std::move(config)) {}

  DirtyRectCoalescerResult Coalesce(
      const DirtyPresentEvent& event,
      const std::vector<MonitorLayout>& active_monitors) const override {
    DirtyRectCoalescerResult result{};
    result.event_sequence = event.event_sequence;

    std::vector<MonitorLayout> selected =
        SelectTargetMonitors(event, active_monitors);
    if (selected.empty()) {
      result.batch_fallback_reason =
          DirtyFullFrameFallbackReason::kNoActiveTargetMonitors;
      return result;
    }

    const bool force_full_frame_by_reason = IsReasonForcedFullFrame(event.reason);
    for (const MonitorLayout& monitor : selected) {
      if (!monitor.IsValid()) {
        continue;
      }

      MonitorDirtyRects monitor_result{};
      monitor_result.monitor = monitor.monitor;
      const int32_t local_right = static_cast<int32_t>(monitor.width);
      const int32_t local_bottom = static_cast<int32_t>(monitor.height);

      DirtyFullFrameFallbackReason fallback_reason =
          DirtyFullFrameFallbackReason::kNone;
      if (event.force_full_frame) {
        fallback_reason = DirtyFullFrameFallbackReason::kForcedByIntent;
      } else if (event.queue_overflow_recovery) {
        fallback_reason = DirtyFullFrameFallbackReason::kQueueOverflowRecovery;
      } else if (force_full_frame_by_reason) {
        fallback_reason = DirtyFullFrameFallbackReason::kReasonRequiresFullFrame;
      }

      std::vector<LocalRect> mapped;
      mapped.reserve(event.dirty_rects.size());
      for (const DirtyRectPx& rect : event.dirty_rects) {
        LocalRect local{};
        if (event.coord_space == DirtyRectCoordSpace::kDesktopPixels) {
          local = MapDesktopRectToMonitorLocal(rect, monitor);
        } else if (event.monitor_target == DirtyMonitorTarget::kExplicitSet) {
          local = ClipToMonitorLocal(rect, monitor);
        } else {
          fallback_reason = DirtyFullFrameFallbackReason::kInvalidOrEmptyDirtySet;
          break;
        }

        if (IsValidRect(local)) {
          mapped.push_back(local);
        }
      }

      if (fallback_reason == DirtyFullFrameFallbackReason::kNone && mapped.empty()) {
        fallback_reason = DirtyFullFrameFallbackReason::kInvalidOrEmptyDirtySet;
      }

      if (fallback_reason == DirtyFullFrameFallbackReason::kNone) {
        CoalesceRects(&mapped, config_.merge_touching_edges);
        if (mapped.size() > config_.max_dirty_rects_per_monitor) {
          fallback_reason =
              DirtyFullFrameFallbackReason::kRectCountThresholdExceeded;
        } else {
          const int64_t monitor_area =
              static_cast<int64_t>(monitor.width) * monitor.height;
          int64_t covered_area = 0;
          for (const LocalRect& rect : mapped) {
            covered_area += RectArea(rect);
          }
          const double coverage_ratio =
              monitor_area <= 0
                  ? 0.0
                  : static_cast<double>(covered_area) /
                        static_cast<double>(monitor_area);
          if (coverage_ratio >= config_.full_frame_area_threshold) {
            fallback_reason =
                DirtyFullFrameFallbackReason::kCoverageThresholdExceeded;
          }
        }
      }

      if (fallback_reason != DirtyFullFrameFallbackReason::kNone) {
        monitor_result.force_full_frame = true;
        monitor_result.fallback_reason = fallback_reason;
        monitor_result.dirty_rects.push_back(
            DirtyRectPx{0, 0, local_right, local_bottom});
      } else {
        monitor_result.force_full_frame = false;
        monitor_result.fallback_reason = DirtyFullFrameFallbackReason::kNone;
        monitor_result.dirty_rects.reserve(mapped.size());
        for (const LocalRect& local : mapped) {
          monitor_result.dirty_rects.push_back(ToDirtyRectPx(local));
        }
      }

      result.has_effective_work =
          result.has_effective_work || !monitor_result.dirty_rects.empty();
      if (result.batch_fallback_reason == DirtyFullFrameFallbackReason::kNone &&
          monitor_result.fallback_reason != DirtyFullFrameFallbackReason::kNone) {
        result.batch_fallback_reason = monitor_result.fallback_reason;
      }
      result.per_monitor.push_back(std::move(monitor_result));
    }
    return result;
  }

  DirtyRectCoalescerConfig config() const override { return config_; }

 private:
  DirtyRectCoalescerConfig config_;
};

std::unique_ptr<IDirtyRectCoalescer> CreateDirtyRectCoalescer(
    DirtyRectCoalescerConfig config) {
  return std::make_unique<DirtyRectCoalescer>(std::move(config));
}

}  // namespace render
