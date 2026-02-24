#pragma once

#include <cstddef>

namespace nim::neutube {

// Port of legacy tz_darray.c math helpers used by tracing/scoring.
[[nodiscard]] double darrayMaxLegacyLike(const double* data, size_t length, size_t* idx);
[[nodiscard]] double darrayDotNLegacyLike(const double* a, const double* b, size_t length);
[[nodiscard]] double darrayDotNWLegacyLike(const double* a, const double* b, size_t length);
[[nodiscard]] double darraySumNLegacyLike(const double* a, size_t length);
[[nodiscard]] double darrayMeanNLegacyLike(const double* a, size_t length);
[[nodiscard]] double darrayCorrcoefNLegacyLike(const double* a, const double* b, size_t length);

} // namespace nim::neutube
