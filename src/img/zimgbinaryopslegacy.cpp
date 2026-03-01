#include "zimgbinaryopslegacy.h"

#include "zlog.h"

#include <algorithm>
#include <limits>

namespace nim {

namespace {

template<typename T>
void invertValueInPlaceTyped(ZImg& img)
{
  const size_t w = img.width();
  const size_t h = img.height();
  const size_t d = img.depth();

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      T minV = std::numeric_limits<T>::max();
      T maxV = std::numeric_limits<T>::lowest();

      for (size_t z = 0; z < d; ++z) {
        const T* row0 = img.rowData<T>(0, z, c, t);
        for (size_t i = 0; i < w * h; ++i) {
          const T v = row0[i];
          minV = std::min(minV, v);
          maxV = std::max(maxV, v);
        }
      }

      const double minD = static_cast<double>(minV);
      const double maxD = static_cast<double>(maxV);

      for (size_t z = 0; z < d; ++z) {
        T* row0 = img.rowData<T>(0, z, c, t);
        for (size_t i = 0; i < w * h; ++i) {
          const double v = static_cast<double>(row0[i]);
          const double inv = minD + maxD - v;
          row0[i] = static_cast<T>(inv);
        }
      }
    }
  }
}

} // namespace

void invertValueInPlaceLegacyLike(ZImg& img)
{
  if (img.isEmpty()) {
    return;
  }

  if (img.voxelFormat() == VoxelFormat::Unsigned) {
    if (img.voxelByteNumber() == sizeof(uint8_t)) {
      invertValueInPlaceTyped<uint8_t>(img);
      return;
    }
    if (img.voxelByteNumber() == sizeof(uint16_t)) {
      invertValueInPlaceTyped<uint16_t>(img);
      return;
    }
  }

  LOG(WARNING) << "invertValueInPlaceLegacyLike: unsupported voxel type " << img.info();
}

ZImg binarizeGreaterThanLegacyLike(const ZImg& img, int threshold)
{
  if (img.isEmpty()) {
    return {};
  }
  if (img.voxelFormat() != VoxelFormat::Unsigned) {
    LOG(WARNING) << "binarizeGreaterThanLegacyLike: expected unsigned input; got " << img.info();
    return {};
  }

  ZImgInfo outInfo = img.info();
  outInfo.setVoxelFormat<uint8_t>();
  outInfo.createDefaultDescriptions();
  ZImg out(outInfo);

  const size_t w = img.width();
  const size_t h = img.height();
  const size_t d = img.depth();

  if (img.voxelByteNumber() == sizeof(uint8_t)) {
    for (size_t t = 0; t < img.numTimes(); ++t) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t z = 0; z < d; ++z) {
          const uint8_t* src = img.rowData<uint8_t>(0, z, c, t);
          uint8_t* dst = out.rowData<uint8_t>(0, z, c, t);
          for (size_t i = 0; i < w * h; ++i) {
            dst[i] = static_cast<uint8_t>(src[i] > threshold);
          }
        }
      }
    }
    return out;
  }

  if (img.voxelByteNumber() == sizeof(uint16_t)) {
    for (size_t t = 0; t < img.numTimes(); ++t) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t z = 0; z < d; ++z) {
          const uint16_t* src = img.rowData<uint16_t>(0, z, c, t);
          uint8_t* dst = out.rowData<uint8_t>(0, z, c, t);
          for (size_t i = 0; i < w * h; ++i) {
            dst[i] = static_cast<uint8_t>(src[i] > threshold);
          }
        }
      }
    }
    return out;
  }

  LOG(WARNING) << "binarizeGreaterThanLegacyLike: unsupported voxel type " << img.info();
  return {};
}

} // namespace nim
