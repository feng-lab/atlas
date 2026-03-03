#pragma once

#include <cmath>
#include <limits>

namespace nim {

// Matches legacy neuTube `neutu::iround` semantics (`int(std::lround(x))`) for all inputs.
// Fast path for finite values that safely fit in `int`: round-to-nearest with ties away from zero.
[[nodiscard]] inline int iroundLegacyLike(double x)
{
  // Legacy reference behavior (NeuTu): return std::lround(v) as an int.
  //
  // Keep the fast-path implementation below (commented out) in case we want to restore
  // it later for performance testing.

#if 0
  // NOTE: Some legacy call sites can feed NaNs here (e.g. `sqrt(r*r - dz*dz)` when `dz > r` due to rounding).
  // `std::lround` handles NaN/inf/out-of-range inputs with well-defined library behavior, and legacy code
  // then converts that `long` to `int`. Keep that exact behavior as the reference.

  if (std::isfinite(x)) {
    const double minInt = static_cast<double>(std::numeric_limits<int>::min());
    const double maxInt = static_cast<double>(std::numeric_limits<int>::max());
    if (x >= minInt && x <= maxInt) {
      return (x > 0.0) ? static_cast<int>(x + 0.5) : static_cast<int>(x - 0.5);
    }
  }
#endif

  return static_cast<int>(std::lround(x));
}

} // namespace nim
