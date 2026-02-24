#include "zneutubelocalneuroseg.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dscalarfield.h"
#include "zneutubeperceptor.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace nim::neutube {

namespace {

constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
constexpr double Tz2PiLegacyLike = 2.0 * TzPiLegacyLike;
constexpr double NeurosegMinRLegacyLike = 0.5;

// Matches `src/neurolabi/c/private/tzp_local_neuroseg.c` (included into
// `tz_local_neuroseg.c`) which defines the finite-difference steps used by the
// legacy optimizer for local-neuroseg fitting.
constexpr std::array<double, 12> LocalNeurosegDeltaLegacyLike = {
  0.1, // r1
  0.1, // c
  0.015, // theta
  0.015, // psi
  1.0, // h
  0.05, // curvature
  0.015, // alpha
  0.05, // scale
  0.5, // x
  0.5, // y
  0.5, // z
  0.1, // z_scale
};

constexpr std::array<double, 12> LocalNeurosegVarMinLegacyLike = {
  0.5,
  -4.0,
  -std::numeric_limits<double>::infinity(),
  -std::numeric_limits<double>::infinity(),
  2.0,
  0.0,
  -std::numeric_limits<double>::infinity(),
  0.2,
  -std::numeric_limits<double>::infinity(),
  -std::numeric_limits<double>::infinity(),
  -std::numeric_limits<double>::infinity(),
  0.5,
};

constexpr std::array<double, 12> LocalNeurosegVarMaxLegacyLike = {
  50.0,
  4.0,
  std::numeric_limits<double>::infinity(),
  std::numeric_limits<double>::infinity(),
  30.0,
  TzPiLegacyLike, // NEUROSEG_MAX_CURVATURE
  std::numeric_limits<double>::infinity(),
  20.0,
  std::numeric_limits<double>::infinity(),
  std::numeric_limits<double>::infinity(),
  std::numeric_limits<double>::infinity(),
  6.0,
};

constexpr std::uint32_t NeurosegVarMaskR1LegacyLike = 0x00000001u;
constexpr std::uint32_t NeurosegVarMaskOrientationLegacyLike = 0x0000000Cu;
constexpr std::uint32_t NeurosegVarMaskScaleLegacyLike = 0x00000080u;
constexpr std::uint32_t NeurosegVarMaskRLegacyLike = NeurosegVarMaskR1LegacyLike;
constexpr std::uint32_t NeuroposVarMaskNoneLegacyLike = 0x00000000u;

[[nodiscard]] std::array<double, 3> neurosegAxisOffsetLegacyLike(const Neuroseg& seg, double axisOffset)
{
  std::array<double, 3> out = {0.0, 0.0, axisOffset};
  rotateXZLegacyLike(&out, 1, seg.theta, seg.psi, 0);
  return out;
}

[[nodiscard]] double normalizeRadianLegacyLike(double r)
{
  double normR = r;
  if (r < 0.0 || r >= Tz2PiLegacyLike) {
    normR = r - std::floor(r / Tz2PiLegacyLike) * Tz2PiLegacyLike;
  }

  return normR;
}

[[nodiscard]] int bitmaskToIndexLegacyLike(std::uint32_t mask, int nparam, int* indices)
{
  CHECK(indices != nullptr);
  CHECK(nparam >= 0);

  int n = 0;
  for (int i = 0; i < nparam; ++i) {
    if (mask & (1u << static_cast<std::uint32_t>(i))) {
      indices[n] = i;
      ++n;
    }
  }

  return n;
}

[[nodiscard]] int
localNeurosegVarMaskToIndexLegacyLike(std::uint32_t neurosegMask, std::uint32_t neuroposMask, int* indices)
{
  CHECK(indices != nullptr);

  constexpr int neurosegNParam = 8;
  constexpr int neuroposNParam = 3;

  const int n1 = bitmaskToIndexLegacyLike(neurosegMask, neurosegNParam, indices);
  const int n2 = bitmaskToIndexLegacyLike(neuroposMask, neuroposNParam, indices + n1);

  for (int i = 0; i < n2; ++i) {
    indices[n1 + i] += neurosegNParam;
  }

  return n1 + n2;
}

void localNeurosegSetVarLegacyLike(LocalNeuroseg* locseg, int varIndex, double value)
{
  CHECK(locseg != nullptr);

  switch (varIndex) {
    case 0:
      locseg->seg.r1 = value;
      return;
    case 1:
      locseg->seg.c = value;
      return;
    case 2:
      locseg->seg.theta = value;
      return;
    case 3:
      locseg->seg.psi = value;
      return;
    case 4:
      locseg->seg.h = value;
      return;
    case 5:
      locseg->seg.curvature = value;
      return;
    case 6:
      locseg->seg.alpha = value;
      return;
    case 7:
      locseg->seg.scale = value;
      return;
    case 8:
      locseg->pos[0] = value;
      return;
    case 9:
      locseg->pos[1] = value;
      return;
    case 10:
      locseg->pos[2] = value;
      return;
    default:
      CHECK(false) << "Unsupported LocalNeuroseg var index: " << varIndex;
      return;
  }
}

void localNeurosegParamArrayLegacyLike(const LocalNeuroseg& locseg, double zScale, double* param)
{
  CHECK(param != nullptr);

  param[0] = locseg.seg.r1;
  param[1] = locseg.seg.c;
  param[2] = locseg.seg.theta;
  param[3] = locseg.seg.psi;
  param[4] = locseg.seg.h;
  param[5] = locseg.seg.curvature;
  param[6] = locseg.seg.alpha;
  param[7] = locseg.seg.scale;
  param[8] = locseg.pos[0];
  param[9] = locseg.pos[1];
  param[10] = locseg.pos[2];
  param[11] = zScale;
}

void localNeurosegValidateLegacyLike(double* var, const double* varMin, const double* varMax, const void* /*param*/)
{
  CHECK(var != nullptr);
  CHECK(varMin != nullptr);
  CHECK(varMax != nullptr);

  for (int i = 0; i < LocalNeurosegNParamLegacyLike; ++i) {
    if (var[i] < varMin[i]) {
      var[i] = varMin[i];
    } else if (var[i] > varMax[i]) {
      var[i] = varMax[i];
    }
  }

  // Note: this is coupled with Neuroseg_Var.
  if (var[1] < 0.0) {
    var[1] = std::max(var[1], (NeurosegMinRLegacyLike - var[0]) / var[4]);
  }
}

double localNeurosegScoreRLegacyLike(const double* var, const void* param)
{
  CHECK(var != nullptr);
  CHECK(param != nullptr);

  const auto* paramArray = static_cast<const void* const*>(param);
  const auto* stack = static_cast<const ZImg*>(paramArray[0]);
  auto* ws = static_cast<LocsegScoreWorkspace*>(const_cast<void*>(paramArray[1]));

  CHECK(stack != nullptr);
  CHECK(ws != nullptr);

  LocalNeuroseg locseg;
  for (int i = 0; i < LocalNeurosegNParamLegacyLike; ++i) {
    localNeurosegSetVarLegacyLike(&locseg, i, var[i]);
  }

  const double zScale = var[LocalNeurosegNParamLegacyLike];
  return localNeurosegScoreWLegacyLike(locseg, *stack, zScale, ws, 0, 0);
}

} // namespace

void setNeurosegPositionLegacyLike(LocalNeuroseg* locseg,
                                   const std::array<double, 3>& pos,
                                   NeuroposReferenceLegacyLike ref)
{
  CHECK(locseg != nullptr);

  locseg->pos = pos;
  if (ref == NeuroposReference) {
    return;
  }

  double axisOffset = 0.0;
  switch (NeuroposReference) {
    case NeuroposReferenceLegacyLike::Bottom: {
      if (ref == NeuroposReferenceLegacyLike::Top) {
        axisOffset = -locseg->seg.h + 1.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Center) {
        axisOffset = -(locseg->seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Top: {
      if (ref == NeuroposReferenceLegacyLike::Bottom) {
        axisOffset = locseg->seg.h - 1.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Center) {
        axisOffset = (locseg->seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Center: {
      if (ref == NeuroposReferenceLegacyLike::Bottom) {
        axisOffset = (locseg->seg.h - 1.0) / 2.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Top) {
        axisOffset = -(locseg->seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Undef:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike::Undef";
      break;
  }

  const std::array<double, 3> apos = neurosegAxisOffsetLegacyLike(locseg->seg, axisOffset);
  locseg->pos[0] += apos[0];
  locseg->pos[1] += apos[1];
  locseg->pos[2] += apos[2];
}

std::array<double, 3> localNeurosegBottomLegacyLike(const LocalNeuroseg& locseg)
{
  switch (NeuroposReference) {
    case NeuroposReferenceLegacyLike::Bottom:
      return locseg.pos;
    case NeuroposReferenceLegacyLike::Top: {
      const std::array<double, 3> offset = neurosegAxisOffsetLegacyLike(locseg.seg, -locseg.seg.h + 1.0);
      return {locseg.pos[0] + offset[0], locseg.pos[1] + offset[1], locseg.pos[2] + offset[2]};
    }
    case NeuroposReferenceLegacyLike::Center: {
      const std::array<double, 3> offset = neurosegAxisOffsetLegacyLike(locseg.seg, (-locseg.seg.h + 1.0) / 2.0);
      return {locseg.pos[0] + offset[0], locseg.pos[1] + offset[1], locseg.pos[2] + offset[2]};
    }
    case NeuroposReferenceLegacyLike::Undef:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike::Undef";
      return {};
  }
}

std::array<double, 3> localNeurosegAxisPositionLegacyLike(const LocalNeuroseg& locseg, double axisOffset)
{
  const std::array<double, 3> apos = neurosegAxisOffsetLegacyLike(locseg.seg, axisOffset);
  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);
  return {bottom[0] + apos[0], bottom[1] + apos[1], bottom[2] + apos[2]};
}

std::array<double, 3> localNeurosegCenterLegacyLike(const LocalNeuroseg& locseg)
{
  if (NeuroposReference == NeuroposReferenceLegacyLike::Center) {
    return locseg.pos;
  }
  return localNeurosegAxisPositionLegacyLike(locseg, (locseg.seg.h - 1.0) / 2.0);
}

std::array<double, 3> localNeurosegTopLegacyLike(const LocalNeuroseg& locseg)
{
  if (NeuroposReference == NeuroposReferenceLegacyLike::Top) {
    return locseg.pos;
  }
  return localNeurosegAxisPositionLegacyLike(locseg, locseg.seg.h - 1.0);
}

Geo3dScalarField localNeurosegFieldSLegacyLike(const LocalNeuroseg& locseg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field = neurosegFieldSFastLegacyLike(locseg.seg, fieldFunc);
  if (field.points.empty()) {
    return field;
  }

  std::array<double, 3> pos0{};
  std::array<double, 3> pos1{};

  switch (NeuroposReference) {
    case NeuroposReferenceLegacyLike::Bottom:
      pos0 = {0.0, 0.0, 0.0};
      pos1 = localNeurosegBottomLegacyLike(locseg);
      break;
    case NeuroposReferenceLegacyLike::Center:
      pos0 = neurosegAxisOffsetLegacyLike(locseg.seg, (locseg.seg.h - 1.0) / 2.0);
      pos1 = localNeurosegCenterLegacyLike(locseg);
      break;
    case NeuroposReferenceLegacyLike::Top:
      pos0 = neurosegAxisOffsetLegacyLike(locseg.seg, locseg.seg.h - 1.0);
      pos1 = localNeurosegTopLegacyLike(locseg);
      break;
    case NeuroposReferenceLegacyLike::Undef:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike::Undef";
      break;
  }

  const double dx = pos1[0] - pos0[0];
  const double dy = pos1[1] - pos0[1];
  const double dz = pos1[2] - pos0[2];

  for (auto& p : field.points) {
    p[0] += dx;
    p[1] += dy;
    p[2] += dz;
  }

  return field;
}

double localNeurosegScoreWLegacyLike(const LocalNeuroseg& locseg,
                                     const ZImg& stack,
                                     double zScale,
                                     LocsegScoreWorkspace* ws,
                                     size_t c,
                                     size_t t)
{
  CHECK(ws != nullptr);

  Geo3dScalarField field = localNeurosegFieldSLegacyLike(locseg, ws->fieldFunc);
  if (ws->mask == nullptr) {
    return geo3dScalarFieldStackScoreLegacyLike(field, stack, zScale, &ws->fs, c, t);
  }

  return geo3dScalarFieldStackScoreMaskedLegacyLike(field, stack, zScale, *ws->mask, &ws->fs, c, t);
}

void defaultLocsegFitWorkspaceLegacyLike(LocsegFitWorkspace* ws)
{
  CHECK(ws != nullptr);

  ws->nvar = localNeurosegVarMaskToIndexLegacyLike(NeurosegVarMaskRLegacyLike | NeurosegVarMaskOrientationLegacyLike |
                                                     NeurosegVarMaskScaleLegacyLike,
                                                   NeuroposVarMaskNoneLegacyLike,
                                                   ws->varIndex.data());

  ws->varLink = nullptr;

  ws->varMin.fill(0.0);
  ws->varMax.fill(0.0);
  std::copy(LocalNeurosegVarMinLegacyLike.begin(), LocalNeurosegVarMinLegacyLike.end(), ws->varMin.begin());
  std::copy(LocalNeurosegVarMaxLegacyLike.begin(), LocalNeurosegVarMaxLegacyLike.end(), ws->varMax.begin());

  ws->sws.fs.n = 1;
  ws->sws.fs.options[0] = 0; // STACK_FIT_DOT
  ws->sws.fieldFunc = nullptr;
  ws->sws.mask = nullptr;

  ws->posAdjust = 1;
}

double fitLocalNeurosegWLegacyLike(LocalNeuroseg* locseg,
                                   const ZImg& stack,
                                   double zScale,
                                   LocsegFitWorkspace* ws,
                                   size_t c,
                                   size_t t)
{
  CHECK(locseg != nullptr);
  CHECK(ws != nullptr);

  if (ws->nvar == 0) {
    return localNeurosegScoreWLegacyLike(*locseg, stack, zScale, &ws->sws, c, t);
  }

  std::array<double, LocalNeurosegParamArraySizeLegacyLike> var{};
  localNeurosegParamArrayLegacyLike(*locseg, zScale, var.data());

  std::vector<double> weight(static_cast<size_t>(ws->nvar), 0.0);
  for (int i = 0; i < ws->nvar; ++i) {
    const int varIndex = ws->varIndex[static_cast<size_t>(i)];
    CHECK(varIndex >= 0);
    CHECK(static_cast<size_t>(varIndex) < LocalNeurosegDeltaLegacyLike.size())
      << "LocalNeuroseg delta array is only defined for indices [0, " << (LocalNeurosegDeltaLegacyLike.size() - 1)
      << "], got varIndex=" << varIndex;
    weight[static_cast<size_t>(i)] = LocalNeurosegDeltaLegacyLike[static_cast<size_t>(varIndex)];
  }

  double wl = 0.0;
  for (double w : weight) {
    wl += w * w;
  }
  wl = std::sqrt(wl);
  CHECK(wl > 0.0);

  for (double& w : weight) {
    w /= wl;
  }

  VariableSet vs;
  vs.var = var.data();
  vs.varIndex = ws->varIndex.data();
  vs.link = ws->varLink;
  vs.nvar = ws->nvar;

  ContinuousFunction cf;
  cf.f = localNeurosegScoreRLegacyLike;
  cf.v = localNeurosegValidateLegacyLike;
  cf.varMin = ws->varMin.data();
  cf.varMax = ws->varMax.data();

  Perceptor perceptor;
  perceptor.vs = &vs;
  perceptor.arg = &ws->sws;
  perceptor.s = &cf;
  perceptor.minGradient = 1e-3;
  perceptor.delta = LocalNeurosegDeltaLegacyLike.data();
  perceptor.weight = weight.data();

  fitPerceptorLegacyLike(&perceptor, &stack);

  for (int i = 0; i < LocalNeurosegNParamLegacyLike; ++i) {
    localNeurosegSetVarLegacyLike(locseg, i, var[static_cast<size_t>(i)]);
  }

  locseg->seg.theta = normalizeRadianLegacyLike(locseg->seg.theta);
  locseg->seg.psi = normalizeRadianLegacyLike(locseg->seg.psi);

  return localNeurosegScoreWLegacyLike(*locseg, stack, zScale, &ws->sws, c, t);
}

} // namespace nim::neutube
