#include "zneutubeneuroseg.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dutils.h"
#include "zneutubegeo3dpointarray.h"
#include "zneutubegeo3dscalarfield.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nim {

namespace {

constexpr int NeurosegDefaultHSlicesLegacyLike = 11;
constexpr double NeurosegMinCurvatureLegacyLike = 0.2;
constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;

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

[[nodiscard]] int testZScaleLegacyLike(double zToXYRatio)
{
  return compareFloatLegacyLike(zToXYRatio, 1.0, 1e-5);
}

void neurosegScaleFieldSlice0LegacyLike(std::array<double, 3>& pt,
                                        double& value,
                                        double rScale,
                                        double r,
                                        double sqrtRScale)
{
  if (value >= 0.0) {
    pt[0] *= rScale;
    pt[1] *= r;
    value *= sqrtRScale;
  } else {
    const double norm = std::sqrt(pt[0] * pt[0] + pt[1] * pt[1]);
    double alpha = norm - 1.0;

    pt[0] *= rScale / norm;
    pt[1] *= r / norm;

    const double enorm = pt[0] * pt[0] + pt[1] * pt[1];
    if (enorm < 1.0) {
      alpha /= std::sqrt(enorm);
    } else if (enorm > 4.0) {
      alpha /= std::sqrt(enorm);
      alpha = alpha + alpha;
    }

    pt[0] *= 1.0 + alpha;
    pt[1] *= 1.0 + alpha;
  }
}

void neurosegScaleFieldConeSliceLegacyLike(std::array<double, 3>& pt,
                                           double& value,
                                           double r,
                                           double scale,
                                           double rScale,
                                           double sqrtRScale)
{
  if (value >= 0.0) {
    pt[0] *= rScale;
    pt[1] *= r;
    value *= sqrtRScale;
  } else {
    const double norm = std::sqrt(pt[0] * pt[0] + pt[1] * pt[1]);
    double alpha = norm - 1.0;

    // positive boundary
    const double rnorm = r / norm;
    pt[0] *= rnorm * scale;
    pt[1] *= rnorm;

    // length to positive boundary
    const double enorm = pt[0] * pt[0] + pt[1] * pt[1];
    if (enorm < 1.0) {
      alpha /= std::sqrt(enorm);
    } else if (enorm > 4.0) {
      alpha *= 2.0 / std::sqrt(enorm);
    }

    pt[0] *= 1.0 + alpha;
    pt[1] *= 1.0 + alpha;
  }
}

} // namespace

double neurosegCoefLegacyLike(const Neuroseg& seg)
{
  if (seg.h == 1.0) {
    return seg.c;
  }
  return std::max(seg.c, (NeurosegMinRLegacyLike - seg.r1) / (seg.h - 1.0));
}

double neurosegRadiusLegacyLike(const Neuroseg& seg, double z)
{
  return seg.r1 + z * neurosegCoefLegacyLike(seg);
}

double neurosegR2LegacyLike(const Neuroseg& seg)
{
  return neurosegRadiusLegacyLike(seg, seg.h - 1.0);
}

double neurosegRCLegacyLike(const Neuroseg& seg)
{
  return seg.r1 + (seg.h - 1.0) * neurosegCoefLegacyLike(seg) / 2.0;
}

double neurosegRALegacyLike(const Neuroseg& seg)
{
  if (seg.c >= 0.0) {
    return seg.r1;
  }
  return neurosegR2LegacyLike(seg);
}

double neurosegRBLegacyLike(const Neuroseg& seg)
{
  if (seg.c <= 0.0) {
    return seg.r1;
  }
  return neurosegR2LegacyLike(seg);
}

double neurosegCRCLegacyLike(const Neuroseg& seg)
{
  return neurosegRCLegacyLike(seg) * std::sqrt(seg.scale);
}

double neurosegBallRangeLegacyLike(const Neuroseg& seg)
{
  // Port of tz_neuroseg.c::Neuroseg_Ball_Range().
  double r = neurosegRBLegacyLike(seg);
  if (seg.scale > 1.0) {
    r *= seg.scale;
  }
  return std::sqrt(seg.h * seg.h + r * r);
}

void neurosegSwellLegacyLike(Neuroseg& seg, double ratio, double diff, double maxDiff)
{
  // Port of tz_neuroseg.c::Neuroseg_Swell().
  const double rby = neurosegRBLegacyLike(seg);
  const double rbx = rby * seg.scale;

  double nrbx = rbx * ratio + diff;
  double nrby = rby * ratio + diff;

  if (maxDiff > 0.0) {
    nrbx = std::min(nrbx, rbx + maxDiff);
    nrby = std::min(nrby, rby + maxDiff);
  }

  seg.scale = nrbx / nrby;
  seg.r1 = nrby;
  if (seg.c > 0.0) {
    seg.r1 -= (seg.h - 1.0) * seg.c;
  }
}

std::vector<double> neurosegDistFilterLegacyLike(const Neuroseg& seg,
                                                 const FieldRangeLegacyLike& range,
                                                 const std::array<double, 3>* offpos,
                                                 double zToXYRatio)
{
  const int sizeX = range.size[0];
  const int sizeY = range.size[1];
  const int sizeZ = range.size[2];
  CHECK(sizeX >= 0);
  CHECK(sizeY >= 0);
  CHECK(sizeZ >= 0);
  const size_t n = static_cast<size_t>(sizeX) * static_cast<size_t>(sizeY) * static_cast<size_t>(sizeZ);

  std::vector<double> filter;
  neurosegDistFilterLegacyLikeInto(seg, range, offpos, zToXYRatio, filter);
  filter.resize(n);
  return filter;
}

void neurosegDistFilterLegacyLikeInto(const Neuroseg& seg,
                                      const FieldRangeLegacyLike& range,
                                      const std::array<double, 3>* offpos,
                                      double zToXYRatio,
                                      std::vector<double>& out)
{
  // Port of tz_neuroseg.c::Neuroseg_Dist_Filter(), written in an allocation-free style for hot loops.
  const int sizeX = range.size[0];
  const int sizeY = range.size[1];
  const int sizeZ = range.size[2];

  CHECK(sizeX >= 0);
  CHECK(sizeY >= 0);
  CHECK(sizeZ >= 0);

  const size_t n = static_cast<size_t>(sizeX) * static_cast<size_t>(sizeY) * static_cast<size_t>(sizeZ);
  if (out.size() < n) {
    out.resize(n);
  }

  const std::array<int, 3> coffset = range.firstCorner;
  const double coef = seg.c;

  const bool needZScale = testZScaleLegacyLike(zToXYRatio) != 0;
  const bool needRotateXZ = seg.theta != 0.0 || seg.psi != 0.0;
  const bool needScaleXRotateZ = seg.scale != 1.0 || seg.alpha != 0.0;

  const double ar0 = std::cos(seg.theta);
  const double ar1 = std::sin(seg.theta);
  const double ar2 = std::cos(seg.psi);
  const double ar3 = std::sin(seg.psi);

  const double cosA = std::cos(seg.alpha);
  const double sinA = std::sin(seg.alpha);

  size_t offset = 0;
  for (int k = 0; k < sizeZ; ++k) {
    for (int j = 0; j < sizeY; ++j) {
      for (int i = 0; i < sizeX; ++i) {
        std::array<double, 3> coord = {static_cast<double>(i + coffset[0]),
                                       static_cast<double>(j + coffset[1]),
                                       static_cast<double>(k + coffset[2])};

        if (needZScale) {
          // `coord` is iterating image-space stack voxels, while the local neuroseg geometry
          // is expressed in trace space where Z is expanded by `zToXYRatio`.
          coord[2] *= zToXYRatio;
        }

        if (offpos != nullptr) {
          coord[0] -= (*offpos)[0];
          coord[1] -= (*offpos)[1];
          coord[2] -= (*offpos)[2];
        }

        if (needRotateXZ) {
          // rotateXZLegacyLike(coord, seg.theta, seg.psi, inverse=1)
          const double inx = coord[0];
          const double iny = coord[1];
          const double inz = coord[2];

          double result0 = ar2 * inx + ar3 * iny;
          double result1 = iny * ar2 - inx * ar3;
          double result2 = inz * ar0 - result1 * ar1;
          result1 = inz * ar1 + result1 * ar0;

          coord[0] = result0;
          coord[1] = result1;
          coord[2] = result2;
        }

        if (needScaleXRotateZ) {
          // scaleXRotateZLegacyLike(coord, seg.scale, seg.alpha, inverse=1)
          const double x = coord[0];
          const double y = coord[1];
          const double tmp = (x * cosA + y * sinA) / seg.scale;
          coord[1] = -x * sinA + y * cosA;
          coord[0] = tmp;
        }

        if (coord[2] < -0.5 || coord[2] > seg.h - 0.5) {
          out[offset++] = 2.0;
          continue;
        }

        const double sigma = coef * coord[2] + seg.r1;
        const double sigma2 = sigma * sigma;
        const double d2 = coord[0] * coord[0] + coord[1] * coord[1];
        const double t = d2 / sigma2;
        out[offset++] = std::sqrt(t);
      }
    }
  }

  CHECK(offset == n);
}

namespace {

[[nodiscard]] double geo3dVectorAngle2LegacyLike(double x1, double y1, double z1, double x2, double y2, double z2)
{
  // Port of tz_geo3d_vector.c::Geo3d_Vector_Angle2().
  const double d1 = x1 * x1 + y1 * y1 + z1 * z1;
  if (d1 == 0.0) {
    return 0.0;
  }

  const double d2 = x2 * x2 + y2 * y2 + z2 * z2;
  if (d2 == 0.0) {
    return 0.0;
  }

  double d12 = (x1 * x2 + y1 * y2 + z1 * z2) / std::sqrt(d1 * d2);

  // Legacy uses ASSERT(fabs(round(d12)) <= 1.0).
  CHECK(std::fabs(std::round(d12)) <= 1.0) << "Invalid dot product: " << d12;

  // To avoid invalid acos input caused by rounding error.
  if (std::fabs(d12) > 1.0) {
    d12 = std::round(d12);
  }

  return std::acos(d12);
}

} // namespace

double neurosegAngleBetweenLegacyLike(const Neuroseg& seg1, const Neuroseg& seg2)
{
  double x1 = 0.0;
  double y1 = 0.0;
  double z1 = 0.0;
  geo3dOrientationNormalLegacyLike(seg1.theta, seg1.psi, x1, y1, z1);

  double x2 = 0.0;
  double y2 = 0.0;
  double z2 = 0.0;
  geo3dOrientationNormalLegacyLike(seg2.theta, seg2.psi, x2, y2, z2);

  return geo3dVectorAngle2LegacyLike(x1, y1, z1, x2, y2, z2);
}

bool neurosegHitTestLegacyLike(const Neuroseg& seg, double x, double y, double z)
{
  // Port of tz_neuroseg.c::Neuroseg_Hit_Test().
  if (z >= -0.5 && z <= seg.h - 0.5) {
    const double d2 = (x * x) / (seg.scale * seg.scale) + y * y;
    const double r = neurosegRadiusLegacyLike(seg, z);
    if (d2 <= r * r) {
      return true;
    }
  }

  return false;
}

double neurosegRxLegacyLike(const Neuroseg& seg, NeuroposReferenceLegacyLike ref)
{
  switch (ref) {
    case NeuroposReferenceLegacyLike::Bottom:
      return seg.r1 * seg.scale;
    case NeuroposReferenceLegacyLike::Top:
      return neurosegR2LegacyLike(seg) * seg.scale;
    case NeuroposReferenceLegacyLike::Center:
      return neurosegRCLegacyLike(seg) * seg.scale;
    default:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike value";
      return 0.0;
  }
}

double neurosegRyLegacyLike(const Neuroseg& seg, NeuroposReferenceLegacyLike ref)
{
  switch (ref) {
    case NeuroposReferenceLegacyLike::Bottom:
      return seg.r1;
    case NeuroposReferenceLegacyLike::Top:
      return neurosegR2LegacyLike(seg);
    case NeuroposReferenceLegacyLike::Center:
      return neurosegRCLegacyLike(seg);
    default:
      CHECK(false) << "Invalid NeuroposReferenceLegacyLike value";
      return 0.0;
  }
}

double
neurosegRxPLegacyLike(const Neuroseg& seg, const std::array<double, 3>& resolution, NeuroposReferenceLegacyLike ref)
{
  // Port of tz_neuroseg.c::Neuroseg_Rx_P().
  return neurosegRxLegacyLike(seg, ref) * resolution[0];
}

double
neurosegRyPLegacyLike(const Neuroseg& seg, const std::array<double, 3>& resolution, NeuroposReferenceLegacyLike ref)
{
  // Port of tz_neuroseg.c::Neuroseg_Ry_P().
  //
  // Note: The legacy implementation uses `cos(theta)` / `sin(theta)` (not squared),
  // which can yield a negative value under the sqrt for certain theta values.
  const double sx = resolution[0] * resolution[0];
  const double sy = resolution[1] * resolution[1];
  const double factor = std::sqrt(sx * std::cos(seg.theta) + sy * std::sin(seg.theta));
  return neurosegRyLegacyLike(seg, ref) * factor;
}

double neurosegRxZLegacyLike(const Neuroseg& seg, double z)
{
  return neurosegRadiusLegacyLike(seg, z) * seg.scale;
}

double neurosegRyZLegacyLike(const Neuroseg& seg, double z)
{
  return neurosegRadiusLegacyLike(seg, z);
}

double neurosegRxyZLegacyLike(const Neuroseg& seg, double z)
{
  return neurosegRadiusLegacyLike(seg, z) * std::sqrt(seg.scale);
}

Neuroseg nextNeurosegLegacyLike(const Neuroseg& seg1, double posStep)
{
  // Port of tz_neuroseg.c::Next_Neuroseg().
  Neuroseg seg2 = seg1;
  seg2.r1 = seg1.r1 + posStep * neurosegCoefLegacyLike(seg1) * (seg1.h - 1.0);

  if (seg2.r1 < NeurosegMinRLegacyLike) {
    seg2.r1 = NeurosegMinRLegacyLike;
  }

  if (NeurosegDefaultHLegacyLike > 1.0) {
    seg2.h = NeurosegDefaultHLegacyLike;
  }

  seg2.c = neurosegCoefLegacyLike(seg2);

  return seg2;
}

double neurofieldLegacyLike(double x, double y)
{
  const double t = x * x + y * y;
  const double value = (1.0 - t) * std::exp(-t);
  return value;
}

NeurosegSliceField neurosegSliceFieldLegacyLike(NeurosegFieldFunctionLegacyLike fieldFunc)
{
  if (fieldFunc == nullptr) {
    fieldFunc = neurofieldLegacyLike;
  }

  NeurosegSliceField out;
  // Matches tz_neuroseg.c::Neuroseg_Slice_Field() output length for end=1.65.
  out.points.reserve(213);
  out.values.reserve(213);

  auto addPlaneFieldPoint = [&](double x, double y) {
    const double v = fieldFunc(x, y);

    out.points.push_back({x, y, 0.0});
    out.values.push_back(v);

    if (x != 0.0) {
      out.points.push_back({-x, y, 0.0});
      out.values.push_back(v);
    }
    if (y != 0.0) {
      out.points.push_back({x, -y, 0.0});
      out.values.push_back(v);
      if (x != 0.0) {
        out.points.push_back({-x, -y, 0.0});
        out.values.push_back(v);
      }
    }
  };

  const double start = 0.2;
  const double end = 1.65;
  const double step = 0.2;

  const double range = (end - 0.05) * (end - 0.05) + 0.1;

  // (0, 0)
  addPlaneFieldPoint(0.0, 0.0);

  // y = 0.0, x = 0.2 : 1.6
  for (double x = start; x < end; x += step) {
    addPlaneFieldPoint(x, 0.0);
  }

  // x = 0.0, y = 0.2 : 1.6
  for (double y = start; y < end; y += step) {
    addPlaneFieldPoint(0.0, y);
  }

  // y = 0.2 : 0.8, x = 0.2 : 1.6, with radius check.
  for (double y = start; y < 0.85; y += step) {
    for (double x = start; x < end; x += step) {
      if (x * x + y * y < range) {
        addPlaneFieldPoint(x, y);
      }
    }
  }

  // y = 1.0, x = 0.2 : 1.6, with radius check.
  {
    const double y = 1.0;
    for (double x = start; x < 1.65; x += step) {
      if (x * x + y * y < range) {
        addPlaneFieldPoint(x, y);
      }
    }
  }

  // y = 1.2 : 1.4, x = 0.2 : 1.4, with radius check.
  for (double y = 1.2; y < 1.45; y += step) {
    for (double x = 0.2; x < 1.45; x += step) {
      if (x * x + y * y < range) {
        addPlaneFieldPoint(x, y);
      }
    }
  }

  // y = 1.6, x = 0.2 : 1.0, with radius check (end >= 1.6).
  if (end >= 1.6) {
    const double y = 1.6;
    for (double x = start; x < 1.05; x += step) {
      if (x * x + y * y < range) {
        addPlaneFieldPoint(x, y);
      }
    }
  }

  CHECK(out.points.size() == out.values.size());
  CHECK(out.points.size() == 213u) << "Neuroseg slice field size mismatch: " << out.points.size();

  return out;
}

Geo3dScalarField neurosegFieldZLegacyLike(const Neuroseg& seg, double z, double step)
{
  Geo3dScalarField field;
  neurosegFieldZLegacyLikeInto(seg, z, step, field);
  return field;
}

void neurosegFieldZLegacyLikeInto(const Neuroseg& seg, double z, double /*step*/, Geo3dScalarField& field)
{
  // Port of tz_neuroseg.c::Neuroseg_Field_Z().
  if (seg.r1 == 0.0 || seg.scale == 0.0) {
    field.points.clear();
    field.values.clear();
    return;
  }

  const double coef = neurosegCoefLegacyLike(seg);
  const double r = seg.r1 + coef * z;

  const double rScale = r * seg.scale;
  const double sqrtR = std::sqrt(r);
  const double sqrtRScale = sqrtR * std::sqrt(std::sqrt(seg.scale));

  static const NeurosegSliceField sliceCached = neurosegSliceFieldLegacyLike(nullptr);
  const NeurosegSliceField& slice = sliceCached;
  CHECK(slice.points.size() == slice.values.size());

  field.points = slice.points;
  field.values = slice.values;

  CHECK(field.points.size() == field.values.size());

  double weight = 0.0;
  for (size_t i = 0; i < field.points.size(); ++i) {
    neurosegScaleFieldSlice0LegacyLike(field.points[i], field.values[i], rScale, r, sqrtRScale);
    field.points[i][2] = z;
    weight += std::fabs(field.values[i]);
  }

  for (double& v : field.values) {
    v /= weight;
  }

  if (seg.alpha != 0.0) {
    rotateZLegacyLike(std::span(field.points.data(), field.points.size()), seg.alpha, 0);
  }

  if (seg.curvature >= NeurosegMinCurvatureLegacyLike) {
    double curvature = seg.curvature;
    if (curvature > TzPiLegacyLike) {
      curvature = TzPiLegacyLike;
    }
    geo3dPointArrayBendLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.h / curvature);
  }

  if (seg.theta != 0.0 || seg.psi != 0.0) {
    rotateXZLegacyLike(std::span(field.points.data(), field.points.size()), seg.theta, seg.psi, 0);
  }
}

NeurosegSliceField neurosegSliceFieldPLegacyLike(NeurosegFieldFunctionLegacyLike fieldFunc)
{
  if (fieldFunc == nullptr) {
    fieldFunc = neurofieldLegacyLike;
  }

  NeurosegSliceField out;
  // Matches tz_neuroseg.c::Neuroseg_Slice_Field_P() output length.
  out.points.reserve(69);
  out.values.reserve(69);

  auto addPlaneFieldPoint = [&](double x, double y) {
    const double v = fieldFunc(x, y);

    out.points.push_back({x, y, 0.0});
    out.values.push_back(v);

    if (x != 0.0) {
      out.points.push_back({-x, y, 0.0});
      out.values.push_back(v);
    }
    if (y != 0.0) {
      out.points.push_back({x, -y, 0.0});
      out.values.push_back(v);
      if (x != 0.0) {
        out.points.push_back({-x, -y, 0.0});
        out.values.push_back(v);
      }
    }
  };

  const double step = 0.2;
  const double start = 0.2;
  const double end = 0.85;

  // (0, 0)
  addPlaneFieldPoint(0.0, 0.0);

  // y = 0.0, x = 0.2 : 0.8
  for (double x = start; x < end; x += step) {
    addPlaneFieldPoint(x, 0.0);
  }

  // x = 0.0, y = 0.2 : 0.8
  for (double y = start; y < end; y += step) {
    addPlaneFieldPoint(0.0, y);
  }

  double y = start;
  for (; y < 0.45; y += step) {
    for (double x = start; x < end; x += step) {
      addPlaneFieldPoint(x, y);
    }
  }

  if (y < 0.65) {
    y = 0.6;
    for (double x = start; x < 0.65; x += step) {
      addPlaneFieldPoint(x, y);
    }
  }

  y = 0.8;
  for (double x = start; x < 0.45; x += step) {
    addPlaneFieldPoint(x, y);
  }

  CHECK(out.points.size() == out.values.size());
  CHECK(out.points.size() == 69u) << "Neuroseg slice field (P) size mismatch: " << out.points.size();

  return out;
}

void neurosegFieldSFastLegacyLikeInto(const Neuroseg& seg,
                                      NeurosegFieldFunctionLegacyLike fieldFunc,
                                      Geo3dScalarField& out)
{
  if (seg.r1 == 0.0 || seg.scale == 0.0) {
    out.points.clear();
    out.values.clear();
    return;
  }

  constexpr int nslice = NeurosegDefaultHSlicesLegacyLike;
  static_assert(nslice > 1, "neurosegFieldSFastLegacyLikeInto expects nslice > 1.");

  const NeurosegSliceField* slicePtr = nullptr;
  NeurosegSliceField sliceOwned;
  if (fieldFunc == nullptr || fieldFunc == neurofieldLegacyLike) {
    static const NeurosegSliceField sliceCached = neurosegSliceFieldLegacyLike(nullptr);
    slicePtr = &sliceCached;
  } else {
    sliceOwned = neurosegSliceFieldLegacyLike(fieldFunc);
    slicePtr = &sliceOwned;
  }
  const NeurosegSliceField& slice = *slicePtr;

  const int length = static_cast<int>(slice.points.size());
  CHECK(length == 213);

  const size_t total = static_cast<size_t>(length) * static_cast<size_t>(nslice);
  out.points.resize(total);
  out.values.resize(total);

  std::memcpy(out.points.data(), slice.points.data(), sizeof(out.points[0]) * static_cast<size_t>(length));
  std::memcpy(out.values.data(), slice.values.data(), sizeof(out.values[0]) * static_cast<size_t>(length));

  const double zStart = 0.0;
  const double zStep = (seg.h - 1.0) / static_cast<double>(nslice - 1);

  const double coef = neurosegCoefLegacyLike(seg) * zStep;
  double r = seg.r1;
  double z = zStart;

  const double sqrtSqrtScale = std::sqrt(std::sqrt(seg.scale));

  if (coef != 0.0) {
    std::memcpy(out.values.data() + static_cast<size_t>(length),
                out.values.data(),
                sizeof(out.values[0]) * static_cast<size_t>(length));
  }

  // Slice 0: scale + normalize values in place.
  {
    const double rScale = r * seg.scale;
    const double sqrtR = std::sqrt(r);
    const double sqrtRScale = sqrtR * sqrtSqrtScale;

    double weight = 0.0;
    for (int i = 0; i < length; ++i) {
      out.points[static_cast<size_t>(i)][2] = z;
      neurosegScaleFieldSlice0LegacyLike(out.points[static_cast<size_t>(i)],
                                         out.values[static_cast<size_t>(i)],
                                         rScale,
                                         r,
                                         sqrtRScale);
      weight += std::fabs(out.values[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < length; ++i) {
      out.values[static_cast<size_t>(i)] /= weight;
    }
  }

  if (coef == 0.0) {
    std::memcpy(out.values.data() + static_cast<size_t>(length),
                out.values.data(),
                sizeof(out.values[0]) * static_cast<size_t>(length));
  }

  // Remaining slices.
  for (int j = 1; j < nslice; ++j) {
    z += zStep;

    if (coef != 0.0) {
      r += coef;
    }

    const size_t offset = static_cast<size_t>(j) * static_cast<size_t>(length);

    // Copy base points (already scaled in slice 0).
    std::memcpy(out.points.data() + offset, out.points.data(), sizeof(out.points[0]) * static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
      out.points[offset + static_cast<size_t>(i)][2] = z;
    }

    // Values:
    // - j == 1 and coef != 0.0: keep the pre-copied raw values.
    // - j > 1: copy normalized slice-0 values before scaling.
    if (j > 1) {
      std::memcpy(out.values.data() + offset, out.values.data(), sizeof(out.values[0]) * static_cast<size_t>(length));
    }

    if (coef != 0.0) {
      const double rScale = r * seg.scale;
      const double sqrtR = std::sqrt(r);
      const double sqrtRScale = sqrtR * sqrtSqrtScale;

      double weight = 0.0;
      for (int i = 0; i < length; ++i) {
        out.points[offset + static_cast<size_t>(i)][2] = z;
        neurosegScaleFieldConeSliceLegacyLike(out.points[offset + static_cast<size_t>(i)],
                                              out.values[offset + static_cast<size_t>(i)],
                                              r,
                                              seg.scale,
                                              rScale,
                                              sqrtRScale);
        weight += std::fabs(out.values[offset + static_cast<size_t>(i)]);
      }
      for (int i = 0; i < length; ++i) {
        out.values[offset + static_cast<size_t>(i)] /= weight;
      }
    }
  }

  if (seg.alpha != 0.0) {
    rotateZLegacyLike(std::span(out.points.data(), out.points.size()), seg.alpha, 0);
  }

  if (seg.curvature >= NeurosegMinCurvatureLegacyLike) {
    double curvature = seg.curvature;
    if (curvature > TzPiLegacyLike) {
      curvature = TzPiLegacyLike;
    }
    geo3dPointArrayBendLegacyLike(out.points.data(), static_cast<int>(out.points.size()), seg.h / curvature);
  }

  if (seg.theta != 0.0 || seg.psi != 0.0) {
    rotateXZLegacyLike(std::span(out.points.data(), out.points.size()), seg.theta, seg.psi, 0);
  }
}

void neurosegFieldSpLegacyLikeInto(const Neuroseg& seg,
                                   NeurosegFieldFunctionLegacyLike fieldFunc,
                                   Geo3dScalarField& out)
{
  if (seg.r1 == 0.0 || seg.scale == 0.0) {
    out.points.clear();
    out.values.clear();
    return;
  }

  constexpr int nslice = NeurosegDefaultHSlicesLegacyLike;

  const NeurosegSliceField* slicePtr = nullptr;
  const double* normalizedValuesPtr = nullptr;
  NeurosegSliceField sliceOwned;
  std::array<double, 69> normalizedValuesOwned{};
  if (fieldFunc == nullptr || fieldFunc == neurofieldLegacyLike) {
    static const NeurosegSliceField sliceCached = neurosegSliceFieldPLegacyLike(nullptr);
    slicePtr = &sliceCached;
    static const std::array<double, 69> normalizedValuesCached = [&]() {
      std::array<double, 69> outArr{};
      double weight = 0.0;
      for (const double v : sliceCached.values) {
        weight += std::fabs(v);
      }
      for (size_t i = 0; i < sliceCached.values.size(); ++i) {
        outArr[i] = sliceCached.values[i] / weight;
      }
      return outArr;
    }();
    normalizedValuesPtr = normalizedValuesCached.data();
  } else {
    sliceOwned = neurosegSliceFieldPLegacyLike(fieldFunc);
    slicePtr = &sliceOwned;
    double weight = 0.0;
    for (const double v : sliceOwned.values) {
      weight += std::fabs(v);
    }
    for (size_t i = 0; i < sliceOwned.values.size(); ++i) {
      normalizedValuesOwned[i] = sliceOwned.values[i] / weight;
    }
    normalizedValuesPtr = normalizedValuesOwned.data();
  }
  const NeurosegSliceField& slice = *slicePtr;

  const int length = static_cast<int>(slice.points.size());
  CHECK(length == 69);

  CHECK(normalizedValuesPtr != nullptr);

  const size_t total = static_cast<size_t>(length) * static_cast<size_t>(nslice);
  out.points.resize(total);
  out.values.resize(total);

  const double zStart = 0.0;
  const double zStep = (seg.h - 1.0) / static_cast<double>(nslice - 1);

  const double coef = neurosegCoefLegacyLike(seg) * zStep;
  double r = seg.r1;
  double z = zStart;

  for (int j = 0; j < nslice; ++j) {
    const size_t offset = static_cast<size_t>(j) * static_cast<size_t>(length);

    for (int i = 0; i < length; ++i) {
      const auto& p0 = slice.points[static_cast<size_t>(i)];
      out.points[offset + static_cast<size_t>(i)] = {p0[0] * (r * seg.scale), p0[1] * r, z};
    }
    std::memcpy(out.values.data() + offset, normalizedValuesPtr, sizeof(out.values[0]) * static_cast<size_t>(length));

    z += zStep;
    r += coef;
  }

  if (seg.alpha != 0.0) {
    rotateZLegacyLike(std::span(out.points.data(), out.points.size()), seg.alpha, 0);
  }

  if (seg.curvature >= NeurosegMinCurvatureLegacyLike) {
    double curvature = seg.curvature;
    if (curvature > TzPiLegacyLike) {
      curvature = TzPiLegacyLike;
    }
    geo3dPointArrayBendLegacyLike(out.points.data(), static_cast<int>(out.points.size()), seg.h / curvature);
  }

  if (seg.theta != 0.0 || seg.psi != 0.0) {
    rotateXZLegacyLike(std::span(out.points.data(), out.points.size()), seg.theta, seg.psi, 0);
  }
}

Geo3dScalarField neurosegFieldSFastLegacyLike(const Neuroseg& seg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;
  neurosegFieldSFastLegacyLikeInto(seg, fieldFunc, field);
  return field;
}

Geo3dScalarField neurosegFieldSpLegacyLike(const Neuroseg& seg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;
  neurosegFieldSpLegacyLikeInto(seg, fieldFunc, field);
  return field;
}

} // namespace nim
