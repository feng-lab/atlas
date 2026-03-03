#pragma once

namespace nim {

// Matches tz_math.c::iround() when HAVE_LRINT is not enabled and HAVE_ROUND is enabled:
// round-to-nearest with ties away from zero.
[[nodiscard]] inline int iroundLegacyLike(double x)
{
  return (x > 0.0) ? static_cast<int>(x + 0.5) : static_cast<int>(x - 0.5);
}

} // namespace nim
