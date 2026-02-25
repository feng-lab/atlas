#pragma once

#include <array>

namespace nim::neutube {

// C++ port of tz_3dgeom.c::Rotate_XZ() operating on std::array-based points.
//
// Pointer-parameter semantics:
// - `points` must be a valid contiguous array of at least `n` points (in/out).
void rotateXZLegacyLike(std::array<double, 3>* points, int n, double theta, double psi, int inverse);

// C++ port of tz_3dgeom.c::Rotate_Z() operating on std::array-based points.
//
// Pointer-parameter semantics:
// - `points` must be a valid contiguous array of at least `n` points (in/out).
void rotateZLegacyLike(std::array<double, 3>* points, int n, double alpha, int inverse);

} // namespace nim::neutube
