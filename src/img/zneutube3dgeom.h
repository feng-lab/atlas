#pragma once

#include <array>
#include <span>

namespace nim {

// C++ port of tz_3dgeom.c::Rotate_XZ() operating on std::array-based points.
void rotateXZLegacyLike(std::span<std::array<double, 3>> points, double theta, double psi, int inverse);

// Single-point convenience overload.
void rotateXZLegacyLike(std::array<double, 3>& point, double theta, double psi, int inverse);

// C++ port of tz_3dgeom.c::Rotate_Z() operating on std::array-based points.
void rotateZLegacyLike(std::span<std::array<double, 3>> points, double alpha, int inverse);

// Single-point convenience overload.
void rotateZLegacyLike(std::array<double, 3>& point, double alpha, int inverse);

} // namespace nim
