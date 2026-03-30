#include "zremoteobjectreader.h"

#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancerremotecontext.h"
#include "zneuroglancerstate.h"
#include "zimginit.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/ScopeGuard.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

DECLARE_uint32(atlas_http_max_retries);
DECLARE_uint32(atlas_http_retry_backoff_initial_ms);
DECLARE_uint32(atlas_http_retry_backoff_max_ms);

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
    std::optional<ZHttpByteRange> exactByteRange;
  };

  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request) const override
  {
    std::optional<ZHttpGetBytesResult> next;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      requests.push_back(Request{.url = std::move(request.url),
                                 .timeout = request.timeout,
                                 .headers = std::move(request.headers),
                                 .exactByteRange = request.exactByteRange});
      if (responses.empty()) {
        throw std::runtime_error("FakeRemoteObjectStore called without a queued response");
      }
      next = std::move(responses.front());
      responses.pop_front();
    }

    if (yieldBeforeResponding) {
      co_await folly::coro::co_reschedule_on_current_executor;
    }

    co_return next;
  }

  mutable std::mutex mutex;
  mutable std::deque<std::optional<ZHttpGetBytesResult>> responses;
  mutable std::vector<Request> requests;
  bool yieldBeforeResponding = false;
};

} // namespace

class ZRemoteObjectReaderImgInitEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    // Retry tests in this binary hit Folly coroutine sleep/timekeeper paths. Keep
    // the init local to this suite by reusing the standard img runtime init.
    (void)ZImgInit::instance("", "", "", false);
  }
};

[[maybe_unused]] ::testing::Environment* const kZRemoteObjectReaderImgInitEnvironment =
  ::testing::AddGlobalTestEnvironment(new ZRemoteObjectReaderImgInitEnvironment());

void expectIdenticalRequestSuffix(const FakeRemoteObjectStore& store, size_t firstSuffixIndex)
{
  ASSERT_GE(store.requests.size(), firstSuffixIndex + 1);
  for (size_t i = firstSuffixIndex + 1; i < store.requests.size(); ++i) {
    EXPECT_EQ(store.requests[i].url, store.requests[firstSuffixIndex].url);
    EXPECT_EQ(store.requests[i].headers, store.requests[firstSuffixIndex].headers);
    EXPECT_EQ(store.requests[i].exactByteRange, store.requests[firstSuffixIndex].exactByteRange);
  }
}

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
  EXPECT_TRUE(store->requests[0].headers.empty());
  ASSERT_TRUE(store->requests[0].exactByteRange.has_value());
  EXPECT_EQ(store->requests[0].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[0].exactByteRange->length, 3U);
}

TEST(ZRemoteObjectReader, RemoteContextRetriesRangeSizeMismatch)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult shortResponse;
  shortResponse.status = 206;
  shortResponse.body = std::vector<uint8_t>{1};
  shortResponse.encodedBodyBytes = 1;
  shortResponse.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(shortResponse);

  ZHttpGetBytesResult goodResponse;
  goodResponse.status = 206;
  goodResponse.body = std::vector<uint8_t>{1, 2, 3};
  goodResponse.encodedBodyBytes = 3;
  goodResponse.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(goodResponse);

  const uint32_t prevRetries = FLAGS_atlas_http_max_retries;
  const uint32_t prevInitialBackoff = FLAGS_atlas_http_retry_backoff_initial_ms;
  const uint32_t prevMaxBackoff = FLAGS_atlas_http_retry_backoff_max_ms;
  auto restoreFlags = folly::makeGuard([&]() {
    FLAGS_atlas_http_max_retries = prevRetries;
    FLAGS_atlas_http_retry_backoff_initial_ms = prevInitialBackoff;
    FLAGS_atlas_http_retry_backoff_max_ms = prevMaxBackoff;
  });
  FLAGS_atlas_http_max_retries = 1;
  FLAGS_atlas_http_retry_backoff_initial_ms = 0;
  FLAGS_atlas_http_retry_backoff_max_ms = 0;

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{222}, store);

  auto bytesOpt = folly::coro::blockingWait(remoteContext->getRangeBytesAsync("https://example.com/range", 7, 3));

  ASSERT_TRUE(bytesOpt.has_value());
  EXPECT_EQ(*bytesOpt, (std::vector<uint8_t>{1, 2, 3}));
  ASSERT_EQ(store->requests.size(), 2U);
  ASSERT_TRUE(store->requests[0].exactByteRange.has_value());
  ASSERT_TRUE(store->requests[1].exactByteRange.has_value());
  EXPECT_EQ(store->requests[0].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[0].exactByteRange->length, 3U);
  EXPECT_EQ(store->requests[1].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[1].exactByteRange->length, 3U);
}

TEST(ZRemoteObjectReader, RemoteContextSlicesFullResponseWhenAllowed)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult fullResponse;
  fullResponse.status = 200;
  fullResponse.body = std::vector<uint8_t>{10, 11, 12, 13, 14, 15};
  fullResponse.encodedBodyBytes = 6;
  fullResponse.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(fullResponse);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{222}, store);

  auto bytesOpt =
    folly::coro::blockingWait(remoteContext->getRangeBytesAsync("https://example.com/range",
                                                                2,
                                                                3,
                                                                ZRemoteRangeReadPolicy::AllowFullResponseSlice));

  ASSERT_TRUE(bytesOpt.has_value());
  EXPECT_EQ(*bytesOpt, (std::vector<uint8_t>{12, 13, 14}));
  ASSERT_EQ(store->requests.size(), 1U);
  ASSERT_TRUE(store->requests[0].exactByteRange.has_value());
  EXPECT_EQ(store->requests[0].exactByteRange->offset, 2U);
  EXPECT_EQ(store->requests[0].exactByteRange->length, 3U);
}

TEST(ZRemoteObjectReader, RemoteContextRejectsStrictRangeReadWithStatus200)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult fullResponse;
  fullResponse.status = 200;
  fullResponse.body = std::vector<uint8_t>{1, 2, 3};
  fullResponse.encodedBodyBytes = 3;
  fullResponse.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(fullResponse);

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{222}, store);

  try {
    (void)folly::coro::blockingWait(
      remoteContext->getRangeBytesAsync("https://example.com/range", 7, 3, ZRemoteRangeReadPolicy::RequireExactLength));
    FAIL() << "Expected strict range read with status 200 to throw";
  }
  catch (const ZException& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("returned 200"), std::string::npos);
    EXPECT_NE(what.find("strict range request"), std::string::npos);
  }

  ASSERT_EQ(store->requests.size(), 1U);
  ASSERT_TRUE(store->requests[0].exactByteRange.has_value());
  EXPECT_EQ(store->requests[0].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[0].exactByteRange->length, 3U);
}

TEST(ZRemoteObjectReader, RemoteContextExhaustsRangeSizeMismatchRetries)
{
  auto store = std::make_shared<FakeRemoteObjectStore>();

  ZHttpGetBytesResult shortResponse;
  shortResponse.status = 206;
  shortResponse.body = std::vector<uint8_t>{1};
  shortResponse.encodedBodyBytes = 1;
  shortResponse.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(shortResponse);
  store->responses.push_back(shortResponse);

  const uint32_t prevRetries = FLAGS_atlas_http_max_retries;
  const uint32_t prevInitialBackoff = FLAGS_atlas_http_retry_backoff_initial_ms;
  const uint32_t prevMaxBackoff = FLAGS_atlas_http_retry_backoff_max_ms;
  auto restoreFlags = folly::makeGuard([&]() {
    FLAGS_atlas_http_max_retries = prevRetries;
    FLAGS_atlas_http_retry_backoff_initial_ms = prevInitialBackoff;
    FLAGS_atlas_http_retry_backoff_max_ms = prevMaxBackoff;
  });
  FLAGS_atlas_http_max_retries = 1;
  FLAGS_atlas_http_retry_backoff_initial_ms = 0;
  FLAGS_atlas_http_retry_backoff_max_ms = 0;

  const auto remoteContext = ZNeuroglancerRemoteContext::create(std::chrono::milliseconds{222}, store);

  try {
    (void)folly::coro::blockingWait(remoteContext->getRangeBytesAsync("https://example.com/range", 7, 3));
    FAIL() << "Expected range size mismatch to throw after retries";
  }
  catch (const ZException& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("size mismatch"), std::string::npos);
    EXPECT_NE(what.find("expected 3 bytes"), std::string::npos);
  }

  ASSERT_EQ(store->requests.size(), 2U);
  ASSERT_TRUE(store->requests[0].exactByteRange.has_value());
  ASSERT_TRUE(store->requests[1].exactByteRange.has_value());
  EXPECT_EQ(store->requests[0].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[0].exactByteRange->length, 3U);
  EXPECT_EQ(store->requests[1].exactByteRange->offset, 7U);
  EXPECT_EQ(store->requests[1].exactByteRange->length, 3U);
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

TEST(ZRemoteObjectReader, ConcurrentChunkReadFailureIsPropagatedToBothCallers)
{
  const uint32_t prevRetries = FLAGS_atlas_http_max_retries;
  auto restoreRetries = folly::makeGuard([prevRetries]() {
    FLAGS_atlas_http_max_retries = prevRetries;
  });
  FLAGS_atlas_http_max_retries = 0;

  auto store = std::make_shared<FakeRemoteObjectStore>();
  store->yieldBeforeResponding = true;

  ZHttpGetBytesResult volumeInfo;
  volumeInfo.status = 200;
  const std::string volumeInfoJson = R"json(
{
  "data_type": "uint8",
  "type": "image",
  "num_channels": 1,
  "scales": [
    {
      "key": "1_1_1",
      "resolution": [1, 1, 1],
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

  ZHttpGetBytesResult badChunk;
  badChunk.status = 500;
  badChunk.encodedBodyBytes = 0;
  badChunk.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(badChunk);
  store->responses.push_back(badChunk);

  const auto volume = ZNeuroglancerPrecomputedVolume::open(QStringLiteral("precomputed://gs://bucket/dataset"),
                                                           std::chrono::milliseconds{321},
                                                           store);
  ASSERT_TRUE(volume);

  const auto chunks = volume->chunksIntersectingBaseBox(/*scaleIndex=*/0,
                                                        /*baseStart=*/{0, 0, 0},
                                                        /*baseEnd=*/{1, 1, 1});
  ASSERT_EQ(chunks.size(), 1U);

  auto readChunkTry = [volume](ZNeuroglancerPrecomputedVolume::Chunk chunk) -> folly::coro::Task<folly::Try<std::shared_ptr<ZImg>>> {
    co_return co_await folly::coro::co_awaitTry(volume->readChunkAsync(std::move(chunk)));
  };

  folly::CPUThreadPoolExecutor executor(/*numThreads=*/2);
  std::vector<folly::coro::TaskWithExecutor<folly::Try<std::shared_ptr<ZImg>>>> tasks;
  tasks.push_back(folly::coro::co_withExecutor(&executor, readChunkTry(chunks.front())));
  tasks.push_back(folly::coro::co_withExecutor(&executor, readChunkTry(chunks.front())));

  const auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));

  ASSERT_EQ(results.size(), 2U);
  for (const auto& result : results) {
    ASSERT_TRUE(result.hasException());
    try {
      (void)result.value();
      FAIL() << "Expected concurrent chunk read to fail";
    }
    catch (const ZException& e) {
      EXPECT_NE(std::string(e.what()).find("Failed to fetch neuroglancer chunk"), std::string::npos);
    }
    catch (const std::exception& e) {
      FAIL() << "Expected ZException, got std::exception: " << e.what();
    }
    catch (...) {
      FAIL() << "Expected ZException";
    }
  }

  ASSERT_GE(store->requests.size(), 2U);
  ASSERT_LE(store->requests.size(), 3U);
  EXPECT_EQ(store->requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(store->requests[1].url, "https://storage.googleapis.com/bucket/dataset/1_1_1/0-1_0-1_0-1");
  EXPECT_TRUE(store->requests[1].headers.empty());
  expectIdenticalRequestSuffix(*store, 1);
  EXPECT_EQ(store->responses.size(), 3U - store->requests.size());
}

TEST(ZRemoteObjectReader, ConcurrentShardedMinishardFailureIsPropagatedToBothCallers)
{
  const uint32_t prevRetries = FLAGS_atlas_http_max_retries;
  auto restoreRetries = folly::makeGuard([prevRetries]() {
    FLAGS_atlas_http_max_retries = prevRetries;
  });
  FLAGS_atlas_http_max_retries = 0;

  auto store = std::make_shared<FakeRemoteObjectStore>();
  store->yieldBeforeResponding = true;

  ZHttpGetBytesResult volumeInfo;
  volumeInfo.status = 200;
  const std::string volumeInfoJson = R"json(
{
  "data_type": "uint8",
  "type": "image",
  "num_channels": 1,
  "scales": [
    {
      "key": "1_1_1",
      "resolution": [1, 1, 1],
      "size": [1, 1, 1],
      "chunk_sizes": [[1, 1, 1]],
      "encoding": "raw",
      "sharding": {
        "@type": "neuroglancer_uint64_sharded_v1",
        "preshift_bits": 0,
        "hash": "identity",
        "minishard_bits": 0,
        "shard_bits": 0,
        "minishard_index_encoding": "raw",
        "data_encoding": "raw"
      }
    }
  ]
}
)json";
  volumeInfo.body.assign(volumeInfoJson.begin(), volumeInfoJson.end());
  volumeInfo.encodedBodyBytes = volumeInfo.body.size();
  volumeInfo.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(volumeInfo);

  ZHttpGetBytesResult badRange;
  badRange.status = 500;
  badRange.encodedBodyBytes = 0;
  badRange.source = ZHttpGetBytesSource::Network;
  store->responses.push_back(badRange);
  store->responses.push_back(badRange);

  const auto volume = ZNeuroglancerPrecomputedVolume::open(QStringLiteral("precomputed://gs://bucket/dataset"),
                                                           std::chrono::milliseconds{321},
                                                           store);
  ASSERT_TRUE(volume);

  const auto chunks = volume->chunksIntersectingBaseBox(/*scaleIndex=*/0,
                                                        /*baseStart=*/{0, 0, 0},
                                                        /*baseEnd=*/{1, 1, 1});
  ASSERT_EQ(chunks.size(), 1U);

  auto readChunkTry = [volume](ZNeuroglancerPrecomputedVolume::Chunk chunk) -> folly::coro::Task<folly::Try<std::shared_ptr<ZImg>>> {
    co_return co_await folly::coro::co_awaitTry(volume->readChunkAsync(std::move(chunk)));
  };

  folly::CPUThreadPoolExecutor executor(/*numThreads=*/2);
  std::vector<folly::coro::TaskWithExecutor<folly::Try<std::shared_ptr<ZImg>>>> tasks;
  tasks.push_back(folly::coro::co_withExecutor(&executor, readChunkTry(chunks.front())));
  tasks.push_back(folly::coro::co_withExecutor(&executor, readChunkTry(chunks.front())));

  const auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));

  ASSERT_EQ(results.size(), 2U);
  for (const auto& result : results) {
    ASSERT_TRUE(result.hasException());
    try {
      (void)result.value();
      FAIL() << "Expected concurrent sharded chunk read to fail";
    }
    catch (const ZException& e) {
      const std::string what = e.what();
      EXPECT_NE(what.find("HTTP range GET failed"), std::string::npos);
      EXPECT_NE(what.find("0.shard"), std::string::npos);
    }
    catch (const std::exception& e) {
      FAIL() << "Expected ZException, got std::exception: " << e.what();
    }
    catch (...) {
      FAIL() << "Expected ZException";
    }
  }

  ASSERT_GE(store->requests.size(), 2U);
  ASSERT_LE(store->requests.size(), 3U);
  EXPECT_EQ(store->requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(store->requests[1].url, "https://storage.googleapis.com/bucket/dataset/1_1_1/0.shard");
  EXPECT_TRUE(store->requests[1].headers.empty());
  ASSERT_TRUE(store->requests[1].exactByteRange.has_value());
  EXPECT_EQ(store->requests[1].exactByteRange->offset, 0U);
  EXPECT_EQ(store->requests[1].exactByteRange->length, 16U);
  expectIdenticalRequestSuffix(*store, 1);
  EXPECT_EQ(store->responses.size(), 3U - store->requests.size());
}

} // namespace nim
