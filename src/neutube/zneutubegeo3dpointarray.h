#pragma once

#include <array>

namespace nim::neutube {

// C++ port of tz_geo3d_point_array.c::Geo3d_Point_Array_Translate().
void geo3dPointArrayTranslateLegacyLike(std::array<double, 3>* points, int n, double dx, double dy, double dz);

// C++ port of tz_geo3d_point_array.c::Geo3d_Point_Array_Bend().
void geo3dPointArrayBendLegacyLike(std::array<double, 3>* points, int n, double c);

} // namespace nim::neutube
