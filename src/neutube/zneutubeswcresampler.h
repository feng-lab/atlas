#pragma once

#include "zswc.h"

namespace nim::neutube {

// Port of neuTube `ZSwcResampler` used by skeletonize (`optimalDownsample`).
class ZNeutubeSwcResampler
{
public:
  ZNeutubeSwcResampler();

  int optimalDownsample(ZSwc* tree) const;

private:
  int suboptimalDownsample(ZSwc* tree) const;

  [[nodiscard]] bool isInterRedundant(const ZSwc::SwcTreeNode& tn, const ZSwc::SwcTreeNode& master) const;

private:
  double m_radiusScale = 1.2;
  double m_distanceScale = 2.0;
  bool m_ignoringInterRedundant = false;
};

} // namespace nim::neutube
