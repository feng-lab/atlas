#include "zneutubeperceptor.h"

#include "zneutubeoptimizeutils.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nim::neutube {

namespace {

constexpr int FitPerceptorMaxIterLegacyLike = 500;

[[nodiscard]] double sqsumLegacyLike(const double* a, int n)
{
  CHECK(a != nullptr);
  CHECK(n >= 0);

  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    s += a[i] * a[i];
  }
  return s;
}

inline void updateVariableLegacyLike(VariableSet* vs, int index, double delta)
{
  CHECK(vs != nullptr);
  CHECK(vs->var != nullptr);
  CHECK(vs->varIndex != nullptr);
  CHECK(index >= 0);
  CHECK(index < vs->nvar);

  vs->var[vs->varIndex[index]] += delta;
  variableSetUpdateLinkLegacyLike(vs);
}

double perceptorGradientPartialLegacyLike(VariableSet* vs,
                                          int index,
                                          const void* stack,
                                          double delta,
                                          double score,
                                          const void* arg,
                                          ScoreFunctionLegacyLike f)
{
  CHECK(vs != nullptr);
  CHECK(f != nullptr);

  updateVariableLegacyLike(vs, index, delta);

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = arg;

  const double rightScore = f(vs->var, paramArray);

  // restore param
  updateVariableLegacyLike(vs, index, -delta);

  double grad = (rightScore - score) / delta;
  if (grad < 0.0) {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs->var, paramArray);
    if (leftScore < score) {
      grad = 0.0;
    } else {
      grad = (score - leftScore) / delta;
    }
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  } else if (grad > 0.0) {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs->var, paramArray);
    if (leftScore > score) {
      grad = (score - leftScore) / delta;
    }
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  } else {
    updateVariableLegacyLike(vs, index, -delta);
    const double leftScore = f(vs->var, paramArray);
    grad = (score - leftScore) / delta;
    // restore param
    updateVariableLegacyLike(vs, index, delta);
  }

  return grad;
}

} // namespace

void perceptorGradientLegacyLike(const Perceptor* perceptor, const void* stack, double* gradient)
{
  CHECK(perceptor != nullptr);
  CHECK(perceptor->vs != nullptr);
  CHECK(perceptor->s != nullptr);
  CHECK(perceptor->s->f != nullptr);
  CHECK(perceptor->delta != nullptr);
  CHECK(gradient != nullptr);

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = perceptor->arg;

  const double score = perceptor->s->f(perceptor->vs->var, paramArray);

  for (int i = 0; i < perceptor->vs->nvar; ++i) {
    const int varIndex = perceptor->vs->varIndex[i];
    gradient[i] = perceptorGradientPartialLegacyLike(perceptor->vs,
                                                     i,
                                                     stack,
                                                     perceptor->delta[varIndex],
                                                     score,
                                                     perceptor->arg,
                                                     perceptor->s->f);
  }
}

double fitPerceptorLegacyLike(Perceptor* perceptor, const void* stack)
{
  CHECK(perceptor != nullptr);
  CHECK(perceptor->vs != nullptr);
  CHECK(perceptor->vs->nvar >= 0);
  CHECK(perceptor->s != nullptr);
  CHECK(perceptor->s->f != nullptr);

  const int nvar = perceptor->vs->nvar;
  LineSearchWorkspace lsw(nvar);
  setLineSearchWorkspaceLegacyLike(&lsw, 0.2, 0.8, 0.01, 0.1, perceptor->minGradient);

  std::vector<double> updateDirection(static_cast<size_t>(nvar));

  perceptorGradientLegacyLike(perceptor, stack, lsw.startGrad.data());

  const void* paramArray[2];
  paramArray[0] = stack;
  paramArray[1] = perceptor->arg;
  lsw.score = perceptor->s->f(perceptor->vs->var, paramArray);

  std::copy(lsw.startGrad.begin(), lsw.startGrad.end(), updateDirection.begin());

  bool stop = false;
  int iter = 0;
  bool succ = true;

  while (!stop) {
    const double directionLength = std::sqrt(sqsumLegacyLike(updateDirection.data(), nvar));
    if (directionLength < lsw.minDirection) {
      succ = false;
    } else {
      succ = lineSearchVarBacktrackLegacyLike(perceptor->vs,
                                              paramArray,
                                              perceptor->s,
                                              perceptor->delta,
                                              perceptor->weight,
                                              updateDirection.data(),
                                              &lsw);
    }

    if (!succ) {
      const double startGradLength = std::sqrt(sqsumLegacyLike(lsw.startGrad.data(), nvar));
      if (startGradLength > perceptor->minGradient) {
        std::copy(lsw.startGrad.begin(), lsw.startGrad.end(), updateDirection.begin());
        succ = lineSearchVarBacktrackLegacyLike(perceptor->vs,
                                                paramArray,
                                                perceptor->s,
                                                perceptor->delta,
                                                perceptor->weight,
                                                updateDirection.data(),
                                                &lsw);
      }
    }

    if (succ) {
      ++iter;
      if (iter >= FitPerceptorMaxIterLegacyLike) {
        stop = true;
      } else {
        perceptorGradientLegacyLike(perceptor, stack, lsw.grad.data());
        conjugateUpdateDirectionLegacyLike(nvar, lsw.grad.data(), lsw.startGrad.data(), updateDirection.data());
        std::copy(lsw.grad.begin(), lsw.grad.end(), lsw.startGrad.begin());
      }
    } else {
      stop = true;
    }
  }

  return lsw.score;
}

} // namespace nim::neutube
