#include "zneutubeswcsignalfitter.h"

#include <gtest/gtest.h>

namespace nim {

TEST(ZNeutubeSwcSignalFitter, FitsRadiusOnSimpleDisk)
{
  constexpr int w = 21;
  constexpr int h = 21;
  constexpr int cx = 10;
  constexpr int cy = 10;
  constexpr int r = 5;

  ZImgInfo info(w, h, /*depth*/ 1, /*numChannels*/ 1, /*numTimes*/ 1, /*ratio*/ 1, VoxelFormat::Unsigned);
  ZImg slice(info);
  slice.fill<uint8_t>(0);

  auto* data = slice.timeData<uint8_t>(0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      if (dx * dx + dy * dy <= r * r) {
        data[static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(w)] = 200;
      }
    }
  }

  SwcNode node(/*id*/ 1,
               /*type*/ 0,
               /*x*/ static_cast<double>(cx),
               /*y*/ static_cast<double>(cy),
               /*z*/ 0.0,
               /*radius*/ 3.0,
               /*parentID*/ -1);

  const bool ok = fitSwcNodeSignalWithFallbackInCroppedSliceLegacyLike(node,
                                                                       slice,
                                                                       /*x0*/ 0,
                                                                       /*y0*/ 0,
                                                                       ZNeutubeImageBackgroundLegacyLike::Dark);
  ASSERT_TRUE(ok);

  EXPECT_NEAR(node.x, static_cast<double>(cx), 1e-6);
  EXPECT_NEAR(node.y, static_cast<double>(cy), 1e-6);
  EXPECT_NEAR(node.radius, static_cast<double>(r), 1e-3);
}

} // namespace nim
