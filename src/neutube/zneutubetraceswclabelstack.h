#pragma once

#include "zimg.h"
#include "zswc.h"

#include <array>

namespace nim::neutube {

struct Neuroseg;
struct LocalNeuroseg;

// Port of tz_neurofield.c::Neurofield7().
[[nodiscard]] double
neurofield7LegacyLike(double coef, double base, double x, double y, double z, double zMin, double zMax);

struct FieldRangeLegacyLike
{
  std::array<int, 3> firstCorner = {0, 0, 0};
  std::array<int, 3> size = {0, 0, 0};
};

// Port of tz_neuroseg.c::Neuroseg_Field_Range() (for label-stack / mask generation).
[[nodiscard]] FieldRangeLegacyLike neurosegFieldRangeLegacyLike(const Neuroseg& seg, double zScale);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Label_C().
void localNeurosegLabelCLegacyLike(const LocalNeuroseg& locseg, ZImg* mask, double zScale, int value);

// Port of tz_geo3d_ball.c::Geo3d_Ball_Label_Stack().
void geo3dBallLabelStackLegacyLike(const std::array<double, 3>& center, double radius, ZImg* mask, int value);

// Port of neuTube's SWC->mask labeling used by `ZSwcTree::labelStack(Stack*)`.
void labelSwcIntoMaskLegacyLike(const ZSwc& swc, ZImg* mask, double zScale, int value);

} // namespace nim::neutube
