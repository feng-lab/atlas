#include "zneutubegeo3dscalarfield.h"

#include "zneutubeimgsampling.h"
#include "zneutubestackfitscore.h"

#include "zvoxelvolume.h"
#include "zvoxelvolumedense.h"

#include "zlog.h"

#include <cmath>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] double nanValue()
{
  return std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] double scaledZ(double z, double zScale)
{
  if (zScale == 1.0) {
    return z;
  }
  return z * zScale;
}

} // namespace

std::array<double, 3> geo3dScalarFieldCenterLegacyLike(const Geo3dScalarField& field)
{
  CHECK(field.points.size() == field.values.size());
  CHECK(!field.points.empty());

  std::array<double, 3> center = {0.0, 0.0, 0.0};
  for (const auto& p : field.points) {
    center[0] += p[0];
    center[1] += p[1];
    center[2] += p[2];
  }

  const double n = static_cast<double>(field.points.size());
  center[0] /= n;
  center[1] /= n;
  center[2] /= n;
  return center;
}

std::array<double, 3> geo3dScalarFieldCentroidLegacyLike(const Geo3dScalarField& field)
{
  CHECK(field.points.size() == field.values.size());
  CHECK(!field.points.empty());

  double weight = 0.0;
  std::array<double, 3> centroid = {0.0, 0.0, 0.0};

  for (size_t i = 0; i < field.points.size(); ++i) {
    const double v = field.values[i];
    if (!std::isnan(v)) {
      weight += v;
      centroid[0] += field.points[i][0] * v;
      centroid[1] += field.points[i][1] * v;
      centroid[2] += field.points[i][2] * v;
    }
  }

  if (weight == 0.0) {
    return geo3dScalarFieldCenterLegacyLike(field);
  }

  centroid[0] /= weight;
  centroid[1] /= weight;
  centroid[2] /= weight;
  return centroid;
}

std::vector<double> geo3dScalarFieldStackSamplingLegacyLike(const Geo3dScalarField& field,
                                                            const ZImg& stack,
                                                            double zScale,
                                                            size_t c,
                                                            size_t t)
{
  const ZImg view = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume vol(view);
  return geo3dScalarFieldStackSamplingLegacyLike(field, vol, zScale);
}

std::vector<double>
geo3dScalarFieldStackSamplingLegacyLike(const Geo3dScalarField& field, const ZVoxelVolume& stack, double zScale)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  for (size_t i = 0; i < field.size(); ++i) {
    const auto& p = field.points[i];
    signal[i] = pointSampleLegacyLike(stack, p[0], p[1], scaledZ(p[2], zScale));
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field,
                                                                    const ZImg& stack,
                                                                    double zScale,
                                                                    size_t c,
                                                                    size_t t)
{
  const ZImg view = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume vol(view);
  return geo3dScalarFieldStackSamplingWeightedLegacyLike(field, vol, zScale);
}

std::vector<double>
geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field, const ZVoxelVolume& stack, double zScale)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zScale);
  for (size_t i = 0; i < field.size(); ++i) {
    signal[i] *= field.values[i];
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingMaskedLegacyLike(const Geo3dScalarField& field,
                                                                  const ZImg& stack,
                                                                  double zScale,
                                                                  const ZImg& mask,
                                                                  size_t c,
                                                                  size_t t)
{
  const ZImg stackView = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZImg maskView = mask.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume stackVol(stackView);
  const ZDenseVoxelVolume maskVol(maskView);
  return geo3dScalarFieldStackSamplingMaskedLegacyLike(field, stackVol, zScale, maskVol);
}

std::vector<double> geo3dScalarFieldStackSamplingMaskedLegacyLike(const Geo3dScalarField& field,
                                                                  const ZVoxelVolume& stack,
                                                                  double zScale,
                                                                  const ZVoxelVolume& mask)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  if (zScale == 1.0) {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_M().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      if (pointHitMaskLegacyLike(mask, p[0], p[1], p[2])) {
        signal[i] = nanValue();
      } else {
        signal[i] = pointSampleLegacyLike(stack, p[0], p[1], p[2]);
      }
    }
  } else {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_Zm().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      const double z = p[2] * zScale;
      if (pointSampleLegacyLike(mask, p[0], p[1], z) > 0.0) {
        signal[i] = nanValue();
      } else {
        signal[i] = pointSampleLegacyLike(stack, p[0], p[1], z);
      }
    }
  }
  return signal;
}

double geo3dScalarFieldStackScoreLegacyLike(const Geo3dScalarField& field,
                                            const ZImg& stack,
                                            double zScale,
                                            StackFitScore* fs,
                                            size_t c,
                                            size_t t)
{
  const ZImg view = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume vol(view);
  return geo3dScalarFieldStackScoreLegacyLike(field, vol, zScale, fs);
}

double geo3dScalarFieldStackScoreLegacyLike(const Geo3dScalarField& field,
                                            const ZVoxelVolume& stack,
                                            double zScale,
                                            StackFitScore* fs)
{
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zScale);
  return computeStackFitScoresLegacyLike(field.values.data(), signal.data(), field.size(), fs);
}

double geo3dScalarFieldStackScoreMaskedLegacyLike(const Geo3dScalarField& field,
                                                  const ZImg& stack,
                                                  double zScale,
                                                  const ZImg& mask,
                                                  StackFitScore* fs,
                                                  size_t c,
                                                  size_t t)
{
  const ZImg stackView = stack.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZImg maskView = mask.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  const ZDenseVoxelVolume stackVol(stackView);
  const ZDenseVoxelVolume maskVol(maskView);
  return geo3dScalarFieldStackScoreMaskedLegacyLike(field, stackVol, zScale, maskVol, fs);
}

double geo3dScalarFieldStackScoreMaskedLegacyLike(const Geo3dScalarField& field,
                                                  const ZVoxelVolume& stack,
                                                  double zScale,
                                                  const ZVoxelVolume& mask,
                                                  StackFitScore* fs)
{
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  std::vector<double> signal = geo3dScalarFieldStackSamplingMaskedLegacyLike(field, stack, zScale, mask);
  return computeStackFitScoresMaskedLegacyLike(field.values.data(), signal.data(), field.size(), fs);
}

} // namespace nim
