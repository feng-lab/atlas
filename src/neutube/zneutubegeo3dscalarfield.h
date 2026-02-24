#pragma once

#include "zneutubetracerecord.h"

#include <array>
#include <cstddef>
#include <vector>

namespace nim {

class ZImg;

}

namespace nim::neutube {

// C++ port of tz_geo3d_scalar_field.h::Geo3d_Scalar_Field.
//
// This is a simple POD-like container: each sample has a 3D point and a scalar value
// (used as "filter"/weight in stack-fit scoring).
struct Geo3dScalarField
{
  std::vector<std::array<double, 3>> points;
  std::vector<double> values;

  [[nodiscard]] size_t size() const
  {
    return points.size();
  }
};

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Center().
[[nodiscard]] std::array<double, 3> geo3dScalarFieldCenterLegacyLike(const Geo3dScalarField& field);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Centroid().
[[nodiscard]] std::array<double, 3> geo3dScalarFieldCentroidLegacyLike(const Geo3dScalarField& field);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Sampling().
[[nodiscard]] std::vector<double> geo3dScalarFieldStackSamplingLegacyLike(const Geo3dScalarField& field,
                                                                          const ZImg& stack,
                                                                          double zScale,
                                                                          size_t c = 0,
                                                                          size_t t = 0);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Sampling_W().
[[nodiscard]] std::vector<double> geo3dScalarFieldStackSamplingWeightedLegacyLike(const Geo3dScalarField& field,
                                                                                  const ZImg& stack,
                                                                                  double zScale,
                                                                                  size_t c = 0,
                                                                                  size_t t = 0);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Sampling_M().
[[nodiscard]] std::vector<double> geo3dScalarFieldStackSamplingMaskedLegacyLike(const Geo3dScalarField& field,
                                                                                const ZImg& stack,
                                                                                double zScale,
                                                                                const ZImg& mask,
                                                                                size_t c = 0,
                                                                                size_t t = 0);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Score().
[[nodiscard]] double geo3dScalarFieldStackScoreLegacyLike(const Geo3dScalarField& field,
                                                          const ZImg& stack,
                                                          double zScale,
                                                          StackFitScore* fs,
                                                          size_t c = 0,
                                                          size_t t = 0);

// Port of tz_geo3d_scalar_field.c::Geo3d_Scalar_Field_Stack_Score_M().
[[nodiscard]] double geo3dScalarFieldStackScoreMaskedLegacyLike(const Geo3dScalarField& field,
                                                                const ZImg& stack,
                                                                double zScale,
                                                                const ZImg& mask,
                                                                StackFitScore* fs,
                                                                size_t c = 0,
                                                                size_t t = 0);

} // namespace nim::neutube
