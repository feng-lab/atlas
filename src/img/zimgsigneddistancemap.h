#pragma once

#include <zcpuinfo.h>
#include "zbenchtimer.h"
#include "zimg.h"
#include "zimgalgorithm.h"
#include "zcpuinfo.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgSignedDistanceMap : public ZImgAlgorithm<ReportProgress>
{
public:
  // default false
  inline void setInsideIsPositive(bool v)
  {
    m_insideIsPositive = v;
  }

  // default false
  inline void setUseSquaredDistance(bool v)
  {
    m_useSquaredDistance = v;
  }

  // TVoxelOut needs to be floating point
  template<typename TVoxelOut>
  ZImg run(const ZImg& img, bool useVoxelSize = false);

protected:
  template<typename TITKImg, typename TVoxelOut>
  void run_Impl(TITKImg* itkimg, bool useVoxelSize, ZImg& res, size_t c, size_t t);

private:
  bool m_insideIsPositive = false;
  bool m_useSquaredDistance = false;
};

} // namespace nim
