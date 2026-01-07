#include "zneuroglancerprecomputedannotations.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace nim {

namespace {

void appendU64LE(std::vector<uint8_t>& out, uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void appendU32LE(std::vector<uint8_t>& out, uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void appendF32LE(std::vector<uint8_t>& out, float f)
{
  uint32_t u = 0;
  static_assert(sizeof(float) == sizeof(uint32_t));
  std::memcpy(&u, &f, sizeof(float));
  appendU32LE(out, u);
}

} // namespace

TEST(ZNeuroglancerPrecomputedAnnotations, ParseInfoAndDecodePointRgba)
{
  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "POINT",
  "properties": [
    { "id": "color", "type": "rgba", "description": "RGBA test" }
  ],
  "relationships": [
    { "id": "segment", "key": "by_segment" }
  ],
  "by_id": { "key": "by_id" }
}
)json";

  auto source = ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                             info,
                                                                             /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                             /*baseVoxelOffset=*/{0, 0, 0},
                                                                             std::chrono::milliseconds(1000));
  ASSERT_TRUE(source);
  EXPECT_EQ(source->annotationType(), ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point);
  ASSERT_EQ(source->relationships().size(), 1U);
  EXPECT_EQ(source->relationships().front().id, QStringLiteral("segment"));

  std::vector<uint8_t> bytes;
  appendU64LE(bytes, 1); // count
  appendF32LE(bytes, 10.0f);
  appendF32LE(bytes, 20.0f);
  appendF32LE(bytes, 30.0f);
  // rgba property (1,2,3,4)
  bytes.push_back(1);
  bytes.push_back(2);
  bytes.push_back(3);
  bytes.push_back(4);
  appendU64LE(bytes, 42); // id list

  const auto anns = source->decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_EQ(anns.size(), 1U);
  EXPECT_EQ(anns[0].id, 42U);
  ASSERT_EQ(anns[0].points.size(), 1U);
  EXPECT_FLOAT_EQ(anns[0].points[0].x, 10.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].y, 20.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].z, 30.0f);
  ASSERT_TRUE(anns[0].rgba8.has_value());
  EXPECT_EQ((*anns[0].rgba8)[0], 1);
  EXPECT_EQ((*anns[0].rgba8)[1], 2);
  EXPECT_EQ((*anns[0].rgba8)[2], 3);
  EXPECT_EQ((*anns[0].rgba8)[3], 4);
}

TEST(ZNeuroglancerPrecomputedAnnotations, DecodeAppliesBaseResolutionAndOffset)
{
  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "POINT",
  "properties": [],
  "relationships": [ { "id": "segment", "key": "by_segment" } ],
  "by_id": { "key": "by_id" }
}
)json";

  auto source = ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                             info,
                                                                             /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                             /*baseVoxelOffset=*/{100, 200, 300},
                                                                             std::chrono::milliseconds(1000));
  ASSERT_TRUE(source);

  std::vector<uint8_t> bytes;
  appendU64LE(bytes, 1); // count
  appendF32LE(bytes, 101.0f);
  appendF32LE(bytes, 202.0f);
  appendF32LE(bytes, 303.0f);
  // already aligned; no props/pad
  appendU64LE(bytes, 1); // id

  const auto anns = source->decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_EQ(anns.size(), 1U);
  ASSERT_EQ(anns[0].points.size(), 1U);
  EXPECT_FLOAT_EQ(anns[0].points[0].x, 1.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].y, 2.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].z, 3.0f);
}

TEST(ZNeuroglancerPrecomputedAnnotations, ParseInfoAndDecodeLine)
{
  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "LINE",
  "properties": [],
  "relationships": [ { "id": "segment", "key": "by_segment" } ],
  "by_id": { "key": "by_id" }
}
)json";

  auto source = ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                             info,
                                                                             /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                             /*baseVoxelOffset=*/{0, 0, 0},
                                                                             std::chrono::milliseconds(1000));
  ASSERT_TRUE(source);
  EXPECT_EQ(source->annotationType(), ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line);

  std::vector<uint8_t> bytes;
  appendU64LE(bytes, 1); // count
  appendF32LE(bytes, 1.0f);
  appendF32LE(bytes, 2.0f);
  appendF32LE(bytes, 3.0f);
  appendF32LE(bytes, 4.0f);
  appendF32LE(bytes, 5.0f);
  appendF32LE(bytes, 6.0f);
  appendU64LE(bytes, 99); // id

  const auto anns = source->decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_EQ(anns.size(), 1U);
  EXPECT_EQ(anns[0].id, 99U);
  ASSERT_EQ(anns[0].points.size(), 2U);
  EXPECT_FLOAT_EQ(anns[0].points[0].x, 1.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].y, 2.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].z, 3.0f);
  EXPECT_FLOAT_EQ(anns[0].points[1].x, 4.0f);
  EXPECT_FLOAT_EQ(anns[0].points[1].y, 5.0f);
  EXPECT_FLOAT_EQ(anns[0].points[1].z, 6.0f);
}

} // namespace nim

