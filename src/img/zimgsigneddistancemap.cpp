#include "zimgsigneddistancemap.h"

#include "zimgitkinterface.h"
#include <itkSignedMaurerDistanceMapImageFilter.h>

namespace nim {

template<typename TVoxelOut>
ZImg ZImgSignedDistanceMap::run(const ZImg& img, bool useVoxelSize)
{
  static_assert(std::is_floating_point_v<TVoxelOut>);
  ZImgInfo info = img.info();
  info.setVoxelFormat<TVoxelOut>();
  ZImg res(info);

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      IMG_ITK_TYPED_CALL_FIX2NDTYPE(run_Impl, img, c, t, TVoxelOut, useVoxelSize, res, c, t)
    }
  }

  this->reportProgress(1.0);
  return res;
}

template<typename TITKImg, typename TVoxelOut>
void ZImgSignedDistanceMap::run_Impl(TITKImg* itkimg, bool useVoxelSize, ZImg& res, size_t c, size_t t)
{
  using TOutputITKImg = itk::Image<TVoxelOut, TITKImg::ImageDimension>;
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
template ZImg ZImgSignedDistanceMap::run<float>(const ZImg&, bool);

template ZImg ZImgSignedDistanceMap::run<double>(const ZImg&, bool);

} // namespace nim
