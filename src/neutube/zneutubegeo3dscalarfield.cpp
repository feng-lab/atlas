#include "zneutubegeo3dscalarfield.h"

#include "zneutubeimgsampling.h"
#include "zneutubestackfitscore.h"

#include "zlog.h"

#include <limits>

namespace nim::neutube {

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

std::vector<double> geo3dScalarFieldStackSamplingLegacyLike(const Geo3dScalarField& field,
                                                            const ZImg& stack,
                                                            double zScale,
                                                            size_t c,
                                                            size_t t)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  for (size_t i = 0; i < field.size(); ++i) {
    const auto& p = field.points[i];
    signal[i] = pointSampleLegacyLike(stack, p[0], p[1], scaledZ(p[2], zScale), c, t);
  }
  return signal;
}

std::vector<double> geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field,
                                                                    const ZImg& stack,
                                                                    double zScale,
                                                                    size_t c,
                                                                    size_t t)
{
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zScale, c, t);
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
  CHECK(field.points.size() == field.values.size());

  std::vector<double> signal(field.size(), 0.0);
  if (zScale == 1.0) {
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
      const double z = p[2] * zScale;
      if (pointSampleLegacyLike(mask, p[0], p[1], z, c, t) > 0.0) {
        signal[i] = nanValue();
      } else {
        signal[i] = pointSampleLegacyLike(stack, p[0], p[1], z, c, t);
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
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  std::vector<double> signal = geo3dScalarFieldStackSamplingLegacyLike(field, stack, zScale, c, t);
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
  CHECK(field.points.size() == field.values.size());
  if (field.size() == 0) {
    return 0.0;
  }

  std::vector<double> signal = geo3dScalarFieldStackSamplingMaskedLegacyLike(field, stack, zScale, mask, c, t);
  return computeStackFitScoresMaskedLegacyLike(field.values.data(), signal.data(), field.size(), fs);
}

} // namespace nim::neutube
