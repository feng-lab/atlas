#include "zrunexport3danimation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace nim {
namespace {

using FrameRange = ZRunExport3DAnimation::FrameRange;

TEST(ZRunExport3DAnimationTest, CapsWorkersAtRemainingFrameCount)
{
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(7, 9, 4u);
  const std::vector<FrameRange> expected{
    {7, 8},
    {8, 9}
  };

  EXPECT_EQ(ranges, expected);
}

TEST(ZRunExport3DAnimationTest, KeepsOneFrameOnOneWorker)
{
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(5, 6, 8u);
  const std::vector<FrameRange> expected{
    {5, 6}
  };

  EXPECT_EQ(ranges, expected);
}

TEST(ZRunExport3DAnimationTest, CoversExplicitNonzeroRangeExactlyOnce)
{
  constexpr int startFrame = 13;
  constexpr int endFrame = 37;
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(startFrame, endFrame, 5u);

  ASSERT_EQ(ranges.size(), 5u);
  EXPECT_EQ(ranges.front().first, startFrame);
  EXPECT_EQ(ranges.back().second, endFrame);

  int expectedStart = startFrame;
  size_t smallestRangeSize = static_cast<size_t>(endFrame - startFrame);
  size_t largestRangeSize = 0u;
  for (const auto& [rangeStart, rangeEnd] : ranges) {
    EXPECT_EQ(rangeStart, expectedStart);
    EXPECT_GT(rangeEnd, rangeStart);
    const size_t rangeSize = static_cast<size_t>(rangeEnd - rangeStart);
    smallestRangeSize = std::min(smallestRangeSize, rangeSize);
    largestRangeSize = std::max(largestRangeSize, rangeSize);
    expectedStart = rangeEnd;
  }
  EXPECT_EQ(expectedStart, endFrame);
  EXPECT_LE(largestRangeSize - smallestRangeSize, 1u);
}

} // namespace
} // namespace nim
