#include "zimgsigneddistancemap.h"

#include "zimgitkinterface.h"
#include <itkSignedMaurerDistanceMapImageFilter.h>

namespace nim {

template<bool ReportProgress>
template<typename TVoxelOut>
ZImg ZImgSignedDistanceMap<ReportProgress>::run(const ZImg& img, bool useVoxelSize)
{
  if (useVoxelSize || !m_useSquaredDistance) {
    if (!std::is_floating_point<TVoxelOut>::value) {
      throw ZImgException("need float output for distance map");
    }
  } else {
    if (!std::is_signed<TVoxelOut>::value && !std::is_floating_point<TVoxelOut>::value) {
      throw ZImgException("need signed or float output for distance map");
    }
  }
  ZImgInfo info = img.info();
  info.setVoxelFormat<TVoxelOut>();
  ZImg res(info);

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      IMG_ITK_TYPED_CALL_FIX2NDTYPE(run_Impl, img, c, t, TVoxelOut,
                                    useVoxelSize, res, c, t);
    }
  }

  this->reportProgress(1.0);
  return res;
}

template<bool ReportProgress>
template<typename TITKImg, typename TVoxelOut>
void ZImgSignedDistanceMap<ReportProgress>::run_Impl(TITKImg* itkimg, bool useVoxelSize, ZImg& res, size_t c, size_t t)
{
  using TOutputITKImg = typename itk::Image<TVoxelOut, TITKImg::ImageDimension>;
  using DistanceMapFilterType = itk::SignedMaurerDistanceMapImageFilter<TITKImg, TOutputITKImg>;
  typename DistanceMapFilterType::Pointer dmFilter = DistanceMapFilterType::New();
  dmFilter->SetInput(itkimg);
  dmFilter->SetInsideIsPositive(m_insideIsPositive);
  dmFilter->SetSquaredDistance(m_useSquaredDistance);
  dmFilter->SetUseImageSpacing(useVoxelSize);
  dmFilter->SetNumberOfWorkUnits(this->m_numThreads);
  dmFilter->Update();
  copyITKImgToMemory(dmFilter->GetOutput(), res.channelData<TVoxelOut>(c, t));
}

template
class ZImgSignedDistanceMap<true>;

template
class ZImgSignedDistanceMap<false>;

//template ZImg ZImgSignedDistanceMap<true>::run<int8_t>(const ZImg&, bool);
//template ZImg ZImgSignedDistanceMap<true>::run<int16_t>(const ZImg&, bool);
template ZImg ZImgSignedDistanceMap<true>::run<int32_t>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<true>::run<int64_t>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<true>::run<float>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<true>::run<double>(const ZImg&, bool);

//template ZImg ZImgSignedDistanceMap<false>::run<int8_t>(const ZImg&, bool);
//template ZImg ZImgSignedDistanceMap<false>::run<int16_t>(const ZImg&, bool);
template ZImg ZImgSignedDistanceMap<false>::run<int32_t>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<false>::run<int64_t>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<false>::run<float>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap<false>::run<double>(const ZImg&, bool);

} // namespace nim
