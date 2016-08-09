#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgFillHole : public ZImgAlgorithm<ReportProgress>
{
public:
  ZImgFillHole();

  // default true
  inline void setFullyConnected(bool v)
  { m_fullyConnected = v; }

  // Defaults to maximum value of InputPixelType.
  inline void setForegroundValue(uint64_t v)
  { m_foregroundValue = v; }

  inline void setNumberOfThreads(int n)
  { m_numThreads = n; }

  ZImg run(const ZImg& img);

protected:
  template<typename TITKImg>
  void run_Impl(TITKImg* itkimg, ZImg& res, size_t c, size_t t);

private:
  bool m_fullyConnected;
  uint64_t m_foregroundValue;
  int m_numThreads;
};

} // namespace nim
