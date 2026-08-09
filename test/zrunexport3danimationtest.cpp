#include "z3danimation.h"
#include "zrunexport3danimation.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace nim {
namespace {

using FrameRange = ZRunExport3DAnimation::FrameRange;

void writeFile(const QString& path, const QByteArray& contents)
{
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write(contents), contents.size());
}

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

TEST(ZRunExport3DAnimationTest, AppliesWeightsToAdjacentPersistentWorkerRanges)
{
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(13, 73, 2u, {2u, 1u});
  const std::vector<FrameRange> expected{
    {13, 53},
    {53, 73}
  };

  EXPECT_EQ(ranges, expected);
}

TEST(ZRunExport3DAnimationTest, AppliesWeightsToTheWholeFrameRange)
{
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(0, 4, 2u, {2u, 1u});
  const std::vector<FrameRange> expected{
    {0, 3},
    {3, 4}
  };

  EXPECT_EQ(ranges, expected);
}

TEST(ZRunExport3DAnimationTest, KeepsEveryWeightedWorkerNonemptyUnderExtremeWeights)
{
  const std::vector<FrameRange> ranges = ZRunExport3DAnimation::splitFrameRange(13, 18, 3u, {1u, 100u, 1u});
  const std::vector<FrameRange> expected{
    {13, 14},
    {14, 17},
    {17, 18}
  };

  EXPECT_EQ(ranges, expected);
}

TEST(Z3DAnimationDurationMetadataTest, ReadsDurationWithoutLoadingReferencedSceneDataOrChangingWorkingDirectory)
{
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString path = directory.filePath("test.animation3d");
  const QString workingDirectory = QDir::currentPath();
  writeFile(path,
            "// Atlas animation metadata\n"
            R"({"Animation3D":{"Doc":{"Mesh 100":"missing.mesh"},"Duration":2.5,},})");

  EXPECT_DOUBLE_EQ(Z3DAnimation::readDurationFromFile(path), 2.5);
  EXPECT_EQ(QDir::currentPath(), workingDirectory);
}

TEST(Z3DAnimationDurationMetadataTest, UsesAnimationDurationDefaultsAndNormalization)
{
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString defaultPath = directory.filePath("default.animation3d");
  const QString shortPath = directory.filePath("short.animation3d");
  writeFile(defaultPath, R"({"Animation3D":{"Doc":{}}})");
  writeFile(shortPath, R"({"Animation3D":{"Doc":{},"Duration":0.25}})");

  EXPECT_DOUBLE_EQ(Z3DAnimation::readDurationFromFile(defaultPath), 10.0);
  EXPECT_DOUBLE_EQ(Z3DAnimation::readDurationFromFile(shortPath), 1.0);
}

TEST(Z3DAnimationDurationMetadataTest, RejectsInvalidAnimationMetadata)
{
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString missingDocPath = directory.filePath("missing_doc.animation3d");
  const QString invalidDurationPath = directory.filePath("invalid_duration.animation3d");
  writeFile(missingDocPath, R"({"Animation3D":{"Duration":2.5}})");
  writeFile(invalidDurationPath, R"({"Animation3D":{"Doc":{},"Duration":"2.5"}})");

  EXPECT_THROW(static_cast<void>(Z3DAnimation::readDurationFromFile(missingDocPath)), std::exception);
  EXPECT_THROW(static_cast<void>(Z3DAnimation::readDurationFromFile(invalidDurationPath)), std::exception);
}

} // namespace
} // namespace nim
