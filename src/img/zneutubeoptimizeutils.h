#pragma once

#include "zneutubecontfun.h"

#include <vector>

namespace nim {

// C++ port of tz_optimize_utils.h::Line_Search_Workspace.
struct LineSearchWorkspace
{
  explicit LineSearchWorkspace(int nvar_)
    : nvar(nvar_)
    , grad(static_cast<size_t>(nvar_))
    , startGrad(static_cast<size_t>(nvar_))
  {}

  int nvar = 0;
  double alpha = 1.0;
  double ro = 0.5;
  double c1 = 0.01;
  double c2 = 0.3;
  double minDirection = 0.0;
  std::vector<double> grad;
  std::vector<double> startGrad;
  double score = 0.0;
};

// Port of tz_optimize_utils.c::Set_Line_Search_Workspace().
void setLineSearchWorkspaceLegacyLike(LineSearchWorkspace& lsw,
                                      double alpha,
                                      double ro,
                                      double c1,
                                      double c2,
                                      double minDirection);

// Port of tz_optimize_utils.c::Line_Search_Var_Backtrack().
//
// Notes:
// - Preserves the legacy in-place scaling of `direction` when `weight != nullptr`.
//
// Pointer-parameter semantics:
// - `param` is forwarded as an opaque context pointer to `cf.f(...)` (legacy API).
// - `delta` is currently unused by the legacy backtracking routine but preserved for signature parity.
// - `weight` is optional; when non-null it scales `direction[i]` in-place before the line-search.
// - `direction` is an in/out array of length `vs.nvar`.
[[nodiscard]] bool lineSearchVarBacktrackLegacyLike(VariableSet& vs,
                                                    const void* param,
                                                    const ContinuousFunction& cf,
                                                    const double* delta,
                                                    /*nullable*/ const double* weight,
                                                    double* direction,
                                                    LineSearchWorkspace& lsw);

// Port of tz_optimize_utils.c::Conjugate_Update_Direction() (Polak-Ribiere+).
void conjugateUpdateDirectionLegacyLike(int nvar, const double* grad, const double* prevGrad, double* direction);

} // namespace nim
