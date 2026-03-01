#include "zimgmedianfilterlegacy.h"

#include "zlog.h"

#include <algorithm>
#include <array>
#include <type_traits>

namespace nim {

namespace {

template<typename T>
[[nodiscard]] constexpr bool isSupportedUnsignedType()
{
  return std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t>;
}

template<typename T>
void medianFilterConn8Impl(const ZImg& in, ZImg& out)
{
  static_assert(isSupportedUnsignedType<T>(), "medianFilterConn8LegacyLike: unsupported voxel type");

  CHECK(in.width() == out.width());
  CHECK(in.height() == out.height());
  CHECK(in.depth() == 1);
  CHECK(out.depth() == 1);
  CHECK(in.numChannels() == out.numChannels());
  CHECK(in.numTimes() == out.numTimes());

  const size_t w = in.width();
  const size_t h = in.height();

  if (w < 3 || h < 3) {
    return;
  }

  std::array<double, 9> values{};

  for (size_t t = 0; t < in.numTimes(); ++t) {
    for (size_t c = 0; c < in.numChannels(); ++c) {
      // Interior pixels only (border remains as the original copy).
      for (size_t y = 1; y + 1 < h; ++y) {
        for (size_t x = 1; x + 1 < w; ++x) {
          size_t k = 0;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const size_t xx = static_cast<size_t>(static_cast<int>(x) + dx);
              const size_t yy = static_cast<size_t>(static_cast<int>(y) + dy);
              values[k++] = static_cast<double>(*in.data<T>(xx,
                                                            yy,
                                                            /*z*/ 0,
                                                            c,
                                                            t));
            }
          }
          std::sort(values.begin(), values.end());
          *out.data<T>(x, y, /*z*/ 0, c, t) = static_cast<T>(values[4]);
        }
      }
    }
  }
}

} // namespace

ZImg medianFilterConn8LegacyLike(const ZImg& img)
{
  if (img.isEmpty()) {
    return {};
  }
  if (img.depth() != 1) {
    LOG(WARNING) << "medianFilterConn8LegacyLike: expected depth=1, got depth=" << img.depth();
    return img;
  }

  ZImg out = img;

  if (img.voxelFormat() == VoxelFormat::Unsigned) {
    if (img.voxelByteNumber() == sizeof(uint8_t)) {
      medianFilterConn8Impl<uint8_t>(img, out);
      return out;
    }
    if (img.voxelByteNumber() == sizeof(uint16_t)) {
      medianFilterConn8Impl<uint16_t>(img, out);
      return out;
    }
  }

  LOG(WARNING) << "medianFilterConn8LegacyLike: unsupported voxel type " << img.info();
  return img;
}

} // namespace nim
