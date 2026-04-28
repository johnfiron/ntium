#pragma once

#include "scene/GeoMath.h"

namespace scene {

enum class CameraMode {
  kFreeFly = 0,
  kOrbit = 1,
};

struct CameraTuning {
  // Free-fly linear speed in meters per second.
  double free_fly_speed_mps {500.0};
  // Additional speed multiplier for boosted movement.
  double free_fly_boost_multiplier {4.0};
  // Orbit angular sensitivity multiplier for per-tick input deltas.
  double orbit_angular_sensitivity {1.0};
  // Orbit zoom scaling (larger values zoom faster).
  double orbit_zoom_units_per_sec {1.0};
  // Minimum and maximum radius in meters for orbit mode.
  double min_orbit_radius_m {10.0};
  double max_orbit_radius_m {40'000'000.0};
  // Pitch safety clamp to avoid singularity near poles.
  double max_pitch_abs_rad {1.5533430342749532};  // ~89 deg
  // Radius used when transitioning from free-fly to orbit.
  double default_orbit_radius_m {10'000.0};
};

struct CameraState {
  CameraMode mode {CameraMode::kOrbit};

  // Authoritative world-space camera pose.
  Vec3d position_ecef {};
  double yaw_rad {0.0};
  double pitch_rad {0.0};

  // Orbit state (target in ECEF meters, radius in meters).
  Vec3d orbit_target_ecef {};
  double orbit_radius_m {1'000.0};
};

struct FreeFlyControlInput {
  // Signed local-space axes in range [-1, 1].
  double move_forward {0.0};
  double move_right {0.0};
  double move_up {0.0};

  // Relative look deltas in radians for this tick.
  double yaw_delta_rad {0.0};
  double pitch_delta_rad {0.0};

  bool boost_active {false};
};

struct OrbitControlInput {
  // Relative orbital deltas in radians for this tick.
  double yaw_delta_rad {0.0};
  double pitch_delta_rad {0.0};

  // Positive value zooms toward the target.
  double zoom_delta {0.0};
};

struct CameraTickInput {
  FreeFlyControlInput free_fly {};
  OrbitControlInput orbit {};
};

class CameraController {
 public:
  CameraController();

  const CameraState& state() const;
  const CameraTuning& tuning() const;

  void SetState(const CameraState& state);
  void SetTuning(const CameraTuning& tuning);

  void SetMode(CameraMode mode, bool preserve_view = true);
  void Tick(double delta_seconds, const CameraTickInput& input);

 private:
  void TickFreeFly(double delta_seconds, const FreeFlyControlInput& input);
  void TickOrbit(double delta_seconds, const OrbitControlInput& input);

  void TransitionToOrbit(bool preserve_view);
  void TransitionToFreeFly(bool preserve_view);

  void ClampState();
  Vec3d ForwardDirection() const;
  Vec3d RightDirection() const;

  CameraState state_ {};
  CameraTuning tuning_ {};
};

}  // namespace scene
