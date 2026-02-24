#pragma once

#include <array>
#include <vector>

namespace nim::neutube {

struct Geo3dScalarField;

// C++ port of tz_neuroseg.h::Neuroseg.
struct Neuroseg
{
  double r1 = 0.0;
  double c = 0.0;
  double h = 0.0;
  double theta = 0.0;
  double psi = 0.0;
  double curvature = 0.0;
  double alpha = 0.0;
  double scale = 1.0;
};

using NeurosegFieldFunctionLegacyLike = double (*)(double x, double y);

// Port of tz_neuroseg.c::neurofield().
[[nodiscard]] double neurofieldLegacyLike(double x, double y);

// C++ representation of tz_neuroseg.c::Neuroseg_Slice_Field() outputs.
struct NeurosegSliceField
{
  std::vector<std::array<double, 3>> points;
  std::vector<double> values;
};

// Port of tz_neuroseg.c::Neuroseg_Slice_Field().
[[nodiscard]] NeurosegSliceField neurosegSliceFieldLegacyLike(NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_neuroseg.c::Neuroseg_Slice_Field_P().
[[nodiscard]] NeurosegSliceField neurosegSliceFieldPLegacyLike(NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_neuroseg.c::Neuroseg_Field_S_Fast().
[[nodiscard]] Geo3dScalarField neurosegFieldSFastLegacyLike(const Neuroseg& seg,
                                                            NeurosegFieldFunctionLegacyLike fieldFunc);

// Port of tz_neuroseg.c::Neuroseg_Field_Sp().
[[nodiscard]] Geo3dScalarField neurosegFieldSpLegacyLike(const Neuroseg& seg,
                                                         NeurosegFieldFunctionLegacyLike fieldFunc);

} // namespace nim::neutube
