#include "zimginterpolate.h"

#include "zimgitkinterface.h"
#include <itkMorphologicalContourInterpolator.h>

namespace nim {

template<bool ReportProgress>
ZImg ZImgInterpolate<ReportProgress>::run(const ZImg& img)
{
  ZImg res = img;

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      INTEGER_IMG_ITK_3D_TYPED_CALL(run_Impl, img, c, t, res, c, t)
    }
  }

  this->reportProgress(1.0);
  return res;
}

template<bool ReportProgress>
template<typename TITKImg>
void ZImgInterpolate<ReportProgress>::run_Impl(TITKImg* itkimg, ZImg& res, size_t c, size_t t)
{
  using MorphologicalContourInterpolatorFilterType = itk::MorphologicalContourInterpolator<TITKImg>;
  typename MorphologicalContourInterpolatorFilterType::Pointer iFilter =
    MorphologicalContourInterpolatorFilterType::New();
  iFilter->SetInput(itkimg);
  iFilter->SetAxis(2);
  iFilter->SetNumberOfWorkUnits(this->m_numThreads);
  iFilter->Update();
  copyITKImgToMemory(iFilter->GetOutput(), res.channelData<typename TITKImg::PixelType>(c, t));
}

template class ZImgInterpolate<true>;

template class ZImgInterpolate<false>;

} // namespace nim
