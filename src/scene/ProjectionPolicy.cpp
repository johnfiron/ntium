#include "scene/ProjectionPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace scene {
namespace {

constexpr std::uint32_t kFallbackDpi = 96U;
constexpr double kPi = 3.14159265358979323846264338327950288;

std::vector<MonitorId> SortedUniqueMonitorIds(std::vector<MonitorId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

bool HasMonitorId(const std::vector<MonitorTopologyEntry>& monitors,
                  MonitorId monitor_id) {
  return std::any_of(monitors.begin(), monitors.end(),
                     [monitor_id](const MonitorTopologyEntry& entry) {
                       return entry.stable_monitor_id == monitor_id;
                     });
}

const MonitorTopologyEntry* FindMonitor(const MonitorTopologySnapshot& topology,
                                        MonitorId monitor_id) {
  const auto it = std::find_if(
      topology.monitors.begin(),
      topology.monitors.end(),
      [monitor_id](const MonitorTopologyEntry& entry) {
        return entry.stable_monitor_id == monitor_id;
      });
  return it == topology.monitors.end() ? nullptr : &(*it);
}

bool IsHigherResolution(const MonitorTopologyEntry& lhs,
                        const MonitorTopologyEntry& rhs) {
  const std::uint64_t lhs_pixels = static_cast<std::uint64_t>(lhs.geometry.width) *
                                   static_cast<std::uint64_t>(lhs.geometry.height);
  const std::uint64_t rhs_pixels = static_cast<std::uint64_t>(rhs.geometry.width) *
                                   static_cast<std::uint64_t>(rhs.geometry.height);
  if (lhs_pixels != rhs_pixels) {
    return lhs_pixels > rhs_pixels;
  }
  const std::uint32_t lhs_dpi =
      std::max(lhs.scale.dpi_x, lhs.scale.dpi_y);
  const std::uint32_t rhs_dpi =
      std::max(rhs.scale.dpi_x, rhs.scale.dpi_y);
  if (lhs_dpi != rhs_dpi) {
    return lhs_dpi > rhs_dpi;
  }
  return lhs.stable_monitor_id < rhs.stable_monitor_id;
}

MonitorId ResolvePrimaryMonitorId(const MonitorTopologySnapshot& topology,
                                  const SpanPolicyRequest& request,
                                  const std::vector<MonitorId>& selected_ids) {
  if (selected_ids.empty()) {
    return 0U;
  }
  if (request.preferred_primary_monitor_id != 0U &&
      std::find(selected_ids.begin(), selected_ids.end(),
                request.preferred_primary_monitor_id) != selected_ids.end()) {
    return request.preferred_primary_monitor_id;
  }
  if (topology.primary_monitor_id != 0U &&
      std::find(selected_ids.begin(), selected_ids.end(),
                topology.primary_monitor_id) != selected_ids.end()) {
    return topology.primary_monitor_id;
  }
  return selected_ids.front();
}

double Clamp(double value, double min_value, double max_value) {
  return std::max(min_value, std::min(value, max_value));
}

double ComputeWorldUnitsPerPixel(double vertical_fov_deg,
                                 std::uint32_t height_px) {
  if (height_px == 0U || vertical_fov_deg <= 0.0) {
    return 0.0;
  }
  const double vertical_fov_rad = vertical_fov_deg * (kPi / 180.0);
  return (2.0 * std::tan(vertical_fov_rad * 0.5)) /
         static_cast<double>(height_px);
}

class DefaultSpanPolicy final : public ISpanPolicy {
 public:
  SpanSelection SelectSpan(const MonitorTopologySnapshot& topology,
                           const SpanPolicyRequest& request) const override {
    SpanSelection selection{};
    if (topology.monitors.empty()) {
      return selection;
    }

    std::vector<MonitorId> selected_ids;
    switch (request.mode) {
      case SpanMode::kSingle: {
        if (request.preferred_primary_monitor_id != 0U &&
            HasMonitorId(topology.monitors, request.preferred_primary_monitor_id)) {
          selected_ids.push_back(request.preferred_primary_monitor_id);
        } else if (topology.primary_monitor_id != 0U &&
                   HasMonitorId(topology.monitors, topology.primary_monitor_id)) {
          selected_ids.push_back(topology.primary_monitor_id);
        } else {
          selected_ids.push_back(topology.monitors.front().stable_monitor_id);
        }
        break;
      }
      case SpanMode::kAll:
        selected_ids.reserve(topology.monitors.size());
        for (const MonitorTopologyEntry& entry : topology.monitors) {
          selected_ids.push_back(entry.stable_monitor_id);
        }
        break;
      case SpanMode::kSubset: {
        std::vector<MonitorId> requested = SortedUniqueMonitorIds(request.monitor_ids);
        for (MonitorId monitor_id : requested) {
          if (HasMonitorId(topology.monitors, monitor_id)) {
            selected_ids.push_back(monitor_id);
          }
        }
        if (selected_ids.empty()) {
          selected_ids.push_back(
              topology.primary_monitor_id != 0U
                  ? topology.primary_monitor_id
                  : topology.monitors.front().stable_monitor_id);
        }
        break;
      }
    }

    selected_ids = SortedUniqueMonitorIds(std::move(selected_ids));
    selection.monitor_ids = selected_ids;
    selection.primary_monitor_id =
        ResolvePrimaryMonitorId(topology, request, selection.monitor_ids);
    return selection;
  }
};

class DefaultMirrorPolicy final : public IMirrorPolicy {
 public:
  MirrorSelection SelectMirror(const MonitorTopologySnapshot& topology,
                               const MirrorPolicyRequest& request) const override {
    MirrorSelection selection{};
    if (request.mode == MirrorMode::kDisabled || topology.monitors.empty()) {
      return selection;
    }

    MonitorId source_monitor_id = request.source_monitor_id;
    if (source_monitor_id == 0U || !HasMonitorId(topology.monitors, source_monitor_id)) {
      source_monitor_id = topology.primary_monitor_id != 0U
                              ? topology.primary_monitor_id
                              : topology.monitors.front().stable_monitor_id;
    }

    std::vector<MonitorId> targets = SortedUniqueMonitorIds(request.target_monitor_ids);
    targets.erase(std::remove(targets.begin(), targets.end(), source_monitor_id),
                  targets.end());
    targets.erase(
        std::remove_if(targets.begin(), targets.end(),
                       [&topology](MonitorId monitor_id) {
                         return !HasMonitorId(topology.monitors, monitor_id);
                       }),
        targets.end());

    if (targets.empty()) {
      return selection;
    }

    selection.enabled = true;
    selection.source_monitor_id = source_monitor_id;
    selection.target_monitor_ids = targets;
    for (MonitorId target : selection.target_monitor_ids) {
      selection.mirrored_to_source[target] = selection.source_monitor_id;
    }
    return selection;
  }
};

class ProjectionPolicy final : public IProjectionPolicy {
 public:
  ProjectionPolicy(std::unique_ptr<ISpanPolicy> span_policy,
                   std::unique_ptr<IMirrorPolicy> mirror_policy)
      : span_policy_(span_policy ? std::move(span_policy) : CreateDefaultSpanPolicy()),
        mirror_policy_(mirror_policy ? std::move(mirror_policy)
                                     : CreateDefaultMirrorPolicy()) {}

  ProjectionEvaluation Evaluate(const MonitorTopologySnapshot& topology,
                                const SpanPolicyRequest& span_request,
                                const MirrorPolicyRequest& mirror_request,
                                const ProjectionPolicyConfig& config) const override {
    ProjectionEvaluation evaluation{};
    evaluation.topology_generation = topology.generation;
    if (topology.monitors.empty()) {
      return evaluation;
    }

    const SpanSelection span_selection = span_policy_->SelectSpan(topology, span_request);
    const MirrorSelection mirror_selection =
        mirror_policy_->SelectMirror(topology, mirror_request);

    evaluation.span_selection = span_selection;
    evaluation.mirror_selection = mirror_selection;
    evaluation.primary_monitor_id = span_selection.primary_monitor_id;

    std::vector<const MonitorTopologyEntry*> active_native_entries;
    active_native_entries.reserve(span_selection.monitor_ids.size());
    for (MonitorId monitor_id : span_selection.monitor_ids) {
      const MonitorTopologyEntry* entry = FindMonitor(topology, monitor_id);
      if (entry != nullptr) {
        active_native_entries.push_back(entry);
      }
    }
    if (active_native_entries.empty()) {
      return evaluation;
    }

    const MonitorTopologyEntry* reference_entry = active_native_entries.front();
    for (const MonitorTopologyEntry* candidate : active_native_entries) {
      if (candidate != nullptr && IsHigherResolution(*candidate, *reference_entry)) {
        reference_entry = candidate;
      }
    }
    evaluation.reference_monitor_id = reference_entry->stable_monitor_id;

    const std::uint32_t reference_height =
        std::max(1U, reference_entry->geometry.height);
    const std::uint32_t reference_dpi =
        std::max(kFallbackDpi,
                 std::max(reference_entry->scale.dpi_x, reference_entry->scale.dpi_y));
    const double reference_density =
        static_cast<double>(reference_height) / static_cast<double>(reference_dpi);
    const double base_vertical_fov = Clamp(config.base_vertical_fov_deg,
                                           config.minimum_vertical_fov_deg,
                                           config.maximum_vertical_fov_deg);

    std::vector<MonitorId> projection_targets = span_selection.monitor_ids;
    if (mirror_selection.enabled) {
      for (MonitorId mirror_target : mirror_selection.target_monitor_ids) {
        projection_targets.push_back(mirror_target);
      }
      projection_targets.push_back(mirror_selection.source_monitor_id);
    }
    projection_targets = SortedUniqueMonitorIds(std::move(projection_targets));

    evaluation.monitor_projections.reserve(projection_targets.size());
    for (MonitorId monitor_id : projection_targets) {
      const MonitorTopologyEntry* entry = FindMonitor(topology, monitor_id);
      if (entry == nullptr) {
        continue;
      }

      MonitorId projection_source = monitor_id;
      bool is_mirror_target = false;
      if (mirror_selection.enabled) {
        const auto mirror_it = mirror_selection.mirrored_to_source.find(monitor_id);
        if (mirror_it != mirror_selection.mirrored_to_source.end()) {
          projection_source = mirror_it->second;
          is_mirror_target = true;
        }
      }

      const MonitorTopologyEntry* source_entry = FindMonitor(topology, projection_source);
      if (source_entry == nullptr) {
        source_entry = entry;
      }

      const std::uint32_t source_height = std::max(1U, source_entry->geometry.height);
      const std::uint32_t source_width = std::max(1U, source_entry->geometry.width);
      const std::uint32_t source_dpi =
          std::max(kFallbackDpi, std::max(source_entry->scale.dpi_x,
                                          source_entry->scale.dpi_y));
      const double source_density =
          static_cast<double>(source_height) / static_cast<double>(source_dpi);
      const double density_scale = reference_density <= 0.0
                                       ? 1.0
                                       : source_density / reference_density;

      const double monitor_vertical_fov = Clamp(
          base_vertical_fov * density_scale,
          config.minimum_vertical_fov_deg,
          config.maximum_vertical_fov_deg);

      MonitorProjection projection{};
      projection.monitor_id = monitor_id;
      projection.projection_source_monitor_id = projection_source;
      projection.is_primary = monitor_id == span_selection.primary_monitor_id;
      projection.is_reference =
          source_entry->stable_monitor_id == reference_entry->stable_monitor_id;
      projection.is_mirror_target = is_mirror_target;
      projection.width_px = entry->geometry.width;
      projection.height_px = entry->geometry.height;
      projection.dpi_x = entry->scale.dpi_x;
      projection.dpi_y = entry->scale.dpi_y;
      projection.aspect_ratio = static_cast<double>(source_width) /
                                static_cast<double>(source_height);
      projection.density_scale = density_scale;
      projection.vertical_fov_deg = monitor_vertical_fov;
      projection.world_units_per_pixel =
          ComputeWorldUnitsPerPixel(monitor_vertical_fov, source_height);
      evaluation.monitor_projections.push_back(std::move(projection));
    }

    std::sort(evaluation.monitor_projections.begin(),
              evaluation.monitor_projections.end(),
              [](const MonitorProjection& lhs, const MonitorProjection& rhs) {
                return lhs.monitor_id < rhs.monitor_id;
              });
    return evaluation;
  }

 private:
  std::unique_ptr<ISpanPolicy> span_policy_;
  std::unique_ptr<IMirrorPolicy> mirror_policy_;
};

}  // namespace

std::unique_ptr<ISpanPolicy> CreateDefaultSpanPolicy() {
  return std::make_unique<DefaultSpanPolicy>();
}

std::unique_ptr<IMirrorPolicy> CreateDefaultMirrorPolicy() {
  return std::make_unique<DefaultMirrorPolicy>();
}

std::unique_ptr<IProjectionPolicy> CreateProjectionPolicy(
    std::unique_ptr<ISpanPolicy> span_policy,
    std::unique_ptr<IMirrorPolicy> mirror_policy) {
  return std::make_unique<ProjectionPolicy>(std::move(span_policy),
                                            std::move(mirror_policy));
}

}  // namespace scene
