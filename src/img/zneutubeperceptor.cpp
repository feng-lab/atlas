#include "zneutubeperceptor.h"

#include "zneutubeoptimizeutils.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace nim {

namespace {

constexpr int FitPerceptorMaxIterLegacyLike = 500;

[[nodiscard]] double sqsumLegacyLike(std::span<const double> a)
{
  double s = 0.0;
  for (double v : a) {
    s += v * v;
  }
  return s;
}

inline void updateVariableLegacyLike(VariableSet& vs, int index, double delta)
{
  vs.var[vs.varIndex[index]] += delta;
  variableSetUpdateLinkLegacyLike(vs);
}

double perceptorGradientPartialLegacyLike(VariableSet& vs,
                                          int index,
                                          const void* stack,
                                          double delta,
                                          double score,
                                          const void* arg,
                                          ScoreFunctionLegacyLike f)
{
  updateVariableLegacyLike(vs, index, delta);

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = arg;

  const double rightScore = f(vs.var, paramArray);

  // restore param
  updateVariableLegacyLike(vs, index, -delta);

  double grad = (rightScore - score) / delta;
  if (grad < 0.0) {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs.var, paramArray);
    if (leftScore < score) {
      grad = 0.0;
    } else {
      grad = (score - leftScore) / delta;
    }
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  } else if (grad > 0.0) {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs.var, paramArray);
    if (leftScore > score) {
      grad = (score - leftScore) / delta;
    }
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  } else {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs.var, paramArray);
    grad = (score - leftScore) / delta;
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  }

  return grad;
}

} // namespace

void perceptorGradientLegacyLike(const Perceptor& perceptor, const void* stack, double* gradient)
{
  CHECK(perceptor.vs != nullptr);
  CHECK(perceptor.s != nullptr);
  CHECK(perceptor.s->f != nullptr);
  CHECK(perceptor.delta != nullptr);
  CHECK(gradient != nullptr);

  VariableSet& vs = *perceptor.vs;
  CHECK(vs.var != nullptr);
  CHECK(vs.varIndex != nullptr);
  CHECK(vs.nvar >= 0);

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = perceptor.arg;

  const double score = perceptor.s->f(vs.var, paramArray);

  for (int i = 0; i < vs.nvar; ++i) {
    const int varIndex = vs.varIndex[i];
    gradient[i] =
      perceptorGradientPartialLegacyLike(vs, i, stack, perceptor.delta[varIndex], score, perceptor.arg, perceptor.s->f);
  }
}

double fitPerceptorLegacyLike(Perceptor& perceptor, const void* stack)
{
  CHECK(perceptor.vs != nullptr);
  CHECK(perceptor.s != nullptr);
  CHECK(perceptor.s->f != nullptr);
  CHECK(perceptor.delta != nullptr);

  VariableSet& vs = *perceptor.vs;
  CHECK(vs.var != nullptr);
  CHECK(vs.varIndex != nullptr);
  CHECK(vs.nvar >= 0);

  const int nvar = vs.nvar;
  static thread_local LineSearchWorkspace lsw(0);
  if (lsw.nvar != nvar) {
    lsw.nvar = nvar;
    lsw.grad.resize(static_cast<size_t>(nvar));
    lsw.startGrad.resize(static_cast<size_t>(nvar));
  }
  setLineSearchWorkspaceLegacyLike(lsw, 0.2, 0.8, 0.01, 0.1, perceptor.minGradient);

  static thread_local std::vector<double> updateDirectionScratch;
  updateDirectionScratch.resize(static_cast<size_t>(nvar));

  perceptorGradientLegacyLike(perceptor, stack, lsw.startGrad.data());

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = perceptor.arg;
  lsw.score = perceptor.s->f(vs.var, paramArray);

  std::copy(lsw.startGrad.begin(), lsw.startGrad.end(), updateDirectionScratch.begin());

  bool stop = false;
  int iter = 0;
  bool succ = true;

  while (!stop) {
    const double directionLength =
      std::sqrt(sqsumLegacyLike(std::span<const double>(updateDirectionScratch.data(), static_cast<size_t>(nvar))));
    if (directionLength < lsw.minDirection) {
      succ = false;
    } else {
      succ = lineSearchVarBacktrackLegacyLike(
        vs,
        paramArray,
        *perceptor.s,
        (perceptor.weight != nullptr) ? std::span<const double>(perceptor.weight, static_cast<size_t>(nvar))
                                      : std::span<const double>{},
        std::span<double>(updateDirectionScratch.data(), updateDirectionScratch.size()),
        lsw);
    }

    if (!succ) {
      const double startGradLength =
        std::sqrt(sqsumLegacyLike(std::span<const double>(lsw.startGrad.data(), static_cast<size_t>(nvar))));
      if (startGradLength > perceptor.minGradient) {
        std::copy(lsw.startGrad.begin(), lsw.startGrad.end(), updateDirectionScratch.begin());
        succ = lineSearchVarBacktrackLegacyLike(
          vs,
          paramArray,
          *perceptor.s,
          (perceptor.weight != nullptr) ? std::span<const double>(perceptor.weight, static_cast<size_t>(nvar))
                                        : std::span<const double>{},
          std::span<double>(updateDirectionScratch.data(), updateDirectionScratch.size()),
          lsw);
      }
    }

    if (succ) {
      ++iter;
      if (iter >= FitPerceptorMaxIterLegacyLike) {
        stop = true;
      } else {
        perceptorGradientLegacyLike(perceptor, stack, lsw.grad.data());
        conjugateUpdateDirectionLegacyLike(
          lsw.grad,
          lsw.startGrad,
          std::span<double>(updateDirectionScratch.data(), updateDirectionScratch.size()));
        std::copy(lsw.grad.begin(), lsw.grad.end(), lsw.startGrad.begin());
      }
    } else {
      stop = true;
    }
  }

  return lsw.score;
}

} // namespace nim
