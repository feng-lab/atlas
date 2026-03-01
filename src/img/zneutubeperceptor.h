#pragma once

#include "zneutubecontfun.h"

namespace nim {

// C++ port of tz_perceptor.h::Perceptor.
struct Perceptor
{
  VariableSet* vs = nullptr; // not owned
  const void* arg = nullptr; // additional argument passed through to the score function
  const ContinuousFunction* s = nullptr; // not owned
  double minGradient = 0.0;
  const double* delta = nullptr; // finite-difference steps indexed by global variable index
  const double* weight = nullptr; // optional per-active-variable weights
};

// Port of tz_perceptor.c::Perceptor_Gradient().
//
// Pointer-parameter semantics:
// - `stack` is forwarded as an opaque context pointer to the score function (legacy API).
// - `gradient` is an output array of length `perceptor.vs->nvar`.
void perceptorGradientLegacyLike(const Perceptor& perceptor, const void* stack, double* gradient);

// Port of tz_perceptor.c::Fit_Perceptor() (modified nonlinear conjugate gradient).
// `stack` is forwarded as an opaque context pointer to the score function (legacy API).
double fitPerceptorLegacyLike(Perceptor& perceptor, const void* stack);

} // namespace nim
