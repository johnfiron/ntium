#include "scene/GeoMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scene {

namespace {

constexpr double kHalfPi = 1.57079632679489661923132169163975144;

double FirstEccentricitySquared(const Ellipsoid& ellipsoid) {
  const double a2 = ellipsoid.semi_major_m * ellipsoid.semi_major_m;
  const double b2 = ellipsoid.semi_minor_m * ellipsoid.semi_minor_m;
  return 1.0 - (b2 / a2);
}

double SecondEccentricitySquared(const Ellipsoid& ellipsoid) {
  const double a2 = ellipsoid.semi_major_m * ellipsoid.semi_major_m;
  const double b2 = ellipsoid.semi_minor_m * ellipsoid.semi_minor_m;
  return (a2 - b2) / b2;
}

Vec3d Scale(const Vec3d& v, double s) {
  return Vec3d {v.x * s, v.y * s, v.z * s};
}

Vec3d Add(const Vec3d& lhs, const Vec3d& rhs) {
  return Vec3d {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

}  // namespace

double Dot(const Vec3d& lhs, const Vec3d& rhs) {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

double Length(const Vec3d& value) {
  return std::sqrt(Dot(value, value));
}

Vec3d Normalize(const Vec3d& value) {
  const double length = Length(value);
  if (length <= std::numeric_limits<double>::epsilon()) {
    return Vec3d {};
  }
  return Scale(value, 1.0 / length);
}

Vec3d GeodeticToEcef(const GeodeticCoord& geodetic, const Ellipsoid& ellipsoid) {
  const double sin_lat = std::sin(geodetic.latitude_rad);
  const double cos_lat = std::cos(geodetic.latitude_rad);
  const double sin_lon = std::sin(geodetic.longitude_rad);
  const double cos_lon = std::cos(geodetic.longitude_rad);
  const double e2 = FirstEccentricitySquared(ellipsoid);

  const double prime_vertical =
      ellipsoid.semi_major_m / std::sqrt(1.0 - (e2 * sin_lat * sin_lat));
  const double radial = prime_vertical + geodetic.altitude_m;
  const double z_term = (prime_vertical * (1.0 - e2)) + geodetic.altitude_m;

  return Vec3d {
      radial * cos_lat * cos_lon,
      radial * cos_lat * sin_lon,
      z_term * sin_lat,
  };
}

GeodeticCoord EcefToGeodetic(const Vec3d& ecef, const Ellipsoid& ellipsoid) {
  const double a = ellipsoid.semi_major_m;
  const double b = ellipsoid.semi_minor_m;
  const double e2 = FirstEccentricitySquared(ellipsoid);
  const double ep2 = SecondEccentricitySquared(ellipsoid);

  const double p = std::hypot(ecef.x, ecef.y);
  const double longitude = std::atan2(ecef.y, ecef.x);

  if (p <= std::numeric_limits<double>::epsilon()) {
    const double latitude = (ecef.z >= 0.0) ? kHalfPi : -kHalfPi;
    const double altitude = std::fabs(ecef.z) - b;
    return GeodeticCoord {latitude, longitude, altitude};
  }

  // Bowring's closed-form initializer is stable for Earth ellipsoids.
  const double theta = std::atan2(ecef.z * a, p * b);
  const double sin_theta = std::sin(theta);
  const double cos_theta = std::cos(theta);
  const double latitude =
      std::atan2(ecef.z + (ep2 * b * sin_theta * sin_theta * sin_theta),
                 p - (e2 * a * cos_theta * cos_theta * cos_theta));

  const double sin_lat = std::sin(latitude);
  const double cos_lat = std::cos(latitude);
  const double prime_vertical = a / std::sqrt(1.0 - (e2 * sin_lat * sin_lat));

  double altitude = 0.0;
  if (std::fabs(cos_lat) > 1e-12) {
    altitude = (p / cos_lat) - prime_vertical;
  } else {
    altitude = (ecef.z / sin_lat) - (prime_vertical * (1.0 - e2));
  }

  return GeodeticCoord {latitude, longitude, altitude};
}

RayEllipsoidIntersection IntersectRayWithEllipsoid(const Vec3d& ray_origin,
                                                   const Vec3d& ray_direction,
                                                   const Ellipsoid& ellipsoid,
                                                   double epsilon) {
  const Vec3d direction = Normalize(ray_direction);
  const double a = ellipsoid.semi_major_m;
  const double b = ellipsoid.semi_minor_m;
  const double inv_a2 = 1.0 / (a * a);
  const double inv_b2 = 1.0 / (b * b);

  const double qa =
      ((direction.x * direction.x) + (direction.y * direction.y)) * inv_a2 +
      (direction.z * direction.z) * inv_b2;
  const double qb = 2.0 * (((ray_origin.x * direction.x) + (ray_origin.y * direction.y)) *
                               inv_a2 +
                           (ray_origin.z * direction.z) * inv_b2);
  const double qc =
      ((ray_origin.x * ray_origin.x) + (ray_origin.y * ray_origin.y)) * inv_a2 +
      (ray_origin.z * ray_origin.z) * inv_b2 - 1.0;

  RayEllipsoidIntersection result {};
  result.starts_inside = qc < 0.0;

  if (qa <= epsilon) {
    return result;
  }

  double discriminant = (qb * qb) - (4.0 * qa * qc);
  if (discriminant < 0.0) {
    // Numerical cancellation can produce tiny negative discriminants for true
    // tangential contacts when the origin is already on the ellipsoid.
    const bool near_zero_disc = std::fabs(discriminant) <= epsilon;
    const bool origin_near_surface = std::fabs(qc) <= epsilon;
    if (near_zero_disc && origin_near_surface) {
      discriminant = 0.0;
    } else {
      return result;
    }
  }

  const double safe_discriminant = std::max(0.0, discriminant);
  const double sqrt_discriminant = std::sqrt(safe_discriminant);
  const double inv_denominator = 1.0 / (2.0 * qa);

  double t0 = (-qb - sqrt_discriminant) * inv_denominator;
  double t1 = (-qb + sqrt_discriminant) * inv_denominator;
  if (t0 > t1) {
    std::swap(t0, t1);
  }

  result.t_near = t0;
  result.t_far = t1;
  result.is_tangent = std::fabs(discriminant) <= epsilon;
  result.intersects = t1 >= 0.0;
  result.point_near = Add(ray_origin, Scale(direction, t0));
  result.point_far = Add(ray_origin, Scale(direction, t1));

  return result;
}

RayEllipsoidForwardHit ClosestForwardRayEllipsoidHit(const Vec3d& ray_origin,
                                                     const Vec3d& ray_direction,
                                                     const Ellipsoid& ellipsoid,
                                                     double epsilon) {
  const RayEllipsoidIntersection intersection =
      IntersectRayWithEllipsoid(ray_origin, ray_direction, ellipsoid, epsilon);

  RayEllipsoidForwardHit hit {};
  if (!intersection.intersects) {
    return hit;
  }

  const bool near_is_forward = intersection.t_near >= 0.0;
  const bool far_is_forward = intersection.t_far >= 0.0;
  if (!near_is_forward && !far_is_forward) {
    return hit;
  }

  if (near_is_forward) {
    hit.hit = true;
    hit.t = intersection.t_near;
    hit.point = intersection.point_near;
    return hit;
  }

  hit.hit = true;
  hit.t = intersection.t_far;
  hit.point = intersection.point_far;
  return hit;
}

}  // namespace scene
