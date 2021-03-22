#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"
#include "zcpuinfo.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgInterpolate : public ZImgAlgorithm<ReportProgress>
{
public:
  ZImg run(const ZImg& img);

protected:
  template<typename TITKImg>
  void run_Impl(TITKImg* itkimg, ZImg& res, size_t c, size_t t);

private:
};

} // namespace nim
