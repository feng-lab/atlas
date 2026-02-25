#pragma once

#include <array>

namespace nim::neutube {

// C++ port of tz_geo3d_point_array.c::Geo3d_Point_Array_Translate().
//
// Pointer-parameter semantics:
// - `points` must be a valid contiguous array of at least `n` points (in/out).
// - The legacy API operates on raw arrays; callers typically pass `vector.data()`.
void geo3dPointArrayTranslateLegacyLike(std::array<double, 3>* points, int n, double dx, double dy, double dz);

// C++ port of tz_geo3d_point_array.c::Geo3d_Point_Array_Bend().
//
// Pointer-parameter semantics:
// - `points` must be a valid contiguous array of at least `n` points (in/out).
void geo3dPointArrayBendLegacyLike(std::array<double, 3>* points, int n, double c);

} // namespace nim::neutube
