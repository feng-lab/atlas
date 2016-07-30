#include "zimgfillhole.h"

#include "zimgitkinterface.h"
#include <itkBinaryFillholeImageFilter.h>
#include <type_traits>
#include <QThread>

namespace nim {

template<bool ReportProgress>
ZImgFillHole<ReportProgress>::ZImgFillHole()
  : m_fullyConnected(true), m_foregroundValue(0), m_numThreads(QThread::idealThreadCount())
{}

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
  typedef itk::BinaryFillholeImageFilter<TITKImg> BinaryFillHoleFilterType;
  typename BinaryFillHoleFilterType::Pointer bfhFilter = BinaryFillHoleFilterType::New();
  bfhFilter->SetInput(itkimg);
  bfhFilter->SetFullyConnected(m_fullyConnected);
  if (m_foregroundValue != 0)
    bfhFilter->SetForegroundValue(m_foregroundValue);
  bfhFilter->SetNumberOfThreads(m_numThreads);
  bfhFilter->Update();
  copyITKImgToMemory(bfhFilter->GetOutput(), res.channelData<typename TITKImg::PixelType>(c, t));
}

template
class ZImgFillHole<true>;

template
class ZImgFillHole<false>;

} // namespace nim

