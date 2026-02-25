#pragma once

#include "zneutubetracerecord.h"

#include <cstddef>

namespace nim {

// Port of the `Stack_Fit_Score` switch logic used by legacy
// `Geo3d_Scalar_Field_Stack_Score*()` functions, but operating directly on
// pre-sampled arrays instead of `Geo3d_Scalar_Field` + `Stack`.
//
// `fieldValues` correspond to `field->values` and `signalValues` correspond to
// the sampled `signal[]` array in tz_geo3d_scalar_field.c.
//
// Returns the primary score (fs->scores[0]) when `fs != nullptr`, otherwise
// returns the legacy default `darray_dot_n(fieldValues, signalValues, length)`.
[[nodiscard]] double computeStackFitScoresLegacyLike(const double* fieldValues,
                                                     const double* signalValues,
                                                     size_t length,
                                                     /*nullable*/ StackFitScore* fs);

// Port of the `Stack_Fit_Score` switch logic used by legacy
// `Geo3d_Scalar_Field_Stack_Score_M()` (masked sampling variant).
//
// Legacy notes:
// - This switch is *not identical* to the unmasked version:
//   - `STACK_FIT_PDOT` and `STACK_FIT_MEAN_SIGNAL` use `fieldValues[i] > 0.0` (strict) instead of `>= 0.0`.
//   - `STACK_FIT_LOW_MEAN_SIGNAL` is not implemented in the legacy masked scorer and triggers an error.
// - When `fs == nullptr`, legacy returns `darray_dot_nw(fieldValues, signalValues, length)` (not dot_n).
[[nodiscard]] double computeStackFitScoresMaskedLegacyLike(const double* fieldValues,
                                                           const double* signalValues,
                                                           size_t length,
                                                           /*nullable*/ StackFitScore* fs);

} // namespace nim
