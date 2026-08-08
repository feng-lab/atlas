#include "zvideoencoder.h"

#include <gtest/gtest.h>

#include <optional>

namespace nim {
namespace {

TEST(ZVideoEncoderTest, BuildsArgumentsForRequestedNonzeroFrameRange)
{
  const auto [program, arguments] = ZVideoEncoder::encodeDryRun(QDir("/tmp/atlas_animation_frames"),
                                                                "frame_",
                                                                5,
                                                                30,
                                                                180,
                                                                size_t{2},
                                                                "/tmp/atlas_animation.mp4");
  EXPECT_FALSE(program.isEmpty());

  const qsizetype startNumberIndex = arguments.indexOf("-start_number");
  ASSERT_GE(startNumberIndex, 0);
  ASSERT_LT(startNumberIndex + 1, arguments.size());
  EXPECT_EQ(arguments[startNumberIndex + 1], "180");

  const qsizetype frameCountIndex = arguments.indexOf("-frames:v");
  ASSERT_GE(frameCountIndex, 0);
  ASSERT_LT(frameCountIndex + 1, arguments.size());
  EXPECT_EQ(arguments[frameCountIndex + 1], "2");
  const qsizetype inputIndex = arguments.indexOf("-i");
  ASSERT_GE(inputIndex, 0);
  EXPECT_LT(startNumberIndex, inputIndex);
  const qsizetype startNumberRangeIndex = arguments.indexOf("-start_number_range");
  ASSERT_GE(startNumberRangeIndex, 0);
  ASSERT_LT(startNumberRangeIndex + 1, arguments.size());
  EXPECT_EQ(arguments[startNumberRangeIndex + 1], "1");
  EXPECT_LT(startNumberRangeIndex, inputIndex);
  EXPECT_LT(frameCountIndex, arguments.size() - 1);
  EXPECT_EQ(arguments.back(), "/tmp/atlas_animation.mp4");
}

TEST(ZVideoEncoderTest, AllowsAnUnboundedSequenceForStandaloneCompression)
{
  const auto [program, arguments] = ZVideoEncoder::encodeDryRun(QDir("/tmp/atlas_animation_frames"),
                                                                "frame_",
                                                                5,
                                                                30,
                                                                0,
                                                                std::nullopt,
                                                                "/tmp/atlas_animation.mp4");
  EXPECT_FALSE(program.isEmpty());
  EXPECT_EQ(arguments.count("-start_number"), 1);
  EXPECT_EQ(arguments.count("-frames:v"), 0);
}

} // namespace
} // namespace nim
