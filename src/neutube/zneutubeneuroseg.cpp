#include "zneutubeneuroseg.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dpointarray.h"
#include "zneutubegeo3dscalarfield.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim::neutube {

namespace {

constexpr double NeurosegMinRLegacyLike = 0.5;
constexpr int NeurosegDefaultHSlicesLegacyLike = 11;
constexpr double NeurosegMinCurvatureLegacyLike = 0.2;
constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;

[[nodiscard]] double neurosegCoefLegacyLike(const Neuroseg& seg)
{
  if (seg.h == 1.0) {
    return seg.c;
  }
  return std::max(seg.c, (NeurosegMinRLegacyLike - seg.r1) / (seg.h - 1.0));
}

void neurosegScaleFieldSlice0LegacyLike(std::array<double, 3>* pt,
                                        double* value,
                                        double rScale,
                                        double r,
                                        double sqrtRScale)
{
  CHECK(pt != nullptr);
  CHECK(value != nullptr);

  if (value[0] >= 0.0) {
    (*pt)[0] *= rScale;
    (*pt)[1] *= r;
    value[0] *= sqrtRScale;
  } else {
    const double norm = std::sqrt((*pt)[0] * (*pt)[0] + (*pt)[1] * (*pt)[1]);
    double alpha = norm - 1.0;

    (*pt)[0] *= rScale / norm;
    (*pt)[1] *= r / norm;

    const double enorm = (*pt)[0] * (*pt)[0] + (*pt)[1] * (*pt)[1];
    if (enorm < 1.0) {
      alpha /= std::sqrt(enorm);
    } else if (enorm > 4.0) {
      alpha /= std::sqrt(enorm);
      alpha = alpha + alpha;
    }

    (*pt)[0] *= 1.0 + alpha;
    (*pt)[1] *= 1.0 + alpha;
  }
}

void neurosegScaleFieldConeSliceLegacyLike(std::array<double, 3>* pt,
                                           double* value,
                                           double r,
                                           double scale,
                                           double rScale,
                                           double sqrtRScale)
{
  CHECK(pt != nullptr);
  CHECK(value != nullptr);

  if (value[0] >= 0.0) {
    (*pt)[0] *= rScale;
    (*pt)[1] *= r;
    value[0] *= sqrtRScale;
  } else {
    const double norm = std::sqrt((*pt)[0] * (*pt)[0] + (*pt)[1] * (*pt)[1]);
    double alpha = norm - 1.0;

    // positive boundary
    const double rnorm = r / norm;
    (*pt)[0] *= rnorm * scale;
    (*pt)[1] *= rnorm;

    // length to positive boundary
    const double enorm = (*pt)[0] * (*pt)[0] + (*pt)[1] * (*pt)[1];
    if (enorm < 1.0) {
      alpha /= std::sqrt(enorm);
    } else if (enorm > 4.0) {
      alpha *= 2.0 / std::sqrt(enorm);
    }

    (*pt)[0] *= 1.0 + alpha;
    (*pt)[1] *= 1.0 + alpha;
  }
}

} // namespace

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

Geo3dScalarField neurosegFieldSFastLegacyLike(const Neuroseg& seg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;

  if (seg.r1 == 0.0 || seg.scale == 0.0) {
    return field;
  }

  constexpr int nslice = NeurosegDefaultHSlicesLegacyLike;

  NeurosegSliceField slice = neurosegSliceFieldLegacyLike(fieldFunc);
  const int length = static_cast<int>(slice.points.size());
  CHECK(length == 213);

  field.points.resize(static_cast<size_t>(length) * static_cast<size_t>(nslice));
  field.values.resize(static_cast<size_t>(length) * static_cast<size_t>(nslice));

  for (int i = 0; i < length; ++i) {
    field.points[static_cast<size_t>(i)] = slice.points[static_cast<size_t>(i)];
    field.values[static_cast<size_t>(i)] = slice.values[static_cast<size_t>(i)];
  }

  const double zStart = 0.0;
  const double zStep = (seg.h - 1.0) / static_cast<double>(nslice - 1);

  const double coef = neurosegCoefLegacyLike(seg) * zStep;
  double r = seg.r1;
  double z = zStart;

  const double sqrtSqrtScale = std::sqrt(std::sqrt(seg.scale));

  if (coef != 0.0 && nslice > 1) {
    for (int i = 0; i < length; ++i) {
      field.values[static_cast<size_t>(length + i)] = field.values[static_cast<size_t>(i)];
    }
  }

  // Slice 0: scale + normalize values in place.
  {
    const double rScale = r * seg.scale;
    const double sqrtR = std::sqrt(r);
    const double sqrtRScale = sqrtR * sqrtSqrtScale;

    double weight = 0.0;
    for (int i = 0; i < length; ++i) {
      field.points[static_cast<size_t>(i)][2] = z;
      neurosegScaleFieldSlice0LegacyLike(&field.points[static_cast<size_t>(i)],
                                         &field.values[static_cast<size_t>(i)],
                                         rScale,
                                         r,
                                         sqrtRScale);
      weight += std::fabs(field.values[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < length; ++i) {
      field.values[static_cast<size_t>(i)] /= weight;
    }
  }

  if (coef == 0.0 && nslice > 1) {
    for (int i = 0; i < length; ++i) {
      field.values[static_cast<size_t>(length + i)] = field.values[static_cast<size_t>(i)];
    }
  }

  // Remaining slices.
  for (int j = 1; j < nslice; ++j) {
    z += zStep;

    if (coef != 0.0) {
      r += coef;
    }

    const size_t offset = static_cast<size_t>(j) * static_cast<size_t>(length);

    // Copy base points (already scaled in slice 0).
    for (int i = 0; i < length; ++i) {
      field.points[offset + static_cast<size_t>(i)] = field.points[static_cast<size_t>(i)];
      field.points[offset + static_cast<size_t>(i)][2] = z;
    }

    // Values:
    // - j == 1 and coef != 0.0: keep the pre-copied raw values.
    // - j > 1: copy normalized slice-0 values before scaling.
    if (j > 1) {
      for (int i = 0; i < length; ++i) {
        field.values[offset + static_cast<size_t>(i)] = field.values[static_cast<size_t>(i)];
      }
    }

    if (coef != 0.0) {
      const double rScale = r * seg.scale;
      const double sqrtR = std::sqrt(r);
      const double sqrtRScale = sqrtR * sqrtSqrtScale;

      double weight = 0.0;
      for (int i = 0; i < length; ++i) {
        field.points[offset + static_cast<size_t>(i)][2] = z;
        neurosegScaleFieldConeSliceLegacyLike(&field.points[offset + static_cast<size_t>(i)],
                                              &field.values[offset + static_cast<size_t>(i)],
                                              r,
                                              seg.scale,
                                              rScale,
                                              sqrtRScale);
        weight += std::fabs(field.values[offset + static_cast<size_t>(i)]);
      }
      for (int i = 0; i < length; ++i) {
        field.values[offset + static_cast<size_t>(i)] /= weight;
      }
    }
  }

  if (seg.alpha != 0.0) {
    rotateZLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.alpha, 0);
  }

  if (seg.curvature >= NeurosegMinCurvatureLegacyLike) {
    double curvature = seg.curvature;
    if (curvature > TzPiLegacyLike) {
      curvature = TzPiLegacyLike;
    }
    geo3dPointArrayBendLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.h / curvature);
  }

  if (seg.theta != 0.0 || seg.psi != 0.0) {
    rotateXZLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.theta, seg.psi, 0);
  }

  return field;
}

Geo3dScalarField neurosegFieldSpLegacyLike(const Neuroseg& seg, NeurosegFieldFunctionLegacyLike fieldFunc)
{
  Geo3dScalarField field;

  if (seg.r1 == 0.0 || seg.scale == 0.0) {
    return field;
  }

  constexpr int nslice = NeurosegDefaultHSlicesLegacyLike;

  NeurosegSliceField slice = neurosegSliceFieldPLegacyLike(fieldFunc);
  const int length = static_cast<int>(slice.points.size());
  CHECK(length == 69);

  field.points.resize(static_cast<size_t>(length) * static_cast<size_t>(nslice));
  field.values.resize(static_cast<size_t>(length) * static_cast<size_t>(nslice));

  const double zStart = 0.0;
  const double zStep = (seg.h - 1.0) / static_cast<double>(nslice - 1);

  const double coef = neurosegCoefLegacyLike(seg) * zStep;
  double r = seg.r1;
  double z = zStart;

  for (int j = 0; j < nslice; ++j) {
    const size_t offset = static_cast<size_t>(j) * static_cast<size_t>(length);

    double weight = 0.0;
    for (int i = 0; i < length; ++i) {
      const auto& p0 = slice.points[static_cast<size_t>(i)];
      field.points[offset + static_cast<size_t>(i)] = {p0[0] * (r * seg.scale), p0[1] * r, z};

      const double v = slice.values[static_cast<size_t>(i)];
      field.values[offset + static_cast<size_t>(i)] = v;
      weight += std::fabs(v);
    }

    for (int i = 0; i < length; ++i) {
      field.values[offset + static_cast<size_t>(i)] /= weight;
    }

    z += zStep;
    r += coef;
  }

  if (seg.alpha != 0.0) {
    rotateZLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.alpha, 0);
  }

  if (seg.curvature >= NeurosegMinCurvatureLegacyLike) {
    double curvature = seg.curvature;
    if (curvature > TzPiLegacyLike) {
      curvature = TzPiLegacyLike;
    }
    geo3dPointArrayBendLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.h / curvature);
  }

  if (seg.theta != 0.0 || seg.psi != 0.0) {
    rotateXZLegacyLike(field.points.data(), static_cast<int>(field.points.size()), seg.theta, seg.psi, 0);
  }

  return field;
}

} // namespace nim::neutube
