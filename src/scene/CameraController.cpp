#include "scene/CameraController.h"

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;

Vec3d Scale(const Vec3d& v, double s) {
  return Vec3d {v.x * s, v.y * s, v.z * s};
}

Vec3d Add(const Vec3d& lhs, const Vec3d& rhs) {
  return Vec3d {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3d Subtract(const Vec3d& lhs, const Vec3d& rhs) {
  return Vec3d {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

double ClampMagnitude(double value, double max_abs) {
  return std::clamp(value, -max_abs, max_abs);
}

Vec3d WrapYawPitchToForward(double yaw_rad, double pitch_rad) {
  const double cos_pitch = std::cos(pitch_rad);
  return Normalize(Vec3d {
      std::cos(yaw_rad) * cos_pitch,
      std::sin(yaw_rad) * cos_pitch,
      std::sin(pitch_rad),
  });
}

double WrapYaw(double yaw_rad) {
  const double wrapped = std::fmod(yaw_rad, kTwoPi);
  if (wrapped > kPi) {
    return wrapped - kTwoPi;
  }
  if (wrapped < -kPi) {
    return wrapped + kTwoPi;
  }
  return wrapped;
}

}  // namespace

CameraController::CameraController() {
  state_.mode = CameraMode::kOrbit;
  state_.position_ecef = Vec3d {0.0, -7'000'000.0, 0.0};
  state_.orbit_target_ecef = Vec3d {};
  state_.orbit_radius_m = 7'000'000.0;
  state_.yaw_rad = kPi * 0.5;
  state_.pitch_rad = 0.0;
  ClampState();
}

const CameraState& CameraController::state() const {
  return state_;
}

const CameraTuning& CameraController::tuning() const {
  return tuning_;
}

void CameraController::SetState(const CameraState& state) {
  state_ = state;
  ClampState();
}

void CameraController::SetTuning(const CameraTuning& tuning) {
  tuning_ = tuning;
  ClampState();
}

void CameraController::SetMode(CameraMode mode, bool preserve_view) {
  if (mode == state_.mode) {
    return;
  }

  if (mode == CameraMode::kOrbit) {
    TransitionToOrbit(preserve_view);
  } else {
    TransitionToFreeFly(preserve_view);
  }

  state_.mode = mode;
  ClampState();
}

void CameraController::Tick(double delta_seconds, const CameraTickInput& input) {
  if (delta_seconds <= 0.0) {
    return;
  }

  if (state_.mode == CameraMode::kFreeFly) {
    TickFreeFly(delta_seconds, input.free_fly);
  } else {
    TickOrbit(delta_seconds, input.orbit);
  }

  ClampState();
}

void CameraController::TickFreeFly(double delta_seconds, const FreeFlyControlInput& input) {
  state_.yaw_rad += input.yaw_delta_rad;
  state_.pitch_rad += input.pitch_delta_rad;

  double speed = tuning_.free_fly_speed_mps;
  if (input.boost_active) {
    speed *= std::max(1.0, tuning_.free_fly_boost_multiplier);
  }

  const Vec3d forward = ForwardDirection();
  const Vec3d right = RightDirection();
  const Vec3d up = Vec3d {0.0, 0.0, 1.0};

  Vec3d linear_velocity = Vec3d {};
  linear_velocity = Add(linear_velocity, Scale(forward, input.move_forward));
  linear_velocity = Add(linear_velocity, Scale(right, input.move_right));
  linear_velocity = Add(linear_velocity, Scale(up, input.move_up));

  const double axis_norm = Length(linear_velocity);
  if (axis_norm > 1.0) {
    linear_velocity = Scale(linear_velocity, 1.0 / axis_norm);
  }

  state_.position_ecef =
      Add(state_.position_ecef, Scale(linear_velocity, speed * delta_seconds));
}

void CameraController::TickOrbit(double delta_seconds, const OrbitControlInput& input) {
  const double angular_scale =
      std::max(0.0, tuning_.orbit_angular_sensitivity) * delta_seconds;
  state_.yaw_rad += input.yaw_delta_rad * angular_scale;
  state_.pitch_rad += input.pitch_delta_rad * angular_scale;

  const Vec3d forward = ForwardDirection();
  const double zoom_scale =
      std::max(0.0, tuning_.orbit_zoom_units_per_sec) * delta_seconds;
  const double zoom_distance_m = input.zoom_delta * zoom_scale;
  if (std::fabs(zoom_distance_m) > 0.0) {
    Vec3d anchor_ecef {};
    if (ResolveOrbitZoomAnchor(input, &anchor_ecef)) {
      ApplyOrbitAnchorDolly(zoom_distance_m, anchor_ecef);
    } else {
      ApplyOrbitCenterFallbackZoom(zoom_distance_m, forward);
    }
  } else {
    // Preserve orbit invariants when only orbiting angularly.
    state_.position_ecef = Add(state_.orbit_target_ecef, Scale(forward, -state_.orbit_radius_m));
  }
}

bool CameraController::ResolveOrbitZoomAnchor(const OrbitControlInput& input,
                                              Vec3d* out_anchor_ecef) const {
  if (out_anchor_ecef == nullptr || !input.has_cursor_ray) {
    return false;
  }

  const RayEllipsoidForwardHit anchor_hit = ClosestForwardRayEllipsoidHit(
      input.cursor_ray_origin_ecef, input.cursor_ray_direction_ecef, Wgs84Ellipsoid());
  if (!anchor_hit.hit) {
    return false;
  }

  *out_anchor_ecef = anchor_hit.point;
  return true;
}

void CameraController::ApplyOrbitAnchorDolly(double zoom_distance_m, const Vec3d& anchor_ecef) {
  const Vec3d to_anchor = Subtract(anchor_ecef, state_.position_ecef);
  const double anchor_distance = Length(to_anchor);
  if (anchor_distance <= 1e-9) {
    return;
  }

  const Vec3d toward_anchor = Scale(to_anchor, 1.0 / anchor_distance);
  const double min_spacing = std::max(0.0, tuning_.min_orbit_radius_m);
  const double max_forward_motion = std::max(0.0, anchor_distance - min_spacing);
  const double max_backward_motion =
      std::max(0.0, tuning_.max_orbit_radius_m - state_.orbit_radius_m);
  const double signed_motion_m =
      ClampMagnitude(zoom_distance_m, zoom_distance_m >= 0.0 ? max_forward_motion
                                                              : max_backward_motion);
  state_.position_ecef = Add(state_.position_ecef, Scale(toward_anchor, signed_motion_m));

  // Keep orbit model authoritative after dolly translation.
  const Vec3d forward = ForwardDirection();
  state_.orbit_target_ecef = Add(state_.position_ecef, Scale(forward, state_.orbit_radius_m));
}

void CameraController::ApplyOrbitCenterFallbackZoom(double zoom_distance_m,
                                                    const Vec3d& forward_direction) {
  const Vec3d forward = Normalize(forward_direction);
  if (Length(forward) <= 1e-12) {
    return;
  }

  // No-hit fallback: dolly along current view center instead of FOV-only zoom.
  const double max_forward_motion =
      std::max(0.0, state_.orbit_radius_m - tuning_.min_orbit_radius_m);
  const double max_backward_motion =
      std::max(0.0, tuning_.max_orbit_radius_m - state_.orbit_radius_m);
  const double signed_motion_m =
      ClampMagnitude(zoom_distance_m, zoom_distance_m >= 0.0 ? max_forward_motion
                                                              : max_backward_motion);
  state_.position_ecef = Add(state_.position_ecef, Scale(forward, signed_motion_m));
  state_.orbit_target_ecef = Add(state_.position_ecef, Scale(forward, state_.orbit_radius_m));
}

void CameraController::TransitionToOrbit(bool preserve_view) {
  if (!preserve_view) {
    state_.orbit_target_ecef = Vec3d {};
    state_.orbit_radius_m = tuning_.default_orbit_radius_m;
    const Vec3d forward = ForwardDirection();
    state_.position_ecef =
        Add(state_.orbit_target_ecef, Scale(forward, -state_.orbit_radius_m));
    return;
  }

  const Vec3d forward = ForwardDirection();
  const double radius =
      std::clamp(tuning_.default_orbit_radius_m, tuning_.min_orbit_radius_m,
                 std::max(tuning_.min_orbit_radius_m, tuning_.max_orbit_radius_m));
  state_.orbit_radius_m = radius;
  state_.orbit_target_ecef = Add(state_.position_ecef, Scale(forward, radius));
}

void CameraController::TransitionToFreeFly(bool preserve_view) {
  if (preserve_view) {
    return;
  }

  // If view preservation is not requested, orient toward the orbit target.
  const Vec3d to_target =
      Normalize(Subtract(state_.orbit_target_ecef, state_.position_ecef));
  if (Length(to_target) <= 0.0) {
    return;
  }

  state_.pitch_rad = std::asin(std::clamp(to_target.z, -1.0, 1.0));
  state_.yaw_rad = std::atan2(to_target.y, to_target.x);
}

void CameraController::ClampState() {
  state_.yaw_rad = WrapYaw(state_.yaw_rad);
  state_.pitch_rad =
      std::clamp(state_.pitch_rad, -tuning_.max_pitch_abs_rad, tuning_.max_pitch_abs_rad);

  tuning_.min_orbit_radius_m = std::max(0.001, tuning_.min_orbit_radius_m);
  tuning_.max_orbit_radius_m =
      std::max(tuning_.min_orbit_radius_m, tuning_.max_orbit_radius_m);
  tuning_.default_orbit_radius_m = std::clamp(
      tuning_.default_orbit_radius_m, tuning_.min_orbit_radius_m, tuning_.max_orbit_radius_m);

  state_.orbit_radius_m = std::clamp(state_.orbit_radius_m, tuning_.min_orbit_radius_m,
                                     tuning_.max_orbit_radius_m);
}

Vec3d CameraController::ForwardDirection() const {
  return WrapYawPitchToForward(state_.yaw_rad, state_.pitch_rad);
}

Vec3d CameraController::RightDirection() const {
  const Vec3d forward = ForwardDirection();
  const Vec3d world_up = Vec3d {0.0, 0.0, 1.0};
  const Vec3d right = Vec3d {
      (world_up.y * forward.z) - (world_up.z * forward.y),
      (world_up.z * forward.x) - (world_up.x * forward.z),
      (world_up.x * forward.y) - (world_up.y * forward.x),
  };
  const double right_len = Length(right);
  if (right_len <= 1e-12) {
    return Vec3d {1.0, 0.0, 0.0};
  }
  return Scale(right, 1.0 / right_len);
}

}  // namespace scene
