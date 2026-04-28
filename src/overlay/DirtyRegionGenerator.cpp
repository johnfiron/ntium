#include "src/overlay/DirtyRegionGenerator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace overlay {

namespace {

bool DeltaOrder(const DirtyRegionDelta& lhs, const DirtyRegionDelta& rhs) {
  if (lhs.event_sequence != rhs.event_sequence) {
    return lhs.event_sequence < rhs.event_sequence;
  }
  return lhs.event_id < rhs.event_id;
}

int32_t ClampToInt32(int64_t value) {
  if (value < static_cast<int64_t>(std::numeric_limits<int32_t>::min())) {
    return std::numeric_limits<int32_t>::min();
  }
  if (value > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
  }
  return static_cast<int32_t>(value);
}

bool RectOrder(const DirtyRegionRect& lhs, const DirtyRegionRect& rhs) {
  if (lhs.top != rhs.top) {
    return lhs.top < rhs.top;
  }
  if (lhs.left != rhs.left) {
    return lhs.left < rhs.left;
  }
  if (lhs.right != rhs.right) {
    return lhs.right < rhs.right;
  }
  return lhs.bottom < rhs.bottom;
}

}  // namespace

std::vector<DirtyRegionRect> DirtyRegionGenerator::Generate(
    const DirtyRegionDelta& delta) {
  std::vector<DirtyRegionRect> rects;

  const bool has_old = delta.old_bounds.has_value() && delta.old_bounds->IsValid();
  const bool has_new = delta.new_bounds.has_value() && delta.new_bounds->IsValid();

  if (!has_old && !has_new) {
    return rects;
  }

  if (has_old && !has_new) {
    rects.push_back(PadRect(*delta.old_bounds, delta.padding_px));
    return MergeTouching(std::move(rects));
  }

  if (!has_old && has_new) {
    rects.push_back(PadRect(*delta.new_bounds, delta.padding_px));
    return MergeTouching(std::move(rects));
  }

  const DirtyRegionRect old_rect = PadRect(*delta.old_bounds, delta.padding_px);
  const DirtyRegionRect new_rect = PadRect(*delta.new_bounds, delta.padding_px);

  if (old_rect.left == new_rect.left && old_rect.top == new_rect.top &&
      old_rect.right == new_rect.right && old_rect.bottom == new_rect.bottom) {
    if (delta.force_redraw_if_unchanged) {
      rects.push_back(new_rect);
    }
    return rects;
  }

  const std::optional<DirtyRegionRect> overlap = Intersect(old_rect, new_rect);
  if (!overlap.has_value()) {
    rects.push_back(old_rect);
    rects.push_back(new_rect);
    return MergeTouching(std::move(rects));
  }

  std::vector<DirtyRegionRect> old_only = Subtract(old_rect, *overlap);
  std::vector<DirtyRegionRect> new_only = Subtract(new_rect, *overlap);
  rects.reserve(old_only.size() + new_only.size());
  rects.insert(rects.end(), old_only.begin(), old_only.end());
  rects.insert(rects.end(), new_only.begin(), new_only.end());

  return MergeTouching(std::move(rects));
}

std::vector<DirtyRegionRect> DirtyRegionGenerator::GenerateForBatch(
    std::vector<DirtyRegionDelta> deltas) {
  std::sort(deltas.begin(), deltas.end(), DeltaOrder);

  std::vector<DirtyRegionRect> dirty_rects;
  for (const DirtyRegionDelta& delta : deltas) {
    std::vector<DirtyRegionRect> per_delta = Generate(delta);
    dirty_rects.insert(dirty_rects.end(), per_delta.begin(), per_delta.end());
  }

  return MergeTouching(std::move(dirty_rects));
}

DirtyRegionRect DirtyRegionGenerator::PadRect(const OverlayBounds& bounds,
                                              int32_t padding) {
  const int64_t pad = std::max<int64_t>(0, padding);
  return DirtyRegionRect {
      ClampToInt32(static_cast<int64_t>(bounds.left) - pad),
      ClampToInt32(static_cast<int64_t>(bounds.top) - pad),
      ClampToInt32(static_cast<int64_t>(bounds.right) + pad),
      ClampToInt32(static_cast<int64_t>(bounds.bottom) + pad),
  };
}

std::optional<DirtyRegionRect> DirtyRegionGenerator::Intersect(
    const DirtyRegionRect& a,
    const DirtyRegionRect& b) {
  DirtyRegionRect intersection {
      std::max(a.left, b.left),
      std::max(a.top, b.top),
      std::min(a.right, b.right),
      std::min(a.bottom, b.bottom),
  };
  if (!intersection.IsValid()) {
    return std::nullopt;
  }
  return intersection;
}

std::vector<DirtyRegionRect> DirtyRegionGenerator::Subtract(
    const DirtyRegionRect& source,
    const DirtyRegionRect& cut) {
  std::vector<DirtyRegionRect> parts;
  if (!source.IsValid()) {
    return parts;
  }
  if (!cut.IsValid()) {
    parts.push_back(source);
    return parts;
  }

  const std::optional<DirtyRegionRect> overlap = Intersect(source, cut);
  if (!overlap.has_value()) {
    parts.push_back(source);
    return parts;
  }

  parts.reserve(4);
  if (source.top < overlap->top) {
    parts.push_back(
        DirtyRegionRect {source.left, source.top, source.right, overlap->top});
  }
  if (overlap->bottom < source.bottom) {
    parts.push_back(DirtyRegionRect {
        source.left, overlap->bottom, source.right, source.bottom});
  }
  if (source.left < overlap->left) {
    parts.push_back(DirtyRegionRect {
        source.left, overlap->top, overlap->left, overlap->bottom});
  }
  if (overlap->right < source.right) {
    parts.push_back(DirtyRegionRect {
        overlap->right, overlap->top, source.right, overlap->bottom});
  }

  parts.erase(std::remove_if(parts.begin(), parts.end(),
                             [](const DirtyRegionRect& rect) {
                               return !rect.IsValid();
                             }),
              parts.end());
  return parts;
}

bool DirtyRegionGenerator::OverlapsOrTouches(const DirtyRegionRect& a,
                                             const DirtyRegionRect& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom &&
         b.top <= a.bottom;
}

DirtyRegionRect DirtyRegionGenerator::UnionRect(const DirtyRegionRect& a,
                                                const DirtyRegionRect& b) {
  return DirtyRegionRect {
      std::min(a.left, b.left),
      std::min(a.top, b.top),
      std::max(a.right, b.right),
      std::max(a.bottom, b.bottom),
  };
}

std::vector<DirtyRegionRect> DirtyRegionGenerator::MergeTouching(
    std::vector<DirtyRegionRect> rects) {
  rects.erase(std::remove_if(rects.begin(), rects.end(),
                             [](const DirtyRegionRect& rect) {
                               return !rect.IsValid();
                             }),
              rects.end());
  if (rects.size() < 2) {
    SortDeterministic(&rects);
    return rects;
  }

  bool merged_any = true;
  while (merged_any) {
    merged_any = false;
    for (std::size_t i = 0; i < rects.size(); ++i) {
      for (std::size_t j = i + 1; j < rects.size(); ++j) {
        if (!OverlapsOrTouches(rects[i], rects[j])) {
          continue;
        }
        rects[i] = UnionRect(rects[i], rects[j]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
        merged_any = true;
        break;
      }
      if (merged_any) {
        break;
      }
    }
  }

  SortDeterministic(&rects);
  return rects;
}

void DirtyRegionGenerator::SortDeterministic(std::vector<DirtyRegionRect>* rects) {
  if (rects == nullptr) {
    return;
  }
  std::sort(rects->begin(), rects->end(), RectOrder);
}

}  // namespace overlay
