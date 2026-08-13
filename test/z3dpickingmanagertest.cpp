#include "z3dpickingmanager.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace nim {
namespace {

Z3DPickingManager::QueryOverride completeQueryOverride()
{
  return {
    [](glm::ivec2) {
      return Z3DPickingManager::PickingObject{};
    },
    [](glm::ivec2) -> GLfloat {
      return 1.f;
    },
    [](glm::ivec2, int, bool) {
      return std::vector<Z3DPickingManager::PickingObject>{};
    },
    [](size_t, glm::ivec2) {
      return std::optional<Z3DPickingManager::ImageDepthSample>{};
    },
  };
}

TEST(Z3DPickingManagerRegistrationTest, ReturnedTokensPreserveExactAliasIdentity)
{
  Z3DPickingManager manager;

  int sharedObjectStorage = 0;
  int secondObjectStorage = 0;
  const void* sharedObject = &sharedObjectStorage;
  const void* secondObject = &secondObjectStorage;
  constexpr size_t firstObjectId = 101u;
  constexpr size_t secondObjectId = 205u;
  const Z3DPickingManager::PickingObject firstAlias{firstObjectId, sharedObject};
  const Z3DPickingManager::PickingObject secondAlias{secondObjectId, sharedObject};
  const Z3DPickingManager::PickingObject secondPayload{firstObjectId, secondObject};

  const glm::col4 firstAliasColor = manager.registerObject(sharedObject, firstObjectId);
  const glm::col4 secondAliasColor = manager.registerObject(sharedObject, secondObjectId);
  const glm::col4 secondPayloadColor = manager.registerObject(secondObject, firstObjectId);

  EXPECT_NE(firstAliasColor, secondAliasColor);
  EXPECT_NE(firstAliasColor, secondPayloadColor);
  EXPECT_NE(secondAliasColor, secondPayloadColor);
  EXPECT_EQ(manager.pickingObjectOfColor(firstAliasColor), firstAlias);
  EXPECT_EQ(manager.pickingObjectOfColor(secondAliasColor), secondAlias);
  EXPECT_EQ(manager.pickingObjectOfColor(secondPayloadColor), secondPayload);

  manager.deregisterObject(firstAliasColor);
  EXPECT_EQ(manager.pickingObjectOfColor(firstAliasColor), Z3DPickingManager::PickingObject{});
  EXPECT_EQ(manager.pickingObjectOfColor(secondAliasColor), secondAlias);
  EXPECT_EQ(manager.pickingObjectOfColor(secondPayloadColor), secondPayload);

  manager.deregisterObject(secondAliasColor);
  EXPECT_EQ(manager.pickingObjectOfColor(secondAliasColor), Z3DPickingManager::PickingObject{});
  EXPECT_EQ(manager.pickingObjectOfColor(secondPayloadColor), secondPayload);

  manager.deregisterObject(secondPayloadColor);
  EXPECT_EQ(manager.pickingObjectOfColor(secondPayloadColor), Z3DPickingManager::PickingObject{});
}

TEST(Z3DPickingManagerQueryOverrideTest, InactiveStateUsesDirectNoTargetBehavior)
{
  Z3DPickingManager manager;

  EXPECT_FALSE(manager.hasQueryOverride());
  EXPECT_EQ(manager.pickingObjectAtWidgetPos(glm::ivec2(7, 11)), Z3DPickingManager::PickingObject{});
  EXPECT_EQ(manager.objectAtWidgetPos(glm::ivec2(7, 11)), nullptr);
  EXPECT_FLOAT_EQ(manager.depthAtWidgetPos(glm::ivec2(13, 17)), 1.f);
  EXPECT_TRUE(manager.sortPickingObjectsByDistanceToPos(glm::ivec2(19, 23), 5, false).empty());
  EXPECT_FALSE(manager.imageDepthAtPhysicalInput(29u, glm::ivec2(31, 37)).has_value());
}

TEST(Z3DPickingManagerQueryOverrideTest, ForwardsExactArgumentsAndResults)
{
  Z3DPickingManager manager;
  manager.setDevicePixelRatio(3.5);

  int sharedObjectStorage = 0;
  const void* sharedObject = &sharedObjectStorage;
  constexpr size_t firstObjectId = 47u;
  constexpr size_t secondObjectId = 53u;
  const Z3DPickingManager::PickingObject firstAlias{firstObjectId, sharedObject};
  const Z3DPickingManager::PickingObject secondAlias{secondObjectId, sharedObject};
  const glm::ivec2 objectPosition(-7, 13);
  const glm::ivec2 depthPosition(17, -19);
  const glm::ivec2 sortPosition(23, 29);
  const glm::ivec2 imagePosition(-31, 37);
  constexpr int radius = 41;
  constexpr bool ascend = false;
  constexpr size_t imageObjectId = std::numeric_limits<size_t>::max() - 43u;
  const std::vector<Z3DPickingManager::PickingObject> sortedPickingObjects{secondAlias, firstAlias};
  const Z3DPickingManager::ImageDepthSample returnedImageDepth{.depth = 0.75f,
                                                               .topLeftPhysicalPixel = glm::uvec2(31u, 37u),
                                                               .fullPhysicalExtent = glm::uvec2(101u, 103u)};

  int objectCallCount = 0;
  int depthCallCount = 0;
  int sortCallCount = 0;
  int imageDepthCallCount = 0;
  glm::ivec2 forwardedObjectPosition(0);
  glm::ivec2 forwardedDepthPosition(0);
  glm::ivec2 forwardedSortPosition(0);
  glm::ivec2 forwardedImagePosition(0);
  int forwardedRadius = 0;
  bool forwardedAscend = true;
  size_t forwardedImageObjectId = 0u;

  Z3DPickingManager::QueryOverride queryOverride{
    [&](glm::ivec2 position) {
      ++objectCallCount;
      forwardedObjectPosition = position;
      return firstAlias;
    },
    [&](glm::ivec2 position) -> GLfloat {
      ++depthCallCount;
      forwardedDepthPosition = position;
      return 0.25f;
    },
    [&](glm::ivec2 position, int forwardedQueryRadius, bool forwardedQueryAscend) {
      ++sortCallCount;
      forwardedSortPosition = position;
      forwardedRadius = forwardedQueryRadius;
      forwardedAscend = forwardedQueryAscend;
      return sortedPickingObjects;
    },
    [&](size_t objectId, glm::ivec2 position) -> std::optional<Z3DPickingManager::ImageDepthSample> {
      ++imageDepthCallCount;
      forwardedImageObjectId = objectId;
      forwardedImagePosition = position;
      return returnedImageDepth;
    },
  };

  {
    auto queryScope = manager.scopedQueryOverride(queryOverride);
    (void)queryScope;
    EXPECT_TRUE(manager.hasQueryOverride());
    EXPECT_EQ(manager.pickingObjectAtWidgetPos(objectPosition), firstAlias);
    EXPECT_EQ(manager.objectAtWidgetPos(objectPosition), sharedObject);
    EXPECT_FLOAT_EQ(manager.depthAtWidgetPos(depthPosition), 0.25f);
    EXPECT_EQ(manager.sortPickingObjectsByDistanceToPos(sortPosition, radius, ascend), sortedPickingObjects);
    const std::optional<Z3DPickingManager::ImageDepthSample> imageDepth =
      manager.imageDepthAtPhysicalInput(imageObjectId, imagePosition);
    ASSERT_TRUE(imageDepth.has_value());
    EXPECT_FLOAT_EQ(imageDepth->depth, returnedImageDepth.depth);
    EXPECT_EQ(imageDepth->topLeftPhysicalPixel, returnedImageDepth.topLeftPhysicalPixel);
    EXPECT_EQ(imageDepth->fullPhysicalExtent, returnedImageDepth.fullPhysicalExtent);
    EXPECT_EQ(imageDepth->bottomLeftUnprojectionPixel(), glm::ivec2(31, 65));
  }

  EXPECT_EQ(objectCallCount, 2);
  EXPECT_EQ(depthCallCount, 1);
  EXPECT_EQ(sortCallCount, 1);
  EXPECT_EQ(imageDepthCallCount, 1);
  EXPECT_EQ(forwardedObjectPosition, objectPosition);
  EXPECT_EQ(forwardedDepthPosition, depthPosition);
  EXPECT_EQ(forwardedSortPosition, sortPosition);
  EXPECT_EQ(forwardedImagePosition, imagePosition);
  EXPECT_EQ(forwardedRadius, radius);
  EXPECT_EQ(forwardedAscend, ascend);
  EXPECT_EQ(forwardedImageObjectId, imageObjectId);
}

TEST(Z3DPickingManagerQueryOverrideTest, ScopeRestoresDirectQueries)
{
  Z3DPickingManager manager;
  int objectStorage = 0;
  const void* object = &objectStorage;
  constexpr size_t objectId = 59u;
  const Z3DPickingManager::PickingObject pickingObject{objectId, object};
  auto queryOverride = completeQueryOverride();
  queryOverride.objectAtWidgetPos = [pickingObject](glm::ivec2) {
    return pickingObject;
  };

  {
    auto queryScope = manager.scopedQueryOverride(queryOverride);
    (void)queryScope;
    ASSERT_TRUE(manager.hasQueryOverride());
    EXPECT_EQ(manager.pickingObjectAtWidgetPos(glm::ivec2(3, 5)), pickingObject);
    EXPECT_EQ(manager.objectAtWidgetPos(glm::ivec2(3, 5)), object);
  }

  EXPECT_FALSE(manager.hasQueryOverride());
  EXPECT_EQ(manager.pickingObjectAtWidgetPos(glm::ivec2(3, 5)), Z3DPickingManager::PickingObject{});
  EXPECT_EQ(manager.objectAtWidgetPos(glm::ivec2(3, 5)), nullptr);
  EXPECT_FLOAT_EQ(manager.depthAtWidgetPos(glm::ivec2(7, 11)), 1.f);
  EXPECT_TRUE(manager.sortPickingObjectsByDistanceToPos(glm::ivec2(13, 17), 19, true).empty());
  EXPECT_FALSE(manager.imageDepthAtPhysicalInput(23u, glm::ivec2(29, 31)).has_value());
}

TEST(Z3DPickingManagerImageDepthSampleTest, MatchesDirectPhysicalSamplingAndUnprojection)
{
  struct Case
  {
    double devicePixelRatio;
    glm::uvec2 fullPhysicalExtent;
    glm::ivec2 physicalInputPosition;
    glm::uvec2 topLeftPhysicalSamplePixel;
    glm::ivec2 bottomLeftUnprojectionPixel;
  };

  const glm::dvec2 logicalPosition(3.75, 2.25);
  const std::array<Case, 3u> cases{
    Case{1.0, glm::uvec2(7u,  5u),  glm::ivec2(3, 2), glm::uvec2(3u, 1u), glm::ivec2(3, 3)},
    Case{2.0, glm::uvec2(14u, 10u), glm::ivec2(7, 4), glm::uvec2(7u, 3u), glm::ivec2(7, 6)},
    Case{1.5, glm::uvec2(10u, 7u),  glm::ivec2(5, 3), glm::uvec2(5u, 2u), glm::ivec2(5, 4)},
  };

  for (const Case& testCase : cases) {
    SCOPED_TRACE(testing::Message() << "devicePixelRatio=" << testCase.devicePixelRatio);
    const glm::ivec2 physicalInputPosition(static_cast<int>(logicalPosition.x * testCase.devicePixelRatio),
                                           static_cast<int>(logicalPosition.y * testCase.devicePixelRatio));
    EXPECT_EQ(physicalInputPosition, testCase.physicalInputPosition);

    const glm::uvec2 samplePixel =
      Z3DPickingManager::ImageDepthSample::samplePixelForPhysicalInput(physicalInputPosition,
                                                                       testCase.fullPhysicalExtent);
    EXPECT_EQ(samplePixel, testCase.topLeftPhysicalSamplePixel);
    const Z3DPickingManager::ImageDepthSample sample{.depth = 0.5f,
                                                     .topLeftPhysicalPixel = samplePixel,
                                                     .fullPhysicalExtent = testCase.fullPhysicalExtent};
    EXPECT_EQ(sample.bottomLeftUnprojectionPixel(), testCase.bottomLeftUnprojectionPixel);
  }
}

TEST(Z3DPickingManagerImageDepthSampleTest, ClampsLikeDirectImageDepthPath)
{
  const glm::uvec2 fullPhysicalExtent(7u, 5u);
  struct Case
  {
    glm::ivec2 physicalInputPosition;
    glm::uvec2 topLeftPhysicalSamplePixel;
    glm::ivec2 bottomLeftUnprojectionPixel;
  };
  const std::array<Case, 5u> cases{
    Case{glm::ivec2(-9, -11), glm::uvec2(0u, 0u), glm::ivec2(0, 4)},
    Case{glm::ivec2(3,  0),   glm::uvec2(3u, 0u), glm::ivec2(3, 4)},
    Case{glm::ivec2(3,  1),   glm::uvec2(3u, 0u), glm::ivec2(3, 4)},
    Case{glm::ivec2(3,  5),   glm::uvec2(3u, 4u), glm::ivec2(3, 0)},
    Case{glm::ivec2(17, 19),  glm::uvec2(6u, 4u), glm::ivec2(6, 0)},
  };

  for (const Case& testCase : cases) {
    const glm::uvec2 samplePixel =
      Z3DPickingManager::ImageDepthSample::samplePixelForPhysicalInput(testCase.physicalInputPosition,
                                                                       fullPhysicalExtent);
    EXPECT_EQ(samplePixel, testCase.topLeftPhysicalSamplePixel);
    const Z3DPickingManager::ImageDepthSample sample{.depth = 0.5f,
                                                     .topLeftPhysicalPixel = samplePixel,
                                                     .fullPhysicalExtent = fullPhysicalExtent};
    EXPECT_EQ(sample.bottomLeftUnprojectionPixel(), testCase.bottomLeftUnprojectionPixel);
  }
}

TEST(Z3DPickingManagerQueryOverrideTest, RejectsPartialOverride)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  Z3DPickingManager manager;
  auto partialOverride = completeQueryOverride();
  partialOverride.depthAtWidgetPos = {};

  EXPECT_DEATH_IF_SUPPORTED(
    {
      auto queryScope = manager.scopedQueryOverride(partialOverride);
      (void)queryScope;
    },
    "requires depthAtWidgetPos");
}

TEST(Z3DPickingManagerQueryOverrideTest, RejectsNestedOverride)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  Z3DPickingManager manager;
  const auto outerOverride = completeQueryOverride();
  const auto nestedOverride = completeQueryOverride();
  auto outerScope = manager.scopedQueryOverride(outerOverride);
  (void)outerScope;
  ASSERT_TRUE(manager.hasQueryOverride());

  EXPECT_DEATH_IF_SUPPORTED(
    {
      auto nestedScope = manager.scopedQueryOverride(nestedOverride);
      (void)nestedScope;
    },
    "cannot be nested");
  EXPECT_TRUE(manager.hasQueryOverride());
}

} // namespace
} // namespace nim
