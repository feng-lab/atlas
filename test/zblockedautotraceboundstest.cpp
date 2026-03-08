#include <gtest/gtest.h>

#include "zblockedautotracebounds.h"

#include <array>
#include <cstdint>

TEST(ZBlockedAutoTraceBounds, OutOfImageForVoxelSamplingUsesInclusiveMaxVoxelIndex)
{
  const int64_t width = 2014;
  const int64_t height = 4887;
  const int64_t depth = 99;

  EXPECT_FALSE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{0.0, 0.0, 0.0}));
  EXPECT_FALSE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0, 4886.0, 98.0}));

  // Fractional positions slightly above (dim-1) are out-of-range for voxel sampling.
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0001, 4886.0, 98.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0, 4886.0001, 98.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0, 4886.0, 98.0001}));

  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2014.0, 4886.0, 98.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0, 4887.0, 98.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{2013.0, 4886.0, 99.0}));

  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{-0.0001, 0.0, 0.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{0.0, -0.0001, 0.0}));
  EXPECT_TRUE(nim::outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{0.0, 0.0, -0.0001}));
}

TEST(ZBlockedAutoTraceBounds, ContinuationWouldLeaveImageForVoxelSamplingProbesBeyondAnchorAndRadius)
{
  const int64_t width = 2014;
  const int64_t height = 4887;
  const int64_t depth = 99;

  const std::array<double, 3> anchor = {106.921, 4885.449, 77.263};

  // Axis probe: anchor is in-range, but a forward step in +Y leaves the dataset (maxY == 4886).
  EXPECT_TRUE(nim::continuationWouldLeaveImageForVoxelSampling(width,
                                                               height,
                                                               depth,
                                                               anchor,
                                                               /*axisDir=*/std::array<double, 3>{0.0, 1.0, 0.0},
                                                               /*step=*/2.0,
                                                               /*radiusXY=*/0.0,
                                                               /*radiusZ=*/0.0));

  // If the axis doesn't point outward, the axis probe alone shouldn't declare out-of-image.
  EXPECT_FALSE(nim::continuationWouldLeaveImageForVoxelSampling(width,
                                                                height,
                                                                depth,
                                                                anchor,
                                                                /*axisDir=*/std::array<double, 3>{1.0, 0.0, 0.0},
                                                                /*step=*/2.0,
                                                                /*radiusXY=*/0.0,
                                                                /*radiusZ=*/0.0));

  // Radius probe: even with axis parallel to the boundary, a large radius footprint can sample out-of-image.
  EXPECT_TRUE(nim::continuationWouldLeaveImageForVoxelSampling(width,
                                                               height,
                                                               depth,
                                                               anchor,
                                                               /*axisDir=*/std::array<double, 3>{1.0, 0.0, 0.0},
                                                               /*step=*/2.0,
                                                               /*radiusXY=*/2.0,
                                                               /*radiusZ=*/0.0));
}
