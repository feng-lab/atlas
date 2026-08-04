#include "z3dtiledescriptor.h"
#include "z3dcamera.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim {
namespace {

const Z3DTileDescriptor& descriptorAt(const std::vector<Z3DTileDescriptor>& descriptors, glm::uvec2 validOutputOrigin)
{
  const auto tileIt = std::find_if(descriptors.begin(), descriptors.end(), [validOutputOrigin](const auto& tile) {
    return tile.validOutputOrigin() == validOutputOrigin;
  });
  CHECK(tileIt != descriptors.end());
  return *tileIt;
}

void expectTileProjectionMatchesFullCamera(RenderBackend backend, Z3DCamera::ProjectionType projectionType)
{
  const glm::uvec2 fullExtent(700, 500);
  const auto descriptors = makeZ3DTileDescriptors(fullExtent, glm::uvec2(300, 200), 20);
  const Z3DTileDescriptor& tile = descriptorAt(descriptors, glm::uvec2(300, 200));

  Z3DCamera fullCamera(backend);
  fullCamera.setCamera(glm::vec3(0.f, 0.f, 5.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
  fullCamera.setFrustum(glm::radians(45.f), 1.f, 0.1f, 50.f);
  fullCamera.setWindowAspectRatio(static_cast<float>(fullExtent.x) / fullExtent.y);
  fullCamera.setProjectionType(projectionType);

  Z3DCamera tileCamera = fullCamera;
  tileCamera.setTileFrustum(tile.normalizedLeft(),
                            tile.normalizedRight(),
                            tile.normalizedBottom(),
                            tile.normalizedTop());

  const std::vector<glm::vec3> worldPoints{
    glm::vec3(0.f, 0.f, 0.f),
    glm::vec3(0.2f, 0.1f, 0.f),
    glm::vec3(-0.2f, -0.1f, 0.f),
  };
  const glm::ivec4 fullViewport(0, 0, fullExtent.x, fullExtent.y);
  const glm::uvec2 attachmentExtent = tile.attachmentExtent();
  const glm::dvec2 expandedRenderOrigin(tile.normalizedLeft() * fullExtent.x, tile.normalizedBottom() * fullExtent.y);
  const glm::ivec4 tileViewport(0, 0, attachmentExtent.x, attachmentExtent.y);
  for (const glm::vec3& worldPoint : worldPoints) {
    const glm::vec3 fullScreen = fullCamera.worldToScreen(worldPoint, fullViewport);
    const glm::vec3 tileScreen = tileCamera.worldToScreen(worldPoint, tileViewport);
    EXPECT_NEAR(tileScreen.x, fullScreen.x - static_cast<float>(expandedRenderOrigin.x), 1e-3f);
    EXPECT_NEAR(tileScreen.y, fullScreen.y - static_cast<float>(expandedRenderOrigin.y), 1e-3f);
  }
}

TEST(Z3DTileDescriptorTest, SingleTileRetainsFullGuardOutsideOutput)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(16, 16), 3);

  ASSERT_EQ(descriptors.size(), 1u);
  const auto& tile = descriptors.front();
  EXPECT_EQ(tile.validOutputOrigin(), glm::uvec2(0, 0));
  EXPECT_EQ(tile.validOutputExtent(), glm::uvec2(7, 5));
  EXPECT_EQ(tile.attachmentExtent(), glm::uvec2(13, 11));
  EXPECT_EQ(tile.validAttachmentOrigin(), glm::uvec2(3, 3));
  EXPECT_EQ(tile.validAttachmentEnd(), glm::uvec2(10, 8));
  EXPECT_DOUBLE_EQ(tile.normalizedLeft(), -3.0 / 7.0);
  EXPECT_DOUBLE_EQ(tile.normalizedRight(), 10.0 / 7.0);
  EXPECT_DOUBLE_EQ(tile.normalizedBottom(), -3.0 / 5.0);
  EXPECT_DOUBLE_EQ(tile.normalizedTop(), 8.0 / 5.0);
}

TEST(Z3DTileDescriptorTest, OddExtentCoverageIsExactAndNonOverlapping)
{
  constexpr uint32_t width = 7;
  constexpr uint32_t height = 5;
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(width, height), glm::uvec2(3, 2), 1);
  std::vector<uint32_t> coverage(static_cast<size_t>(width) * height, 0u);

  for (const auto& tile : descriptors) {
    const glm::uvec2 end = tile.validOutputOrigin() + tile.validOutputExtent();
    for (uint32_t y = tile.validOutputOrigin().y; y < end.y; ++y) {
      for (uint32_t x = tile.validOutputOrigin().x; x < end.x; ++x) {
        ++coverage[static_cast<size_t>(y) * width + x];
      }
    }
  }

  EXPECT_TRUE(std::all_of(coverage.begin(), coverage.end(), [](uint32_t count) {
    return count == 1u;
  }));
}

TEST(Z3DTileDescriptorTest, TraversalPreservesSerpentineExportOrder)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(3, 2), 0);
  ASSERT_EQ(descriptors.size(), 9u);

  const std::vector<glm::uvec2> expectedOrigins{
    {0, 0},
    {3, 0},
    {6, 0},
    {6, 2},
    {3, 2},
    {0, 2},
    {0, 4},
    {3, 4},
    {6, 4},
  };
  for (size_t i = 0; i < descriptors.size(); ++i) {
    EXPECT_EQ(descriptors[i].validOutputOrigin(), expectedOrigins[i]);
  }
}

TEST(Z3DTileDescriptorTest, EdgeTilesKeepUnclippedGuardAndOneToOneProjection)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(3, 2), 4);
  const auto tileIt = std::find_if(descriptors.begin(), descriptors.end(), [](const Z3DTileDescriptor& tile) {
    return tile.validOutputOrigin() == glm::uvec2(6, 4);
  });
  ASSERT_NE(tileIt, descriptors.end());

  EXPECT_EQ(tileIt->validOutputExtent(), glm::uvec2(1, 1));
  EXPECT_EQ(tileIt->attachmentExtent(), glm::uvec2(9, 9));
  EXPECT_EQ(tileIt->validAttachmentOrigin(), glm::uvec2(4, 4));
  EXPECT_EQ(tileIt->validAttachmentEnd(), glm::uvec2(5, 5));
  EXPECT_DOUBLE_EQ(tileIt->normalizedLeft(), 2.0 / 7.0);
  EXPECT_DOUBLE_EQ(tileIt->normalizedRight(), 11.0 / 7.0);
  EXPECT_DOUBLE_EQ(tileIt->normalizedBottom(), 0.0);
  EXPECT_DOUBLE_EQ(tileIt->normalizedTop(), 9.0 / 5.0);
}

TEST(Z3DTileDescriptorTest, PerspectiveTileProjectionMatchesFullCamera)
{
  expectTileProjectionMatchesFullCamera(RenderBackend::OpenGL, Z3DCamera::ProjectionType::Perspective);
  expectTileProjectionMatchesFullCamera(RenderBackend::Vulkan, Z3DCamera::ProjectionType::Perspective);
}

TEST(Z3DTileDescriptorTest, OrthographicTileProjectionMatchesFullCamera)
{
  expectTileProjectionMatchesFullCamera(RenderBackend::OpenGL, Z3DCamera::ProjectionType::Orthographic);
  expectTileProjectionMatchesFullCamera(RenderBackend::Vulkan, Z3DCamera::ProjectionType::Orthographic);
}

TEST(Z3DTileDescriptorTest, TopLeftAssemblyOriginConvertsBottomLeftOutputCoordinates)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(3, 2), 0);
  const auto tileIt = std::find_if(descriptors.begin(), descriptors.end(), [](const Z3DTileDescriptor& tile) {
    return tile.validOutputOrigin() == glm::uvec2(3, 2);
  });
  ASSERT_NE(tileIt, descriptors.end());

  EXPECT_EQ(tileIt->validOutputOrigin(), glm::uvec2(3, 2));
  EXPECT_EQ(tileIt->topLeftAssemblyOrigin(), glm::uvec2(3, 1));
}

TEST(Z3DTileDescriptorTest, InvalidExtentsAndAttachmentOverflowFailFast)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DTileDescriptors(glm::uvec2(0, 5), glm::uvec2(3, 2), 0),
                            "width must be positive");
  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(0, 2), 0),
                            "Tile width must be positive");
  EXPECT_DEATH_IF_SUPPORTED(
    (void)makeZ3DTileDescriptors(glm::uvec2(1, 1), glm::uvec2(1, 1), std::numeric_limits<uint32_t>::max()),
    "attachment extent exceeds");
}

} // namespace
} // namespace nim
