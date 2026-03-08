#include "zneutubegeo3dscalarfield.h"

#include "zneutubeimgsampling.h"
#include "zneutubestackfitscore.h"

#include "zvoxelvolume.h"
#include "zvoxelvolumedense.h"

#include "zlog.h"
#include "zimg.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] double nanValue()
{
  return std::numeric_limits<double>::quiet_NaN();
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
                                                            double zToXYRatio,
                                                            size_t c,
                                                            size_t t)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  for (size_t i = 0; i < field.size(); ++i) {
    const auto& p = field.points[i];
    const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] / zToXYRatio);
    signal[i] = pointSampleLegacyLike(stack, p[0], p[1], z, c, t);
  }
  return signal;
}

std::vector<double>
geo3dScalarFieldStackSamplingLegacyLike(const Geo3dScalarField& field, const ZVoxelVolume& stack, double zToXYRatio)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  for (size_t i = 0; i < field.size(); ++i) {
    const auto& p = field.points[i];
    const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] / zToXYRatio);
    signal[i] = pointSampleLegacyLike(stack, p[0], p[1], z);
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field,
                                                                    const ZImg& stack,
                                                                    double zToXYRatio,
                                                                    size_t c,
                                                                    size_t t)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zToXYRatio, c, t);
  for (size_t i = 0; i < field.size(); ++i) {
    signal[i] *= field.values[i];
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field,
                                                                    const ZVoxelVolume& stack,
                                                                    double zToXYRatio)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zToXYRatio);
  for (size_t i = 0; i < field.size(); ++i) {
    signal[i] *= field.values[i];
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingMaskedLegacyLike(const Geo3dScalarField& field,
                                                                  const ZImg& stack,
                                                                  double zToXYRatio,
                                                                  const ZImg& mask,
                                                                  size_t c,
                                                                  size_t t)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  if (zToXYRatio == 1.0) {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_M().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      if (pointHitMaskLegacyLike(mask, p[0], p[1], p[2], c, t)) {
        signal[i] = nanValue();
      } else {
        signal[i] = pointSampleLegacyLike(stack, p[0], p[1], p[2], c, t);
      }
    }
  } else {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_Zm().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      const double z = p[2] / zToXYRatio;
      if (pointSampleLegacyLike(mask, p[0], p[1], z, c, t) > 0.0) {
        signal[i] = nanValue();
      } else {
        signal[i] = pointSampleLegacyLike(stack, p[0], p[1], z, c, t);
      }
    }
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingMaskedLegacyLike(const Geo3dScalarField& field,
                                                                  const ZVoxelVolume& stack,
                                                                  double zToXYRatio,
                                                                  const ZVoxelVolume& mask)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  if (zToXYRatio == 1.0) {
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
      const double z = p[2] / zToXYRatio;
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
                                            double zToXYRatio,
                                            StackFitScore* fs,
                                            size_t c,
                                            size_t t)
{
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  CHECK(!stack.isEmpty());
  CHECK(stack.numChannels() > c);
  CHECK(stack.numTimes() > t);

  static thread_local std::vector<double> signalScratch;
  signalScratch.resize(field.size());
  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();
  const size_t channelVoxelNumber = width * height * depth;

  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    const TVoxel* array = stack.timeData<TVoxel>(t);
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] / zToXYRatio);
      signalScratch[i] =
        pointSampleLegacyLikeTypedFast<TVoxel>(array, width, height, depth, channelVoxelNumber, c, p[0], p[1], z);
    }
  });

  return computeStackFitScoresLegacyLike(std::span<const double>(field.values.data(), field.values.size()),
                                         std::span<const double>(signalScratch.data(), signalScratch.size()),
                                         fs);
}

double geo3dScalarFieldStackScoreLegacyLike(const Geo3dScalarField& field,
                                            const ZVoxelVolume& stack,
                                            double zToXYRatio,
                                            StackFitScore* fs)
{
  // Hot path: this is called extremely frequently by the perceptor optimizer.
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  // This function is called in tight loops (e.g. local-neuroseg fitting). Avoid per-call
  // allocations by reusing a thread-local scratch buffer for sampled signal values.
  static thread_local std::vector<double> signalScratch;
  signalScratch.resize(field.size());
  for (size_t i = 0; i < field.size(); ++i) {
    const auto& p = field.points[i];
    const double z = (zToXYRatio == 1.0) ? p[2] : (p[2] / zToXYRatio);
    signalScratch[i] = pointSampleLegacyLike(stack, p[0], p[1], z);
  }

  return computeStackFitScoresLegacyLike(std::span<const double>(field.values.data(), field.values.size()),
                                         std::span<const double>(signalScratch.data(), signalScratch.size()),
                                         fs);
}

double geo3dScalarFieldStackScoreMaskedLegacyLike(const Geo3dScalarField& field,
                                                  const ZImg& stack,
                                                  double zToXYRatio,
                                                  const ZImg& mask,
                                                  StackFitScore* fs,
                                                  size_t c,
                                                  size_t t)
{
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  static thread_local std::vector<double> signalScratch;
  signalScratch.resize(field.size());

  if (zToXYRatio == 1.0) {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_M().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      if (pointHitMaskLegacyLike(mask, p[0], p[1], p[2], c, t)) {
        signalScratch[i] = nanValue();
      } else {
        signalScratch[i] = pointSampleLegacyLike(stack, p[0], p[1], p[2], c, t);
      }
    }
  } else {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_Zm().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      const double z = p[2] / zToXYRatio;
      if (pointSampleLegacyLike(mask, p[0], p[1], z, c, t) > 0.0) {
        signalScratch[i] = nanValue();
      } else {
        signalScratch[i] = pointSampleLegacyLike(stack, p[0], p[1], z, c, t);
      }
    }
  }

  return computeStackFitScoresMaskedLegacyLike(std::span<const double>(field.values.data(), field.values.size()),
                                               std::span<const double>(signalScratch.data(), signalScratch.size()),
                                               fs);
}

double geo3dScalarFieldStackScoreMaskedLegacyLike(const Geo3dScalarField& field,
                                                  const ZVoxelVolume& stack,
                                                  double zToXYRatio,
                                                  const ZVoxelVolume& mask,
                                                  StackFitScore* fs)
{
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  // Called in tight loops; avoid per-call allocations by reusing a thread-local scratch buffer.
  static thread_local std::vector<double> signalScratch;
  signalScratch.resize(field.size());

  if (zToXYRatio == 1.0) {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_M().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      if (pointHitMaskLegacyLike(mask, p[0], p[1], p[2])) {
        signalScratch[i] = nanValue();
      } else {
        signalScratch[i] = pointSampleLegacyLike(stack, p[0], p[1], p[2]);
      }
    }
  } else {
    // Matches Geo3d_Scalar_Field_Stack_Sampling_M() -> Stack_Points_Sampling_Zm().
    for (size_t i = 0; i < field.size(); ++i) {
      const auto& p = field.points[i];
      const double z = p[2] / zToXYRatio;
      if (pointSampleLegacyLike(mask, p[0], p[1], z) > 0.0) {
        signalScratch[i] = nanValue();
      } else {
        signalScratch[i] = pointSampleLegacyLike(stack, p[0], p[1], z);
      }
    }
  }

  return computeStackFitScoresMaskedLegacyLike(std::span<const double>(field.values.data(), field.values.size()),
                                               std::span<const double>(signalScratch.data(), signalScratch.size()),
                                               fs);
}

} // namespace nim
