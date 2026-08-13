#include "z3dtiledescriptor.h"
#include "z3dcamera.h"
#include "z3drenderedframe.h"

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

TEST(Z3DTileDescriptorTest, FixedRegionsSplitIntoStableVerticalStrips)
{
  const auto descriptors = makeZ3DFixedRegionDescriptors(glm::uvec2(11, 7), 3u, 2u);
  ASSERT_EQ(descriptors.size(), 3u);

  const std::vector<glm::uvec2> expectedOrigins{
    {0, 0},
    {4, 0},
    {8, 0}
  };
  const std::vector<glm::uvec2> expectedExtents{
    {4, 7},
    {4, 7},
    {3, 7}
  };
  for (size_t i = 0u; i < descriptors.size(); ++i) {
    EXPECT_EQ(descriptors[i].fullOutputExtent(), glm::uvec2(11, 7));
    EXPECT_EQ(descriptors[i].validOutputOrigin(), expectedOrigins[i]);
    EXPECT_EQ(descriptors[i].validOutputExtent(), expectedExtents[i]);
    EXPECT_EQ(descriptors[i].guardPixels(), 2u);
    EXPECT_EQ(descriptors[i].attachmentExtent(), expectedExtents[i] + glm::uvec2(4u));
  }
}

TEST(Z3DTileDescriptorTest, FixedRegionsCoverOddExtentExactlyOnce)
{
  constexpr uint32_t width = 13u;
  constexpr uint32_t height = 5u;
  const auto descriptors = makeZ3DFixedRegionDescriptors(glm::uvec2(width, height), 5u, 1u);
  std::vector<uint32_t> coverage(static_cast<size_t>(width) * height, 0u);

  for (const Z3DTileDescriptor& region : descriptors) {
    const glm::uvec2 end = region.validOutputOrigin() + region.validOutputExtent();
    for (uint32_t y = region.validOutputOrigin().y; y < end.y; ++y) {
      for (uint32_t x = region.validOutputOrigin().x; x < end.x; ++x) {
        ++coverage[static_cast<size_t>(y) * width + x];
      }
    }
  }

  EXPECT_TRUE(std::all_of(coverage.begin(), coverage.end(), [](uint32_t count) {
    return count == 1u;
  }));
}

TEST(Z3DTileDescriptorTest, FixedRegionsOwnAndMapTopLeftOutputPixels)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  constexpr uint32_t width = 11u;
  constexpr uint32_t height = 7u;
  constexpr uint32_t guardPixels = 2u;
  const auto descriptors = makeZ3DFixedRegionDescriptors(glm::uvec2(width, height), 3u, guardPixels);
  ASSERT_EQ(descriptors.size(), 3u);

  for (uint32_t y = 0u; y < height; ++y) {
    for (uint32_t x = 0u; x < width; ++x) {
      const glm::uvec2 pixel(x, y);
      const auto owner = std::find_if(descriptors.begin(), descriptors.end(), [pixel](const auto& descriptor) {
        return descriptor.containsTopLeftOutputPixel(pixel);
      });
      ASSERT_NE(owner, descriptors.end());
      EXPECT_EQ(std::count_if(descriptors.begin(),
                              descriptors.end(),
                              [pixel](const auto& descriptor) {
                                return descriptor.containsTopLeftOutputPixel(pixel);
                              }),
                1);

      const glm::uvec2 expectedAttachmentPixel =
        owner->validAttachmentOrigin() + pixel - owner->topLeftAssemblyOrigin();
      EXPECT_EQ(owner->topLeftAttachmentPixel(pixel), expectedAttachmentPixel);
    }
  }

  EXPECT_EQ(descriptors[0].topLeftAttachmentPixel(glm::uvec2(0u, 0u)), glm::uvec2(2u, 2u));
  EXPECT_EQ(descriptors[0].topLeftAttachmentPixel(glm::uvec2(3u, 6u)), glm::uvec2(5u, 8u));
  EXPECT_FALSE(descriptors[0].containsTopLeftOutputPixel(glm::uvec2(4u, 0u)));
  EXPECT_EQ(descriptors[1].topLeftAttachmentPixel(glm::uvec2(4u, 0u)), glm::uvec2(2u, 2u));
  EXPECT_FALSE(descriptors[1].containsTopLeftOutputPixel(glm::uvec2(8u, 6u)));
  EXPECT_EQ(descriptors[2].topLeftAttachmentPixel(glm::uvec2(8u, 6u)), glm::uvec2(2u, 8u));
  EXPECT_EQ(descriptors[2].topLeftAttachmentPixel(glm::uvec2(10u, 6u)), glm::uvec2(4u, 8u));

  for (const Z3DTileDescriptor& descriptor : descriptors) {
    EXPECT_FALSE(descriptor.containsTopLeftOutputPixel(glm::uvec2(width, 0u)));
    EXPECT_FALSE(descriptor.containsTopLeftOutputPixel(glm::uvec2(0u, height)));
  }
  EXPECT_DEATH_IF_SUPPORTED((void)descriptors[0].topLeftAttachmentPixel(glm::uvec2(4u, 0u)),
                            "must belong to this tile");
}

TEST(Z3DTileDescriptorTest, SingleFixedRegionUsesTheFullOutput)
{
  const auto descriptors = makeZ3DFixedRegionDescriptors(glm::uvec2(7, 5), 1u, 3u);

  ASSERT_EQ(descriptors.size(), 1u);
  EXPECT_EQ(descriptors.front().validOutputOrigin(), glm::uvec2(0u));
  EXPECT_EQ(descriptors.front().validOutputExtent(), glm::uvec2(7u, 5u));
  EXPECT_EQ(descriptors.front().attachmentExtent(), glm::uvec2(13u, 11u));
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

TEST(Z3DTileDescriptorTest, TopLeftPixelOwnershipConvertsBottomLeftTileCoordinates)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(7, 5), glm::uvec2(3, 2), 1);
  const Z3DTileDescriptor& tile = descriptorAt(descriptors, glm::uvec2(3, 2));

  EXPECT_EQ(tile.topLeftAssemblyOrigin(), glm::uvec2(3u, 1u));
  EXPECT_TRUE(tile.containsTopLeftOutputPixel(glm::uvec2(3u, 1u)));
  EXPECT_TRUE(tile.containsTopLeftOutputPixel(glm::uvec2(5u, 2u)));
  EXPECT_FALSE(tile.containsTopLeftOutputPixel(glm::uvec2(3u, 0u)));
  EXPECT_FALSE(tile.containsTopLeftOutputPixel(glm::uvec2(3u, 3u)));
  EXPECT_EQ(tile.topLeftAttachmentPixel(glm::uvec2(3u, 1u)), glm::uvec2(1u, 1u));
  EXPECT_EQ(tile.topLeftAttachmentPixel(glm::uvec2(5u, 2u)), glm::uvec2(3u, 2u));
}

TEST(Z3DTileDescriptorTest, RenderedFrameAssemblyPlacesMonoAndStereoTilesInTopLeftCoordinates)
{
  const auto descriptors = makeZ3DTileDescriptors(glm::uvec2(5, 3), glm::uvec2(2, 2), 1);
  Z3DRenderedFrame monoFrame(glm::uvec2(5, 3), false);
  Z3DRenderedFrame frame(glm::uvec2(5, 3), true);
  std::vector<Z3DRenderedTile> renderedTiles;
  renderedTiles.reserve(descriptors.size());
  for (const Z3DTileDescriptor& descriptor : descriptors) {
    const uint8_t primaryValue =
      static_cast<uint8_t>(1u + descriptor.validOutputOrigin().x + descriptor.validOutputOrigin().y * 10u);
    const uint8_t rightValue = static_cast<uint8_t>(primaryValue + 100u);
    Z3DRenderedTile renderedTile;
    renderedTile.primaryColor =
      ZImg(ZImgInfo(descriptor.validOutputExtent().x, descriptor.validOutputExtent().y, 1, 4));
    std::fill_n(renderedTile.primaryColor.timeData(0u), renderedTile.primaryColor.timeByteNumber(), primaryValue);
    renderedTile.rightColor = ZImg(ZImgInfo(descriptor.validOutputExtent().x, descriptor.validOutputExtent().y, 1, 4));
    std::fill_n(renderedTile.rightColor->timeData(0u), renderedTile.rightColor->timeByteNumber(), rightValue);

    Z3DRenderedTile monoTile;
    monoTile.primaryColor = renderedTile.primaryColor;
    monoFrame.pasteTile(descriptor, monoTile);
    frame.pasteTile(descriptor, renderedTile);
    renderedTiles.push_back(std::move(renderedTile));
  }

  EXPECT_FALSE(monoFrame.rightColor.has_value());
  ASSERT_TRUE(frame.rightColor.has_value());
  EXPECT_EQ(monoFrame.primaryColor.info().lastChannelIsAlphaChannel, true);
  EXPECT_EQ(frame.primaryColor.info().lastChannelIsAlphaChannel, true);
  EXPECT_EQ(frame.rightColor->info().lastChannelIsAlphaChannel, true);
  for (size_t tileIndex = 0u; tileIndex < descriptors.size(); ++tileIndex) {
    const Z3DTileDescriptor& descriptor = descriptors[tileIndex];
    const glm::uvec2 assemblyOrigin = descriptor.topLeftAssemblyOrigin();
    const uint8_t expectedPrimary = *renderedTiles[tileIndex].primaryColor.data(0u, 0u);
    const uint8_t expectedRight = *renderedTiles[tileIndex].rightColor->data(0u, 0u);
    for (uint32_t y = 0u; y < descriptor.validOutputExtent().y; ++y) {
      for (uint32_t x = 0u; x < descriptor.validOutputExtent().x; ++x) {
        for (size_t channel = 0u; channel < 4u; ++channel) {
          EXPECT_EQ(*frame.primaryColor.data(assemblyOrigin.x + x, assemblyOrigin.y + y, 0u, channel), expectedPrimary);
          EXPECT_EQ(*monoFrame.primaryColor.data(assemblyOrigin.x + x, assemblyOrigin.y + y, 0u, channel),
                    expectedPrimary);
          EXPECT_EQ(*frame.rightColor->data(assemblyOrigin.x + x, assemblyOrigin.y + y, 0u, channel), expectedRight);
        }
      }
    }
  }
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
  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DFixedRegionDescriptors(glm::uvec2(0, 5), 2u, 0u),
                            "output width must be positive");
  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DFixedRegionDescriptors(glm::uvec2(7, 0), 2u, 0u),
                            "output height must be positive");
  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DFixedRegionDescriptors(glm::uvec2(7, 5), 0u, 0u), "count must be positive");
  EXPECT_DEATH_IF_SUPPORTED((void)makeZ3DFixedRegionDescriptors(glm::uvec2(2, 5), 3u, 0u),
                            "cannot exceed the physical output width");
}

} // namespace
} // namespace nim
