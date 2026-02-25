#pragma once

#include "zneutubefieldrange.h"

#include <array>
#include <vector>

namespace nim::neutube {

struct Geo3dScalarField;

inline constexpr double NeurosegMinRLegacyLike = 0.5;
inline constexpr double NeurosegDefaultHLegacyLike = 11.0;

// C++ port of tz_neuroseg.h::Neuropos_Reference_e.
enum class NeuroposReferenceLegacyLike : int
{
  Undef = -1,
  Bottom = 0,
  Top = 1,
  Center = 2
};

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

// Ports of tz_neuroseg.h macros (NEUROSEG_*).
[[nodiscard]] double neurosegCoefLegacyLike(const Neuroseg& seg);
[[nodiscard]] double neurosegRadiusLegacyLike(const Neuroseg& seg, double z);
[[nodiscard]] double neurosegR2LegacyLike(const Neuroseg& seg);
[[nodiscard]] double neurosegRCLegacyLike(const Neuroseg& seg);
[[nodiscard]] double neurosegRALegacyLike(const Neuroseg& seg);
[[nodiscard]] double neurosegRBLegacyLike(const Neuroseg& seg);
[[nodiscard]] double neurosegCRCLegacyLike(const Neuroseg& seg);

// Port of tz_neuroseg.c::Neuroseg_Ball_Range().
[[nodiscard]] double neurosegBallRangeLegacyLike(const Neuroseg& seg);

// Port of tz_neuroseg.c::Neuroseg_Swell().
void neurosegSwellLegacyLike(Neuroseg* seg, double ratio, double diff, double maxDiff);

// Port of tz_neuroseg.c::Neuroseg_Dist_Filter().
//
// Returns a 3D array in Z-major, then Y, then X order, matching the legacy C layout:
//   offset = i + sizeX * (j + sizeY * k)
// The caller is expected to interpret values <= 1 as "inside" the segment.
[[nodiscard]] std::vector<double> neurosegDistFilterLegacyLike(const Neuroseg& seg,
                                                               const FieldRangeLegacyLike& range,
                                                               const std::array<double, 3>* offpos,
                                                               double zScale);

// Port of tz_neuroseg.c::Neuroseg_Angle_Between().
[[nodiscard]] double neurosegAngleBetweenLegacyLike(const Neuroseg& seg1, const Neuroseg& seg2);

// Port of tz_neuroseg.c::Neuroseg_Hit_Test().
[[nodiscard]] bool neurosegHitTestLegacyLike(const Neuroseg& seg, double x, double y, double z);

// Ports of tz_neuroseg.c::Neuroseg_Rx/Ry (and Z/T variants).
[[nodiscard]] double neurosegRxLegacyLike(const Neuroseg& seg, NeuroposReferenceLegacyLike ref);
[[nodiscard]] double neurosegRyLegacyLike(const Neuroseg& seg, NeuroposReferenceLegacyLike ref);
[[nodiscard]] double
neurosegRxPLegacyLike(const Neuroseg& seg, const std::array<double, 3>& resolution, NeuroposReferenceLegacyLike ref);
[[nodiscard]] double
neurosegRyPLegacyLike(const Neuroseg& seg, const std::array<double, 3>& resolution, NeuroposReferenceLegacyLike ref);
[[nodiscard]] double neurosegRxZLegacyLike(const Neuroseg& seg, double z);
[[nodiscard]] double neurosegRyZLegacyLike(const Neuroseg& seg, double z);
[[nodiscard]] double neurosegRxyZLegacyLike(const Neuroseg& seg, double z);

// Port of tz_neuroseg.c::Next_Neuroseg().
[[nodiscard]] Neuroseg nextNeurosegLegacyLike(const Neuroseg& seg1, double posStep);

// Port of tz_neuroseg.c::neurofield().
[[nodiscard]] double neurofieldLegacyLike(double x, double y);

// C++ representation of tz_neuroseg.c::Neuroseg_Slice_Field() outputs.
struct NeurosegSliceField
{
  std::vector<std::array<double, 3>> points;
  std::vector<double> values;
};

// Port of tz_neuroseg.c::Neuroseg_Field_Z().
[[nodiscard]] Geo3dScalarField neurosegFieldZLegacyLike(const Neuroseg& seg, double z, double step);

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
