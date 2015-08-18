#ifndef ZIMGSIGNEDDISTANCEMAP_H
#define ZIMGSIGNEDDISTANCEMAP_H

#include "zimg.h"
#include "zimgalgorithm.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgSignedDistanceMap : public ZImgAlgorithm<ReportProgress>
{
public:
  ZImgSignedDistanceMap();

  // default false
  inline void setInsideIsPositive(bool v) { m_insideIsPositive = v; }
  // default false
  inline void setUseSquaredDistance(bool v) { m_useSquaredDistance = v; }
  inline void setNumberOfThreads(int n) { m_numThreads = n; }

  template<typename TVoxelOut>
  ZImg run(const ZImg& img, bool useVoxelSize = false);

protected:
  template<typename TITKImg, typename TVoxelOut>
  void run_Impl(TITKImg* itkimg, bool useVoxelSize, ZImg& res, size_t c, size_t t);

private:
  bool m_insideIsPositive;
  bool m_useSquaredDistance;
  int m_numThreads;
};

} // namespace nim

#endif // ZIMGSIGNEDDISTANCEMAP_H
