#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerremotecontext.h"
#include "zremoteobjectstore.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

class FakeRemoteObjectStore final : public ZRemoteObjectStore
{
public:
  struct Request
  {
    std::string url;
    std::chrono::milliseconds timeout{0};
    std::vector<std::pair<std::string, std::string>> headers;
  };

  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>>
  getBytes(std::string url,
           std::chrono::milliseconds timeout,
           std::vector<std::pair<std::string, std::string>> requestHeaders) const override
  {
    requests.push_back(Request{.url = std::move(url), .timeout = timeout, .headers = std::move(requestHeaders)});
    if (responses.empty()) {
      throw std::runtime_error("FakeRemoteObjectStore called without a queued response");
    }
    auto next = std::move(responses.front());
    responses.pop_front();
    co_return next;
  }

  mutable std::deque<std::optional<ZHttpGetBytesResult>> responses;
  mutable std::vector<Request> requests;
};

folly::coro::Task<std::vector<ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadUpdate>>
collectSpatialUpdates(const ZNeuroglancerPrecomputedAnnotationsSource& source,
                      const glm::dvec3& voxelMin,
                      const glm::dvec3& voxelMax,
                      ZNeuroglancerPrecomputedAnnotationsSource::SpatialStreamOptions options)
{
  std::vector<ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadUpdate> out;
  auto updates = source.streamAnnotationsIntersectingVoxelBoxAsync(voxelMin, voxelMax, options);
  while (auto update = co_await updates.next()) {
    out.push_back(std::move(*update));
  }
  co_return out;
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

TEST(ZNeuroglancerPrecomputedAnnotations, ParseInfoAndDecodeEllipsoidRadii)
{
  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "ELLIPSOID",
  "properties": [],
  "relationships": [],
  "by_id": { "key": "by_id" }
}
)json";

  auto source = ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                             info,
                                                                             /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                             /*baseVoxelOffset=*/{0, 0, 0},
                                                                             std::chrono::milliseconds(1000));
  ASSERT_TRUE(source);
  EXPECT_EQ(source->annotationType(), ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid);

  std::vector<uint8_t> bytes;
  appendU64LE(bytes, 1); // count
  // center (coord units -> voxel == coord because dimScale == baseResolution)
  appendF32LE(bytes, 1.0f);
  appendF32LE(bytes, 2.0f);
  appendF32LE(bytes, 3.0f);
  // radii in coord units -> voxel radii
  appendF32LE(bytes, 10.0f);
  appendF32LE(bytes, 20.0f);
  appendF32LE(bytes, 30.0f);
  // padding/properties: none
  appendU64LE(bytes, 7); // id list

  const auto anns = source->decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_EQ(anns.size(), 1U);
  EXPECT_EQ(anns[0].id, 7U);
  ASSERT_EQ(anns[0].points.size(), 1U);
  EXPECT_FLOAT_EQ(anns[0].points[0].x, 1.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].y, 2.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].z, 3.0f);
  ASSERT_TRUE(anns[0].ellipsoidRadiiVoxel.has_value());
  EXPECT_FLOAT_EQ(anns[0].ellipsoidRadiiVoxel->x, 10.0f);
  EXPECT_FLOAT_EQ(anns[0].ellipsoidRadiiVoxel->y, 20.0f);
  EXPECT_FLOAT_EQ(anns[0].ellipsoidRadiiVoxel->z, 30.0f);
}

TEST(ZNeuroglancerPrecomputedAnnotations, DecodeMultiplePropertyTypes)
{
  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "POINT",
  "properties": [
    { "id": "score", "type": "uint32" },
    { "id": "label", "type": "int16" },
    { "id": "quality", "type": "float32" },
    { "id": "color", "type": "rgb" },
    { "id": "flag", "type": "uint8" }
  ],
  "relationships": [],
  "by_id": { "key": "by_id" }
}
)json";

  auto source = ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                             info,
                                                                             /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                             /*baseVoxelOffset=*/{0, 0, 0},
                                                                             std::chrono::milliseconds(1000));
  ASSERT_TRUE(source);

  std::vector<uint8_t> bytes;
  appendU64LE(bytes, 1); // count
  // point position
  appendF32LE(bytes, 1.0f);
  appendF32LE(bytes, 2.0f);
  appendF32LE(bytes, 3.0f);
  // 4-byte properties in properties order (score, quality)
  appendU32LE(bytes, 123456u);
  appendF32LE(bytes, 0.25f);
  // 2-byte properties (label)
  bytes.push_back(0x34);
  bytes.push_back(0x12); // 0x1234 == 4660
  // rgb property
  bytes.push_back(9);
  bytes.push_back(8);
  bytes.push_back(7);
  // uint8 property
  bytes.push_back(1);
  // padding to 4-byte alignment (2 bytes)
  bytes.push_back(0);
  bytes.push_back(0);
  appendU64LE(bytes, 99); // id list

  const auto anns = source->decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_EQ(anns.size(), 1U);
  ASSERT_EQ(anns[0].propertyValues.size(), 5U);

  EXPECT_EQ(std::get<uint32_t>(anns[0].propertyValues[0]), 123456u);
  EXPECT_EQ(std::get<int16_t>(anns[0].propertyValues[1]), 0x1234);
  EXPECT_FLOAT_EQ(std::get<float>(anns[0].propertyValues[2]), 0.25f);
  const auto rgb = std::get<std::array<uint8_t, 3>>(anns[0].propertyValues[3]);
  EXPECT_EQ(rgb[0], 9);
  EXPECT_EQ(rgb[1], 8);
  EXPECT_EQ(rgb[2], 7);
  EXPECT_EQ(std::get<uint8_t>(anns[0].propertyValues[4]), 1);

  // RGB property should set rgba8 if no explicit rgba was present.
  ASSERT_TRUE(anns[0].rgba8.has_value());
  EXPECT_EQ((*anns[0].rgba8)[0], 9);
  EXPECT_EQ((*anns[0].rgba8)[1], 8);
  EXPECT_EQ((*anns[0].rgba8)[2], 7);
  EXPECT_EQ((*anns[0].rgba8)[3], 255);
}

TEST(ZNeuroglancerPrecomputedAnnotations, LoadAnnotationsForRelatedObjectAsyncUsesRemoteContext)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();
  ZHttpGetBytesResult response;
  response.status = 200;

  std::vector<uint8_t> body;
  appendU64LE(body, 1); // count
  appendF32LE(body, 10.0f);
  appendF32LE(body, 20.0f);
  appendF32LE(body, 30.0f);
  appendU64LE(body, 17); // id list
  response.body = body;
  response.encodedBodyBytes = body.size();
  response.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(response);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{321}, store);

  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "POINT",
  "properties": [],
  "relationships": [ { "id": "segment", "key": "by_segment" } ]
}
)json";

  const auto source =
    ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                 info,
                                                                 /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                 /*baseVoxelOffset=*/{0, 0, 0},
                                                                 remoteContext);
  ASSERT_TRUE(source);

  const auto anns =
    folly::coro::blockingWait(source->loadAnnotationsForRelatedObjectAsync(QStringLiteral("segment"), 17));
  ASSERT_EQ(anns.size(), 1U);
  EXPECT_EQ(anns[0].id, 17U);
  ASSERT_EQ(anns[0].points.size(), 1U);
  EXPECT_FLOAT_EQ(anns[0].points[0].x, 10.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].y, 20.0f);
  EXPECT_FLOAT_EQ(anns[0].points[0].z, 30.0f);

  ASSERT_EQ(store->requests.size(), 1U);
  EXPECT_EQ(store->requests[0].url, "https://example.invalid/ann/by_segment/17");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(321));
  EXPECT_TRUE(store->requests[0].headers.empty());
}

TEST(ZNeuroglancerPrecomputedAnnotations, StreamAnnotationsIntersectingVoxelBoxAsyncUsesRemoteContextAndDedupes)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult cell0;
  cell0.status = 200;
  {
    std::vector<uint8_t> body;
    appendU64LE(body, 1); // count
    appendF32LE(body, 10.0f);
    appendF32LE(body, 20.0f);
    appendF32LE(body, 30.0f);
    appendU64LE(body, 11); // id
    cell0.body = std::move(body);
    cell0.encodedBodyBytes = cell0.body.size();
    cell0.source = ZHttpGetBytesSource::Network;
  }
  store->responses.push_back(cell0);

  ZHttpGetBytesResult cell1;
  cell1.status = 200;
  {
    std::vector<uint8_t> body;
    appendU64LE(body, 3); // count
    appendF32LE(body, 10.0f);
    appendF32LE(body, 20.0f);
    appendF32LE(body, 30.0f); // duplicate id 11
    appendF32LE(body, 150.0f);
    appendF32LE(body, 50.0f);
    appendF32LE(body, 50.0f); // new id 22, inside query box
    appendF32LE(body, 250.0f);
    appendF32LE(body, 50.0f);
    appendF32LE(body, 50.0f); // id 33, outside query box
    appendU64LE(body, 11);
    appendU64LE(body, 22);
    appendU64LE(body, 33);
    cell1.body = std::move(body);
    cell1.encodedBodyBytes = cell1.body.size();
    cell1.source = ZHttpGetBytesSource::Network;
  }
  store->responses.push_back(cell1);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{321}, store);

  const std::string info = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [4e-9, "m"], "y": [4e-9, "m"], "z": [4e-8, "m"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [200, 100, 100],
  "annotation_type": "POINT",
  "properties": [],
  "relationships": [],
  "spatial": [
    {
      "key": "spatial",
      "grid_shape": [2, 1, 1],
      "chunk_size": [100, 100, 100],
      "limit": 1
    }
  ]
}
)json";

  const auto source =
    ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(QUrl("https://example.invalid/ann/"),
                                                                 info,
                                                                 /*baseResolutionNm=*/{4.0, 4.0, 40.0},
                                                                 /*baseVoxelOffset=*/{0, 0, 0},
                                                                 remoteContext);
  ASSERT_TRUE(source);

  const auto updates =
    folly::coro::blockingWait(collectSpatialUpdates(*source,
                                                    /*voxelMin=*/glm::dvec3(0.0, 0.0, 0.0),
                                                    /*voxelMax=*/glm::dvec3(199.0, 99.0, 99.0),
                                                    ZNeuroglancerPrecomputedAnnotationsSource::SpatialStreamOptions{
                                                      .minUpdateInterval = std::chrono::milliseconds::zero(),
                                                      .maxAnnotationsPerUpdate = 1}));

  ASSERT_EQ(updates.size(), 3U);

  ASSERT_EQ(updates[0].newAnnotations.size(), 1U);
  EXPECT_EQ(updates[0].newAnnotations[0].id, 11U);
  EXPECT_EQ(updates[0].progress.visitedCells, 1U);
  EXPECT_EQ(updates[0].progress.totalCells, 2U);
  EXPECT_EQ(updates[0].progress.uniqueAnnotations, 1U);

  ASSERT_EQ(updates[1].newAnnotations.size(), 1U);
  EXPECT_EQ(updates[1].newAnnotations[0].id, 22U);
  EXPECT_EQ(updates[1].newAnnotations[0].points.size(), 1U);
  EXPECT_FLOAT_EQ(updates[1].newAnnotations[0].points[0].x, 150.0f);
  EXPECT_EQ(updates[1].progress.visitedCells, 2U);
  EXPECT_EQ(updates[1].progress.totalCells, 2U);
  EXPECT_EQ(updates[1].progress.uniqueAnnotations, 2U);

  EXPECT_TRUE(updates[2].newAnnotations.empty());
  EXPECT_EQ(updates[2].progress.visitedCells, 2U);
  EXPECT_EQ(updates[2].progress.totalCells, 2U);
  EXPECT_EQ(updates[2].progress.uniqueAnnotations, 2U);

  ASSERT_EQ(store->requests.size(), 2U);
  EXPECT_EQ(store->requests[0].url, "https://example.invalid/ann/spatial/0_0_0");
  EXPECT_EQ(store->requests[1].url, "https://example.invalid/ann/spatial/1_0_0");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(321));
  EXPECT_EQ(store->requests[1].timeout, std::chrono::milliseconds(321));
  EXPECT_TRUE(store->requests[0].headers.empty());
  EXPECT_TRUE(store->requests[1].headers.empty());
}

} // namespace nim
