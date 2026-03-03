#include "zimgmedianfilterlegacy.h"

#include "zlog.h"

#include <algorithm>
#include <array>

namespace nim {

namespace {

template<typename TVoxel>
void medianFilterConn8Impl(const ZImg& in, ZImg& out)
{
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

  std::array<TVoxel, 9> values{};

  for (size_t t = 0; t < in.numTimes(); ++t) {
    for (size_t c = 0; c < in.numChannels(); ++c) {
      // Interior pixels only (border remains as the original copy).
      for (size_t y = 1; y + 1 < h; ++y) {
        const TVoxel* rowPrev = in.rowData<TVoxel>(y - 1, /*z*/ 0, c, t);
        const TVoxel* rowCur = in.rowData<TVoxel>(y, /*z*/ 0, c, t);
        const TVoxel* rowNext = in.rowData<TVoxel>(y + 1, /*z*/ 0, c, t);

        for (size_t x = 1; x + 1 < w; ++x) {
          values[0] = rowPrev[x - 1];
          values[1] = rowPrev[x];
          values[2] = rowPrev[x + 1];
          values[3] = rowCur[x - 1];
          values[4] = rowCur[x];
          values[5] = rowCur[x + 1];
          values[6] = rowNext[x - 1];
          values[7] = rowNext[x];
          values[8] = rowNext[x + 1];

          std::sort(values.begin(), values.end());
          *out.data<TVoxel>(x, y, /*z*/ 0, c, t) = values[4];
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

  imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
    medianFilterConn8Impl<TVoxel>(img, out);
  });

  return out;
}

} // namespace nim
