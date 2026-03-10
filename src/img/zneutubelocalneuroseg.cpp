#include "zneutubelocalneuroseg.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dpointarray.h"
#include "zneutubegeo3dscalarfield.h"
#include "zneutubegeo3dutils.h"
#include "zneutubeimgsampling.h"
#include "zneutubemathutils.h"
#include "zneutubeperceptor.h"
#include "zswcgeom.h"
#include "zneutubestackfitoptions.h"

#include "zlog.h"
#include "zimg.h"
#include "zvoxelvolume.h"
#include "zvoxelvolumedense.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace nim {

namespace {

constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
constexpr double Tz2PiLegacyLike = 2.0 * TzPiLegacyLike;

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
  rotateXZLegacyLike(out, seg.theta, seg.psi, 0);
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

[[nodiscard]] int bitmaskToIndexLegacyLike(std::uint32_t mask, int nparam, std::span<int> indices)
{
  CHECK(nparam >= 0);
  CHECK(indices.size() >= static_cast<size_t>(nparam));

  int n = 0;
  for (int i = 0; i < nparam; ++i) {
    if (mask & (1u << static_cast<std::uint32_t>(i))) {
      indices[static_cast<size_t>(n)] = i;
      ++n;
    }
  }

  return n;
}

[[nodiscard]] int
localNeurosegVarMaskToIndexLegacyLike(std::uint32_t neurosegMask, std::uint32_t neuroposMask, std::span<int> indices)
{
  constexpr int neurosegNParam = 8;
  constexpr int neuroposNParam = 3;

  const int n1 = bitmaskToIndexLegacyLike(neurosegMask, neurosegNParam, indices);
  const int n2 = bitmaskToIndexLegacyLike(neuroposMask, neuroposNParam, indices.subspan(static_cast<size_t>(n1)));

  for (int i = 0; i < n2; ++i) {
    indices[static_cast<size_t>(n1 + i)] += neurosegNParam;
  }

  return n1 + n2;
}

void localNeurosegSetVarLegacyLike(LocalNeuroseg& locseg, int varIndex, double value)
{
  switch (varIndex) {
    case 0:
      locseg.seg.r1 = value;
      return;
    case 1:
      locseg.seg.c = value;
      return;
    case 2:
      locseg.seg.theta = value;
      return;
    case 3:
      locseg.seg.psi = value;
      return;
    case 4:
      locseg.seg.h = value;
      return;
    case 5:
      locseg.seg.curvature = value;
      return;
    case 6:
      locseg.seg.alpha = value;
      return;
    case 7:
      locseg.seg.scale = value;
      return;
    case 8:
      locseg.pos[0] = value;
      return;
    case 9:
      locseg.pos[1] = value;
      return;
    case 10:
      locseg.pos[2] = value;
      return;
    default:
      CHECK(false) << "Unsupported LocalNeuroseg var index: " << varIndex;
      return;
  }
}

void localNeurosegParamArrayLegacyLike(const LocalNeuroseg& locseg, double zToXYRatio, std::span<double> param)
{
  CHECK(param.size() >= static_cast<size_t>(LocalNeurosegParamArraySizeLegacyLike));

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
  param[11] = zToXYRatio;
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
  const auto* paramArray = static_cast<const void* const*>(param);
  const auto* stack = static_cast<const ZVoxelVolume*>(paramArray[0]);
  auto* ws = static_cast<LocsegScoreWorkspace*>(const_cast<void*>(paramArray[1]));

  LocalNeuroseg locseg;
  locseg.seg.r1 = var[0];
  locseg.seg.c = var[1];
  locseg.seg.theta = var[2];
  locseg.seg.psi = var[3];
  locseg.seg.h = var[4];
  locseg.seg.curvature = var[5];
  locseg.seg.alpha = var[6];
  locseg.seg.scale = var[7];
  locseg.pos[0] = var[8];
  locseg.pos[1] = var[9];
  locseg.pos[2] = var[10];

  const double zToXYRatio = var[LocalNeurosegNParamLegacyLike];
  return localNeurosegScoreWLegacyLike(locseg, *stack, zToXYRatio, *ws);
}

double localNeurosegScoreRLegacyLikeZImg(const double* var, const void* param)
{
  const auto* paramArray = static_cast<const void* const*>(param);
  const auto* stack = static_cast<const ZImg*>(paramArray[0]);
  auto* ws = static_cast<LocsegScoreWorkspace*>(const_cast<void*>(paramArray[1]));

  LocalNeuroseg locseg;
  locseg.seg.r1 = var[0];
  locseg.seg.c = var[1];
  locseg.seg.theta = var[2];
  locseg.seg.psi = var[3];
  locseg.seg.h = var[4];
  locseg.seg.curvature = var[5];
  locseg.seg.alpha = var[6];
  locseg.seg.scale = var[7];
  locseg.pos[0] = var[8];
  locseg.pos[1] = var[9];
  locseg.pos[2] = var[10];

  const double zToXYRatio = var[LocalNeurosegNParamLegacyLike];
  return localNeurosegScoreWLegacyLike(locseg, *stack, zToXYRatio, *ws);
}

[[nodiscard]] int compareFloatLegacyLike(double a, double b, double eps)
{
  if (a < b - eps) {
    return -1;
  }
  if (a > b + eps) {
    return 1;
  }
  return 0;
}

[[nodiscard]] int testZToXYRatioLegacyLike(double zToXYRatio)
{
  return compareFloatLegacyLike(zToXYRatio, 1.0, 1e-5);
}

[[nodiscard]] double distancePointEllipseSpecialLegacyLike(double u,
                                                           double v,
                                                           double a,
                                                           double b,
                                                           double epsilon,
                                                           int maxIter,
                                                           double* outX,
                                                           double* outY)
{
  // Port of tz_geometry.c::DistancePointEllipseSpecial() (David Eberly).
  double t = b * (v - b);

  for (int i = 0; i < maxIter; ++i) {
    const double tpASqr = t + a * a;
    const double tpBSqr = t + b * b;
    const double invTpASqr = 1.0 / tpASqr;
    const double invTpBSqr = 1.0 / tpBSqr;

    const double xDivA = a * u * invTpASqr;
    const double yDivB = b * v * invTpBSqr;
    const double xDivASqr = xDivA * xDivA;
    const double yDivBSqr = yDivB * yDivB;

    const double f = xDivASqr + yDivBSqr - 1.0;
    if (f < epsilon) {
      *outX = xDivA * a;
      *outY = yDivB * b;
      break;
    }

    const double fDer = 2.0 * (xDivASqr * invTpASqr + yDivBSqr * invTpBSqr);
    const double ratio = f / fDer;
    if (ratio < epsilon) {
      *outX = xDivA * a;
      *outY = yDivB * b;
      break;
    }

    t += ratio;
  }

  const double dx = *outX - u;
  const double dy = *outY - v;
  return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] double ellipsePointDistanceLegacyLike(double u, double v, double a, double b, double* outX, double* outY)
{
  // Port of tz_geometry.c::Ellipse_Point_Distance().
  constexpr double epsilon = 1e-5;
  constexpr int maxIter = 100;

  double tmpX = 0.0;
  double tmpY = 0.0;
  if (outX == nullptr) {
    outX = &tmpX;
  }
  if (outY == nullptr) {
    outY = &tmpY;
  }

  if (std::fabs(a - b) < epsilon) {
    const double length = std::sqrt(u * u + v * v);
    *outX = a / length * u;
    *outY = a / length * v;
    return std::fabs(length - a);
  }

  bool xReflect = false;
  if (u > epsilon) {
    xReflect = false;
  } else if (u < -epsilon) {
    xReflect = true;
    u = -u;
  } else {
    xReflect = false;
    u = 0.0;
  }

  bool yReflect = false;
  if (v > epsilon) {
    yReflect = false;
  } else if (v < -epsilon) {
    yReflect = true;
    v = -v;
  } else {
    yReflect = false;
    v = 0.0;
  }

  bool transpose = false;
  if (a < b) {
    transpose = true;
    std::swap(a, b);
    std::swap(u, v);
  }

  double dist = 0.0;
  if (u != 0.0) {
    if (v != 0.0) {
      dist = distancePointEllipseSpecialLegacyLike(u, v, a, b, epsilon, maxIter, outX, outY);
    } else {
      const double bSqr = b * b;
      if (u < a - bSqr / a) {
        const double aSqr = a * a;
        *outX = aSqr * u / (aSqr - bSqr);
        const double xDivA = *outX / a;
        *outY = b * std::sqrt(std::fabs(1.0 - xDivA * xDivA));
        const double dx = *outX - u;
        dist = std::sqrt(dx * dx + (*outY) * (*outY));
      } else {
        dist = std::fabs(u - a);
        *outX = a;
        *outY = 0.0;
      }
    }
  } else {
    dist = std::fabs(v - b);
    *outX = 0.0;
    *outY = b;
  }

  if (transpose) {
    std::swap(*outX, *outY);
  }
  if (yReflect) {
    *outY = -(*outY);
  }
  if (xReflect) {
    *outX = -(*outX);
  }

  return dist;
}

[[nodiscard]] bool pointInEllipseLegacyLike(double x, double y, double a, double b)
{
  x /= a;
  y /= b;
  return (x * x + y * y) <= 1.0;
}

[[nodiscard]] double
localNeurosegPointDistSLegacyLike(const LocalNeuroseg& locseg, double x, double y, double z, std::array<double, 3>* pt)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Point_Dist_S().
  std::array<double, 3> tmpPos = localNeurosegBottomLegacyLike(locseg);
  tmpPos[0] = x - tmpPos[0];
  tmpPos[1] = y - tmpPos[1];
  tmpPos[2] = z - tmpPos[2];

  rotateXZLegacyLike(tmpPos, locseg.seg.theta, locseg.seg.psi, 1);
  rotateZLegacyLike(tmpPos, locseg.seg.alpha, 1);

  double minDist = 0.0;
  const double coef = neurosegCoefLegacyLike(locseg.seg);

  if (!neurosegHitTestLegacyLike(locseg.seg, tmpPos[0], tmpPos[1], tmpPos[2])) {
    double rx = 0.0;
    double ry = locseg.seg.r1;
    rx = ry * locseg.seg.scale;

    double tmpTx = 0.0;
    double tmpTy = 0.0;
    double tmpTz = 0.0;

    if (tmpPos[2] <= 0.0) {
      if (pointInEllipseLegacyLike(tmpPos[0], tmpPos[1], rx, ry)) {
        minDist = -tmpPos[2];
        tmpTx = 0.0;
        tmpTy = 0.0;
        tmpTz = 0.0;
      } else {
        const double d = ellipsePointDistanceLegacyLike(tmpPos[0], tmpPos[1], rx, ry, &tmpTx, &tmpTy);
        minDist = std::sqrt(tmpPos[2] * tmpPos[2] + d * d);
        tmpTz = 0.0;
      }

      if (pt != nullptr) {
        (*pt)[0] = tmpTx;
        (*pt)[1] = tmpTy;
        (*pt)[2] = tmpTz;
      }
    } else if (tmpPos[2] >= locseg.seg.h - 1.0) {
      ry = locseg.seg.r1 + (locseg.seg.h - 1.0) * coef;
      rx = ry * locseg.seg.scale;
      tmpTz = locseg.seg.h - 1.0;

      if (pointInEllipseLegacyLike(tmpPos[0], tmpPos[1], rx, ry)) {
        tmpTx = 0.0;
        tmpTy = 0.0;
        minDist = tmpPos[2] - tmpTz;
        if (pt != nullptr) {
          (*pt)[0] = tmpTx;
          (*pt)[1] = tmpTy;
          (*pt)[2] = tmpTz;
        }
      } else {
        const double d = ellipsePointDistanceLegacyLike(tmpPos[0], tmpPos[1], rx, ry, &tmpTx, &tmpTy);
        const double dz = tmpPos[2] - tmpTz;
        minDist = std::sqrt(dz * dz + d * d);
        if (pt != nullptr) {
          (*pt)[0] = tmpTx;
          (*pt)[1] = tmpTy;
          (*pt)[2] = tmpTz;
        }
      }
    } else {
      if (coef != 0.0) {
        ry += coef * tmpPos[2];
        rx = ry * locseg.seg.scale;
      }

      minDist = ellipsePointDistanceLegacyLike(tmpPos[0], tmpPos[1], rx, ry, &tmpTx, &tmpTy);
      tmpTz = tmpPos[2];
      if (pt != nullptr) {
        (*pt)[0] = tmpTx;
        (*pt)[1] = tmpTy;
        (*pt)[2] = tmpTz;
      }

      if (coef != 0.0) {
        ry = locseg.seg.r1;
        for (double h = 0.5; h < locseg.seg.h - 1.0; h += 0.5) {
          ry += coef * 0.5;
          rx = ry * locseg.seg.scale;
          const double d = ellipsePointDistanceLegacyLike(tmpPos[0], tmpPos[1], rx, ry, &tmpTx, &tmpTy);
          const double dz = tmpPos[2] - h;
          const double dd = std::sqrt(d * d + dz * dz);
          if (minDist > dd) {
            minDist = dd;
            if (pt != nullptr) {
              (*pt)[0] = tmpTx;
              (*pt)[1] = tmpTy;
              (*pt)[2] = h;
            }
          }
        }
      }
    }

    if (pt != nullptr) {
      rotateZLegacyLike(*pt, locseg.seg.alpha, 0);
      rotateXZLegacyLike(*pt, locseg.seg.theta, locseg.seg.psi, 0);
      (*pt)[0] += locseg.pos[0];
      (*pt)[1] += locseg.pos[1];
      (*pt)[2] += locseg.pos[2];
    }
  } else {
    if (pt != nullptr) {
      (*pt)[0] = x;
      (*pt)[1] = y;
      (*pt)[2] = z;
    }
  }

  return minDist;
}

[[nodiscard]] double localNeurosegLineSegDistSLegacyLike(const LocalNeuroseg& locseg,
                                                         const std::array<double, 3>& start,
                                                         const std::array<double, 3>& end,
                                                         std::array<double, 3>* pos)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Lineseg_Dist_S().
  constexpr double res = 1.0;
  double d = geo3dDist(start[0], start[1], start[2], end[0], end[1], end[2]);

  if (d < res) {
    return localNeurosegPointDistSLegacyLike(locseg, start[0], start[1], start[2], pos);
  }

  const std::array<double, 3> step = {(end[0] - start[0]) / d * res,
                                      (end[1] - start[1]) / d * res,
                                      (end[2] - start[2]) / d * res};

  double x = start[0];
  double y = start[1];
  double z = start[2];

  std::array<double, 3> bestPos{};
  double minDist = localNeurosegPointDistSLegacyLike(locseg, x, y, z, pos ? &bestPos : nullptr);
  if (pos != nullptr) {
    *pos = bestPos;
  }

  std::array<double, 3> tmpPos{};

  while ((d > 0.0) && (minDist > 0.0)) {
    d -= res;
    x += step[0];
    y += step[1];
    z += step[2];

    const double dist = localNeurosegPointDistSLegacyLike(locseg, x, y, z, &tmpPos);
    if (dist < minDist) {
      minDist = dist;
      if (pos != nullptr) {
        *pos = tmpPos;
      }
    }
  }

  return minDist;
}

} // namespace

void localNeurosegStackPositionLegacyLike(const std::array<double, 3>& position,
                                          std::array<int, 3>& c,
                                          std::array<double, 3>& offpos,
                                          double zToXYRatio)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Stack_Position() and its
  // helper local_neuroseg_stack_position() in private/tzp_local_neuroseg.c.
  c[0] = iroundLegacyLike(position[0]);
  c[1] = iroundLegacyLike(position[1]);
  offpos[0] = position[0] - static_cast<double>(c[0]);
  offpos[1] = position[1] - static_cast<double>(c[1]);

  if (testZToXYRatioLegacyLike(zToXYRatio) != 0) {
    c[2] = iroundLegacyLike(position[2] * (1.0 / zToXYRatio));
    offpos[2] = position[2] * (1.0 / zToXYRatio) - static_cast<double>(c[2]);
  } else {
    c[2] = iroundLegacyLike(position[2]);
    offpos[2] = position[2] - static_cast<double>(c[2]);
  }
}

void setNeurosegPositionLegacyLike(LocalNeuroseg& locseg,
                                   const std::array<double, 3>& pos,
                                   NeuroposReferenceLegacyLike ref)
{
  locseg.pos = pos;
  if (ref == NeuroposReference) {
    return;
  }

  double axisOffset = 0.0;
  switch (NeuroposReference) {
    case NeuroposReferenceLegacyLike::Bottom: {
      if (ref == NeuroposReferenceLegacyLike::Top) {
        axisOffset = -locseg.seg.h + 1.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Center) {
        axisOffset = -(locseg.seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Top: {
      if (ref == NeuroposReferenceLegacyLike::Bottom) {
        axisOffset = locseg.seg.h - 1.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Center) {
        axisOffset = (locseg.seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Center: {
      if (ref == NeuroposReferenceLegacyLike::Bottom) {
        axisOffset = (locseg.seg.h - 1.0) / 2.0;
      }
      if (ref == NeuroposReferenceLegacyLike::Top) {
        axisOffset = -(locseg.seg.h - 1.0) / 2.0;
      }
      break;
    }
    case NeuroposReferenceLegacyLike::Undef:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike::Undef";
      break;
  }

  const std::array<double, 3> apos = neurosegAxisOffsetLegacyLike(locseg.seg, axisOffset);
  locseg.pos[0] += apos[0];
  locseg.pos[1] += apos[1];
  locseg.pos[2] += apos[2];
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

std::array<double, 3> localNeurosegAxisCoordNLegacyLike(const LocalNeuroseg& locseg, double t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Axis_Coord_N().
  return localNeurosegAxisPositionLegacyLike(locseg, t * (locseg.seg.h - 1.0));
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

void flipLocalNeurosegLegacyLike(LocalNeuroseg& locseg)
{
  switch (NeuroposReference) {
    case NeuroposReferenceLegacyLike::Bottom: {
      const std::array<double, 3> pos = localNeurosegTopLegacyLike(locseg);
      setNeurosegPositionLegacyLike(locseg, pos, NeuroposReferenceLegacyLike::Bottom);
      break;
    }
    case NeuroposReferenceLegacyLike::Top: {
      const std::array<double, 3> pos = localNeurosegBottomLegacyLike(locseg);
      setNeurosegPositionLegacyLike(locseg, pos, NeuroposReferenceLegacyLike::Top);
      break;
    }
    case NeuroposReferenceLegacyLike::Center:
    case NeuroposReferenceLegacyLike::Undef:
      CHECK(false) << "Unexpected NeuroposReference for Flip_Local_Neuroseg: " << static_cast<int>(NeuroposReference);
  }

  locseg.seg.theta += TzPiLegacyLike;
  locseg.seg.r1 = neurosegR2LegacyLike(locseg.seg);
  locseg.seg.c = -locseg.seg.c;
}

void localNeurosegChopLegacyLike(LocalNeuroseg& locseg, double ratio)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Chop().
  if (ratio > 0.0) { // bottom half
    const std::array<double, 3> pos = localNeurosegBottomLegacyLike(locseg);
    locseg.seg.h = (locseg.seg.h - 1.0) * ratio + 1.0;

    if (NeuroposReference != NeuroposReferenceLegacyLike::Bottom) {
      setNeurosegPositionLegacyLike(locseg, pos, NeuroposReferenceLegacyLike::Bottom);
    }
  } else { // top half
    const std::array<double, 3> pos = localNeurosegTopLegacyLike(locseg);
    locseg.seg.h = -(locseg.seg.h - 1.0) * ratio + 1.0;
    locseg.seg.r1 = neurosegRadiusLegacyLike(locseg.seg, locseg.seg.h - 1.0);

    if (NeuroposReference != NeuroposReferenceLegacyLike::Top) {
      setNeurosegPositionLegacyLike(locseg, pos, NeuroposReferenceLegacyLike::Top);
    }
  }
}

void nextLocalNeurosegLegacyLike(const LocalNeuroseg& locseg1, LocalNeuroseg& locseg2, double posStep)
{
  // Port of tz_local_neuroseg.c::Next_Local_Neuroseg().
  locseg2.seg = nextNeurosegLegacyLike(locseg1.seg, posStep);

  const std::array<double, 3> bottom = localNeurosegAxisPositionLegacyLike(locseg1, posStep * (locseg1.seg.h - 1.0));
  setNeurosegPositionLegacyLike(locseg2, bottom, NeuroposReferenceLegacyLike::Bottom);
}

LocalNeuroseg nextLocalNeurosegLegacyLike(const LocalNeuroseg& locseg1, double posStep)
{
  LocalNeuroseg out{};
  nextLocalNeurosegLegacyLike(locseg1, out, posStep);
  return out;
}

void localNeurosegScaleZLegacyLike(LocalNeuroseg& locseg, double zScaleFactor)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Scale_Z().
  locseg.pos[2] /= zScaleFactor;

  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  geo3dOrientationNormalLegacyLike(locseg.seg.theta, locseg.seg.psi, nx, ny, nz);

  const double invZScale = 1.0 / zScaleFactor;
  const double modelHeight = locseg.seg.h - 1.0;
  const double scaleFactor = std::sqrt(1.0 + ((invZScale * invZScale) - 1.0) * (nz * nz));
  locseg.seg.h = modelHeight * scaleFactor + 1.0;

  CHECK(locseg.seg.alpha == 0.0) << "Alpha not allowed yet.";
  const double cosTheta = std::cos(locseg.seg.theta);
  const double sinTheta = std::sin(locseg.seg.theta);
  const double crossFactor = std::sqrt(cosTheta * cosTheta + (sinTheta / zScaleFactor) * (sinTheta / zScaleFactor));
  locseg.seg.r1 *= crossFactor;
  locseg.seg.scale /= crossFactor;

  nz /= zScaleFactor;
  const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
  CHECK(len > 0.0);
  nx /= len;
  ny /= len;
  nz /= len;

  geo3dNormalOrientationLegacyLike(nx, ny, nz, locseg.seg.theta, locseg.seg.psi);
}

void localNeurosegScaleLegacyLike(LocalNeuroseg& locseg, double xyScale, double zScaleFactor)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Scale().
  if (xyScale == 1.0 && zScaleFactor == 1.0) {
    return;
  }

  locseg.pos[0] *= xyScale;
  locseg.pos[1] *= xyScale;
  locseg.pos[2] *= zScaleFactor;

  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  geo3dOrientationNormalLegacyLike(locseg.seg.theta, locseg.seg.psi, nx, ny, nz);

  const double s = std::sqrt((nx * xyScale) * (nx * xyScale) + (ny * xyScale) * (ny * xyScale) +
                             (nz * zScaleFactor) * (nz * zScaleFactor));
  const double modelHeight = locseg.seg.h - 1.0;
  locseg.seg.h = s * modelHeight + 1.0;

  CHECK(locseg.seg.alpha == 0.0) << "Alpha not allowed yet.";
  const double cosTheta = std::cos(locseg.seg.theta);
  const double sinTheta = std::sin(locseg.seg.theta);
  const double factor =
    std::sqrt((cosTheta * xyScale) * (cosTheta * xyScale) + (sinTheta * zScaleFactor) * (sinTheta * zScaleFactor));

  locseg.seg.r1 *= factor;
  locseg.seg.scale *= xyScale / factor;

  nx *= xyScale;
  ny *= xyScale;
  nz *= zScaleFactor;
  const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
  CHECK(len > 0.0);
  nx /= len;
  ny /= len;
  nz /= len;

  geo3dNormalOrientationLegacyLike(nx, ny, nz, locseg.seg.theta, locseg.seg.psi);
}

bool localNeurosegGoodScoreLegacyLike(const LocalNeuroseg& locseg, double score, double minScore)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Good_Score().
  const double calScore = minScore * (1.0 + 1.0 / (2.0 + std::exp(4.0 - neurosegCRCLegacyLike(locseg.seg))));
  return score > calScore;
}

double localNeurosegAverageSignalLegacyLike(const LocalNeuroseg& locseg,
                                            const ZImg& stack,
                                            double zToXYRatio,
                                            size_t c,
                                            size_t t)
{
  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::MeanSignal);
  (void)localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, &fs, c, t);
  return fs.scores[0];
}

double
localNeurosegTopSampleLegacyLike(const LocalNeuroseg& locseg, const ZImg& stack, double zToXYRatio, size_t c, size_t t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Top_Sample().
  std::array<double, 3> pos = localNeurosegTopLegacyLike(locseg);
  double value = pointSampleLegacyLike(stack, pos[0], pos[1], pos[2] * (1.0 / zToXYRatio), c, t);

  for (double lambda = 0.6; lambda < 0.95; lambda += 0.1) {
    pos = localNeurosegAxisPositionLegacyLike(locseg, locseg.seg.h * lambda);
    const double value2 = pointSampleLegacyLike(stack, pos[0], pos[1], pos[2] * (1.0 / zToXYRatio), c, t);
    if (value2 > value) {
      value = value2;
    }
  }

  return value;
}

namespace {

void localNeurosegFieldZLegacyLikeInto(const LocalNeuroseg& locseg,
                                       double z,
                                       double step,
                                       NeurosegFieldFunctionLegacyLike fieldFunc,
                                       Geo3dScalarField& out);

} // namespace

std::vector<double> localNeurosegHeightProfileLegacyLike(const LocalNeuroseg& locseg,
                                                         const ZImg& stack,
                                                         double zToXYRatio,
                                                         int n,
                                                         int option,
                                                         NeurosegFieldFunctionLegacyLike fieldFunc,
                                                         size_t c,
                                                         size_t t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Profile() (ZImg overload).
  CHECK(n >= 0);
  std::vector<double> profile;
  profile.resize(static_cast<size_t>(n), 0.0);
  if (n == 0) {
    return profile;
  }

  const double step = locseg.seg.h / static_cast<double>(n);

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = option;

  double z = 0.0;
  for (int i = 0; i < n; ++i) {
    const Geo3dScalarField field = localNeurosegFieldZLegacyLike(locseg, z, step, fieldFunc);
    profile[static_cast<size_t>(i)] = geo3dScalarFieldStackScoreLegacyLike(field, stack, zToXYRatio, &fs, c, t);
    z += step;
  }

  return profile;
}

bool localNeurosegHeightSearchWLegacyLike(LocalNeuroseg& locseg,
                                          const ZImg& stack,
                                          double zToXYRatio,
                                          LocsegScoreWorkspace& sws,
                                          size_t c,
                                          size_t t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Search_W() (ZImg overload).
  const int length = iroundLegacyLike(locseg.seg.h);
  const int oldLength = length;
  if (length <= 0) {
    return false;
  }

  const double step = locseg.seg.h / static_cast<double>(length);

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::Corrcoef);

  static thread_local std::vector<double> profileScratch;
  profileScratch.resize(static_cast<size_t>(length));

  static thread_local Geo3dScalarField fieldScratch;

  double z = 0.0;
  for (int i = 0; i < length; ++i) {
    localNeurosegFieldZLegacyLikeInto(locseg, z, step, sws.fieldFunc, fieldScratch);
    profileScratch[static_cast<size_t>(i)] =
      geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs, c, t);
    z += step;
  }

  int index = 0;
  for (index = length - 1; index > 0; --index) {
    if (profileScratch[static_cast<size_t>(index)] > 0.5) {
      break;
    }
  }

  const int newLength = index + 1;
  locseg.seg.h = static_cast<double>(newLength);

  return newLength != oldLength;
}

double localNeurosegAverageSignalLegacyLike(const LocalNeuroseg& locseg, const ZVoxelVolume& stack, double zToXYRatio)
{
  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::MeanSignal);
  (void)localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, &fs);
  return fs.scores[0];
}

double localNeurosegTopSampleLegacyLike(const LocalNeuroseg& locseg, const ZVoxelVolume& stack, double zToXYRatio)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Top_Sample().
  std::array<double, 3> pos = localNeurosegTopLegacyLike(locseg);
  double value = pointSampleLegacyLike(stack, pos[0], pos[1], pos[2] * (1.0 / zToXYRatio));

  for (double lambda = 0.6; lambda < 0.95; lambda += 0.1) {
    pos = localNeurosegAxisPositionLegacyLike(locseg, locseg.seg.h * lambda);
    const double value2 = pointSampleLegacyLike(stack, pos[0], pos[1], pos[2] * (1.0 / zToXYRatio));
    if (value2 > value) {
      value = value2;
    }
  }

  return value;
}

std::vector<double> localNeurosegHeightProfileLegacyLike(const LocalNeuroseg& locseg,
                                                         const ZVoxelVolume& stack,
                                                         double zToXYRatio,
                                                         int n,
                                                         int option,
                                                         NeurosegFieldFunctionLegacyLike fieldFunc)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Profile().
  CHECK(n >= 0);
  std::vector<double> profile;
  profile.resize(static_cast<size_t>(n), 0.0);
  if (n == 0) {
    return profile;
  }

  const double step = locseg.seg.h / static_cast<double>(n);

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = option;

  double z = 0.0;
  for (int i = 0; i < n; ++i) {
    const Geo3dScalarField field = localNeurosegFieldZLegacyLike(locseg, z, step, fieldFunc);
    profile[static_cast<size_t>(i)] = geo3dScalarFieldStackScoreLegacyLike(field, stack, zToXYRatio, &fs);
    z += step;
  }

  return profile;
}

bool localNeurosegHeightSearchWLegacyLike(LocalNeuroseg& locseg,
                                          const ZVoxelVolume& stack,
                                          double zToXYRatio,
                                          LocsegScoreWorkspace& sws)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Height_Search_W().
  const int length = iroundLegacyLike(locseg.seg.h);
  const int oldLength = length;
  if (length <= 0) {
    return false;
  }

  const double step = locseg.seg.h / static_cast<double>(length);

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::Corrcoef);

  static thread_local std::vector<double> profileScratch;
  profileScratch.resize(static_cast<size_t>(length));

  static thread_local Geo3dScalarField fieldScratch;

  double z = 0.0;
  for (int i = 0; i < length; ++i) {
    localNeurosegFieldZLegacyLikeInto(locseg, z, step, sws.fieldFunc, fieldScratch);
    profileScratch[static_cast<size_t>(i)] = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs);
    z += step;
  }

  int index = 0;
  for (index = length - 1; index > 0; --index) {
    if (profileScratch[static_cast<size_t>(index)] > 0.5) {
      break;
    }
  }

  const int newLength = index + 1;
  locseg.seg.h = static_cast<double>(newLength);

  return newLength != oldLength;
}

namespace {

[[nodiscard]] double neurosegRcZLegacyLike(const Neuroseg& seg, double z, int option)
{
  switch (option) {
    case 0: // NEUROSEG_CIRCLE_RX
      return neurosegRxZLegacyLike(seg, z);
    case 1: // NEUROSEG_CIRCLE_RY
      return neurosegRyZLegacyLike(seg, z);
    case 2: // NEUROSEG_CIRCLE_RXY
      return neurosegRxyZLegacyLike(seg, z);
    default:
      CHECK(false) << "Invalid neuroseg circle option: " << option;
      return 0.0;
  }
}

} // namespace

Geo3dCircle localNeurosegToCircleZLegacyLike(const LocalNeuroseg& locseg, double z, int option)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_To_Circle_Z().
  Geo3dCircle circle;
  circle.radius = neurosegRcZLegacyLike(locseg.seg, z, option);
  circle.center = localNeurosegAxisPositionLegacyLike(locseg, z);
  circle.orientation = {locseg.seg.theta, locseg.seg.psi};
  return circle;
}

Geo3dCircle localNeurosegToCircleTLegacyLike(const LocalNeuroseg& locseg, double t, int option)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_To_Circle_T().
  const double z = (locseg.seg.h - 1.0) * t;
  return localNeurosegToCircleZLegacyLike(locseg, z, option);
}

bool localNeurosegHitTestLegacyLike(const LocalNeuroseg& locseg, double x, double y, double z)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Hit_Test().
  std::array<double, 3> tmpPos = localNeurosegBottomLegacyLike(locseg);

  tmpPos[0] = x - tmpPos[0];
  tmpPos[1] = y - tmpPos[1];
  tmpPos[2] = z - tmpPos[2];

  rotateXZLegacyLike(tmpPos, locseg.seg.theta, locseg.seg.psi, 1);
  rotateZLegacyLike(tmpPos, locseg.seg.alpha, 1);

  return neurosegHitTestLegacyLike(locseg.seg, tmpPos[0], tmpPos[1], tmpPos[2]);
}

bool localNeurosegHitMaskLegacyLike(const LocalNeuroseg& locseg, const ZImg& mask, double zToXYRatio)
{
  if (mask.isEmpty()) {
    return false;
  }
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  // Port of `ZLocalNeuroseg::hitMask(const Stack*)` via its `sample(1.0, 1.0)` + transform pipeline.
  //
  // Performance note: this is called in hot loops (seed sorting and chain tracing). Avoid wrapping the
  // mask into a ZVoxelVolume and avoid valueAsDouble() virtual dispatch. Read mask voxels directly.
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);

  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());
  const size_t plane = mask.width() * mask.height();

  return imgTypeDispatcher(mask.info(), [&]<typename TVoxel>() -> bool {
    const TVoxel* maskData = mask.timeData<TVoxel>(0);

    auto maskValue = [&](int x, int y, int z) -> int {
      if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
        return 0;
      }

      const size_t idx =
        static_cast<size_t>(x) + static_cast<size_t>(y) * mask.width() + static_cast<size_t>(z) * plane;
      return static_cast<int>(maskData[idx]);
    };

    constexpr double xyStep = 1.0;
    constexpr double zStep = 1.0;

    constexpr double eps = 1.001;
    constexpr double minCurvature = 0.2; // NEUROSEG_MIN_CURVATURE
    constexpr double maxCurvature = TzPiLegacyLike; // NEUROSEG_MAX_CURVATURE

    const double heightSeg = locseg.seg.h;
    const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

    for (double z = 0.0; z <= heightSeg; z += zStep) {
      const double radius = neurosegRadiusLegacyLike(locseg.seg, z);
      if (radius <= xyStep * 0.1) {
        continue;
      }

      const double rr = radius * radius;

      for (double y = -radius; y <= radius; y += xyStep) {
        for (double x = -radius; x <= radius; x += xyStep) {
          if ((x * x + y * y) / rr >= eps) {
            continue;
          }

          std::array<double, 3> p = {x, y, z};

          if (locseg.seg.alpha != 0.0) {
            rotateZLegacyLike(p, locseg.seg.alpha, 0);
          }

          if (locseg.seg.curvature >= minCurvature) {
            double curvature = locseg.seg.curvature;
            if (curvature > maxCurvature) {
              curvature = maxCurvature;
            }
            geo3dPointArrayBendLegacyLike(&p, 1, heightSeg / curvature);
          }

          if (locseg.seg.theta != 0.0 || locseg.seg.psi != 0.0) {
            rotateXZLegacyLike(p, locseg.seg.theta, locseg.seg.psi, 0);
          }

          p[0] += bottom[0];
          p[1] += bottom[1];
          p[2] += bottom[2];

          const int ix = iroundLegacyLike(p[0]);
          const int iy = iroundLegacyLike(p[1]);
          const int iz = iroundLegacyLike(p[2] / zToXYRatio);
          if (maskValue(ix, iy, iz) > 0) {
            return true;
          }
        }
      }
    }

    return false;
  });
}

bool localNeurosegHitMaskLegacyLike(const LocalNeuroseg& locseg, const ZVoxelVolume& mask, double zToXYRatio)
{
  // Port of `ZLocalNeuroseg::hitMask(const Stack*)` via its `sample(1.0, 1.0)` + transform pipeline.
  if (mask.isEmpty()) {
    return false;
  }
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);
  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());

  auto maskValue = [&](int x, int y, int z) -> int {
    if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
      return 0;
    }
    return static_cast<int>(mask.valueAsDouble(x, y, z));
  };

  constexpr double xyStep = 1.0;
  constexpr double zStep = 1.0;

  constexpr double eps = 1.001;
  constexpr double minCurvature = 0.2; // NEUROSEG_MIN_CURVATURE
  constexpr double maxCurvature = TzPiLegacyLike; // NEUROSEG_MAX_CURVATURE

  const double heightSeg = locseg.seg.h;
  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

  for (double z = 0.0; z <= heightSeg; z += zStep) {
    const double radius = neurosegRadiusLegacyLike(locseg.seg, z);
    if (radius <= xyStep * 0.1) {
      continue;
    }

    const double rr = radius * radius;

    for (double y = -radius; y <= radius; y += xyStep) {
      for (double x = -radius; x <= radius; x += xyStep) {
        if ((x * x + y * y) / rr >= eps) {
          continue;
        }

        std::array<double, 3> p = {x, y, z};

        if (locseg.seg.alpha != 0.0) {
          rotateZLegacyLike(p, locseg.seg.alpha, 0);
        }

        if (locseg.seg.curvature >= minCurvature) {
          double curvature = locseg.seg.curvature;
          if (curvature > maxCurvature) {
            curvature = maxCurvature;
          }
          geo3dPointArrayBendLegacyLike(&p, 1, heightSeg / curvature);
        }

        if (locseg.seg.theta != 0.0 || locseg.seg.psi != 0.0) {
          rotateXZLegacyLike(p, locseg.seg.theta, locseg.seg.psi, 0);
        }

        p[0] += bottom[0];
        p[1] += bottom[1];
        p[2] += bottom[2];

        const int ix = iroundLegacyLike(p[0]);
        const int iy = iroundLegacyLike(p[1]);
        const int iz = iroundLegacyLike(p[2] / zToXYRatio);
        if (maskValue(ix, iy, iz) > 0) {
          return true;
        }
      }
    }
  }

  return false;
}

namespace {

void localNeurosegFieldSLegacyLikeInto(const LocalNeuroseg& locseg,
                                       NeurosegFieldFunctionLegacyLike fieldFunc,
                                       Geo3dScalarField& out)
{
  neurosegFieldSFastLegacyLikeInto(locseg.seg, fieldFunc, out);
  if (out.points.empty()) {
    return;
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

  for (auto& p : out.points) {
    p[0] += dx;
    p[1] += dy;
    p[2] += dz;
  }
}

void localNeurosegFieldSpLegacyLikeInto(const LocalNeuroseg& locseg,
                                        NeurosegFieldFunctionLegacyLike fieldFunc,
                                        Geo3dScalarField& out)
{
  neurosegFieldSpLegacyLikeInto(locseg.seg, fieldFunc, out);
  if (out.points.empty()) {
    return;
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

  for (auto& p : out.points) {
    p[0] += dx;
    p[1] += dy;
    p[2] += dz;
  }
}

void localNeurosegFieldZLegacyLikeInto(const LocalNeuroseg& locseg,
                                       double z,
                                       double step,
                                       NeurosegFieldFunctionLegacyLike fieldFunc,
                                       Geo3dScalarField& out)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Field_Z() (Into variant).
  //
  // Note: the legacy implementation does not use `fieldFunc` for the Z-slice field.
  (void)fieldFunc;

  neurosegFieldZLegacyLikeInto(locseg.seg, z, step, out);
  if (out.points.empty()) {
    return;
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

  for (auto& p : out.points) {
    p[0] += dx;
    p[1] += dy;
    p[2] += dz;
  }
}

} // namespace

Geo3dScalarField localNeurosegFieldSLegacyLike(const LocalNeuroseg& locseg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;
  localNeurosegFieldSLegacyLikeInto(locseg, fieldFunc, field);
  return field;
}

Geo3dScalarField localNeurosegFieldSpLegacyLike(const LocalNeuroseg& locseg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;
  localNeurosegFieldSpLegacyLikeInto(locseg, fieldFunc, field);
  return field;
}

Geo3dScalarField localNeurosegFieldZLegacyLike(const LocalNeuroseg& locseg,
                                               double z,
                                               double step,
                                               NeurosegFieldFunctionLegacyLike /*fieldFunc*/)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Field_Z().
  Geo3dScalarField field = neurosegFieldZLegacyLike(locseg.seg, z, step);
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

double localNeurosegScorePLegacyLike(const LocalNeuroseg& locseg,
                                     const ZImg& stack,
                                     double zToXYRatio,
                                     StackFitScore* fs,
                                     size_t c,
                                     size_t t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Score_P() (ZImg overload).
  double score = 0.0;

  if (locseg.seg.r1 > 0.0 && locseg.seg.scale > 0.0) {
    static thread_local Geo3dScalarField fieldScratch;
    localNeurosegFieldSLegacyLikeInto(locseg, nullptr, fieldScratch);
    score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, fs, c, t);
  }

  return score;
}

void localNeurosegPositionAdjustLegacyLike(LocalNeuroseg& locseg,
                                           const ZImg& stack,
                                           double zToXYRatio,
                                           size_t c,
                                           size_t t)
{
  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSLegacyLikeInto(locseg, nullptr, fieldScratch);
  CHECK(!fieldScratch.points.empty());

  fieldScratch.values.resize(fieldScratch.size());
  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();
  const size_t channelVoxelNumber = width * height * depth;

  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    const TVoxel* array = stack.timeData<TVoxel>(t);
    for (size_t i = 0; i < fieldScratch.size(); ++i) {
      const auto& p = fieldScratch.points[i];
      const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] * (1.0 / zToXYRatio));
      fieldScratch.values[i] =
        pointSampleLegacyLikeTypedFast<TVoxel>(array, width, height, depth, channelVoxelNumber, c, p[0], p[1], z);
    }
  });

  const std::array<double, 3> center = geo3dScalarFieldCentroidLegacyLike(fieldScratch);
  setNeurosegPositionLegacyLike(locseg, center, NeuroposReferenceLegacyLike::Center);
}

double localNeurosegOrientationSearchCLegacyLike(LocalNeuroseg& locseg,
                                                 const ZImg& stack,
                                                 double zToXYRatio,
                                                 StackFitScore& fs,
                                                 size_t c,
                                                 size_t t)
{
  double bestTheta = 0.0;
  double bestPsi = 0.0;

  const std::array<double, 3> center = localNeurosegCenterLegacyLike(locseg);

  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSpLegacyLikeInto(locseg, nullptr, fieldScratch);

  double bestScore = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs, c, t);
  bestTheta = locseg.seg.theta;
  bestPsi = locseg.seg.psi;

  LocalNeuroseg tmpLocseg = locseg;

  const double thetaRange = TzPiLegacyLike * 0.75;

  for (double theta = 0.1; theta <= thetaRange; theta += 0.2) {
    const double step = 2.0 / locseg.seg.h / std::sin(theta);
    for (double psi = 0.0; psi < Tz2PiLegacyLike; psi += step) {
      tmpLocseg.seg.theta = theta;
      tmpLocseg.seg.psi = psi;

      geo3dRotateOrientationLegacyLike(locseg.seg.theta, locseg.seg.psi, tmpLocseg.seg.theta, tmpLocseg.seg.psi);

      setNeurosegPositionLegacyLike(tmpLocseg, center, NeuroposReferenceLegacyLike::Center);

      localNeurosegFieldSpLegacyLikeInto(tmpLocseg, nullptr, fieldScratch);

      const double score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs, c, t);

      if (score > bestScore) {
        bestTheta = tmpLocseg.seg.theta;
        bestPsi = tmpLocseg.seg.psi;
        bestScore = score;
      }
    }
  }

  locseg.seg.theta = bestTheta;
  locseg.seg.psi = bestPsi;
  setNeurosegPositionLegacyLike(locseg, center, NeuroposReferenceLegacyLike::Center);

  return bestScore;
}

double localNeurosegOrientationSearchBLegacyLike(LocalNeuroseg& locseg,
                                                 const ZImg& stack,
                                                 double zToXYRatio,
                                                 StackFitScore& fs,
                                                 size_t c,
                                                 size_t t)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Orientation_Search_B() (ZImg overload).
  double bestTheta = locseg.seg.theta;
  double bestPsi = locseg.seg.psi;

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSpLegacyLikeInto(locseg, nullptr, fieldScratch);
  double bestScore = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs, c, t);

  LocalNeuroseg tmpLocseg = locseg;

  const double thetaRange = TzPiLegacyLike * 0.5;
  for (double theta = 0.1; theta <= thetaRange; theta += 0.1) {
    const double step = 2.0 / locseg.seg.h / std::sin(theta);
    for (double psi = 0.0; psi < Tz2PiLegacyLike; psi += step) {
      tmpLocseg.seg.theta = theta;
      tmpLocseg.seg.psi = psi;

      geo3dRotateOrientationLegacyLike(locseg.seg.theta, locseg.seg.psi, tmpLocseg.seg.theta, tmpLocseg.seg.psi);
      setNeurosegPositionLegacyLike(tmpLocseg, bottom, NeuroposReferenceLegacyLike::Bottom);

      localNeurosegFieldSpLegacyLikeInto(tmpLocseg, nullptr, fieldScratch);
      const double score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs, c, t);

      if (score > bestScore) {
        bestTheta = tmpLocseg.seg.theta;
        bestPsi = tmpLocseg.seg.psi;
        bestScore = score;
      }
    }
  }

  locseg.seg.theta = bestTheta;
  locseg.seg.psi = bestPsi;
  setNeurosegPositionLegacyLike(locseg, bottom, NeuroposReferenceLegacyLike::Bottom);

  return bestScore;
}

double localNeurosegScorePLegacyLike(const LocalNeuroseg& locseg,
                                     const ZVoxelVolume& stack,
                                     double zToXYRatio,
                                     StackFitScore* fs)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Score_P().
  double score = 0.0;

  if (locseg.seg.r1 > 0.0 && locseg.seg.scale > 0.0) {
    static thread_local Geo3dScalarField fieldScratch;
    localNeurosegFieldSLegacyLikeInto(locseg, nullptr, fieldScratch);
    score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, fs);
  }

  return score;
}

void localNeurosegPositionAdjustLegacyLike(LocalNeuroseg& locseg, const ZVoxelVolume& stack, double zToXYRatio)
{
  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSLegacyLikeInto(locseg, nullptr, fieldScratch);
  CHECK(!fieldScratch.points.empty());

  fieldScratch.values.resize(fieldScratch.size());
  for (size_t i = 0; i < fieldScratch.size(); ++i) {
    const auto& p = fieldScratch.points[i];
    const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] * (1.0 / zToXYRatio));
    fieldScratch.values[i] = pointSampleLegacyLike(stack, p[0], p[1], z);
  }

  const std::array<double, 3> center = geo3dScalarFieldCentroidLegacyLike(fieldScratch);
  setNeurosegPositionLegacyLike(locseg, center, NeuroposReferenceLegacyLike::Center);
}

double localNeurosegOrientationSearchCLegacyLike(LocalNeuroseg& locseg,
                                                 const ZVoxelVolume& stack,
                                                 double zToXYRatio,
                                                 StackFitScore& fs)
{
  double bestTheta = 0.0;
  double bestPsi = 0.0;

  const std::array<double, 3> center = localNeurosegCenterLegacyLike(locseg);

  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSpLegacyLikeInto(locseg, nullptr, fieldScratch);

  double bestScore = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs);
  bestTheta = locseg.seg.theta;
  bestPsi = locseg.seg.psi;

  LocalNeuroseg tmpLocseg = locseg;

  const double thetaRange = TzPiLegacyLike * 0.75;

  for (double theta = 0.1; theta <= thetaRange; theta += 0.2) {
    const double step = 2.0 / locseg.seg.h / std::sin(theta);
    for (double psi = 0.0; psi < Tz2PiLegacyLike; psi += step) {
      tmpLocseg.seg.theta = theta;
      tmpLocseg.seg.psi = psi;

      geo3dRotateOrientationLegacyLike(locseg.seg.theta, locseg.seg.psi, tmpLocseg.seg.theta, tmpLocseg.seg.psi);

      setNeurosegPositionLegacyLike(tmpLocseg, center, NeuroposReferenceLegacyLike::Center);

      localNeurosegFieldSpLegacyLikeInto(tmpLocseg, nullptr, fieldScratch);

      const double score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs);

      if (score > bestScore) {
        bestTheta = tmpLocseg.seg.theta;
        bestPsi = tmpLocseg.seg.psi;
        bestScore = score;
      }
    }
  }

  locseg.seg.theta = bestTheta;
  locseg.seg.psi = bestPsi;
  setNeurosegPositionLegacyLike(locseg, center, NeuroposReferenceLegacyLike::Center);

  return bestScore;
}

double localNeurosegOrientationSearchBLegacyLike(LocalNeuroseg& locseg,
                                                 const ZVoxelVolume& stack,
                                                 double zToXYRatio,
                                                 StackFitScore& fs)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Orientation_Search_B().
  double bestTheta = locseg.seg.theta;
  double bestPsi = locseg.seg.psi;

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSpLegacyLikeInto(locseg, nullptr, fieldScratch);
  double bestScore = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs);

  LocalNeuroseg tmpLocseg = locseg;

  const double thetaRange = TzPiLegacyLike * 0.5;
  for (double theta = 0.1; theta <= thetaRange; theta += 0.1) {
    const double step = 2.0 / locseg.seg.h / std::sin(theta);
    for (double psi = 0.0; psi < Tz2PiLegacyLike; psi += step) {
      tmpLocseg.seg.theta = theta;
      tmpLocseg.seg.psi = psi;

      geo3dRotateOrientationLegacyLike(locseg.seg.theta, locseg.seg.psi, tmpLocseg.seg.theta, tmpLocseg.seg.psi);
      setNeurosegPositionLegacyLike(tmpLocseg, bottom, NeuroposReferenceLegacyLike::Bottom);

      localNeurosegFieldSpLegacyLikeInto(tmpLocseg, nullptr, fieldScratch);
      const double score = geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &fs);

      if (score > bestScore) {
        bestTheta = tmpLocseg.seg.theta;
        bestPsi = tmpLocseg.seg.psi;
        bestScore = score;
      }
    }
  }

  locseg.seg.theta = bestTheta;
  locseg.seg.psi = bestPsi;
  setNeurosegPositionLegacyLike(locseg, bottom, NeuroposReferenceLegacyLike::Bottom);

  return bestScore;
}

double localNeurosegRScaleSearchLegacyLike(LocalNeuroseg& locseg,
                                           const ZImg& stack,
                                           double zToXYRatio,
                                           double rStart,
                                           double rEnd,
                                           double rStep,
                                           double sStart,
                                           double sEnd,
                                           double sStep,
                                           StackFitScore* fs,
                                           size_t c,
                                           size_t t)
{
  double bestScore = localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, fs, c, t);
  double bestS = locseg.seg.scale;
  double bestR = locseg.seg.r1;

  for (double r = rStart; r <= rEnd; r += rStep) {
    for (double s = sStart; s <= sEnd; s += sStep) {
      if (r * s > NeurosegMinRLegacyLike && r * s < 30.0) {
        locseg.seg.r1 = r;
        locseg.seg.scale = s;

        const double score = localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, fs, c, t);
        if (score > bestScore) {
          bestScore = score;
          bestS = s;
          bestR = r;
        }
      }
    }
  }

  locseg.seg.scale = bestS;
  locseg.seg.r1 = bestR;

  return bestScore;
}

double localNeurosegScoreWLegacyLike(const LocalNeuroseg& locseg,
                                     const ZImg& stack,
                                     double zToXYRatio,
                                     LocsegScoreWorkspace& ws,
                                     size_t c,
                                     size_t t)
{
  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSLegacyLikeInto(locseg, ws.fieldFunc, fieldScratch);
  if (ws.mask == nullptr) {
    return geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &ws.fs, c, t);
  }

  // Fallback for the (rare) masked scoring path: keep the original ZVoxelVolume implementation.
  const ZImg view = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume vol(view);
  return geo3dScalarFieldStackScoreMaskedLegacyLike(fieldScratch, vol, zToXYRatio, *ws.mask, &ws.fs);
}

void defaultLocsegFitWorkspaceLegacyLike(LocsegFitWorkspace& ws)
{
  ws.nvar = localNeurosegVarMaskToIndexLegacyLike(NeurosegVarMaskRLegacyLike | NeurosegVarMaskOrientationLegacyLike |
                                                    NeurosegVarMaskScaleLegacyLike,
                                                  NeuroposVarMaskNoneLegacyLike,
                                                  ws.varIndex);

  ws.varLink = nullptr;

  ws.varMin.fill(0.0);
  ws.varMax.fill(0.0);
  std::copy(LocalNeurosegVarMinLegacyLike.begin(), LocalNeurosegVarMinLegacyLike.end(), ws.varMin.begin());
  std::copy(LocalNeurosegVarMaxLegacyLike.begin(), LocalNeurosegVarMaxLegacyLike.end(), ws.varMax.begin());

  ws.sws.fs.n = 1;
  ws.sws.fs.options[0] = 0; // STACK_FIT_DOT
  ws.sws.fieldFunc = nullptr;
  ws.sws.mask = nullptr;

  ws.posAdjust = 1;
}

double fitLocalNeurosegWLegacyLike(LocalNeuroseg& locseg,
                                   const ZImg& stack,
                                   double zToXYRatio,
                                   LocsegFitWorkspace& ws,
                                   size_t c,
                                   size_t t)
{
  std::optional<ZImg> viewStorage;
  const ZImg* viewPtr = &stack;
  if (c != 0 || t != 0 || stack.numChannels() != 1 || stack.numTimes() != 1) {
    viewStorage = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
    viewPtr = &(*viewStorage);
  }

  const ZImg& view = *viewPtr;
  CHECK(view.numChannels() == 1);
  CHECK(view.numTimes() == 1);

  if (ws.nvar == 0) {
    return localNeurosegScoreWLegacyLike(locseg, view, zToXYRatio, ws.sws);
  }

  std::array<double, LocalNeurosegParamArraySizeLegacyLike> var{};
  localNeurosegParamArrayLegacyLike(locseg, zToXYRatio, var);

  std::array<double, LocsegFitWorkspaceMaxVarNumberLegacyLike> weight{};
  for (int i = 0; i < ws.nvar; ++i) {
    const int varIndex = ws.varIndex[static_cast<size_t>(i)];
    CHECK(varIndex >= 0);
    CHECK(static_cast<size_t>(varIndex) < LocalNeurosegDeltaLegacyLike.size())
      << "LocalNeuroseg delta array is only defined for indices [0, " << (LocalNeurosegDeltaLegacyLike.size() - 1)
      << "], got varIndex=" << varIndex;
    weight[static_cast<size_t>(i)] = LocalNeurosegDeltaLegacyLike[static_cast<size_t>(varIndex)];
  }

  double wl = 0.0;
  for (int i = 0; i < ws.nvar; ++i) {
    wl += weight[static_cast<size_t>(i)] * weight[static_cast<size_t>(i)];
  }
  wl = std::sqrt(wl);
  CHECK(wl > 0.0);

  for (int i = 0; i < ws.nvar; ++i) {
    weight[static_cast<size_t>(i)] /= wl;
  }

  VariableSet vs;
  vs.var = var.data();
  vs.varIndex = ws.varIndex.data();
  vs.link = ws.varLink;
  vs.nvar = ws.nvar;

  ContinuousFunction cf;
  cf.f = localNeurosegScoreRLegacyLikeZImg;
  cf.v = localNeurosegValidateLegacyLike;
  cf.varMin = ws.varMin.data();
  cf.varMax = ws.varMax.data();

  Perceptor perceptor;
  perceptor.vs = &vs;
  perceptor.arg = &ws.sws;
  perceptor.s = &cf;
  perceptor.minGradient = 1e-3;
  perceptor.delta = LocalNeurosegDeltaLegacyLike.data();
  perceptor.weight = weight.data();

  fitPerceptorLegacyLike(perceptor, &view);

  for (int i = 0; i < LocalNeurosegNParamLegacyLike; ++i) {
    localNeurosegSetVarLegacyLike(locseg, i, var[static_cast<size_t>(i)]);
  }

  locseg.seg.theta = normalizeRadianLegacyLike(locseg.seg.theta);
  locseg.seg.psi = normalizeRadianLegacyLike(locseg.seg.psi);

  return localNeurosegScoreWLegacyLike(locseg, view, zToXYRatio, ws.sws);
}

double localNeurosegRScaleSearchLegacyLike(LocalNeuroseg& locseg,
                                           const ZVoxelVolume& stack,
                                           double zToXYRatio,
                                           double rStart,
                                           double rEnd,
                                           double rStep,
                                           double sStart,
                                           double sEnd,
                                           double sStep,
                                           StackFitScore* fs)
{
  double bestScore = localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, fs);
  double bestS = locseg.seg.scale;
  double bestR = locseg.seg.r1;

  for (double r = rStart; r <= rEnd; r += rStep) {
    for (double s = sStart; s <= sEnd; s += sStep) {
      if (r * s > NeurosegMinRLegacyLike && r * s < 30.0) {
        locseg.seg.r1 = r;
        locseg.seg.scale = s;

        const double score = localNeurosegScorePLegacyLike(locseg, stack, zToXYRatio, fs);
        if (score > bestScore) {
          bestScore = score;
          bestS = s;
          bestR = r;
        }
      }
    }
  }

  locseg.seg.scale = bestS;
  locseg.seg.r1 = bestR;

  return bestScore;
}

double localNeurosegScoreWLegacyLike(const LocalNeuroseg& locseg,
                                     const ZVoxelVolume& stack,
                                     double zToXYRatio,
                                     LocsegScoreWorkspace& ws)
{
  static thread_local Geo3dScalarField fieldScratch;
  localNeurosegFieldSLegacyLikeInto(locseg, ws.fieldFunc, fieldScratch);
  if (ws.mask == nullptr) {
    return geo3dScalarFieldStackScoreLegacyLike(fieldScratch, stack, zToXYRatio, &ws.fs);
  }

  return geo3dScalarFieldStackScoreMaskedLegacyLike(fieldScratch, stack, zToXYRatio, *ws.mask, &ws.fs);
}

double
fitLocalNeurosegWLegacyLike(LocalNeuroseg& locseg, const ZVoxelVolume& stack, double zToXYRatio, LocsegFitWorkspace& ws)
{
  if (ws.nvar == 0) {
    return localNeurosegScoreWLegacyLike(locseg, stack, zToXYRatio, ws.sws);
  }

  std::array<double, LocalNeurosegParamArraySizeLegacyLike> var{};
  localNeurosegParamArrayLegacyLike(locseg, zToXYRatio, var);

  std::array<double, LocsegFitWorkspaceMaxVarNumberLegacyLike> weight{};
  for (int i = 0; i < ws.nvar; ++i) {
    const int varIndex = ws.varIndex[static_cast<size_t>(i)];
    CHECK(varIndex >= 0);
    CHECK(static_cast<size_t>(varIndex) < LocalNeurosegDeltaLegacyLike.size())
      << "LocalNeuroseg delta array is only defined for indices [0, " << (LocalNeurosegDeltaLegacyLike.size() - 1)
      << "], got varIndex=" << varIndex;
    weight[static_cast<size_t>(i)] = LocalNeurosegDeltaLegacyLike[static_cast<size_t>(varIndex)];
  }

  double wl = 0.0;
  for (int i = 0; i < ws.nvar; ++i) {
    wl += weight[static_cast<size_t>(i)] * weight[static_cast<size_t>(i)];
  }
  wl = std::sqrt(wl);
  CHECK(wl > 0.0);

  for (int i = 0; i < ws.nvar; ++i) {
    weight[static_cast<size_t>(i)] /= wl;
  }

  VariableSet vs;
  vs.var = var.data();
  vs.varIndex = ws.varIndex.data();
  vs.link = ws.varLink;
  vs.nvar = ws.nvar;

  ContinuousFunction cf;
  cf.f = localNeurosegScoreRLegacyLike;
  cf.v = localNeurosegValidateLegacyLike;
  cf.varMin = ws.varMin.data();
  cf.varMax = ws.varMax.data();

  Perceptor perceptor;
  perceptor.vs = &vs;
  perceptor.arg = &ws.sws;
  perceptor.s = &cf;
  perceptor.minGradient = 1e-3;
  perceptor.delta = LocalNeurosegDeltaLegacyLike.data();
  perceptor.weight = weight.data();

  fitPerceptorLegacyLike(perceptor, &stack);

  for (int i = 0; i < LocalNeurosegNParamLegacyLike; ++i) {
    localNeurosegSetVarLegacyLike(locseg, i, var[static_cast<size_t>(i)]);
  }

  locseg.seg.theta = normalizeRadianLegacyLike(locseg.seg.theta);
  locseg.seg.psi = normalizeRadianLegacyLike(locseg.seg.psi);

  return localNeurosegScoreWLegacyLike(locseg, stack, zToXYRatio, ws.sws);
}

double localNeurosegOptimizeWLegacyLike(LocalNeuroseg& locseg,
                                        const ZImg& stack,
                                        double zToXYRatio,
                                        int option,
                                        LocsegFitWorkspace& ws,
                                        size_t c,
                                        size_t t)
{
  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::Corrcoef);

  for (int i = 0; i < ws.posAdjust; ++i) {
    localNeurosegPositionAdjustLegacyLike(locseg, stack, zToXYRatio, c, t);
  }

  (void)localNeurosegOrientationSearchCLegacyLike(locseg, stack, zToXYRatio, fs, c, t);

  if (option <= 1) {
    for (int i = 0; i < 3; ++i) {
      localNeurosegPositionAdjustLegacyLike(locseg, stack, zToXYRatio, c, t);
    }
  }

  if (option == 1 || option == 2) {
    (void)localNeurosegRScaleSearchLegacyLike(locseg, stack, zToXYRatio, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr, c, t);
  }

  const double score = fitLocalNeurosegWLegacyLike(locseg, stack, zToXYRatio, ws, c, t);
  return score;
}

double localNeurosegOptimizeWLegacyLike(LocalNeuroseg& locseg,
                                        const ZVoxelVolume& stack,
                                        double zToXYRatio,
                                        int option,
                                        LocsegFitWorkspace& ws)
{
  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(StackFitOption::Corrcoef);

  for (int i = 0; i < ws.posAdjust; ++i) {
    localNeurosegPositionAdjustLegacyLike(locseg, stack, zToXYRatio);
  }

  (void)localNeurosegOrientationSearchCLegacyLike(locseg, stack, zToXYRatio, fs);

  if (option <= 1) {
    for (int i = 0; i < 3; ++i) {
      localNeurosegPositionAdjustLegacyLike(locseg, stack, zToXYRatio);
    }
  }

  if (option == 1 || option == 2) {
    (void)localNeurosegRScaleSearchLegacyLike(locseg, stack, zToXYRatio, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr);
  }

  const double score = fitLocalNeurosegWLegacyLike(locseg, stack, zToXYRatio, ws);
  return score;
}

void localNeurosegBallBoundLegacyLike(const LocalNeuroseg& locseg, Geo3dBallLegacyLike& ball)
{
  ball.center = localNeurosegCenterLegacyLike(locseg);
  ball.radius = neurosegBallRangeLegacyLike(locseg.seg) / 2.0;
}

double localNeurosegPlanarDistLLegacyLike(const LocalNeuroseg& locseg1, const LocalNeuroseg& locseg2)
{
  const std::array<double, 3> bottom1 = localNeurosegBottomLegacyLike(locseg1);
  const std::array<double, 3> top1 = localNeurosegTopLegacyLike(locseg1);
  const std::array<double, 3> bottom2 = localNeurosegBottomLegacyLike(locseg2);
  const std::array<double, 3> top2 = localNeurosegTopLegacyLike(locseg2);
  return geo3dLineLineDistLegacyLike(bottom1, top1, bottom2, top2);
}

double
localNeurosegDist2LegacyLike(const LocalNeuroseg& locseg1, const LocalNeuroseg& locseg2, std::array<double, 3>* pos)
{
  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg1);
  const std::array<double, 3> top = localNeurosegTopLegacyLike(locseg1);
  return localNeurosegLineSegDistSLegacyLike(locseg2, bottom, top, pos);
}

} // namespace nim
