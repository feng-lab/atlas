#pragma once

#include "zneutubecontfun.h"

namespace nim::neutube {

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
void perceptorGradientLegacyLike(const Perceptor* perceptor, const void* stack, double* gradient);

// Port of tz_perceptor.c::Fit_Perceptor() (modified nonlinear conjugate gradient).
double fitPerceptorLegacyLike(Perceptor* perceptor, const void* stack);

} // namespace nim::neutube
