#include "zneuroglancerexternalsource.h"
#include "zneuroglancerprecomputedmesh.h"

#include <gtest/gtest.h>

namespace nim {
namespace {

using DesiredChunk = ZNeuroglancerPrecomputedMeshSource::MultiLodDesiredChunk;
using DrawChunk = ZNeuroglancerPrecomputedMeshSource::MultiLodDrawChunk;
using Manifest = ZNeuroglancerPrecomputedMeshSource::MultiLodManifest;
using OctreeNode = ZNeuroglancerPrecomputedMeshSource::MultiLodOctreeNode;

[[nodiscard]] Manifest makeSimpleManifest()
{
  Manifest manifest;
  manifest.chunkShape = glm::vec3(10.0F, 20.0F, 30.0F);
  manifest.gridOrigin = glm::vec3(5.0F, 6.0F, -50.0F);
  manifest.clipLowerBound = glm::vec3(20.0F, 23.0F, -50.0F);
  manifest.clipUpperBound = glm::vec3(40.0F, 45.0F, -20.0F);
  manifest.lodScales = {20.0F, 40.0F};
  manifest.vertexOffsets = {glm::vec3(0.0F), glm::vec3(0.0F)};
  manifest.octreeNodes = {
    OctreeNode{glm::uvec3(0U, 0U, 0U), 0U, 0U},
    OctreeNode{glm::uvec3(0U, 0U, 0U), 0U, 1U},
  };
  manifest.rowLods = {0U, 1U};
  manifest.offsets = {0U, 0U, 0U};
  return manifest;
}

[[nodiscard]] Manifest makeEightChildManifest()
{
  Manifest manifest;
  manifest.chunkShape = glm::vec3(10.0F, 20.0F, 30.0F);
  manifest.gridOrigin = glm::vec3(5.0F, 6.0F, -50.0F);
  manifest.clipLowerBound = glm::vec3(5.0F, 6.0F, -50.0F);
  manifest.clipUpperBound = glm::vec3(100.0F, 200.0F, 10.0F);
  manifest.lodScales = {20.0F, 40.0F};
  manifest.vertexOffsets = {glm::vec3(0.0F), glm::vec3(0.0F)};
  manifest.octreeNodes = {
    OctreeNode{glm::uvec3(0U, 0U, 0U), 0U, 0U},
    OctreeNode{glm::uvec3(1U, 0U, 0U), 0U, 0U},
    OctreeNode{glm::uvec3(0U, 1U, 0U), 0U, 0U},
    OctreeNode{glm::uvec3(1U, 1U, 0U), 0U, 0U},
    OctreeNode{glm::uvec3(0U, 0U, 1U), 0U, 0U},
    OctreeNode{glm::uvec3(1U, 0U, 1U), 0U, 0U},
    OctreeNode{glm::uvec3(0U, 1U, 1U), 0U, 0U},
    OctreeNode{glm::uvec3(1U, 1U, 1U), 0U, 0U},
    OctreeNode{glm::uvec3(0U, 0U, 0U), 0U, 8U},
  };
  manifest.rowLods = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
  manifest.offsets = std::vector<uint64_t>(manifest.octreeNodes.size() + 1U, 0U);
  return manifest;
}

[[nodiscard]] glm::mat4 makePerspectiveMvp(uint32_t viewportWidth, uint32_t viewportHeight)
{
  return glm::perspective(glm::half_pi<float>(), static_cast<float>(viewportWidth) / viewportHeight, 5.0F, 100.0F);
}

void expectDesiredChunk(const DesiredChunk& actual, uint32_t lod, uint32_t row, float renderScale, bool empty)
{
  EXPECT_EQ(actual.lod, lod);
  EXPECT_EQ(actual.row, row);
  EXPECT_FLOAT_EQ(actual.renderScale, renderScale);
  EXPECT_EQ(actual.empty, empty);
}

void expectDrawChunk(const DrawChunk& actual,
                     uint32_t lod,
                     uint32_t row,
                     uint32_t subChunkBegin,
                     uint32_t subChunkEnd,
                     float renderScale)
{
  EXPECT_EQ(actual.lod, lod);
  EXPECT_EQ(actual.row, row);
  EXPECT_EQ(actual.subChunkBegin, subChunkBegin);
  EXPECT_EQ(actual.subChunkEnd, subChunkEnd);
  EXPECT_FLOAT_EQ(actual.renderScale, renderScale);
}

TEST(ZNeuroglancerPrecomputedMeshTest, DesiredChunksSimpleManifestMatchesNeuroglancer)
{
  const Manifest manifest = makeSimpleManifest();
  const uint32_t viewportWidth = 640;
  const uint32_t viewportHeight = 480;
  const glm::mat4 modelViewProjection = makePerspectiveMvp(viewportWidth, viewportHeight);
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);

  const auto coarseOnly = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(manifest,
                                                                                   modelViewProjection,
                                                                                   clippingPlanes,
                                                                                   1000.0F,
                                                                                   viewportWidth,
                                                                                   viewportHeight);
  ASSERT_EQ(coarseOnly.size(), 1U);
  expectDesiredChunk(coarseOnly[0], 1U, 1U, 960.0F, false);

  const auto refined = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(manifest,
                                                                                modelViewProjection,
                                                                                clippingPlanes,
                                                                                800.0F,
                                                                                viewportWidth,
                                                                                viewportHeight);
  ASSERT_EQ(refined.size(), 2U);
  expectDesiredChunk(refined[0], 1U, 1U, 960.0F, false);
  expectDesiredChunk(refined[1], 0U, 0U, 480.0F, false);
}

TEST(ZNeuroglancerPrecomputedMeshTest, DrawChunksFallBackToLoadedParents)
{
  const Manifest manifest = makeEightChildManifest();
  const uint32_t viewportWidth = 640;
  const uint32_t viewportHeight = 480;
  const glm::mat4 modelViewProjection = makePerspectiveMvp(viewportWidth, viewportHeight);
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);

  const auto allLoaded = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
    manifest,
    modelViewProjection,
    clippingPlanes,
    4000.0F,
    viewportWidth,
    viewportHeight,
    [](uint32_t /*lod*/, uint32_t /*row*/, float /*renderScale*/) {
      return true;
    });
  ASSERT_EQ(allLoaded.size(), 1U);
  expectDrawChunk(allLoaded[0], 1U, 8U, 0U, 8U, 3840.0F);

  const auto mixed =
    ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(manifest,
                                                            modelViewProjection,
                                                            clippingPlanes,
                                                            1000.0F,
                                                            viewportWidth,
                                                            viewportHeight,
                                                            [](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
                                                              return row != 4U;
                                                            });
  ASSERT_EQ(mixed.size(), 3U);
  expectDrawChunk(mixed[0], 1U, 8U, 0U, 5U, 3840.0F);
  expectDrawChunk(mixed[1], 0U, 5U, 0U, 1U, 1920.0F);
  expectDrawChunk(mixed[2], 1U, 8U, 6U, 8U, 3840.0F);
}

TEST(ZNeuroglancerPrecomputedMeshTest, ExternalSourceJsonRoundTripsBaseGeometry)
{
  const json::value v = makeNeuroglancerMeshExternalSourceJson(QStringLiteral("precomputed://gs://bucket/seg"),
                                                               QStringLiteral("precomputed://gs://bucket/seg/mesh/"),
                                                               42,
                                                               std::array<double, 3>{8.0, 8.0, 30.0},
                                                               std::array<int64_t, 3>{1, 2, 3});

  const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(v);
  ASSERT_TRUE(keyOpt.has_value());
  const auto& sourceObj = v.as_object();
  EXPECT_EQ(keyOpt->rootUrl, json::value_to<QString>(sourceObj.at("segmentation_root_url")));
  EXPECT_EQ(keyOpt->meshSourceDirUrl, json::value_to<QString>(sourceObj.at("mesh_source_url")));
  EXPECT_EQ(keyOpt->segmentId, 42U);
  ASSERT_TRUE(keyOpt->baseResolutionNm.has_value());
  ASSERT_TRUE(keyOpt->baseVoxelOffset.has_value());
  EXPECT_EQ(*keyOpt->baseResolutionNm, (std::array<double, 3>{8.0, 8.0, 30.0}));
  EXPECT_EQ(*keyOpt->baseVoxelOffset, (std::array<int64_t, 3>{1, 2, 3}));

  const QString keyWithGeometry = neuroglancerMeshKeyString(keyOpt->rootUrl,
                                                            keyOpt->meshSourceDirUrl,
                                                            keyOpt->segmentId,
                                                            keyOpt->baseResolutionNm,
                                                            keyOpt->baseVoxelOffset);
  const QString keyWithoutGeometry =
    neuroglancerMeshKeyString(keyOpt->rootUrl, keyOpt->meshSourceDirUrl, keyOpt->segmentId);
  EXPECT_NE(keyWithGeometry, keyWithoutGeometry);
}

TEST(ZNeuroglancerPrecomputedMeshTest, ExternalSourceCompatTreatsMissingGeometryAsSameObject)
{
  const json::value legacy =
    makeNeuroglancerMeshExternalSourceJson(QStringLiteral("precomputed://gs://bucket/seg"),
                                           QStringLiteral("precomputed://gs://bucket/seg/mesh/"),
                                           42);
  const json::value normalized =
    makeNeuroglancerMeshExternalSourceJson(QStringLiteral("precomputed://gs://bucket/seg"),
                                           QStringLiteral("precomputed://gs://bucket/seg/mesh/"),
                                           42,
                                           std::array<double, 3>{8.0, 8.0, 30.0},
                                           std::array<int64_t, 3>{1, 2, 3});
  const json::value differentGeometry =
    makeNeuroglancerMeshExternalSourceJson(QStringLiteral("precomputed://gs://bucket/seg"),
                                           QStringLiteral("precomputed://gs://bucket/seg/mesh/"),
                                           42,
                                           std::array<double, 3>{16.0, 16.0, 30.0},
                                           std::array<int64_t, 3>{1, 2, 3});

  const auto legacyKey = parseNeuroglancerMeshExternalSourceKey(legacy);
  const auto normalizedKey = parseNeuroglancerMeshExternalSourceKey(normalized);
  const auto differentGeometryKey = parseNeuroglancerMeshExternalSourceKey(differentGeometry);
  ASSERT_TRUE(legacyKey.has_value());
  ASSERT_TRUE(normalizedKey.has_value());
  ASSERT_TRUE(differentGeometryKey.has_value());

  EXPECT_TRUE(sameNeuroglancerMeshSourceCompat(*legacyKey, *normalizedKey));
  EXPECT_TRUE(sameNeuroglancerMeshSourceCompat(*normalizedKey, *legacyKey));
  EXPECT_FALSE(sameNeuroglancerMeshSourceCompat(*normalizedKey, *differentGeometryKey));
}

} // namespace
} // namespace nim
