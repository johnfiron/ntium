#pragma once

#include <cstddef>

namespace scene {

struct Vec3d {
  double x {0.0};
  double y {0.0};
  double z {0.0};
};

struct GeodeticCoord {
  // Latitude/longitude are expressed in radians.
  double latitude_rad {0.0};
  double longitude_rad {0.0};
  // Height above the ellipsoid surface in meters.
  double altitude_m {0.0};
};

struct Ellipsoid {
  // Equatorial radius (a) in meters.
  double semi_major_m {0.0};
  // Polar radius (b) in meters.
  double semi_minor_m {0.0};
};

struct RayEllipsoidIntersection {
  bool intersects {false};
  bool starts_inside {false};
  bool is_tangent {false};
  // Parametric distances along origin + t * direction.
  double t_near {0.0};
  double t_far {0.0};
  Vec3d point_near {};
  Vec3d point_far {};
};

constexpr double kWgs84SemiMajorMeters = 6378137.0;
constexpr double kWgs84Flattening = 1.0 / 298.257223563;
constexpr double kWgs84SemiMinorMeters =
    kWgs84SemiMajorMeters * (1.0 - kWgs84Flattening);

constexpr Ellipsoid Wgs84Ellipsoid() {
  return Ellipsoid {kWgs84SemiMajorMeters, kWgs84SemiMinorMeters};
}

double Dot(const Vec3d& lhs, const Vec3d& rhs);
double Length(const Vec3d& value);
Vec3d Normalize(const Vec3d& value);

Vec3d GeodeticToEcef(const GeodeticCoord& geodetic,
                     const Ellipsoid& ellipsoid = Wgs84Ellipsoid());

GeodeticCoord EcefToGeodetic(const Vec3d& ecef,
                             const Ellipsoid& ellipsoid = Wgs84Ellipsoid());

RayEllipsoidIntersection IntersectRayWithEllipsoid(
    const Vec3d& ray_origin,
    const Vec3d& ray_direction,
    const Ellipsoid& ellipsoid = Wgs84Ellipsoid(),
    double epsilon = 1e-12);

}  // namespace scene
