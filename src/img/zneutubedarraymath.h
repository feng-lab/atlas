#pragma once

#include <cstddef>
#include <span>

namespace nim {

// Port of legacy tz_darray.c math helpers used by tracing/scoring.
// - `idx` is optional; pass nullptr when the max index is not needed (legacy callers do this in score paths).
//
// Contracts:
// - Input spans must be non-empty where legacy assumes `length > 0` (e.g. `darrayMaxLegacyLike`,
// `darrayMeanNLegacyLike`).
// - For pairwise operations, spans must be the same size (legacy callers guarantee this).
[[nodiscard]] double darrayMaxLegacyLike(std::span<const double> data, /*nullable*/ size_t* idx);
[[nodiscard]] double darrayDotNLegacyLike(std::span<const double> a, std::span<const double> b);
[[nodiscard]] double darrayDotNWLegacyLike(std::span<const double> a, std::span<const double> b);
[[nodiscard]] double darraySumNLegacyLike(std::span<const double> a);
[[nodiscard]] double darrayMeanNLegacyLike(std::span<const double> a);
[[nodiscard]] double darrayCorrcoefNLegacyLike(std::span<const double> a, std::span<const double> b);

} // namespace nim
