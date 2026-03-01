#pragma once

namespace nim {

// Port of tz_geometry.c::Vector_Angle().
[[nodiscard]] double vectorAngleLegacyLike(double x, double y);

// Port of tz_geo3d_utils.c::Geo3d_Orientation_Normal().
void geo3dOrientationNormalLegacyLike(double theta, double psi, double& x, double& y, double& z);

// Port of tz_geo3d_utils.c::Geo3d_Normal_Orientation().
void geo3dNormalOrientationLegacyLike(double x, double y, double z, double& theta, double& psi);

// Port of tz_geo3d_utils.c::Geo3d_Rotate_Orientation().
void geo3dRotateOrientationLegacyLike(double rtheta, double rpsi, double& theta, double& psi);

} // namespace nim
