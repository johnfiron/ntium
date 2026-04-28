# Camera Foundation (S1-301 / S1-302)

This document defines the foundational scene/camera model used by the first
SceneCamera batch:

- `src/scene/GeoMath.h/.cpp`
- `src/scene/CameraController.h/.cpp`

The intent is to establish deterministic, double-precision authoritative state
and stable control interfaces before input plumbing and rendering integration.

## Coordinate Spaces

### Authoritative world space

- **ECEF meters (`double`)** are the source of truth for camera position,
  orbit target, and geospatial intersection math.
- ECEF uses the standard Earth-centered, Earth-fixed axes:
  - +X at lat=0, lon=0
  - +Y at lat=0, lon=90E
  - +Z at north pole

### Geodetic interface

- Geodetic coordinates are represented as:
  - latitude (radians)
  - longitude (radians)
  - altitude above ellipsoid (meters)
- WGS84 constants:
  - semi-major axis `a = 6378137.0 m`
  - flattening `f = 1 / 298.257223563`
  - semi-minor axis `b = a * (1 - f)`

## Geo math utilities

`GeoMath` provides:

1. **Geodetic -> ECEF conversion** (closed form).
2. **ECEF -> Geodetic conversion** (Bowring-style closed-form initializer).
3. **Ray vs oblate ellipsoid intersection** returning:
   - hit/miss
   - tangent flag
   - inside/outside start classification
   - near/far parametric distances and points

### Numerical constraints

- All values are `double`.
- Direction vectors are normalized internally before intersection.
- Ray intersection discriminant uses epsilon handling to:
  - classify near-zero as tangent
  - avoid sqrt of tiny negative values due to floating-point noise

## Camera control model

### Modes

- **Free-fly**
  - User drives translational movement along local forward/right plus global up.
  - Yaw/pitch evolve directly from per-tick look deltas.
- **Orbit**
  - Camera orbits around an ECEF target point.
  - Yaw/pitch rotate view direction.
  - Radius changes through zoom input and is clamped to allowed bounds.

### Authoritative camera state

- `position_ecef` (`double` meters)
- `yaw_rad`, `pitch_rad` (`double` radians)
- `orbit_target_ecef` (`double` meters)
- `orbit_radius_m` (`double` meters)
- `mode` (`free-fly` or `orbit`)

### Tick skeleton behavior

`CameraController::Tick(dt, input)`:

1. Rejects non-positive `dt`.
2. Dispatches to mode-specific update path:
   - `TickFreeFly`
   - `TickOrbit`
3. Applies safety clamps:
   - yaw wrapped into `[-pi, pi]`
   - pitch clamped to `[-maxPitch, +maxPitch]`
   - orbit radius clamped to `[minOrbitRadius, maxOrbitRadius]`

This structure intentionally keeps mode update logic isolated so later tickets
can add smoothing and external control arbitration without destabilizing basic
kinematics.

## Mode switching semantics

`SetMode(mode, preserve_view)` supports explicit transitions:

- **free-fly -> orbit**
  - `preserve_view=true`: derive orbit target in front of current position.
  - `preserve_view=false`: reset target/radius to defaults.
- **orbit -> free-fly**
  - `preserve_view=true`: keep current orientation.
  - `preserve_view=false`: reorient camera toward orbit target.

## Current guardrails

- Pitch clamps avoid singularity near poles (`~89 deg` default).
- Orbit radius lower bound stays positive.
- Tuning is clamped/sanitized before use.
- Right vector fallback is stable when forward is near world-up.

## Out of scope for this batch

The following are deferred to later tickets:

- input device routing and focus/capture handling
- cursor-anchored dolly zoom
- inertial smoothing and damping model
- persisted camera/session state
- multi-monitor scale-preserving projection policy
