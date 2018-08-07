#include "zimgfillhole.h"

#include "zimgitkinterface.h"
#include <itkBinaryFillholeImageFilter.h>

namespace nim {

template<bool ReportProgress>
ZImg ZImgFillHole<ReportProgress>::run(const ZImg& img)
{
  ZImg res = img;

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      IMG_ITK_TYPED_CALL(run_Impl, img, c, t, res, c, t);
    }
  }

  this->reportProgress(1.0);
  return res;
}

template<bool ReportProgress>
template<typename TITKImg>
void ZImgFillHole<ReportProgress>::run_Impl(TITKImg* itkimg, ZImg& res, size_t c, size_t t)
{
  using BinaryFillHoleFilterType = itk::BinaryFillholeImageFilter<TITKImg>;
  typename BinaryFillHoleFilterType::Pointer bfhFilter = BinaryFillHoleFilterType::New();
  bfhFilter->SetInput(itkimg);
  bfhFilter->SetFullyConnected(m_fullyConnected);
  if (m_foregroundValue != 0)
    bfhFilter->SetForegroundValue(m_foregroundValue);
  bfhFilter->SetNumberOfWorkUnits(this->m_numThreads);
  bfhFilter->Update();
  copyITKImgToMemory(bfhFilter->GetOutput(), res.channelData<typename TITKImg::PixelType>(c, t));
}

template
class ZImgFillHole<true>;

template
class ZImgFillHole<false>;

} // namespace nim

