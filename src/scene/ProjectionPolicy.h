#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "src/platform/win32/MonitorManager.h"

namespace scene {

using MonitorId = ntium::platform::win32::MonitorId;
using MonitorTopologySnapshot = ntium::platform::win32::MonitorTopologySnapshot;
using MonitorTopologyEntry = ntium::platform::win32::MonitorTopologyEntry;

enum class SpanMode : std::uint8_t {
  kSingle = 0,
  kAll,
  kSubset,
};

struct SpanPolicyRequest {
  SpanMode mode = SpanMode::kAll;
  MonitorId preferred_primary_monitor_id = 0;
  std::vector<MonitorId> monitor_ids;
};

struct SpanSelection {
  MonitorId primary_monitor_id = 0;
  std::vector<MonitorId> monitor_ids;
};

class ISpanPolicy {
 public:
  virtual ~ISpanPolicy() = default;
  virtual SpanSelection SelectSpan(const MonitorTopologySnapshot& topology,
                                   const SpanPolicyRequest& request) const = 0;
};

enum class MirrorMode : std::uint8_t {
  kDisabled = 0,
  kEnabled,
};

struct MirrorPolicyRequest {
  MirrorMode mode = MirrorMode::kDisabled;
  MonitorId source_monitor_id = 0;
  std::vector<MonitorId> target_monitor_ids;
};

struct MirrorSelection {
  bool enabled = false;
  MonitorId source_monitor_id = 0;
  std::vector<MonitorId> target_monitor_ids;
  std::unordered_map<MonitorId, MonitorId> mirrored_to_source;
};

class IMirrorPolicy {
 public:
  virtual ~IMirrorPolicy() = default;
  virtual MirrorSelection SelectMirror(const MonitorTopologySnapshot& topology,
                                       const MirrorPolicyRequest& request) const = 0;
};

struct ProjectionPolicyConfig {
  double base_vertical_fov_deg = 50.0;
  double minimum_vertical_fov_deg = 10.0;
  double maximum_vertical_fov_deg = 120.0;
};

struct MonitorProjection {
  MonitorId monitor_id = 0;
  MonitorId projection_source_monitor_id = 0;
  bool is_primary = false;
  bool is_reference = false;
  bool is_mirror_target = false;

  std::uint32_t width_px = 0;
  std::uint32_t height_px = 0;
  std::uint32_t dpi_x = 96;
  std::uint32_t dpi_y = 96;
  double aspect_ratio = 1.0;

  // Relative to highest-resolution monitor in the active native projection set.
  double density_scale = 1.0;
  // Approximate world units represented by one vertical pixel at unit depth.
  double world_units_per_pixel = 0.0;
  // Vertical field-of-view derived from density scaling.
  double vertical_fov_deg = 0.0;
};

struct ProjectionEvaluation {
  std::uint64_t topology_generation = 0;
  MonitorId primary_monitor_id = 0;
  MonitorId reference_monitor_id = 0;
  SpanSelection span_selection;
  MirrorSelection mirror_selection;
  std::vector<MonitorProjection> monitor_projections;
};

class IProjectionPolicy {
 public:
  virtual ~IProjectionPolicy() = default;

  virtual ProjectionEvaluation Evaluate(const MonitorTopologySnapshot& topology,
                                        const SpanPolicyRequest& span_request,
                                        const MirrorPolicyRequest& mirror_request,
                                        const ProjectionPolicyConfig& config) const = 0;
};

std::unique_ptr<ISpanPolicy> CreateDefaultSpanPolicy();
std::unique_ptr<IMirrorPolicy> CreateDefaultMirrorPolicy();
std::unique_ptr<IProjectionPolicy> CreateProjectionPolicy(
    std::unique_ptr<ISpanPolicy> span_policy = {},
    std::unique_ptr<IMirrorPolicy> mirror_policy = {});

}  // namespace scene
