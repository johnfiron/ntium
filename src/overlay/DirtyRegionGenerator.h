#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/overlay/EventStore.h"

namespace overlay {

struct DirtyRegionRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  bool IsValid() const noexcept { return left < right && top < bottom; }
};

struct DirtyRegionDelta {
  std::string event_id;
  uint64_t event_sequence = 0;
  std::optional<OverlayBounds> old_bounds;
  std::optional<OverlayBounds> new_bounds;
  int32_t padding_px = 0;
  bool force_redraw_if_unchanged = false;
};

class DirtyRegionGenerator {
 public:
  // Computes minimal dirty coverage for one overlay event transition.
  // Output is deterministic and sorted by top/left/right/bottom.
  static std::vector<DirtyRegionRect> Generate(const DirtyRegionDelta& delta);

  // Computes dirty coverage for a batch of transitions. Deltas are consumed in
  // deterministic event_sequence order and output is merged by touch/overlap.
  static std::vector<DirtyRegionRect> GenerateForBatch(
      std::vector<DirtyRegionDelta> deltas);

 private:
  static DirtyRegionRect PadRect(const OverlayBounds& bounds, int32_t padding);
  static std::optional<DirtyRegionRect> Intersect(
      const DirtyRegionRect& a,
      const DirtyRegionRect& b);
  static std::vector<DirtyRegionRect> Subtract(
      const DirtyRegionRect& source,
      const DirtyRegionRect& cut);
  static bool OverlapsOrTouches(
      const DirtyRegionRect& a,
      const DirtyRegionRect& b);
  static DirtyRegionRect UnionRect(
      const DirtyRegionRect& a,
      const DirtyRegionRect& b);
  static std::vector<DirtyRegionRect> MergeTouching(
      std::vector<DirtyRegionRect> rects);
  static void SortDeterministic(std::vector<DirtyRegionRect>* rects);
};

}  // namespace overlay
