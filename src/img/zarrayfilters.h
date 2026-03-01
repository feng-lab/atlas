#pragma once

#include <cstddef>

namespace nim {

// Ports of `tz_darray.c` helpers used by neuTube SWC path smoothing:
// - darray_medfilter
// - darray_avgsmooth
//
// Notes:
// - These implement the same edge handling (window clamped to [0, length-1]).
// - `wndsize` is the full window length (not radius).
// - Callers must provide `out` with at least `length` elements.

void medianFilter1DLegacyLike(const double* in, size_t length, int wndsize, double* out);

void averageSmooth1DLegacyLike(const double* in, size_t length, int wndsize, double* out);

} // namespace nim
