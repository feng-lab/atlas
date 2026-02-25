#include "zneutubeoptimizeutils.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim::neutube {

namespace {

constexpr double StopGradientLegacyLike = 1e-1;

[[nodiscard]] double dotLegacyLike(const double* a, const double* b, int n)
{
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  CHECK(n >= 0);

  double d = 0.0;
  for (int i = 0; i < n; ++i) {
    d += a[i] * b[i];
  }
  return d;
}

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

} // namespace

void setLineSearchWorkspaceLegacyLike(LineSearchWorkspace& lsw,
                                      double alpha,
                                      double ro,
                                      double c1,
                                      double c2,
                                      double minDirection)
{
  lsw.alpha = alpha;
  lsw.ro = ro;
  lsw.c1 = c1;
  lsw.c2 = c2;
  lsw.minDirection = minDirection;
}

bool lineSearchVarBacktrackLegacyLike(VariableSet& vs,
                                      const void* param,
                                      const ContinuousFunction& cf,
                                      const double* /*delta*/,
                                      const double* weight,
                                      double* direction,
                                      LineSearchWorkspace& lsw)
{
  CHECK(direction != nullptr);
  CHECK(cf.f != nullptr);
  CHECK(cf.v != nullptr);
  CHECK(cf.varMin != nullptr);
  CHECK(cf.varMax != nullptr);

  if (weight != nullptr) {
    for (int i = 0; i < vs.nvar; ++i) {
      direction[i] *= weight[i];
    }
  }

  const double directionLength = std::sqrt(sqsumLegacyLike(direction, vs.nvar));

  bool improved = true;
  if (directionLength > lsw.minDirection) {
    std::vector<double> orgVar(static_cast<size_t>(vs.nvar));
    for (int i = 0; i < vs.nvar; ++i) {
      orgVar[static_cast<size_t>(i)] = vs.var[vs.varIndex[i]];
    }

    const double startScore = lsw.score;
    double alpha = lsw.alpha / directionLength;

    const double gdDot = dotLegacyLike(lsw.startGrad.data(), direction, vs.nvar);
    const double gdDotC1 = gdDot * lsw.c1;

    double wolfe1 = 0.0;
    do {
      for (int i = 0; i < vs.nvar; ++i) {
        vs.var[vs.varIndex[i]] = alpha * direction[i];
        vs.var[vs.varIndex[i]] += orgVar[static_cast<size_t>(i)];
      }

      cf.v(vs.var, cf.varMin, cf.varMax, nullptr);
      variableSetUpdateLinkLegacyLike(vs);
      lsw.score = cf.f(vs.var, param);

      alpha *= lsw.ro;
      if (alpha * directionLength < StopGradientLegacyLike) {
        for (int i = 0; i < vs.nvar; ++i) {
          vs.var[vs.varIndex[i]] = orgVar[static_cast<size_t>(i)];
        }
        variableSetUpdateLinkLegacyLike(vs);
        lsw.score = startScore;
        improved = false;
        break;
      }

      wolfe1 = std::max(alpha / lsw.ro * gdDotC1, 0.0);
    } while (lsw.score < startScore + wolfe1);
  } else {
    improved = false;
  }

  return improved;
}

void conjugateUpdateDirectionLegacyLike(int nvar, const double* grad, const double* prevGrad, double* direction)
{
  CHECK(grad != nullptr);
  CHECK(prevGrad != nullptr);
  CHECK(direction != nullptr);
  CHECK(nvar >= 0);

  std::vector<double> dg(static_cast<size_t>(nvar));
  for (int i = 0; i < nvar; ++i) {
    dg[static_cast<size_t>(i)] = grad[i] - prevGrad[i];
  }

  double beta = dotLegacyLike(grad, dg.data(), nvar) / dotLegacyLike(prevGrad, prevGrad, nvar);
  if (beta < 0.0) {
    beta = 0.0;
  }

  for (int i = 0; i < nvar; ++i) {
    direction[i] *= beta;
    direction[i] += grad[i];
  }
}

} // namespace nim::neutube
