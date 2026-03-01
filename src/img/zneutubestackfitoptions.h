#pragma once

namespace nim {

// Port of tz_geo3d_scalar_field.h STACK_FIT_* option values.
enum class StackFitOption : int
{
  Dot = 0,
  Corrcoef = 1,
  Edot = 2,
  Stat = 3,
  Pdot = 4,
  MeanSignal = 5,
  CorrcoefSc = 6,
  DotCenter = 7,
  OuterSignal = 8,
  ValidSignalRatio = 9,
  LowMeanSignal = 10
};

} // namespace nim
