#ifndef ZIMGITKINTERFACE_H
#define ZIMGITKINTERFACE_H

#include "zimg.h"
#include <itkImage.h>
#include <itkImportImageFilter.h>

// On mac system,
// qglobal.h force min mac version to 1040 which breaks ITK
// To make ITK work, change itkLightObject.h:
// (force use volatile int64_t since MAC_OS_X_VERSION_MIN_REQUIRED used
// while building ITK would be the currently build-on mac version which
// is larger than 1050)

//#elif defined( __GLIBCPP__ ) || defined( __GLIBCXX__ )
//  //typedef _Atomic_word InternalReferenceCountType;
//  typedef volatile int64_t InternalReferenceCountType;
//#else

// see http://www.itk.org/pipermail/insight-users/2011-October/042755.html
// see https://bugreports.qt-project.org/browse/QTBUG-22154

namespace nim {

// ZImg channel to 3d itk image
// no memory copy, img still own the memory
// throw ZImgException if TVoxel don't match img type
template<typename TVoxel>
typename itk::Image<TVoxel, 3>::Pointer wrapZImgChannelAsITKImg(const ZImg& img, size_t c = 0, size_t t = 0)
{
  if (!img.isType<TVoxel>()) {
    throw ZImgException(
      QString("wrapZImgChannelAsITKImg wrap img <%1> to wrong type of itk img").arg(img.info().toQString()));
  }
  if (c >= img.numChannels() || t >= img.numTimes() || img.isEmpty()) {
    throw ZImgException(QString("wrapZImgChannelAsITKImg invalid pos of img, c:%1, t:%2, img:<%3>")
                          .arg(c).arg(t).arg(img.info().toQString()));
  }
  typedef typename itk::ImportImageFilter<TVoxel, 3> ImportFilterType;
  typename ImportFilterType::Pointer importFilter = ImportFilterType::New();
  typename ImportFilterType::SizeType size;
  typedef itk::Image<TVoxel, 3> OutputImageType;
  typedef typename OutputImageType::SpacingType SpacingType;
  typedef typename OutputImageType::PointType OriginType;
  size[0] = img.width();
  size[1] = img.height();
  size[2] = img.depth();
  typename ImportFilterType::IndexType start;
  start.Fill(0);
  typename ImportFilterType::RegionType region;
  region.SetIndex(start);
  region.SetSize(size);
  importFilter->SetRegion(region);
  OriginType origin;
  origin[0] = 0;
  origin[1] = 0;
  origin[2] = 0;
  importFilter->SetOrigin(origin);
  SpacingType spacing;
  spacing[0] = img.info().voxelSizeXInUm();
  spacing[1] = img.info().voxelSizeYInUm();
  spacing[2] = img.info().voxelSizeZInUm();
  importFilter->SetSpacing(spacing);
  importFilter->SetImportPointer(const_cast<TVoxel*>(img.channelData<TVoxel>(c, t)), img.channelVoxelNumber(), false);
  importFilter->Update();
  return importFilter->GetOutput();
}

// ZImg plane to 2d itk image
// no memory copy, img still own the memory
// throw ZImgException if TVoxel don't match img type
template<typename TVoxel>
typename itk::Image<TVoxel, 2>::Pointer wrapZImgPlaneAsITKImg(const ZImg& img, size_t z, size_t c = 0, size_t t = 0)
{
  if (!img.isType<TVoxel>()) {
    throw ZImgException(
      QString("wrapZImgPlaneAsITKImg wrap img <%1> to wrong type of itk img").arg(img.info().toQString()));
  }
  if (z >= img.depth() || c >= img.numChannels() || t >= img.numTimes() || img.isEmpty()) {
    throw ZImgException(QString("wrapZImgPlaneAsITKImg invalid pos of img, z:%1 c:%2, t:%3, img:<%4>")
                          .arg(z).arg(c).arg(t).arg(img.info().toQString()));
  }
  typedef typename itk::ImportImageFilter<TVoxel, 2> ImportFilterType;
  typename ImportFilterType::Pointer importFilter = ImportFilterType::New();
  typename ImportFilterType::SizeType size;
  typedef itk::Image<TVoxel, 2> OutputImageType;
  typedef typename OutputImageType::SpacingType SpacingType;
  typedef typename OutputImageType::PointType OriginType;
  size[0] = img.width();
  size[1] = img.height();
  typename ImportFilterType::IndexType start;
  start.Fill(0);
  typename ImportFilterType::RegionType region;
  region.SetIndex(start);
  region.SetSize(size);
  importFilter->SetRegion(region);
  OriginType origin;
  origin[0] = 0;
  origin[1] = 0;
  importFilter->SetOrigin(origin);
  SpacingType spacing;
  spacing[0] = img.info().voxelSizeXInUm();
  spacing[1] = img.info().voxelSizeYInUm();
  importFilter->SetSpacing(spacing);
  importFilter->SetImportPointer(const_cast<TVoxel*>(img.planeData<TVoxel>(z, c, t)), img.planeVoxelNumber(), false);
  importFilter->Update();
  return importFilter->GetOutput();
}

// tell itk image to use our pre-allocated memory as internal buffer so we can save one memory copy
// **note** not work for some filter
// pass the output of last filter of the itk pipeline as first parameter, pass memory location and size to other parameters
// **note** call this after set up the pipeline and before update the pipeline (at least before updating last filter)
// for example, to tell lastFilter to put result of size (w,h,d) into data:

// ..... set up pipeline ....
// letITKImgUseMemory(lastFilter->GetOutput(), data, w, h, d);
// lastFilter->Update();
// ...other stuff...
//
template<typename TVoxel>
void letITKImgUseMemory(itk::Image<TVoxel, 3>* itkImg, TVoxel* data, size_t width, size_t height, size_t depth)
{
  typedef itk::Image<TVoxel, 3> TITKImg;
  typename TITKImg::SizeType size;
  size[0] = width;
  size[1] = height;
  size[2] = depth;
  typename TITKImg::IndexType start;
  start.Fill(0);
  typename TITKImg::RegionType region;
  region.SetIndex(start);
  region.SetSize(size);
  itkImg->SetRegions(region);
  itkImg->GetPixelContainer()->SetImportPointer(data, width * height * depth, false);
  itkImg->Allocate();
}

template<typename TVoxel>
void letITKImgUseMemory(itk::Image<TVoxel, 2>* itkImg, TVoxel* data, size_t width, size_t height, size_t dummyDepth = 1)
{
  Q_UNUSED(dummyDepth)
  typedef itk::Image<TVoxel, 2> TITKImg;
  typename TITKImg::SizeType size;
  size[0] = width;
  size[1] = height;
  typename TITKImg::IndexType start;
  start.Fill(0);
  typename TITKImg::RegionType region;
  region.SetIndex(start);
  region.SetSize(size);
  itkImg->SetRegions(region);
  itkImg->GetPixelContainer()->SetImportPointer(data, width * height, false);
  itkImg->Allocate();
}

// copy memory from itk to create ZImg
// itk image to ZImg
template<typename TVoxel>
ZImg convertITKImgToZImg(const itk::Image<TVoxel, 3>* image)
{
  typename itk::Image<TVoxel, 3>::SizeType size = image->GetLargestPossibleRegion().GetSize();
  ZImgInfo info(size[0], size[1], size[2]);
  info.setVoxelFormat<TVoxel>();
  info.createDefaultDescriptions();
  ZImg res(info);
  const TVoxel* array = image->GetBufferPointer();
  memcpy(res.channelData<TVoxel>(0, 0), array, res.channelByteNumber());
  return res;
}

template<typename TVoxel>
ZImg convertITKImgToZImg(const itk::Image<TVoxel, 2>* image)
{
  typename itk::Image<TVoxel, 2>::SizeType size = image->GetLargestPossibleRegion().GetSize();
  ZImgInfo info(size[0], size[1], 1);
  info.setVoxelFormat<TVoxel>();
  info.createDefaultDescriptions();
  ZImg res(info);
  const TVoxel* array = image->GetBufferPointer();
  memcpy(res.channelData<TVoxel>(0, 0), array, res.channelByteNumber());
  return res;
}

template<typename TVoxel>
void copyITKImgToMemory(const itk::Image<TVoxel, 3>* image, TVoxel* data)
{
  typename itk::Image<TVoxel, 3>::SizeType size = image->GetLargestPossibleRegion().GetSize();
  memcpy(data, image->GetBufferPointer(), size[0] * size[1] * size[2] * sizeof(TVoxel));
}

template<typename TVoxel>
void copyITKImgToMemory(const itk::Image<TVoxel, 2>* image, TVoxel* data)
{
  typename itk::Image<TVoxel, 2>::SizeType size = image->GetLargestPossibleRegion().GetSize();
  memcpy(data, image->GetBufferPointer(), size[0] * size[1] * sizeof(TVoxel));
}

// Macro to help use itk library
// To use itk library for ZImg
// write a function:
//   template<typename TITKImage>
//   void function(TITKImage* image, t1 arg1, t2 arg2) {
//    ...somefilter->SetInput(image)...
//   }
//
// and use this macro to dispatch:
//   IMG_ITK_TYPED_CALL(function, zimg, c, t, arg1, arg2);
//
// if the function return something, use
//   IMG_RETURN_ITK_TYPED_CALL(function, zimg, c, t, arg1, arg2);
//
//

#define TO_ITK_3D_IMG_AND_CALL(function, img, c, t, TVoxel, ...) { \
  itk::Image<TVoxel, 3>::Pointer itkimg = wrapZImgChannelAsITKImg<TVoxel>(img, c, t); \
  function(itkimg.GetPointer(), __VA_ARGS__); \
  }

#define TO_ITK_3D_IMG_AND_CALL_R(function, img, c, t, TVoxel, ...) { \
  itk::Image<TVoxel, 3>::Pointer itkimg = wrapZImgChannelAsITKImg<TVoxel>(img, c, t); \
  return function(itkimg.GetPointer(), __VA_ARGS__); \
  }

#define TO_ITK_2D_IMG_AND_CALL(function, img, z, c, t, TVoxel, ...) { \
  itk::Image<TVoxel, 2>::Pointer itkimg = wrapZImgPlaneAsITKImg<TVoxel>(img, z, c, t); \
  function(itkimg.GetPointer(), __VA_ARGS__); \
  }

#define TO_ITK_2D_IMG_AND_CALL_R(function, img, z, c, t, TVoxel, ...) { \
  itk::Image<TVoxel, 2>::Pointer itkimg = wrapZImgPlaneAsITKImg<TVoxel>(img, z, c, t); \
  return function(itkimg.GetPointer(), __VA_ARGS__); \
  }

#define TO_ITK_IMG_AND_CALL(function, img, c, t, TVoxel, ...) { \
  if (img.is2DImg()) { \
    itk::Image<TVoxel, 2>::Pointer itkimg = wrapZImgPlaneAsITKImg<TVoxel>(img, 0, c, t); \
    function(itkimg.GetPointer(), __VA_ARGS__); \
  } else { \
    itk::Image<TVoxel, 3>::Pointer itkimg = wrapZImgChannelAsITKImg<TVoxel>(img, c, t); \
    function(itkimg.GetPointer(), __VA_ARGS__); \
  } \
  }

#define TO_ITK_IMG_AND_CALL_R(function, img, c, t, TVoxel, ...) { \
  if (img.is2DImg()) { \
    itk::Image<TVoxel, 2>::Pointer itkimg = wrapZImgPlaneAsITKImg<TVoxel>(img, 0, c, t); \
    return function(itkimg.GetPointer(), __VA_ARGS__); \
  } else { \
    itk::Image<TVoxel, 3>::Pointer itkimg = wrapZImgChannelAsITKImg<TVoxel>(img, c, t); \
    return function(itkimg.GetPointer(), __VA_ARGS__); \
  } \
  }

#define TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, TVoxel, T2ND, ...) { \
  if (img.is2DImg()) { \
    itk::Image<TVoxel, 2>::Pointer itkimg = wrapZImgPlaneAsITKImg<TVoxel>(img, 0, c, t); \
    function<itk::Image<TVoxel, 2>, T2ND>(itkimg.GetPointer(), __VA_ARGS__); \
  } else { \
    itk::Image<TVoxel, 3>::Pointer itkimg = wrapZImgChannelAsITKImg<TVoxel>(img, c, t); \
    function<itk::Image<TVoxel, 3>, T2ND>(itkimg.GetPointer(), __VA_ARGS__); \
  } \
  }

#define IMG_ITK_TYPED_CALL(function, img, c, t, ...) {                    \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                                    \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, uint8_t, __VA_ARGS__)      \
      break;                                                                 \
    case 2:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, uint16_t, __VA_ARGS__)     \
      break;                                                                 \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, uint32_t, __VA_ARGS__)     \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, uint64_t, __VA_ARGS__)     \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                                \
    switch (img.bytesPerVoxel()) {                                           \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, float, __VA_ARGS__)        \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, double, __VA_ARGS__)       \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                               \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, int8_t, __VA_ARGS__)       \
      break;                                                                 \
    case 2:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, int16_t, __VA_ARGS__)      \
      break;                                                                 \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, int32_t, __VA_ARGS__)      \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL(function, img, c, t, int64_t, __VA_ARGS__)      \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  }                                                                          \
}

#define IMG_RETURN_ITK_TYPED_CALL(function, img, c, t, ...) {               \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                                      \
    switch (img.bytesPerVoxel()) {                                             \
    case 1:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, uint8_t, __VA_ARGS__)      \
      break;                                                                   \
    case 2:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, uint16_t, __VA_ARGS__)     \
      break;                                                                   \
    case 4:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, uint32_t, __VA_ARGS__)     \
      break;                                                                   \
    case 8:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, uint64_t, __VA_ARGS__)     \
      break;                                                                   \
    default:                                                                   \
      break;                                                                   \
    }                                                                          \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                                  \
    switch (img.bytesPerVoxel()) {                                             \
    case 4:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, float, __VA_ARGS__)        \
      break;                                                                   \
    case 8:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, double, __VA_ARGS__)       \
      break;                                                                   \
    default:                                                                   \
      break;                                                                   \
    }                                                                          \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                                 \
    switch (img.bytesPerVoxel()) {                                             \
    case 1:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, int8_t, __VA_ARGS__)       \
      break;                                                                   \
    case 2:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, int16_t, __VA_ARGS__)      \
      break;                                                                   \
    case 4:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, int32_t, __VA_ARGS__)      \
      break;                                                                   \
    case 8:                                                                    \
      TO_ITK_IMG_AND_CALL_R(function, img, c, t, int64_t, __VA_ARGS__)      \
      break;                                                                   \
    default:                                                                   \
      break;                                                                   \
    }                                                                          \
  }                                                                            \
}

#define IMG_ITK_TYPED_CALL_FIX2NDTYPE(function, img, c, t, T2ND, ...) {   \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                                    \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, uint8_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 2:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, uint16_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, uint32_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, uint64_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                                \
    switch (img.bytesPerVoxel()) {                                           \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, float, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, double, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                               \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, int8_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 2:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, int16_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 4:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, int32_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    case 8:                                                                  \
      TO_ITK_IMG_AND_CALL_FIX2NDTYPE(function, img, c, t, int64_t, T2ND, __VA_ARGS__)      \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  }                                                                          \
}

} // namespace nim

#endif // ZIMGITKINTERFACE_H
