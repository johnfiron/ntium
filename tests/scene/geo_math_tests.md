# Geo Math Test Vectors and Behavioral Spec (S1-301)

This file defines deterministic test vectors for `scene::GeodeticToEcef`,
`scene::EcefToGeodetic`, and `scene::IntersectRayWithEllipsoid` in
`src/scene/GeoMath.*`.

## Constants

- Ellipsoid: WGS84
  - `a = 6378137.0 m`
  - `f = 1 / 298.257223563`
  - `b = 6356752.314245179 m`

## Numeric tolerances

Recommended absolute tolerances unless otherwise noted:

- Geodetic latitude/longitude roundtrip: `<= 1e-10 rad`
- Altitude roundtrip: `<= 1e-4 m`
- ECEF component checks: `<= 1e-3 m`
- Ray parameter checks (`t_near`, `t_far`): `<= 1e-6`

## Geodetic -> ECEF vectors

All angles below are degrees for readability.

1. **Equator origin**
   - Input: lat=0, lon=0, h=0
   - Expected ECEF: `(6378137.0, 0.0, 0.0)`

2. **Mid-latitude elevated**
   - Input: lat=45, lon=45, h=1000
   - Expected ECEF:
     - `x=3194919.145061`
     - `y=3194919.145061`
     - `z=4488055.515647`

3. **San Francisco reference**
   - Input: lat=37.7749, lon=-122.4194, h=30
   - Expected ECEF:
     - `x=-2706187.559291`
     - `y=-4261079.506291`
     - `z=3885743.866849`

4. **Sydney reference**
   - Input: lat=-33.8688, lon=151.2093, h=80
   - Expected ECEF:
     - `x=-4646109.486166`
     - `y=2553238.333389`
     - `z=-3534416.971357`

5. **Anti-meridian on equator**
   - Input: lat=0, lon=180, h=500
   - Expected ECEF:
     - `x=-6378637.0`
     - `y=0.0`
     - `z=0.0`

6. **Near north pole**
   - Input: lat=89.999, lon=0, h=0
   - Expected ECEF:
     - `x=111.693980`
     - `y=0.0`
     - `z=6356752.313270`

## ECEF -> Geodetic roundtrip requirements

For each vector above:

1. Convert geodetic -> ECEF.
2. Convert resulting ECEF -> geodetic.
3. Verify:
   - latitude error within `1e-10 rad`
   - longitude error within `1e-10 rad` with angle-wrap handling
   - altitude error within `1e-4 m`

Additional edge cases:

- **Polar axis positive**: ECEF `(0, 0, b + 50)` should return latitude near
  `+pi/2` and altitude near `50`.
- **Polar axis negative**: ECEF `(0, 0, -(b + 50))` should return latitude near
  `-pi/2` and altitude near `50`.

## Ray-Ellipsoid intersection cases

Use `Wgs84Ellipsoid()` and normalize direction as implementation does.

1. **Direct hit from +X**
   - Origin: `(7000000, 0, 0)`
   - Direction: `(-1, 0, 0)`
   - Expect:
     - `intersects=true`
     - `starts_inside=false`
     - `is_tangent=false`
     - `t_near ~ 621863`
     - `t_far ~ 13378137`

2. **Miss**
   - Origin: `(7000000, 0, 0)`
   - Direction: `(0, 1, 0)`
   - Expect:
     - `intersects=false`

3. **Tangent**
   - Origin: `(a, 0, 0)` (point on ellipsoid surface)
   - Direction: `(0, 1, 0)`
   - Expect:
     - `intersects=true`
     - `is_tangent=true`
     - `t_near ~= t_far`

4. **Starts inside**
   - Origin: `(0, 0, 0)`
   - Direction: `(1, 0, 0)`
   - Expect:
     - `starts_inside=true`
     - `intersects=true`
     - `t_near < 0`
     - `t_far > 0`

## Non-functional assertions

- All authoritative computations remain in `double`.
- No conversion utility should throw for finite inputs.
- Degenerate direction vectors for ray tests must return `intersects=false`.
