#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"
#include "zcpuinfo.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgFillHole : public ZImgAlgorithm<ReportProgress>
{
public:
  // default true
  inline void setFullyConnected(bool v)
  {
    m_fullyConnected = v;
  }

  // Defaults to maximum value of InputPixelType.
  inline void setForegroundValue(uint64_t v)
  {
    m_foregroundValue = v;
  }

  ZImg run(const ZImg& img);

protected:
  template<typename TITKImg>
  void run_Impl(TITKImg* itkimg, ZImg& res, size_t c, size_t t);

private:
  bool m_fullyConnected = true;
  uint64_t m_foregroundValue = 0;
};

} // namespace nim
