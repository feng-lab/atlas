#include "zneuroglancerprecomputedskeleton.h"

#include "zexception.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nim {
namespace {

void appendU32LE(std::vector<uint8_t>& out, uint32_t v)
{
  out.push_back(static_cast<uint8_t>((v >> 0) & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

void appendF32LE(std::vector<uint8_t>& out, float f)
{
  uint32_t u = 0;
  static_assert(sizeof(float) == sizeof(uint32_t));
  std::memcpy(&u, &f, sizeof(uint32_t));
  appendU32LE(out, u);
}

} // namespace

TEST(ZNeuroglancerPrecomputedSkeleton, ParseInfoAndDecodeWithRadius)
{
  const std::string info = R"JSON(
{
  "@type": "neuroglancer_skeletons",
  "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0],
  "vertex_attributes": [
    { "id": "radius", "data_type": "float32", "num_components": 1 },
    { "id": "foo", "data_type": "uint8", "num_components": 2 }
  ]
}
)JSON";

  auto source = ZNeuroglancerPrecomputedSkeletonSource::parseInfoJsonText(QUrl("https://example.invalid/skeletons/"),
                                                                          info,
                                                                          /*baseResolutionNm=*/{1.0, 1.0, 1.0},
                                                                          /*baseVoxelOffset=*/{0, 0, 0},
                                                                          std::chrono::milliseconds(123));
  ASSERT_TRUE(source);
  EXPECT_FALSE(source->hasSharding());

  // skeleton binary format:
  // [u32 numVertices][u32 numEdges]
  // [numVertices * vec3<float32> positions]
  // [numEdges * u32 u32 edge indices]
  // [per-vertex attributes as declared in info]
  std::vector<uint8_t> bytes;
  appendU32LE(bytes, 2); // vertices
  appendU32LE(bytes, 1); // edges

  // v0 = (1,2,3), v1 = (4,5,6)
  appendF32LE(bytes, 1.0f);
  appendF32LE(bytes, 2.0f);
  appendF32LE(bytes, 3.0f);
  appendF32LE(bytes, 4.0f);
  appendF32LE(bytes, 5.0f);
  appendF32LE(bytes, 6.0f);

  // edge 0->1
  appendU32LE(bytes, 0);
  appendU32LE(bytes, 1);

  // radius (float32 per vertex)
  appendF32LE(bytes, 2.0f);
  appendF32LE(bytes, 3.0f);

  // foo (uint8 x2 per vertex) - skipped by decoder but must be consumed
  bytes.push_back(0xaa);
  bytes.push_back(0xbb);
  bytes.push_back(0xcc);
  bytes.push_back(0xdd);

  auto skel = source->decodeSkeletonBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(skel);
  EXPECT_EQ(skel->numVertices(), 2U);
  EXPECT_EQ(skel->numEdges(), 1U);
  ASSERT_EQ(skel->vertices().size(), 2U);
  ASSERT_EQ(skel->edges().size(), 1U);

  EXPECT_FLOAT_EQ(skel->vertices()[0].x, 1.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[0].y, 2.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[0].z, 3.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].x, 4.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].y, 5.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].z, 6.0f);

  EXPECT_EQ(skel->edges()[0].x, 0U);
  EXPECT_EQ(skel->edges()[0].y, 1U);

  ASSERT_TRUE(skel->hasRadii());
  ASSERT_EQ(skel->radii().size(), 2U);
  EXPECT_FLOAT_EQ(skel->radii()[0], 2.0f);
  EXPECT_FLOAT_EQ(skel->radii()[1], 3.0f);
}

TEST(ZNeuroglancerPrecomputedSkeleton, DecodeAppliesBaseResolutionAndOffset)
{
  const std::string info = R"JSON(
{
  "@type": "neuroglancer_skeletons",
  "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0]
}
)JSON";

  // baseResolutionNm and baseVoxelOffset come from the segmentation volume.
  // Skeleton vertices are stored in "model" units (nm), and decoded into local-voxel coordinates:
  // voxel = (model_nm / baseResolutionNm) - baseVoxelOffset
  auto source = ZNeuroglancerPrecomputedSkeletonSource::parseInfoJsonText(QUrl("https://example.invalid/skeletons/"),
                                                                          info,
                                                                          /*baseResolutionNm=*/{2.0, 4.0, 8.0},
                                                                          /*baseVoxelOffset=*/{10, 20, 30},
                                                                          std::chrono::milliseconds(123));
  ASSERT_TRUE(source);

  std::vector<uint8_t> bytes;
  appendU32LE(bytes, 2);
  appendU32LE(bytes, 1);

  // v0 model nm = (20,80,240) -> voxel=(10,20,30)-offset=(0,0,0)
  // v1 model nm = (22,84,248) -> voxel=(11,21,31)-offset=(1,1,1)
  appendF32LE(bytes, 20.0f);
  appendF32LE(bytes, 80.0f);
  appendF32LE(bytes, 240.0f);
  appendF32LE(bytes, 22.0f);
  appendF32LE(bytes, 84.0f);
  appendF32LE(bytes, 248.0f);

  appendU32LE(bytes, 0);
  appendU32LE(bytes, 1);

  auto skel = source->decodeSkeletonBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(skel);
  ASSERT_EQ(skel->vertices().size(), 2U);

  EXPECT_FLOAT_EQ(skel->vertices()[0].x, 0.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[0].y, 0.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[0].z, 0.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].x, 1.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].y, 1.0f);
  EXPECT_FLOAT_EQ(skel->vertices()[1].z, 1.0f);

  EXPECT_FALSE(skel->hasRadii());
}

TEST(ZNeuroglancerPrecomputedSkeleton, RejectsInvalidRadiusAttribute)
{
  const std::string info = R"JSON(
{
  "@type": "neuroglancer_skeletons",
  "vertex_attributes": [
    { "id": "radius", "data_type": "uint16", "num_components": 1 }
  ]
}
)JSON";

  EXPECT_THROW((void)ZNeuroglancerPrecomputedSkeletonSource::parseInfoJsonText(QUrl("https://example.invalid/skeletons/"),
                                                                               info,
                                                                               /*baseResolutionNm=*/{1.0, 1.0, 1.0},
                                                                               /*baseVoxelOffset=*/{0, 0, 0},
                                                                               std::chrono::milliseconds(123)),
               ZException);
}

} // namespace nim

