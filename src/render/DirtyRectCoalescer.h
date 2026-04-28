#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "src/render/RenderTypes.h"

namespace render {

// Target monitor selection for invalidation events.
enum class DirtyMonitorTarget : uint8_t {
  kPrimaryOnly = 0,
  kAllActiveMonitors,
  kExplicitSet
};

// Coordinate space used for incoming dirty rectangles.
enum class DirtyRectCoordSpace : uint8_t {
  kDesktopPixels = 0,
  kMonitorLocalPixels
};

// Explains deterministic full-frame fallback decisions.
enum class DirtyFullFrameFallbackReason : uint8_t {
  kNone = 0,
  kForcedByIntent,
  kReasonRequiresFullFrame,
  kQueueOverflowRecovery,
  kRectCountThresholdExceeded,
  kCoverageThresholdExceeded,
  kInvalidOrEmptyDirtySet,
  kNoActiveTargetMonitors
};

struct DirtyRectCoalescerConfig {
  uint32_t max_dirty_rects_per_monitor = 32;
  double full_frame_area_threshold = 0.60;
  bool merge_touching_edges = true;
};

// Snapshot of active monitor geometry used to map desktop-space dirty
// rectangles into per-monitor local coordinates.
struct MonitorLayout {
  MonitorIdentity monitor;
  int32_t desktop_left = 0;
  int32_t desktop_top = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_primary = false;

  bool IsValid() const noexcept { return width > 0 && height > 0; }
};

// Event payload consumed by the dirty mapping/coalescing pipeline.
struct DirtyPresentEvent {
  uint64_t event_sequence = 0;
  PresentReason reason = PresentReason::kInvalidation;
  DirtyMonitorTarget monitor_target = DirtyMonitorTarget::kAllActiveMonitors;
  DirtyRectCoordSpace coord_space = DirtyRectCoordSpace::kDesktopPixels;
  bool force_full_frame = false;
  bool queue_overflow_recovery = false;
  bool bypass_overlay_throttle = false;
  std::vector<MonitorIdentity> explicit_monitors;
  std::vector<DirtyRectPx> dirty_rects;
};

struct MonitorDirtyRects {
  MonitorIdentity monitor;
  DirtyFullFrameFallbackReason fallback_reason =
      DirtyFullFrameFallbackReason::kNone;
  bool force_full_frame = false;
  std::vector<DirtyRectPx> dirty_rects;
};

struct DirtyRectCoalescerResult {
  uint64_t event_sequence = 0;
  bool has_effective_work = false;
  DirtyFullFrameFallbackReason batch_fallback_reason =
      DirtyFullFrameFallbackReason::kNone;
  std::vector<MonitorDirtyRects> per_monitor;
};

// Authoritative dirty-rectangle mapper/coalescer.
// This API is event-driven and stateless from a scheduling perspective:
// every call maps one explicit event into per-monitor dirty results.
class IDirtyRectCoalescer {
 public:
  virtual ~IDirtyRectCoalescer() = default;

  virtual DirtyRectCoalescerResult Coalesce(
      const DirtyPresentEvent& event,
      const std::vector<MonitorLayout>& active_monitors) const = 0;

  virtual DirtyRectCoalescerConfig config() const = 0;
};

std::unique_ptr<IDirtyRectCoalescer> CreateDirtyRectCoalescer(
    DirtyRectCoalescerConfig config = {});

}  // namespace render
