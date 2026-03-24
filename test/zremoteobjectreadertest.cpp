#include "zremoteobjectreader.h"

#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancerremotecontext.h"
#include "zneuroglancerstate.h"

#include <folly/coro/BlockingWait.h>
#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nim {
namespace {

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

} // namespace

TEST(ZRemoteObjectReader, RemoteContextUsesInjectedStoreForFullObjectReads)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();
  ZHttpGetBytesResult response;
  response.status = 200;
  response.body = std::vector<uint8_t>{'o', 'k'};
  response.encodedBodyBytes = 2;
  response.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(response);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{1234}, store);

  auto bytesOpt = folly::coro::blockingWait(remoteContext->getBytesAsync("https://example.com/object"));

  ASSERT_TRUE(bytesOpt.has_value());
  EXPECT_EQ(*bytesOpt, (std::vector<uint8_t>{'o', 'k'}));
  ASSERT_EQ(store->requests.size(), 1U);
  EXPECT_EQ(store->requests[0].url, "https://example.com/object");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(1234));
  EXPECT_TRUE(store->requests[0].headers.empty());
}

TEST(ZRemoteObjectReader, RemoteContextUsesInjectedStoreForRangeReads)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();
  ZHttpGetBytesResult response;
  response.status = 206;
  response.body = std::vector<uint8_t>{1, 2, 3};
  response.encodedBodyBytes = 3;
  response.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(response);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{222}, store);

  auto bytesOpt = folly::coro::blockingWait(remoteContext->getRangeBytesAsync("https://example.com/range", 7, 3));

  ASSERT_TRUE(bytesOpt.has_value());
  EXPECT_EQ(*bytesOpt, (std::vector<uint8_t>{1, 2, 3}));
  ASSERT_EQ(store->requests.size(), 1U);
  EXPECT_EQ(store->requests[0].url, "https://example.com/range");
  ASSERT_EQ(store->requests[0].headers.size(), 1U);
  EXPECT_EQ(store->requests[0].headers[0].first, "range");
  EXPECT_EQ(store->requests[0].headers[0].second, "bytes=7-9");
}

TEST(ZRemoteObjectReader, StateParsingUsesInjectedStore)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();
  ZHttpGetBytesResult response;
  response.status = 200;
  const std::string jsonText =
    R"({"layers":[{"type":"image","name":"img","source":"precomputed://gs://bucket/dataset"}]})";
  response.body.assign(jsonText.begin(), jsonText.end());
  response.encodedBodyBytes = response.body.size();
  response.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(response);

  const auto parseResult =
    ZNeuroglancerState::parseInputText("https://example.com/state.json", std::chrono::milliseconds{456}, store);

  EXPECT_EQ(parseResult.status, ZNeuroglancerState::InputStatus::Parsed);
  ASSERT_EQ(store->requests.size(), 1U);
  EXPECT_EQ(store->requests[0].url, "https://example.com/state.json");
}

TEST(ZRemoteObjectReader, VolumeRemoteContextCanBeReusedForChildMeshOpen)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult volumeInfo;
  volumeInfo.status = 200;
  const std::string volumeInfoJson = R"json(
{
  "data_type": "uint64",
  "type": "segmentation",
  "num_channels": 1,
  "mesh": "mesh",
  "scales": [
    {
      "key": "8_8_8",
      "resolution": [8, 8, 8],
      "size": [1, 1, 1],
      "chunk_sizes": [[1, 1, 1]],
      "encoding": "raw"
    }
  ]
}
)json";
  volumeInfo.body.assign(volumeInfoJson.begin(), volumeInfoJson.end());
  volumeInfo.encodedBodyBytes = volumeInfo.body.size();
  volumeInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(volumeInfo);

  // Mesh info missing => legacy mesh. The important part here is that the child open reuses the volume context
  // and therefore hits the same injected store instead of silently falling back to the default HTTP store.
  store->responses.push_back(std::nullopt);

  const auto volume = ZNeuroglancerPrecomputedVolume::open(QStringLiteral("precomputed://gs://bucket/dataset"),
                                                           std::chrono::milliseconds{654},
                                                           store);
  ASSERT_TRUE(volume);

  const auto meshSource = ZNeuroglancerPrecomputedMeshSource::open(
    volume->meshDirUrl(),
    {volume->baseImgInfo().voxelSizeX, volume->baseImgInfo().voxelSizeY, volume->baseImgInfo().voxelSizeZ},
    volume->baseVoxelOffset(),
    volume->sharedRemoteContext());
  ASSERT_TRUE(meshSource);
  EXPECT_EQ(meshSource->meshType(), ZNeuroglancerPrecomputedMeshSource::MeshType::Legacy);

  ASSERT_EQ(store->requests.size(), 2U);
  EXPECT_EQ(store->requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(654));
  EXPECT_EQ(store->requests[1].url, "https://storage.googleapis.com/bucket/dataset/mesh/info");
  EXPECT_EQ(store->requests[1].timeout, std::chrono::milliseconds(654));
}

TEST(ZRemoteObjectReader, VolumeRemoteContextCanBeReusedForChildSkeletonOpen)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult volumeInfo;
  volumeInfo.status = 200;
  const std::string volumeInfoJson = R"json(
{
  "data_type": "uint64",
  "type": "segmentation",
  "num_channels": 1,
  "skeletons": "skeletons",
  "scales": [
    {
      "key": "8_8_8",
      "resolution": [8, 8, 8],
      "size": [1, 1, 1],
      "chunk_sizes": [[1, 1, 1]],
      "encoding": "raw"
    }
  ]
}
)json";
  volumeInfo.body.assign(volumeInfoJson.begin(), volumeInfoJson.end());
  volumeInfo.encodedBodyBytes = volumeInfo.body.size();
  volumeInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(volumeInfo);

  ZHttpGetBytesResult skeletonInfo;
  skeletonInfo.status = 200;
  const std::string skeletonInfoJson = R"json({"@type":"neuroglancer_skeletons"})json";
  skeletonInfo.body.assign(skeletonInfoJson.begin(), skeletonInfoJson.end());
  skeletonInfo.encodedBodyBytes = skeletonInfo.body.size();
  skeletonInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(skeletonInfo);

  const auto volume = ZNeuroglancerPrecomputedVolume::open(QStringLiteral("precomputed://gs://bucket/dataset"),
                                                           std::chrono::milliseconds{777},
                                                           store);
  ASSERT_TRUE(volume);

  const auto skeletonSource = folly::coro::blockingWait(volume->loadSkeletonSourceAsync());
  ASSERT_TRUE(skeletonSource);

  ASSERT_EQ(store->requests.size(), 2U);
  EXPECT_EQ(store->requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(777));
  EXPECT_EQ(store->requests[1].url, "https://storage.googleapis.com/bucket/dataset/skeletons/info");
  EXPECT_EQ(store->requests[1].timeout, std::chrono::milliseconds(777));
}

TEST(ZRemoteObjectReader, VolumeRemoteContextCanBeReusedForChildAnnotationsOpen)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult volumeInfo;
  volumeInfo.status = 200;
  const std::string volumeInfoJson = R"json(
{
  "data_type": "uint64",
  "type": "segmentation",
  "num_channels": 1,
  "scales": [
    {
      "key": "8_8_8",
      "resolution": [8, 8, 8],
      "size": [1, 1, 1],
      "chunk_sizes": [[1, 1, 1]],
      "encoding": "raw"
    }
  ]
}
)json";
  volumeInfo.body.assign(volumeInfoJson.begin(), volumeInfoJson.end());
  volumeInfo.encodedBodyBytes = volumeInfo.body.size();
  volumeInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(volumeInfo);

  ZHttpGetBytesResult annotationsInfo;
  annotationsInfo.status = 200;
  const std::string annotationsInfoJson = R"json(
{
  "@type": "neuroglancer_annotations_v1",
  "dimensions": { "x": [8, "nm"], "y": [8, "nm"], "z": [8, "nm"] },
  "lower_bound": [0, 0, 0],
  "upper_bound": [1, 1, 1],
  "annotation_type": "POINT",
  "relationships": [ { "id": "segment", "key": "by_segment" } ]
}
)json";
  annotationsInfo.body.assign(annotationsInfoJson.begin(), annotationsInfoJson.end());
  annotationsInfo.encodedBodyBytes = annotationsInfo.body.size();
  annotationsInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(annotationsInfo);

  const auto volume = ZNeuroglancerPrecomputedVolume::open(QStringLiteral("precomputed://gs://bucket/dataset"),
                                                           std::chrono::milliseconds{888},
                                                           store);
  ASSERT_TRUE(volume);

  const auto annotationsSource = folly::coro::blockingWait(ZNeuroglancerPrecomputedAnnotationsSource::openAsync(
    QUrl("https://storage.googleapis.com/bucket/annotations/"),
    {volume->baseImgInfo().voxelSizeX, volume->baseImgInfo().voxelSizeY, volume->baseImgInfo().voxelSizeZ},
    volume->baseVoxelOffset(),
    volume->sharedRemoteContext()));
  ASSERT_TRUE(annotationsSource);

  ASSERT_EQ(store->requests.size(), 2U);
  EXPECT_EQ(store->requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(store->requests[0].timeout, std::chrono::milliseconds(888));
  EXPECT_EQ(store->requests[1].url, "https://storage.googleapis.com/bucket/annotations/info");
  EXPECT_EQ(store->requests[1].timeout, std::chrono::milliseconds(888));
}

TEST(ZRemoteObjectReader, MeshSourceOpenScopesInfoMetadataByStore)
{
  auto storeA = std::make_shared<FakeRemoteObjectStore>();
  auto storeB = std::make_shared<FakeRemoteObjectStore>();

  // Store A sees no mesh/info and therefore treats the source as legacy.
  storeA->responses.push_back(std::nullopt);

  // Store B must still fetch its own mesh/info and discover multiscale metadata instead of inheriting A's legacy
  // fallback from the global cache.
  ZHttpGetBytesResult meshInfo;
  meshInfo.status = 200;
  const std::string meshInfoJson =
    R"({"@type":"neuroglancer_multilod_draco","lod_scale_multiplier":1,"vertex_quantization_bits":10})";
  meshInfo.body.assign(meshInfoJson.begin(), meshInfoJson.end());
  meshInfo.encodedBodyBytes = meshInfo.body.size();
  meshInfo.source = ZHttpGetBytesSource::Network;
  storeB->responses.push_back(meshInfo);

  const auto remoteContextA = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{100}, storeA);
  const auto remoteContextB = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{200}, storeB);

  const auto sourceA = ZNeuroglancerPrecomputedMeshSource::open(QUrl("https://example.com/mesh/"),
                                                                {8.0, 8.0, 8.0},
                                                                {0, 0, 0},
                                                                remoteContextA);
  const auto sourceB = ZNeuroglancerPrecomputedMeshSource::open(QUrl("https://example.com/mesh/"),
                                                                {8.0, 8.0, 8.0},
                                                                {0, 0, 0},
                                                                remoteContextB);

  ASSERT_TRUE(sourceA);
  ASSERT_TRUE(sourceB);
  EXPECT_NE(sourceA.get(), sourceB.get());
  EXPECT_EQ(sourceA->meshType(), ZNeuroglancerPrecomputedMeshSource::MeshType::Legacy);
  EXPECT_EQ(sourceB->meshType(), ZNeuroglancerPrecomputedMeshSource::MeshType::MultiLodDraco);
  EXPECT_TRUE(sourceB->supportsRuntimeLod());

  ASSERT_EQ(storeA->requests.size(), 1U);
  EXPECT_EQ(storeA->requests[0].url, "https://example.com/mesh/info");
  EXPECT_EQ(storeA->requests[0].timeout, std::chrono::milliseconds(100));

  ASSERT_EQ(storeB->requests.size(), 1U);
  EXPECT_EQ(storeB->requests[0].url, "https://example.com/mesh/info");
  EXPECT_EQ(storeB->requests[0].timeout, std::chrono::milliseconds(200));
}

} // namespace nim
