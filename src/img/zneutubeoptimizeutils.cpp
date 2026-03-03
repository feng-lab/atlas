#include "zneutubeoptimizeutils.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace nim {

namespace {

constexpr double StopGradientLegacyLike = 1e-1;

[[nodiscard]] double dotLegacyLike(std::span<const double> a, std::span<const double> b)
{
  CHECK(a.size() == b.size());

  double d = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    d += a[i] * b[i];
  }
  return d;
}

[[nodiscard]] double sqsumLegacyLike(std::span<const double> a)
{
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
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
                                      std::span<const double> weight,
                                      std::span<double> direction,
                                      LineSearchWorkspace& lsw)
{
  CHECK(vs.nvar >= 0);
  CHECK(direction.size() == static_cast<size_t>(vs.nvar));
  CHECK(weight.empty() || weight.size() == direction.size());
  CHECK(cf.f != nullptr);
  CHECK(cf.v != nullptr);
  CHECK(cf.varMin != nullptr);
  CHECK(cf.varMax != nullptr);

  if (!weight.empty()) {
    for (int i = 0; i < vs.nvar; ++i) {
      direction[i] *= weight[i];
    }
  }

  const double directionLength = std::sqrt(sqsumLegacyLike(direction));

  bool improved = true;
  if (directionLength > lsw.minDirection) {
    static thread_local std::vector<double> orgVarScratch;
    orgVarScratch.resize(static_cast<size_t>(vs.nvar));
    for (int i = 0; i < vs.nvar; ++i) {
      orgVarScratch[static_cast<size_t>(i)] = vs.var[vs.varIndex[i]];
    }

    const double startScore = lsw.score;
    double alpha = lsw.alpha / directionLength;

    const double gdDot = dotLegacyLike(lsw.startGrad, direction);
    const double gdDotC1 = gdDot * lsw.c1;

    double wolfe1 = 0.0;
    do {
      for (int i = 0; i < vs.nvar; ++i) {
        vs.var[vs.varIndex[i]] = alpha * direction[i];
        vs.var[vs.varIndex[i]] += orgVarScratch[static_cast<size_t>(i)];
      }

      cf.v(vs.var, cf.varMin, cf.varMax, nullptr);
      variableSetUpdateLinkLegacyLike(vs);
      lsw.score = cf.f(vs.var, param);

      alpha *= lsw.ro;
      if (alpha * directionLength < StopGradientLegacyLike) {
        for (int i = 0; i < vs.nvar; ++i) {
          vs.var[vs.varIndex[i]] = orgVarScratch[static_cast<size_t>(i)];
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

void conjugateUpdateDirectionLegacyLike(std::span<const double> grad,
                                        std::span<const double> prevGrad,
                                        std::span<double> direction)
{
  CHECK(grad.size() == prevGrad.size());
  CHECK(grad.size() == direction.size());

  static thread_local std::vector<double> dgScratch;
  dgScratch.resize(grad.size());
  for (size_t i = 0; i < grad.size(); ++i) {
    dgScratch[i] = grad[i] - prevGrad[i];
  }

  double beta = dotLegacyLike(grad, dgScratch) / dotLegacyLike(prevGrad, prevGrad);
  if (beta < 0.0) {
    beta = 0.0;
  }

  for (size_t i = 0; i < grad.size(); ++i) {
    direction[i] *= beta;
    direction[i] += grad[i];
  }
}

} // namespace nim
