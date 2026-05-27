#include "zimgbinaryopslegacy.h"

#include "zint128.h"

#include <cstdint>
#include <limits>
#include <type_traits>

namespace nim {

namespace {

template<typename TVoxel>
void invertValueInPlaceTyped(ZImg& img)
{
  const size_t channelVoxelNumber = img.channelVoxelNumber();

  constexpr bool isSignedIntegral = std::is_integral_v<TVoxel> && std::is_signed_v<TVoxel>;

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      const ZImg view = img.createView(static_cast<int>(c), static_cast<int>(t));
      TVoxel minV{};
      TVoxel maxV{};
      view.computeMinMax(minV, maxV);

      TVoxel* data = img.channelData<TVoxel>(c, t);

      if constexpr (isSignedIntegral) {
        const detail::int128 sum = static_cast<detail::int128>(minV) + static_cast<detail::int128>(maxV);
        for (size_t i = 0; i < channelVoxelNumber; ++i) {
          data[i] = static_cast<TVoxel>(sum - static_cast<detail::int128>(data[i]));
        }
      } else {
        const auto sum = minV + maxV;
        for (size_t i = 0; i < channelVoxelNumber; ++i) {
          data[i] = static_cast<TVoxel>(sum - data[i]);
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

  imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
    invertValueInPlaceTyped<TVoxel>(img);
  });
}

ZImg binarizeGreaterThanLegacyLike(const ZImg& img, int threshold)
{
  if (img.isEmpty()) {
    return {};
  }

  ZImgInfo outInfo = img.info();
  outInfo.setVoxelFormat<uint8_t>();
  outInfo.createDefaultDescriptions();
  ZImg out(outInfo);

  const size_t w = img.width();
  const size_t h = img.height();
  const size_t d = img.depth();

  imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
    if constexpr (std::is_integral_v<TVoxel>) {
      if constexpr (std::is_unsigned_v<TVoxel>) {
        if (threshold < 0) {
          out.fill(1);
          return;
        }
        const std::uint64_t thr = static_cast<std::uint64_t>(threshold);
        const std::uint64_t maxV = static_cast<std::uint64_t>(std::numeric_limits<TVoxel>::max());
        if (thr >= maxV) {
          out.fill(0);
          return;
        }
      } else {
        const std::int64_t thr = static_cast<std::int64_t>(threshold);
        const std::int64_t minV = static_cast<std::int64_t>(std::numeric_limits<TVoxel>::lowest());
        const std::int64_t maxV = static_cast<std::int64_t>(std::numeric_limits<TVoxel>::max());
        if (thr < minV) {
          out.fill(1);
          return;
        }
        if (thr >= maxV) {
          out.fill(0);
          return;
        }
      }
    }

    const TVoxel thresholdV = static_cast<TVoxel>(threshold);

    for (size_t t = 0; t < img.numTimes(); ++t) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t z = 0; z < d; ++z) {
          const TVoxel* src = img.rowData<TVoxel>(0, z, c, t);
          uint8_t* dst = out.rowData<uint8_t>(0, z, c, t);
          for (size_t i = 0; i < w * h; ++i) {
            dst[i] = static_cast<uint8_t>(src[i] > thresholdV);
          }
        }
      }
    }
  });

  return out;
}

} // namespace nim
