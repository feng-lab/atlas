#pragma once

#include <vector>

namespace nim {

// Legacy-compatible port of neuTube's `darray_qsort(...)` (tz_darray.c).
//
// Sorts `values` in-place (ascending) and optionally returns the permutation indices in `outIndices`.
// The algorithm is intentionally identical to the legacy iterative quicksort to preserve tie behavior.
void darrayQsortLegacy(std::vector<double>& values, /*nullable*/ std::vector<int>* outIndices);

} // namespace nim
