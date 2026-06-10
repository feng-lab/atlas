#include "zneuroglancerprecomputed.h"
#include "zcommandlineflags.h"

#include "zcancellation.h"
#include "zcpuinfo.h"
#include "zneuroglancerprecomputedchunkdecoder.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancershardedreader.h"
#include "zneuroglancerremotecontext.h"
#include "zneuroglancerurl.h"
#include "zneuroglanceruint64sharding.h"
#include "zfolly.h"
#include "zimgpng.h"
#include "zlog.h"
#include "zconcurrentlrucache.h"

#include <folly/ExceptionWrapper.h>
#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <folly/coro/Sleep.h>
#include <future>
#else
#include <folly/coro/SharedPromise.h>
#endif
#include <boost/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>

ABSL_FLAG(double,
          atlas_ng_precomputed_chunk_cache_memory_proportion,
          0.3,
          "Proportion of RAM that will be used for Neuroglancer precomputed chunk cache, default is 0.3");

ABSL_FLAG(
  double,
  atlas_ng_precomputed_minishard_index_cache_memory_proportion,
  0.05,
  "Proportion of RAM that will be used for Neuroglancer precomputed sharded minishard index cache, default is 0.05");

namespace nim {
namespace json = boost::json;

namespace {

using ChunkCacheKey = std::tuple<size_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t>;
using MinishardIndexCacheKey = std::tuple<size_t, uint64_t>;
using DecodedMinishardIndex = ZNeuroglancerUint64Sharding::DecodedMinishardIndex;

#if defined(_MSC_VER) && !defined(__clang__)

enum class SharedLoadErrorKind
{
  None,
  ZCancellation,
  OperationCancelled,
  NotFound,
  ZException,
  StdException,
  Unknown,
};

template<typename T>
struct SharedLoadResult
{
  T value{};
  SharedLoadErrorKind errorKind = SharedLoadErrorKind::None;
  std::string errorMessage;
};

template<typename T>
using SharedLoadResultPtr = std::shared_ptr<const SharedLoadResult<T>>;

template<typename T>
struct SharedLoadCompletion
{
  SharedLoadCompletion()
    : future(promise.get_future().share())
  {}

  std::promise<SharedLoadResultPtr<T>> promise;
  std::shared_future<SharedLoadResultPtr<T>> future;
};

template<typename T>
using SharedLoadFuture = std::shared_future<SharedLoadResultPtr<T>>;

template<typename T>
[[nodiscard]] SharedLoadFuture<T> getSharedLoadFuture(SharedLoadCompletion<T>& completion)
{
  return completion.future;
}

template<typename T, typename U>
[[nodiscard]] SharedLoadResultPtr<T> makeSharedLoadValue(U&& value)
{
  auto out = std::make_shared<SharedLoadResult<T>>();
  out->value = std::forward<U>(value);
  return out;
}

template<typename T>
[[nodiscard]] SharedLoadResultPtr<T> captureSharedLoadError()
{
  auto out = std::make_shared<SharedLoadResult<T>>();
  try {
    throw;
  }
  catch (const ZCancellationException&) {
    out->errorKind = SharedLoadErrorKind::ZCancellation;
  }
  catch (const folly::OperationCancelled&) {
    out->errorKind = SharedLoadErrorKind::OperationCancelled;
  }
  catch (const ZNotFoundException& e) {
    out->errorKind = SharedLoadErrorKind::NotFound;
    out->errorMessage = e.what();
  }
  catch (const ZException& e) {
    out->errorKind = SharedLoadErrorKind::ZException;
    out->errorMessage = e.what();
  }
  catch (const std::exception& e) {
    out->errorKind = SharedLoadErrorKind::StdException;
    out->errorMessage = e.what();
  }
  catch (...) {
    out->errorKind = SharedLoadErrorKind::Unknown;
    out->errorMessage = "Unknown exception";
  }
  return out;
}

template<typename T>
void throwSharedLoadErrorIfAny(const SharedLoadResult<T>& result)
{
  switch (result.errorKind) {
    case SharedLoadErrorKind::None:
      return;
    case SharedLoadErrorKind::ZCancellation:
      throw ZCancellationException();
    case SharedLoadErrorKind::OperationCancelled:
      throw folly::OperationCancelled();
    case SharedLoadErrorKind::NotFound:
      throw ZNotFoundException(result.errorMessage);
    case SharedLoadErrorKind::ZException:
      throw ZException(result.errorMessage);
    case SharedLoadErrorKind::StdException:
      throw ZException(result.errorMessage);
    case SharedLoadErrorKind::Unknown:
      throw ZException(result.errorMessage.empty() ? "Unknown exception" : result.errorMessage);
  }

  CHECK(false) << "Unhandled SharedLoadErrorKind";
}

template<typename T, typename U>
void fulfillSharedLoadValue(SharedLoadCompletion<T>& completion, U&& value)
{
  completion.promise.set_value(makeSharedLoadValue<T>(std::forward<U>(value)));
}

// Only call this helper from inside a catch block. Native MSVC's coroutine codegen
// previously crashed in Folly's SharedPromise path during duplicate-waiter wakeup and
// exception propagation, so that compiler keeps the slower shared_future fallback.
template<typename T>
void fulfillSharedLoadException(SharedLoadCompletion<T>& completion)
{
  completion.promise.set_value(captureSharedLoadError<T>());
}

constexpr auto kSharedLoadPollInterval = std::chrono::milliseconds(1);

template<typename T>
folly::coro::Task<T> awaitSharedLoad(SharedLoadFuture<T> future, folly::CancellationToken cancellationToken)
{
  while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    maybeCancel(cancellationToken);
    co_await folly::coro::sleep(kSharedLoadPollInterval);
  }

  maybeCancel(cancellationToken);
  try {
    auto shared = future.get();
    CHECK(shared);
    throwSharedLoadErrorIfAny(*shared);
    co_return shared->value;
  }
  catch (const std::future_error& e) {
    CHECK(false) << "Shared Neuroglancer load future became broken: " << e.what();
  }
}

#else

template<typename T>
struct SharedLoadCompletion
{
  folly::coro::SharedPromise<T> promise;
};

template<typename T>
using SharedLoadFuture = folly::coro::Future<T>;

template<typename T>
[[nodiscard]] SharedLoadFuture<T> getSharedLoadFuture(SharedLoadCompletion<T>& completion)
{
  return completion.promise.getFuture();
}

template<typename T, typename U>
void fulfillSharedLoadValue(SharedLoadCompletion<T>& completion, U&& value)
{
  completion.promise.setValue(std::forward<U>(value));
}

// Only call this helper from inside a catch block.
template<typename T>
void fulfillSharedLoadException(SharedLoadCompletion<T>& completion)
{
  completion.promise.setException(folly::exception_wrapper(std::current_exception()));
}

template<typename T>
folly::coro::Task<T> awaitSharedLoad(SharedLoadFuture<T> future, folly::CancellationToken /*cancellationToken*/)
{
  co_return co_await std::move(future);
}

#endif

QString requireString(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_string()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer info", key));
  }
  return json::value_to<QString>(it->value());
}

QString optionalString(const json::object& obj, const char* key, QString defaultValue = {})
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return defaultValue;
  }
  if (!it->value().is_string()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info (expected string)", key));
  }
  return json::value_to<QString>(it->value());
}

int64_t requireInt64(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_int64()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer info", key));
  }
  return it->value().as_int64();
}

uint64_t requireUint64(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end()) {
    throw ZException(fmt::format("Missing '{}' in neuroglancer info", key));
  }
  const auto& v = it->value();
  if (v.is_uint64()) {
    return v.as_uint64();
  }
  if (v.is_int64()) {
    const int64_t s = v.as_int64();
    if (s < 0) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer info (expected >= 0)", key));
    }
    return static_cast<uint64_t>(s);
  }
  throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer info", key));
}

std::optional<bool> optionalBool(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return std::nullopt;
  }
  if (!it->value().is_bool()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info (expected bool)", key));
  }
  return it->value().as_bool();
}

std::array<double, 3> requireDouble3(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer info", key));
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected length 3", key));
  }
  std::array<double, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    if (!arr[i].is_number()) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected numeric", key));
    }
    out[i] = arr[i].to_number<double>();
  }
  return out;
}

std::array<int64_t, 3> requireInt3(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer info", key));
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected length 3", key));
  }
  std::array<int64_t, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    if (arr[i].is_int64()) {
      out[i] = arr[i].as_int64();
    } else if (arr[i].is_uint64()) {
      auto v = arr[i].as_uint64();
      if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw ZException(fmt::format("Invalid '{}' in neuroglancer info: value out of range", key));
      }
      out[i] = static_cast<int64_t>(v);
    } else {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected int", key));
    }
  }
  return out;
}

std::optional<std::array<int64_t, 3>> optionalInt3(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return std::nullopt;
  }
  if (!it->value().is_array()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected array", key));
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected length 3", key));
  }
  std::array<int64_t, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    if (arr[i].is_int64()) {
      out[i] = arr[i].as_int64();
    } else if (arr[i].is_uint64()) {
      auto v = arr[i].as_uint64();
      if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw ZException(fmt::format("Invalid '{}' in neuroglancer info: value out of range", key));
      }
      out[i] = static_cast<int64_t>(v);
    } else {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer info: expected int", key));
    }
  }
  return out;
}

std::vector<std::array<int64_t, 3>> requireChunkSizes(const json::object& obj)
{
  auto it = obj.find("chunk_sizes");
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException("Missing or invalid 'chunk_sizes' in neuroglancer scale");
  }
  const auto& arr = it->value().as_array();
  std::vector<std::array<int64_t, 3>> out;
  out.reserve(arr.size());
  for (const auto& v : arr) {
    if (!v.is_array()) {
      throw ZException("Invalid 'chunk_sizes' in neuroglancer scale: expected array elements");
    }
    const auto& a = v.as_array();
    if (a.size() != 3) {
      throw ZException("Invalid 'chunk_sizes' in neuroglancer scale: expected 3-element arrays");
    }
    std::array<int64_t, 3> cs{};
    for (size_t i = 0; i < 3; ++i) {
      if (!a[i].is_int64() && !a[i].is_uint64()) {
        throw ZException("Invalid 'chunk_sizes' in neuroglancer scale: expected ints");
      }
      cs[i] = a[i].is_int64() ? a[i].as_int64() : static_cast<int64_t>(a[i].as_uint64());
      if (cs[i] <= 0) {
        throw ZException("Invalid 'chunk_sizes' in neuroglancer scale: must be > 0");
      }
    }
    out.push_back(cs);
  }
  if (out.empty()) {
    throw ZException("Invalid 'chunk_sizes' in neuroglancer scale: empty");
  }
  return out;
}

std::string toStdString(const QString& s)
{
  auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

void swapEndianInPlace(uint8_t* data, size_t bytesPerVoxel, size_t elementCount)
{
  CHECK(bytesPerVoxel == 2 || bytesPerVoxel == 4 || bytesPerVoxel == 8);
  for (size_t i = 0; i < elementCount; ++i) {
    uint8_t* p = data + i * bytesPerVoxel;
    std::reverse(p, p + bytesPerVoxel);
  }
}

ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding parseShardedEncoding(QString s, const char* field)
{
  s = s.trimmed().toLower();
  if (s.isEmpty() || s == "raw") {
    return ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Raw;
  }
  if (s == "gzip") {
    return ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Gzip;
  }
  throw ZException(fmt::format("Unsupported '{}' in neuroglancer sharding: '{}'", field, toStdString(s)));
}

} // namespace

struct ZNeuroglancerPrecomputedVolume::ChunkCache
{
  struct InFlightChunkRead
  {
    SharedLoadCompletion<std::shared_ptr<ZImg>> completion;
  };

  struct InFlightMinishardIndexRead
  {
    SharedLoadCompletion<std::shared_ptr<const DecodedMinishardIndex>> completion;
  };

  ChunkCache(size_t chunkMaxBytes, size_t minishardIndexMaxBytes)
    : chunkCache(chunkMaxBytes, ZCpuInfo::instance().nLogicalCores * 2)
    , minishardIndexCache(minishardIndexMaxBytes, ZCpuInfo::instance().nLogicalCores * 2)
  {}

  ZConcurrentLRUCache<ChunkCacheKey, std::shared_ptr<ZImg>> chunkCache;
  ZConcurrentLRUCache<MinishardIndexCacheKey, std::shared_ptr<const DecodedMinishardIndex>> minishardIndexCache;

  std::mutex inFlightMutex;
  std::map<ChunkCacheKey, std::shared_ptr<InFlightChunkRead>> inFlightChunkReads;
  std::map<MinishardIndexCacheKey, std::shared_ptr<InFlightMinishardIndexRead>> inFlightMinishardIndexReads;
};

struct ZNeuroglancerPrecomputedVolume::InFlightSegmentPropertiesLoad
{
  SharedLoadCompletion<std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>> completion;
};

struct ZNeuroglancerPrecomputedVolume::InFlightMeshSourceLoad
{
  SharedLoadCompletion<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource>> completion;
};

struct ZNeuroglancerPrecomputedVolume::InFlightSkeletonSourceLoad
{
  SharedLoadCompletion<std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource>> completion;
};

QString ZNeuroglancerPrecomputedVolume::normalizeRootUrl(QString url)
{
  return normalizeNeuroglancerPrecomputedRootUrl(std::move(url));
}

QUrl ZNeuroglancerPrecomputedVolume::infoUrl() const
{
  return m_rootQUrl.resolved(QUrl("info"));
}

int64_t ZNeuroglancerPrecomputedVolume::floorDiv(int64_t a, int64_t b)
{
  CHECK(b > 0);
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && a < 0) {
    --q;
  }
  return q;
}

int64_t ZNeuroglancerPrecomputedVolume::ceilDiv(int64_t a, int64_t b)
{
  CHECK(b > 0);
  return -floorDiv(-a, b);
}

std::shared_ptr<ZNeuroglancerPrecomputedVolume>
ZNeuroglancerPrecomputedVolume::open(QString url,
                                     std::chrono::milliseconds timeout,
                                     std::shared_ptr<const ZRemoteObjectStore> objectStore)
{
  auto vol = std::shared_ptr<ZNeuroglancerPrecomputedVolume>(new ZNeuroglancerPrecomputedVolume());
  vol->m_remoteContext = ZNeuroglancerRemoteContext::create(timeout, std::move(objectStore));

  vol->m_rootUrl = normalizeRootUrl(std::move(url));
  vol->m_rootQUrl = QUrl(vol->m_rootUrl);
  if (!vol->m_rootQUrl.isValid()) {
    throw ZException(fmt::format("Invalid URL '{}'", toStdString(vol->m_rootUrl)));
  }

  const std::string infoUrlStr = toStdString(vol->infoUrl().toString());
  auto infoResOpt = folly::coro::blockingWait(vol->m_remoteContext->getResponseAsync(infoUrlStr));
  if (!infoResOpt) {
    throw ZException(fmt::format(
      "Neuroglancer precomputed info not found (HTTP 403/404) at '{}'. Ensure the URL points to a precomputed dataset root (directory containing an 'info' file).",
      infoUrlStr));
  }
  if (infoResOpt->status != 200) {
    throw ZException(
      fmt::format("Failed to fetch neuroglancer precomputed info from '{}' (HTTP {})", infoUrlStr, infoResOpt->status));
  }

  const std::string infoText(reinterpret_cast<const char*>(infoResOpt->body.data()), infoResOpt->body.size());
  json::value jv = json::parse(infoText);
  if (!jv.is_object()) {
    throw ZException("Neuroglancer info is not a JSON object");
  }
  const auto& root = jv.as_object();

  vol->m_dataTypeString = requireString(root, "data_type").toLower();
  vol->m_volumeTypeString = requireString(root, "type").toLower();
  vol->m_numChannels = static_cast<size_t>(requireInt64(root, "num_channels"));
  if (vol->m_numChannels == 0) {
    throw ZException("Invalid neuroglancer info: num_channels must be > 0");
  }

  auto scalesIt = root.find("scales");
  if (scalesIt == root.end() || !scalesIt->value().is_array()) {
    throw ZException("Missing or invalid 'scales' in neuroglancer info");
  }
  const auto& scalesArr = scalesIt->value().as_array();
  if (scalesArr.empty()) {
    throw ZException("Invalid 'scales' in neuroglancer info: empty");
  }

  // Parse base scale first
  if (!scalesArr[0].is_object()) {
    throw ZException("Invalid 'scales' in neuroglancer info: expected objects");
  }
  const auto& baseObj = scalesArr[0].as_object();
  vol->m_baseResolutionNm = requireDouble3(baseObj, "resolution");
  vol->m_baseVoxelOffset = optionalInt3(baseObj, "voxel_offset").value_or(std::array<int64_t, 3>{0, 0, 0});

  auto baseSize = requireInt3(baseObj, "size");
  for (size_t i = 0; i < 3; ++i) {
    if (baseSize[i] <= 0) {
      throw ZException("Invalid neuroglancer base scale: size must be > 0");
    }
  }

  ZImgInfo baseInfo(static_cast<size_t>(baseSize[0]),
                    static_cast<size_t>(baseSize[1]),
                    static_cast<size_t>(baseSize[2]),
                    vol->m_numChannels,
                    1);
  baseInfo.setVoxelFormat(vol->m_dataTypeString);
  baseInfo.voxelSizeUnit = VoxelSizeUnit::nm;
  baseInfo.voxelSizeX = vol->m_baseResolutionNm[0];
  baseInfo.voxelSizeY = vol->m_baseResolutionNm[1];
  baseInfo.voxelSizeZ = vol->m_baseResolutionNm[2];
  baseInfo.createDefaultDescriptions();
  vol->m_baseImgInfo = baseInfo;

  vol->m_segmentPropertiesKey = optionalString(root, "segment_properties");
  if (!vol->m_segmentPropertiesKey.isEmpty()) {
    QString segDir = vol->m_segmentPropertiesKey;
    if (!segDir.endsWith('/')) {
      segDir += '/';
    }
    vol->m_segmentPropertiesDirUrl = vol->m_rootQUrl.resolved(QUrl(segDir));
  }

  vol->m_meshKey = optionalString(root, "mesh");
  if (!vol->m_meshKey.isEmpty()) {
    if (vol->m_volumeTypeString != "segmentation") {
      throw ZException("Invalid neuroglancer info: 'mesh' is only valid for segmentation volumes");
    }
    QString meshDir = vol->m_meshKey;
    if (!meshDir.endsWith('/')) {
      meshDir += '/';
    }
    vol->m_meshDirUrl = vol->m_rootQUrl.resolved(QUrl(meshDir));
  }

  vol->m_skeletonKey = optionalString(root, "skeletons");
  if (!vol->m_skeletonKey.isEmpty()) {
    if (vol->m_volumeTypeString != "segmentation") {
      throw ZException("Invalid neuroglancer info: 'skeletons' is only valid for segmentation volumes");
    }
    QString skelDir = vol->m_skeletonKey;
    if (!skelDir.endsWith('/')) {
      skelDir += '/';
    }
    vol->m_skeletonDirUrl = vol->m_rootQUrl.resolved(QUrl(skelDir));
  }

  vol->m_scales.clear();
  vol->m_ratioToScaleIndex.clear();
  vol->m_scales.reserve(scalesArr.size());

  for (size_t i = 0; i < scalesArr.size(); ++i) {
    if (!scalesArr[i].is_object()) {
      throw ZException("Invalid 'scales' in neuroglancer info: expected objects");
    }
    const auto& so = scalesArr[i].as_object();
    Scale scale{};
    scale.key = requireString(so, "key");
    scale.url = vol->m_rootQUrl.resolved(QUrl(scale.key + "/"));
    scale.resolutionNm = requireDouble3(so, "resolution");
    scale.voxelOffset = optionalInt3(so, "voxel_offset").value_or(std::array<int64_t, 3>{0, 0, 0});
    scale.size = requireInt3(so, "size");
    for (size_t d = 0; d < 3; ++d) {
      if (scale.size[d] <= 0) {
        throw ZException("Invalid neuroglancer scale: size must be > 0");
      }
    }
    scale.chunkSizes = requireChunkSizes(so);
    scale.chunkSize = scale.chunkSizes.front();
    for (size_t d = 0; d < 3; ++d) {
      scale.chunkGridSize[d] = static_cast<uint64_t>(ceilDiv(scale.size[d], scale.chunkSize[d]));
      CHECK(scale.chunkGridSize[d] > 0);
    }
    scale.encoding = requireString(so, "encoding");
    scale.hidden = optionalBool(so, "hidden").value_or(false);

    auto shardingIt = so.find("sharding");
    if (shardingIt != so.end() && !shardingIt->value().is_null()) {
      if (!shardingIt->value().is_object()) {
        throw ZException("Invalid 'sharding' in neuroglancer scale: expected object");
      }
      const auto& shObj = shardingIt->value().as_object();
      Scale::Sharding sharding{};
      const QString type = requireString(shObj, "@type");
      if (type != "neuroglancer_uint64_sharded_v1") {
        throw ZException(fmt::format("Unsupported neuroglancer sharding '@type': '{}'", toStdString(type)));
      }

      sharding.preshiftBits = static_cast<size_t>(requireUint64(shObj, "preshift_bits"));
      if (sharding.preshiftBits > 63) {
        throw ZException("Invalid neuroglancer sharding: preshift_bits must be <= 63");
      }

      const QString hashStr = requireString(shObj, "hash").trimmed().toLower();
      if (hashStr == "identity") {
        sharding.hash = Scale::Sharding::Hash::Identity;
      } else if (hashStr == "murmurhash3_x86_128") {
        sharding.hash = Scale::Sharding::Hash::MurmurHash3X86_128;
      } else {
        throw ZException(fmt::format("Unsupported neuroglancer sharding hash '{}'", toStdString(hashStr)));
      }

      sharding.minishardBits = static_cast<size_t>(requireUint64(shObj, "minishard_bits"));
      sharding.shardBits = static_cast<size_t>(requireUint64(shObj, "shard_bits"));
      if (sharding.shardBits >= 64) {
        throw ZException("Invalid neuroglancer sharding: shard_bits must be <= 63");
      }
      if (sharding.minishardBits + sharding.shardBits > 64) {
        throw ZException("Invalid neuroglancer sharding: minishard_bits + shard_bits must be <= 64");
      }

      const QString minishardEncStr = optionalString(shObj, "minishard_index_encoding", "raw");
      sharding.minishardIndexEncoding = parseShardedEncoding(minishardEncStr, "minishard_index_encoding");

      const QString dataEncStr = optionalString(shObj, "data_encoding", "raw");
      sharding.dataEncoding = parseShardedEncoding(dataEncStr, "data_encoding");

      if (scale.chunkSizes.size() != 1) {
        throw ZException("Invalid neuroglancer scale: sharded format requires a single chunk size");
      }
      if (sharding.minishardBits >= 60) {
        throw ZException("Invalid neuroglancer sharding: minishard_bits too large");
      }

      sharding.shardIndexSize = 16ULL << sharding.minishardBits;
      CHECK((sharding.shardIndexSize % 16ULL) == 0);

      sharding.minishardMask = sharding.minishardBits == 0 ? 0 : ((1ULL << sharding.minishardBits) - 1ULL);
      sharding.shardMask = sharding.shardBits == 0 ? 0 : ((1ULL << sharding.shardBits) - 1ULL);
      const size_t sumBits = sharding.minishardBits + sharding.shardBits;
      sharding.shardAndMinishardMask = sumBits == 64 ? ~0ULL : (sumBits == 0 ? 0 : ((1ULL << sumBits) - 1ULL));
      sharding.shardHexDigits = static_cast<int>((sharding.shardBits + 3) / 4);

      scale.sharding = sharding;
    }

    const QString encLower = scale.encoding.trimmed().toLower();
    if (encLower == "raw") {
      scale.chunkEncoding = Scale::ChunkEncoding::Raw;
      if (so.find("compressed_segmentation_block_size") != so.end()) {
        throw ZException("Invalid neuroglancer scale: 'compressed_segmentation_block_size' is only valid for encoding 'compressed_segmentation'");
      }
    } else if (encLower == "jpeg") {
      scale.chunkEncoding = Scale::ChunkEncoding::Jpeg;
      if (vol->m_dataTypeString != "uint8") {
        throw ZException("Neuroglancer jpeg encoding requires data_type 'uint8'");
      }
      if (vol->m_numChannels != 1 && vol->m_numChannels != 3) {
        throw ZException("Neuroglancer jpeg encoding requires num_channels to be 1 or 3");
      }
      if (so.find("compressed_segmentation_block_size") != so.end()) {
        throw ZException("Invalid neuroglancer scale: 'compressed_segmentation_block_size' is only valid for encoding 'compressed_segmentation'");
      }
    } else if (encLower == "png") {
      scale.chunkEncoding = Scale::ChunkEncoding::Png;
      if (vol->m_baseImgInfo.voxelFormat != VoxelFormat::Unsigned) {
        throw ZException("Neuroglancer png encoding requires an unsigned data_type");
      }
      if (vol->m_baseImgInfo.bytesPerVoxel != 1 && vol->m_baseImgInfo.bytesPerVoxel != 2) {
        throw ZException("Neuroglancer png encoding requires data_type with 1 or 2 bytes per voxel");
      }
      if (vol->m_numChannels == 0 || vol->m_numChannels > 4) {
        throw ZException("Neuroglancer png encoding requires num_channels to be in the range 1..4");
      }
      if (so.find("compressed_segmentation_block_size") != so.end()) {
        throw ZException("Invalid neuroglancer scale: 'compressed_segmentation_block_size' is only valid for encoding 'compressed_segmentation'");
      }
    } else if (encLower == "compresso") {
      scale.chunkEncoding = Scale::ChunkEncoding::Compresso;
      if (vol->m_baseImgInfo.voxelFormat != VoxelFormat::Unsigned) {
        throw ZException("Neuroglancer compresso encoding requires an unsigned data_type");
      }
      if (vol->m_baseImgInfo.bytesPerVoxel != 1 && vol->m_baseImgInfo.bytesPerVoxel != 2 && vol->m_baseImgInfo.bytesPerVoxel != 4 &&
          vol->m_baseImgInfo.bytesPerVoxel != 8) {
        throw ZException("Neuroglancer compresso encoding requires data_type with 1, 2, 4, or 8 bytes per voxel");
      }
      if (vol->m_numChannels != 1) {
        throw ZException("Neuroglancer compresso encoding requires num_channels to be 1");
      }
      if (so.find("compressed_segmentation_block_size") != so.end()) {
        throw ZException("Invalid neuroglancer scale: 'compressed_segmentation_block_size' is only valid for encoding 'compressed_segmentation'");
      }
    } else if (encLower == "compressed_segmentation") {
      scale.chunkEncoding = Scale::ChunkEncoding::CompressedSegmentation;
      if (vol->m_dataTypeString != "uint32" && vol->m_dataTypeString != "uint64") {
        throw ZException("Neuroglancer compressed_segmentation encoding requires data_type 'uint32' or 'uint64'");
      }
      scale.compressedSegmentationBlockSize = requireInt3(so, "compressed_segmentation_block_size");
      for (size_t d = 0; d < 3; ++d) {
        if ((*scale.compressedSegmentationBlockSize)[d] <= 0) {
          throw ZException("Invalid neuroglancer scale: compressed_segmentation_block_size must be > 0");
        }
      }
    } else {
      throw ZException(fmt::format(
        "Neuroglancer precomputed encoding '{}' is not supported yet (supported: 'raw', 'jpeg', 'png', 'compresso', 'compressed_segmentation')",
        toStdString(scale.encoding)));
    }

    std::array<size_t, 3> ratio{};
    for (size_t d = 0; d < 3; ++d) {
      if (vol->m_baseResolutionNm[d] <= 0.0 || scale.resolutionNm[d] <= 0.0) {
        throw ZException("Invalid neuroglancer resolution: must be > 0");
      }
      double rel = scale.resolutionNm[d] / vol->m_baseResolutionNm[d];
      double nearest = std::round(rel);
      if (std::abs(rel - nearest) > 1e-6 || nearest < 1.0) {
        throw ZException("Neuroglancer scales with non-integer resolution ratios are not supported yet");
      }
      ratio[d] = static_cast<size_t>(nearest);
    }
    if (ratio[0] != ratio[1]) {
      throw ZException("Neuroglancer scales with x/y ratio mismatch are not supported yet");
    }
    scale.ratioToBase = ratio;

    size_t scaleIndex = vol->m_scales.size();
    vol->m_scales.push_back(scale);
    vol->m_ratioToScaleIndex.emplace(ratio, scaleIndex);
  }

  auto validateCacheProportion = [](double p, const char* flagName) {
    if (!std::isfinite(p) || p < 0.0 || p > 1.0) {
      throw ZException(fmt::format("Invalid flag --{}={} (expected a proportion in [0, 1])", flagName, p));
    }
  };

  validateCacheProportion(absl::GetFlag(FLAGS_atlas_ng_precomputed_chunk_cache_memory_proportion),
                          "atlas_ng_precomputed_chunk_cache_memory_proportion");
  validateCacheProportion(absl::GetFlag(FLAGS_atlas_ng_precomputed_minishard_index_cache_memory_proportion),
                          "atlas_ng_precomputed_minishard_index_cache_memory_proportion");

  const double totalProp = absl::GetFlag(FLAGS_atlas_ng_precomputed_chunk_cache_memory_proportion) +
                           absl::GetFlag(FLAGS_atlas_ng_precomputed_minishard_index_cache_memory_proportion);
  if (totalProp > 0.85) {
    LOG(WARNING) << "Neuroglancer precomputed caches are configured to use "
                 << static_cast<int>(std::lround(totalProp * 100.0))
                 << "% of physical RAM per opened volume. This may increase paging or cause out-of-memory conditions.";
  }

  const size_t chunkCacheBytes =
    static_cast<size_t>(static_cast<double>(ZCpuInfo::instance().nPhysicalRAM) *
                        absl::GetFlag(FLAGS_atlas_ng_precomputed_chunk_cache_memory_proportion));
  const size_t minishardIndexCacheBytes =
    static_cast<size_t>(static_cast<double>(ZCpuInfo::instance().nPhysicalRAM) *
                        absl::GetFlag(FLAGS_atlas_ng_precomputed_minishard_index_cache_memory_proportion));
  vol->m_chunkCache = std::make_unique<ChunkCache>(chunkCacheBytes, minishardIndexCacheBytes);
  return vol;
}

std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> ZNeuroglancerPrecomputedVolume::segmentPropertiesShared() const
{
  const std::scoped_lock lock(m_segmentPropertiesMutex);
  return m_segmentProperties;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>>
ZNeuroglancerPrecomputedVolume::loadSegmentPropertiesAsync() const
{
  if (!hasSegmentPropertiesDirectory()) {
    throw ZException("This Neuroglancer dataset does not specify 'segment_properties' in its volume info.");
  }

  std::shared_ptr<InFlightSegmentPropertiesLoad> inFlight;
  std::optional<SharedLoadFuture<std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>>> sharedFuture;
  bool isLeader = false;
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  {
    const std::scoped_lock lock(m_segmentPropertiesMutex);
    if (m_segmentProperties) {
      co_return m_segmentProperties;
    }
    if (m_segmentPropertiesInFlight) {
      inFlight = m_segmentPropertiesInFlight;
      sharedFuture = getSharedLoadFuture(inFlight->completion);
    } else {
      m_segmentPropertiesInFlight = std::make_shared<InFlightSegmentPropertiesLoad>();
      inFlight = m_segmentPropertiesInFlight;
      isLeader = true;
    }
  }

  if (!isLeader) {
    CHECK(sharedFuture);
    co_return co_await awaitSharedLoad(std::move(*sharedFuture), cancellationToken);
  }

  auto clearInFlight = [&]() {
    const std::scoped_lock lock(m_segmentPropertiesMutex);
    if (m_segmentPropertiesInFlight == inFlight) {
      m_segmentPropertiesInFlight.reset();
    }
  };

  try {
    std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> loaded =
      co_await ZNeuroglancerPrecomputedSegmentProperties::openAsync(segmentPropertiesDirUrl(), m_remoteContext);
    fulfillSharedLoadValue(inFlight->completion, loaded);

    {
      const std::scoped_lock lock(m_segmentPropertiesMutex);
      m_segmentProperties = loaded;
    }
    clearInFlight();
    co_return loaded;
  }
  catch (...) {
    fulfillSharedLoadException(inFlight->completion);
    clearInFlight();
    throw;
  }
}

std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>
ZNeuroglancerPrecomputedVolume::loadSegmentPropertiesBlocking() const
{
  return folly::coro::blockingWait(loadSegmentPropertiesAsync());
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> ZNeuroglancerPrecomputedVolume::meshSourceShared() const
{
  const std::scoped_lock lock(m_meshMutex);
  return m_meshSource;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource>>
ZNeuroglancerPrecomputedVolume::loadMeshSourceAsync() const
{
  if (!hasMeshDirectory()) {
    throw ZException("This Neuroglancer dataset does not specify 'mesh' in its volume info.");
  }

  std::shared_ptr<InFlightMeshSourceLoad> inFlight;
  std::optional<SharedLoadFuture<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource>>> sharedFuture;
  bool isLeader = false;
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  {
    const std::scoped_lock lock(m_meshMutex);
    if (m_meshSource) {
      co_return m_meshSource;
    }
    if (m_meshSourceInFlight) {
      inFlight = m_meshSourceInFlight;
      sharedFuture = getSharedLoadFuture(inFlight->completion);
    } else {
      m_meshSourceInFlight = std::make_shared<InFlightMeshSourceLoad>();
      inFlight = m_meshSourceInFlight;
      isLeader = true;
    }
  }

  if (!isLeader) {
    CHECK(sharedFuture);
    co_return co_await awaitSharedLoad(std::move(*sharedFuture), cancellationToken);
  }

  auto clearInFlight = [&]() {
    const std::scoped_lock lock(m_meshMutex);
    if (m_meshSourceInFlight == inFlight) {
      m_meshSourceInFlight.reset();
    }
  };

  try {
    std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> loaded =
      co_await ZNeuroglancerPrecomputedMeshSource::openAsync(meshDirUrl(),
                                                             m_baseResolutionNm,
                                                             m_baseVoxelOffset,
                                                             m_remoteContext);
    fulfillSharedLoadValue(inFlight->completion, loaded);

    {
      const std::scoped_lock lock(m_meshMutex);
      m_meshSource = loaded;
    }
    clearInFlight();
    co_return loaded;
  }
  catch (...) {
    fulfillSharedLoadException(inFlight->completion);
    clearInFlight();
    throw;
  }
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> ZNeuroglancerPrecomputedVolume::loadMeshSourceBlocking() const
{
  return folly::coro::blockingWait(loadMeshSourceAsync());
}

std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> ZNeuroglancerPrecomputedVolume::skeletonSourceShared() const
{
  const std::scoped_lock lock(m_skeletonMutex);
  return m_skeletonSource;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource>>
ZNeuroglancerPrecomputedVolume::loadSkeletonSourceAsync() const
{
  if (!hasSkeletonDirectory()) {
    throw ZException("This Neuroglancer dataset does not specify 'skeletons' in its volume info.");
  }

  std::shared_ptr<InFlightSkeletonSourceLoad> inFlight;
  std::optional<SharedLoadFuture<std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource>>> sharedFuture;
  bool isLeader = false;
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  {
    const std::scoped_lock lock(m_skeletonMutex);
    if (m_skeletonSource) {
      co_return m_skeletonSource;
    }
    if (m_skeletonSourceInFlight) {
      inFlight = m_skeletonSourceInFlight;
      sharedFuture = getSharedLoadFuture(inFlight->completion);
    } else {
      m_skeletonSourceInFlight = std::make_shared<InFlightSkeletonSourceLoad>();
      inFlight = m_skeletonSourceInFlight;
      isLeader = true;
    }
  }

  if (!isLeader) {
    CHECK(sharedFuture);
    co_return co_await awaitSharedLoad(std::move(*sharedFuture), cancellationToken);
  }

  auto clearInFlight = [&]() {
    const std::scoped_lock lock(m_skeletonMutex);
    if (m_skeletonSourceInFlight == inFlight) {
      m_skeletonSourceInFlight.reset();
    }
  };

  try {
    std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> loaded =
      co_await ZNeuroglancerPrecomputedSkeletonSource::openAsync(skeletonDirUrl(),
                                                                 m_baseResolutionNm,
                                                                 m_baseVoxelOffset,
                                                                 m_remoteContext);
    fulfillSharedLoadValue(inFlight->completion, loaded);

    {
      const std::scoped_lock lock(m_skeletonMutex);
      m_skeletonSource = loaded;
    }
    clearInFlight();
    co_return loaded;
  }
  catch (...) {
    fulfillSharedLoadException(inFlight->completion);
    clearInFlight();
    throw;
  }
}

std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource>
ZNeuroglancerPrecomputedVolume::loadSkeletonSourceBlocking() const
{
  return folly::coro::blockingWait(loadSkeletonSourceAsync());
}

std::optional<size_t> ZNeuroglancerPrecomputedVolume::scaleIndexForRatio(const std::array<size_t, 3>& ratio) const
{
  auto it = m_ratioToScaleIndex.find(ratio);
  if (it == m_ratioToScaleIndex.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<std::array<size_t, 3>> ZNeuroglancerPrecomputedVolume::availableRatios() const
{
  std::vector<std::array<size_t, 3>> out;
  out.reserve(m_ratioToScaleIndex.size());
  for (const auto& kv : m_ratioToScaleIndex) {
    out.push_back(kv.first);
  }
  return out;
}

std::vector<ZNeuroglancerPrecomputedVolume::Chunk> ZNeuroglancerPrecomputedVolume::chunksIntersectingBaseBox(
  size_t scaleIndex,
  const std::array<int64_t, 3>& baseStart,
  const std::array<int64_t, 3>& baseEnd) const
{
  CHECK(scaleIndex < m_scales.size());
  const Scale& scale = m_scales[scaleIndex];
  const auto ratio = scale.ratioToBase;

  // Convert base box to global scale box (in scale voxels) and intersect with volume bounds.
  std::array<int64_t, 3> globalScaleStart{};
  std::array<int64_t, 3> globalScaleEnd{};
  for (size_t d = 0; d < 3; ++d) {
    CHECK(ratio[d] > 0);
    const int64_t r = static_cast<int64_t>(ratio[d]);
    const int64_t globalBaseStart = baseStart[d] + m_baseVoxelOffset[d];
    const int64_t globalBaseEnd = baseEnd[d] + m_baseVoxelOffset[d];
    globalScaleStart[d] = floorDiv(globalBaseStart, r);
    globalScaleEnd[d] = ceilDiv(globalBaseEnd, r);
  }

  std::array<int64_t, 3> overlapGlobalStart{};
  std::array<int64_t, 3> overlapGlobalEnd{};
  for (size_t d = 0; d < 3; ++d) {
    const int64_t lower = scale.voxelOffset[d];
    const int64_t upper = scale.voxelOffset[d] + scale.size[d];
    overlapGlobalStart[d] = std::max(globalScaleStart[d], lower);
    overlapGlobalEnd[d] = std::min(globalScaleEnd[d], upper);
    if (overlapGlobalStart[d] >= overlapGlobalEnd[d]) {
      return {};
    }
  }

  std::array<int64_t, 3> localStart{};
  std::array<int64_t, 3> localEnd{};
  for (size_t d = 0; d < 3; ++d) {
    localStart[d] = overlapGlobalStart[d] - scale.voxelOffset[d];
    localEnd[d] = overlapGlobalEnd[d] - scale.voxelOffset[d];
  }

  std::array<int64_t, 3> chunkIdxStart{};
  std::array<int64_t, 3> chunkIdxEnd{};
  for (size_t d = 0; d < 3; ++d) {
    const int64_t cs = scale.chunkSize[d];
    CHECK(cs > 0);
    chunkIdxStart[d] = localStart[d] / cs;
    chunkIdxEnd[d] = ceilDiv(localEnd[d], cs);
  }

  std::vector<Chunk> out;
  for (int64_t cz = chunkIdxStart[2]; cz < chunkIdxEnd[2]; ++cz) {
    const int64_t z0 = cz * scale.chunkSize[2];
    const int64_t z1 = std::min(z0 + scale.chunkSize[2], scale.size[2]);
    for (int64_t cy = chunkIdxStart[1]; cy < chunkIdxEnd[1]; ++cy) {
      const int64_t y0 = cy * scale.chunkSize[1];
      const int64_t y1 = std::min(y0 + scale.chunkSize[1], scale.size[1]);
      for (int64_t cx = chunkIdxStart[0]; cx < chunkIdxEnd[0]; ++cx) {
        const int64_t x0 = cx * scale.chunkSize[0];
        const int64_t x1 = std::min(x0 + scale.chunkSize[0], scale.size[0]);

        Chunk c{};
        c.scaleIndex = scaleIndex;
        c.globalStart = {x0 + scale.voxelOffset[0], y0 + scale.voxelOffset[1], z0 + scale.voxelOffset[2]};
        c.globalEnd = {x1 + scale.voxelOffset[0], y1 + scale.voxelOffset[1], z1 + scale.voxelOffset[2]};
        for (size_t d = 0; d < 3; ++d) {
          const int64_t r = static_cast<int64_t>(ratio[d]);
          c.baseStart[d] = c.globalStart[d] * r - m_baseVoxelOffset[d];
          c.baseEnd[d] = c.globalEnd[d] * r - m_baseVoxelOffset[d];
        }
        out.push_back(c);
      }
    }
  }
  return out;
}

QUrl ZNeuroglancerPrecomputedVolume::chunkUrl(const Chunk& chunk) const
{
  CHECK(chunk.scaleIndex < m_scales.size());
  const auto& scale = m_scales[chunk.scaleIndex];
  const QString name = QString("%1-%2_%3-%4_%5-%6")
                         .arg(chunk.globalStart[0])
                         .arg(chunk.globalEnd[0])
                         .arg(chunk.globalStart[1])
                         .arg(chunk.globalEnd[1])
                         .arg(chunk.globalStart[2])
                         .arg(chunk.globalEnd[2]);
  return scale.url.resolved(QUrl(name));
}

folly::coro::Task<std::shared_ptr<ZImg>>
ZNeuroglancerPrecomputedVolume::readChunkAsync(Chunk chunk,
                                               ZImgReadStatsSink* statsSink,
                                               ZImgReadStatsContext statsContext) const
{
  CHECK(m_chunkCache);
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);
  CHECK(chunk.scaleIndex < m_scales.size());
  const auto& scale = m_scales[chunk.scaleIndex];

  const ChunkCacheKey cacheKey =
    std::make_tuple(chunk.scaleIndex,
                    chunk.globalStart[0],
                    chunk.globalEnd[0],
                    chunk.globalStart[1],
                    chunk.globalEnd[1],
                    chunk.globalStart[2],
                    chunk.globalEnd[2]);
  if (auto cached =
        m_chunkCache->chunkCache.find(cacheKey,
                                      ZConcurrentLRUCache<ChunkCacheKey, std::shared_ptr<ZImg>>::FindStrategy::MaybeUpdateLRUList);
      cached) {
    co_return cached.value();
  }

  std::shared_ptr<ChunkCache::InFlightChunkRead> inFlight;
  std::optional<SharedLoadFuture<std::shared_ptr<ZImg>>> sharedFuture;
  bool isLeader = false;
  {
    const std::scoped_lock lock(m_chunkCache->inFlightMutex);
    auto it = m_chunkCache->inFlightChunkReads.find(cacheKey);
    if (it != m_chunkCache->inFlightChunkReads.end()) {
      inFlight = it->second;
      sharedFuture = getSharedLoadFuture(inFlight->completion);
    } else {
      inFlight = std::make_shared<ChunkCache::InFlightChunkRead>();
      m_chunkCache->inFlightChunkReads.emplace(cacheKey, inFlight);
      isLeader = true;
    }
  }

  if (!isLeader) {
    CHECK(sharedFuture);
    std::shared_ptr<ZImg> sharedImg = co_await awaitSharedLoad(std::move(*sharedFuture), cancellationToken);
    maybeCancel(cancellationToken);
    co_return sharedImg;
  }

  auto eraseInFlight = [&]() {
    const std::scoped_lock lock(m_chunkCache->inFlightMutex);
    auto it = m_chunkCache->inFlightChunkReads.find(cacheKey);
    if (it != m_chunkCache->inFlightChunkReads.end() && it->second == inFlight) {
      m_chunkCache->inFlightChunkReads.erase(it);
    }
  };

  auto doRead = [&]() -> folly::coro::Task<std::shared_ptr<ZImg>> {
    maybeCancel(cancellationToken);
    const size_t sx = static_cast<size_t>(chunk.globalEnd[0] - chunk.globalStart[0]);
    const size_t sy = static_cast<size_t>(chunk.globalEnd[1] - chunk.globalStart[1]);
    const size_t sz = static_cast<size_t>(chunk.globalEnd[2] - chunk.globalStart[2]);
    const size_t bytesPerVoxel = m_baseImgInfo.bytesPerVoxel;
    const size_t expectedBytes = sx * sy * sz * m_numChannels * bytesPerVoxel;

    std::vector<uint8_t> payload;
    std::string chunkDebugUrl;

    if (scale.sharding) {
      const auto& sharding = *scale.sharding;
      std::array<uint64_t, 3> g{};
      for (size_t d = 0; d < 3; ++d) {
        const int64_t localStart = chunk.globalStart[d] - scale.voxelOffset[d];
        CHECK(localStart >= 0);
        const int64_t cs = scale.chunkSize[d];
        CHECK(cs > 0);
        CHECK((localStart % cs) == 0);
        g[d] = static_cast<uint64_t>(localStart / cs);
      }
      const uint64_t chunkId = ZNeuroglancerUint64Sharding::compressedMortonCode(g, scale.chunkGridSize);

      const uint64_t shiftedChunkId = chunkId >> sharding.preshiftBits;
      const uint64_t hashCode = (sharding.hash == Scale::Sharding::Hash::Identity)
                                  ? shiftedChunkId
                                  : ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(shiftedChunkId, /*seed=*/0);
      const uint64_t shardAndMinishard = hashCode & sharding.shardAndMinishardMask;
      const uint64_t minishard = shardAndMinishard & sharding.minishardMask;
      const uint64_t shard = (shardAndMinishard >> sharding.minishardBits) & sharding.shardMask;

      const MinishardIndexCacheKey minishardCacheKey = std::make_tuple(chunk.scaleIndex, shardAndMinishard);

      auto getOrFetchMinishardIndex = [&]() -> folly::coro::Task<std::shared_ptr<const DecodedMinishardIndex>> {
        maybeCancel(cancellationToken);
        if (auto cached = m_chunkCache->minishardIndexCache.find(
              minishardCacheKey,
              ZConcurrentLRUCache<MinishardIndexCacheKey, std::shared_ptr<const DecodedMinishardIndex>>::FindStrategy::MaybeUpdateLRUList);
            cached) {
          co_return cached.value();
        }

        std::shared_ptr<ChunkCache::InFlightMinishardIndexRead> inFlightMinishard;
        std::optional<SharedLoadFuture<std::shared_ptr<const DecodedMinishardIndex>>> sharedMinishardFuture;
        bool isMinishardLeader = false;
        {
          const std::scoped_lock lock(m_chunkCache->inFlightMutex);
          auto it = m_chunkCache->inFlightMinishardIndexReads.find(minishardCacheKey);
          if (it != m_chunkCache->inFlightMinishardIndexReads.end()) {
            inFlightMinishard = it->second;
            sharedMinishardFuture = getSharedLoadFuture(inFlightMinishard->completion);
          } else {
            inFlightMinishard = std::make_shared<ChunkCache::InFlightMinishardIndexRead>();
            m_chunkCache->inFlightMinishardIndexReads.emplace(minishardCacheKey, inFlightMinishard);
            isMinishardLeader = true;
          }
        }

        if (!isMinishardLeader) {
          CHECK(sharedMinishardFuture);
          std::shared_ptr<const DecodedMinishardIndex> sharedIndex =
            co_await awaitSharedLoad(std::move(*sharedMinishardFuture), cancellationToken);
          maybeCancel(cancellationToken);
          co_return sharedIndex;
        }

        auto eraseMinishardInFlight = [&]() {
          const std::scoped_lock lock(m_chunkCache->inFlightMutex);
          auto it = m_chunkCache->inFlightMinishardIndexReads.find(minishardCacheKey);
          if (it != m_chunkCache->inFlightMinishardIndexReads.end() && it->second == inFlightMinishard) {
            m_chunkCache->inFlightMinishardIndexReads.erase(it);
          }
        };

        try {
          auto entryOpt = co_await getNeuroglancerShardIndexEntryAsync(*m_remoteContext,
                                                                       scale.url,
                                                                       sharding,
                                                                       shard,
                                                                       minishard,
                                                                       ZRemoteRangeReadPolicy::RequireExactLength,
                                                                       statsSink,
                                                                       statsContext);
          maybeCancel(cancellationToken);
          if (!entryOpt) {
            fulfillSharedLoadValue(inFlightMinishard->completion, std::shared_ptr<const DecodedMinishardIndex>());
            eraseMinishardInFlight();
            co_return std::shared_ptr<const DecodedMinishardIndex>();
          }

          std::shared_ptr<const DecodedMinishardIndex> minishardIndex;
          auto decodedIndexOpt =
            co_await getNeuroglancerDecodedMinishardIndexAsync(*m_remoteContext,
                                                               *entryOpt,
                                                               sharding,
                                                               ZRemoteRangeReadPolicy::RequireExactLength,
                                                               statsSink,
                                                               statsContext);
          maybeCancel(cancellationToken);
          if (!decodedIndexOpt) {
            fulfillSharedLoadValue(inFlightMinishard->completion, std::shared_ptr<const DecodedMinishardIndex>());
            eraseMinishardInFlight();
            co_return std::shared_ptr<const DecodedMinishardIndex>();
          }
          minishardIndex = std::make_shared<DecodedMinishardIndex>(std::move(*decodedIndexOpt));

          maybeCancel(cancellationToken);
          m_chunkCache->minishardIndexCache.insert(minishardCacheKey, minishardIndex, minishardIndex->byteSize());
          fulfillSharedLoadValue(inFlightMinishard->completion, minishardIndex);
          eraseMinishardInFlight();
          co_return minishardIndex;
        }
        catch (...) {
          fulfillSharedLoadException(inFlightMinishard->completion);
          eraseMinishardInFlight();
          throw;
        }
      };

      auto minishardIndex = co_await getOrFetchMinishardIndex();
      maybeCancel(cancellationToken);
      if (!minishardIndex) {
        co_return std::shared_ptr<ZImg>();
      }

      auto locationOpt = findNeuroglancerShardedPayloadLocation(*minishardIndex, chunkId);
      if (!locationOpt) {
        co_return std::shared_ptr<ZImg>();
      }

      std::string dataUrl;
      if (!chunkDebugUrl.empty()) {
        dataUrl = chunkDebugUrl;
      } else {
        const auto shardedUrls = makeNeuroglancerShardedUrls(scale.url, sharding, shard);
        const int mode = sharding.shardFileMode.load();
        if (mode == 2) {
          dataUrl = toStdString(shardedUrls.dataUrl.toString());
        } else if (mode == 1) {
          dataUrl = toStdString(shardedUrls.shardUrl.toString());
        } else {
          auto entryOpt = co_await getNeuroglancerShardIndexEntryAsync(*m_remoteContext,
                                                                       scale.url,
                                                                       sharding,
                                                                       shard,
                                                                       minishard,
                                                                       ZRemoteRangeReadPolicy::RequireExactLength,
                                                                       statsSink,
                                                                       statsContext);
          maybeCancel(cancellationToken);
          if (!entryOpt) {
            co_return std::shared_ptr<ZImg>();
          }
          dataUrl = entryOpt->dataUrl;
        }
      }

      auto payloadOpt =
        co_await getNeuroglancerDecodedShardedPayloadBytesAsync(*m_remoteContext,
                                                                dataUrl,
                                                                *locationOpt,
                                                                sharding,
                                                                ZRemoteRangeReadPolicy::RequireExactLength,
                                                                statsSink,
                                                                statsContext);
      maybeCancel(cancellationToken);
      if (!payloadOpt) {
        co_return std::shared_ptr<ZImg>();
      }
      payload = std::move(payloadOpt->bytes);
      chunkDebugUrl = dataUrl;
    } else {
      const std::string urlStr = toStdString(chunkUrl(chunk).toString());
      chunkDebugUrl = urlStr;
      auto resOpt = co_await m_remoteContext->getResponseAsync(urlStr, statsSink, statsContext);
      maybeCancel(cancellationToken);
      if (!resOpt) {
        // Missing unsharded chunk objects are a soft miss in Neuroglancer.
        // Sparse datasets may omit all-zero chunks entirely, and getBytes()
        // normalizes both HTTP 403 and 404 to "not found" for parity.
        co_return std::shared_ptr<ZImg>();
      }
      if (resOpt->status != 200) {
        throw ZException(fmt::format("Failed to fetch neuroglancer chunk from '{}' (HTTP {})", urlStr, resOpt->status));
      }
      payload = std::move(resOpt->body);
    }

    maybeCancel(cancellationToken);
    std::vector<uint8_t> body;
    switch (scale.chunkEncoding) {
      case Scale::ChunkEncoding::Raw:
        body = std::move(payload);
        break;
      case Scale::ChunkEncoding::Jpeg: {
        CHECK(bytesPerVoxel == 1);
        body = ZNeuroglancerPrecomputedChunkDecoder::decodeJpegToRaw(
          std::span<const uint8_t>(payload.data(), payload.size()),
          /*expectedVoxelCount=*/sx * sy * sz,
          /*expectedChannels=*/m_numChannels);
        break;
      }
      case Scale::ChunkEncoding::Png: {
        CHECK(bytesPerVoxel == 1 || bytesPerVoxel == 2);
        body = ZImgPng::readMemRaw(std::span<const uint8_t>(payload.data(), payload.size()),
                                   /*expectedVoxelCount=*/sx * sy * sz,
                                   /*expectedChannels=*/m_numChannels,
                                   bytesPerVoxel);
        break;
      }
      case Scale::ChunkEncoding::Compresso: {
        CHECK(bytesPerVoxel == 1 || bytesPerVoxel == 2 || bytesPerVoxel == 4 || bytesPerVoxel == 8);
        CHECK(m_numChannels == 1);
        body = ZNeuroglancerPrecomputedChunkDecoder::decodeCompressoToRaw(
          std::span<const uint8_t>(payload.data(), payload.size()),
          /*expectedChunkSize=*/{sx, sy, sz},
          bytesPerVoxel);
        break;
      }
      case Scale::ChunkEncoding::CompressedSegmentation: {
        CHECK(scale.compressedSegmentationBlockSize);
        CHECK(m_dataTypeString == "uint32" || m_dataTypeString == "uint64");
        std::array<size_t, 3> blockSize{};
        for (size_t d = 0; d < 3; ++d) {
          const int64_t v = (*scale.compressedSegmentationBlockSize)[d];
          CHECK(v > 0);
          blockSize[d] = static_cast<size_t>(v);
        }
        const bool isUint64 = (m_dataTypeString == "uint64");
        body = ZNeuroglancerPrecomputedChunkDecoder::decodeCompressedSegmentationToRaw(
          std::span<const uint8_t>(payload.data(), payload.size()),
          /*chunkSize=*/{sx, sy, sz},
          /*numChannels=*/m_numChannels,
          blockSize,
          isUint64);
        break;
      }
    }

    if (body.size() != expectedBytes) {
      throw ZException(fmt::format("Neuroglancer chunk decode size mismatch for '{}': got {} bytes, expected {} bytes",
                                   chunkDebugUrl,
                                   body.size(),
                                   expectedBytes));
    }

    if (bytesPerVoxel > 1 && std::endian::native == std::endian::big) {
      swapEndianInPlace(body.data(), bytesPerVoxel, sx * sy * sz * m_numChannels);
    }

    ZImgInfo info;
    info.width = sx;
    info.height = sy;
    info.depth = sz;
    info.numChannels = m_numChannels;
    info.numTimes = 1;
    info.bytesPerVoxel = m_baseImgInfo.bytesPerVoxel;
    info.voxelFormat = m_baseImgInfo.voxelFormat;
    info.validBitCount = m_baseImgInfo.validBitCount;
    info.voxelSizeUnit = VoxelSizeUnit::nm;
    info.voxelSizeX = scale.resolutionNm[0];
    info.voxelSizeY = scale.resolutionNm[1];
    info.voxelSizeZ = scale.resolutionNm[2];
    info.createDefaultDescriptions();

    auto img = std::make_shared<ZImg>(info);
    std::memcpy(img->timeData<uint8_t>(0), body.data(), body.size());

    maybeCancel(cancellationToken);
    if (statsSink) {
      statsSink->onSourceLogicalBytes(statsContext, img ? img->byteNumber() : 0);
    }

    maybeCancel(cancellationToken);
    m_chunkCache->chunkCache.insert(cacheKey, img, img->byteNumber());
    co_return img;
  };

  try {
    auto img = co_await doRead();
    fulfillSharedLoadValue(inFlight->completion, img);
    eraseInFlight();
    co_return img;
  }
  catch (...) {
    fulfillSharedLoadException(inFlight->completion);
    eraseInFlight();
    throw;
  }
}

std::shared_ptr<ZImg> ZNeuroglancerPrecomputedVolume::tryGetCachedChunk(const Chunk& chunk) const
{
  CHECK(m_chunkCache);
  const ChunkCacheKey cacheKey =
    std::make_tuple(chunk.scaleIndex,
                    chunk.globalStart[0],
                    chunk.globalEnd[0],
                    chunk.globalStart[1],
                    chunk.globalEnd[1],
                    chunk.globalStart[2],
                    chunk.globalEnd[2]);
  auto cached =
    m_chunkCache->chunkCache.find(cacheKey,
                                  ZConcurrentLRUCache<ChunkCacheKey, std::shared_ptr<ZImg>>::FindStrategy::MaybeUpdateLRUList);
  return cached ? cached.value() : std::shared_ptr<ZImg>();
}

std::shared_ptr<ZImg> ZNeuroglancerPrecomputedVolume::readChunkBlocking(Chunk chunk) const
{
  return folly::coro::blockingWait(readChunkAsync(std::move(chunk)));
}

namespace {

folly::coro::Task<folly::Try<std::shared_ptr<ZImg>>> readChunkTryAsync(const ZNeuroglancerPrecomputedVolume& volume,
                                                                       ZNeuroglancerPrecomputedVolume::Chunk chunk)
{
  co_return co_await folly::coro::co_awaitTry(volume.readChunkAsync(std::move(chunk)));
}

struct CachedSliceBatchResult
{
  std::vector<std::shared_ptr<ZImg>> imgs;
  std::vector<QPoint> locs;
  bool fullyCached = true;
};

struct CachedSliceResult
{
  std::shared_ptr<ZImg> img;
  QPoint loc;
};

[[nodiscard]] int qPointCoordinateChecked(int64_t v)
{
  if (v < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
      v > static_cast<int64_t>(std::numeric_limits<int>::max())) {
    throw ZException(fmt::format("Neuroglancer chunk coordinate {} is out of QPoint range", v));
  }
  return static_cast<int>(v);
}

folly::coro::Task<CachedSliceResult> buildCachedSliceAsync(const ZNeuroglancerPrecomputedVolume& volume,
                                                           ZNeuroglancerPrecomputedVolume::Chunk chunk,
                                                           int64_t z0,
                                                           int64_t ratioZ)
{
  CHECK(ratioZ > 0);

  CachedSliceResult out;

  auto chunkImg = volume.tryGetCachedChunk(chunk);
  if (!chunkImg) {
    co_return out;
  }

  const int64_t localZBase = z0 - chunk.baseStart[2];
  if (localZBase < 0) {
    co_return out;
  }

  const int64_t localZ = localZBase / ratioZ;
  if (localZ < 0) {
    co_return out;
  }
  if (static_cast<size_t>(localZ) >= chunkImg->depth()) {
    co_return out;
  }

  ZImg sliceImg = chunkImg->crop(ZImgRegion(0,
                                            static_cast<index_t>(chunkImg->width()),
                                            0,
                                            static_cast<index_t>(chunkImg->height()),
                                            static_cast<index_t>(localZ),
                                            static_cast<index_t>(localZ + 1)));
  out.img = std::make_shared<ZImg>(std::move(sliceImg));
  out.loc = QPoint(qPointCoordinateChecked(chunk.baseStart[0]), qPointCoordinateChecked(chunk.baseStart[1]));
  co_return out;
}

folly::coro::Task<CachedSliceBatchResult>
buildCachedSliceBatchAsync(const ZNeuroglancerPrecomputedVolume& volume,
                           const std::array<size_t, 3>& ratio,
                           const std::vector<ZNeuroglancerPrecomputedVolume::Chunk>& chunks,
                           int64_t z0)
{
  CachedSliceBatchResult out;
  if (chunks.empty()) {
    co_return out;
  }

  out.imgs.resize(chunks.size());
  out.locs.resize(chunks.size());

  const int64_t ratioZ = static_cast<int64_t>(ratio[2]);
  CHECK(ratioZ > 0);

  const size_t maxConcurrent = std::min(std::max<size_t>(1, ZCpuInfo::instance().nLogicalCores), chunks.size());
  CHECK(maxConcurrent > 0);

  std::vector<folly::coro::TaskWithExecutor<CachedSliceResult>> tasks;
  tasks.reserve(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    tasks.push_back(
      folly::coro::co_withExecutor(getAtlasBackgroundExecutor(), buildCachedSliceAsync(volume, chunks[i], z0, ratioZ)));
  }

  std::vector<CachedSliceResult> results = co_await folly::coro::collectAllWindowed(std::move(tasks), maxConcurrent);
  CHECK(results.size() == chunks.size());

  out.imgs.resize(results.size());
  out.locs.resize(results.size());
  out.fullyCached = true;
  for (size_t i = 0; i < results.size(); ++i) {
    out.imgs[i] = std::move(results[i].img);
    out.locs[i] = results[i].loc;
    if (!out.imgs[i]) {
      out.fullyCached = false;
    }
  }

  co_return out;
}

[[nodiscard]] std::array<size_t, 3> bestRatioForIsotropicScale(const std::vector<std::array<size_t, 3>>& ratios,
                                                               double scale) // x=y=z
{
  CHECK(scale > 0);
  CHECK(!ratios.empty());

  std::array<size_t, 3> best = ratios.front();
  double bestErr = std::numeric_limits<double>::infinity();
  size_t bestRatioSum = 0;

  for (const auto& ratio : ratios) {
    const double ex = std::abs(static_cast<double>(ratio[0]) * scale - 1.0);
    const double ey = std::abs(static_cast<double>(ratio[1]) * scale - 1.0);
    const double ez = std::abs(static_cast<double>(ratio[2]) * scale - 1.0);
    const double err = ex + ey + ez;
    const size_t ratioSum = ratio[0] + ratio[1] + ratio[2];

    // Tie-break toward coarser levels (larger ratios) to reduce network I/O.
    if (err < bestErr - 1e-12 || (std::abs(err - bestErr) <= 1e-12 && ratioSum > bestRatioSum)) {
      best = ratio;
      bestErr = err;
      bestRatioSum = ratioSum;
    }
  }

  return best;
}

[[nodiscard]] std::array<size_t, 3> coarsestRatioFor2DPreview(const std::vector<std::array<size_t, 3>>& ratios)
{
  CHECK(!ratios.empty());

  // For 2D slice rendering we care primarily about XY resolution; preserve slice
  // fidelity by preferring smaller Z downsampling when possible.
  std::array<size_t, 3> best = ratios.front();
  for (const auto& ratio : ratios) {
    if (ratio[0] > best[0]) {
      best = ratio;
      continue;
    }
    if (ratio[0] == best[0] && ratio[2] < best[2]) {
      best = ratio;
      continue;
    }
  }
  return best;
}

[[nodiscard]] std::vector<std::array<size_t, 3>> ratiosAtLeastAsCoarseAs(const std::vector<std::array<size_t, 3>>& ratios,
                                                                         const std::array<size_t, 3>& target)
{
  std::vector<std::array<size_t, 3>> out;
  out.reserve(ratios.size());
  for (const auto& r : ratios) {
    if (r[0] >= target[0] && r[1] >= target[1] && r[2] >= target[2]) {
      out.push_back(r);
    }
  }

  std::sort(out.begin(), out.end(), [](const std::array<size_t, 3>& a, const std::array<size_t, 3>& b) {
    const size_t as = a[0] + a[1] + a[2];
    const size_t bs = b[0] + b[1] + b[2];
    if (as != bs) {
      return as > bs; // coarser first
    }
    return a > b;
  });

  return out;
}

} // namespace

folly::coro::Task<ZNeuroglancerPrecomputedVolume::SliceTilePack>
ZNeuroglancerPrecomputedVolume::sliceTilePackFor2DViewportCacheBestEffortAsync(size_t z,
                                                                               size_t t,
                                                                               const QRectF& viewport,
                                                                               double renderScale) const
{
  CHECK(t == 0) << "Neuroglancer precomputed volumes do not support time dimension yet (t must be 0)";
  CHECK(renderScale > 0);

  SliceTilePack out;

  std::vector<std::array<size_t, 3>> ratios = availableRatios();
  if (ratios.empty()) {
    co_return out;
  }

  const std::array<size_t, 3> targetRatio = bestRatioForIsotropicScale(ratios, renderScale);
  out.targetRatio = targetRatio;

  if (targetRatio[0] != targetRatio[1]) {
    throw ZException(fmt::format("Neuroglancer 2D display currently requires isotropic XY downsampling ratios, but got ratio [{},{},{}]",
                                 targetRatio[0],
                                 targetRatio[1],
                                 targetRatio[2]));
  }

  const int64_t z0 = static_cast<int64_t>(z);
  const std::array<int64_t, 3> boxStart{static_cast<int64_t>(std::floor(viewport.x())),
                                        static_cast<int64_t>(std::floor(viewport.y())),
                                        z0};
  const std::array<int64_t, 3> boxEnd{static_cast<int64_t>(std::ceil(viewport.right())),
                                      static_cast<int64_t>(std::ceil(viewport.bottom())),
                                      z0 + 1};

  const std::vector<std::array<size_t, 3>> candidateRatios = ratiosAtLeastAsCoarseAs(ratios, targetRatio);
  if (candidateRatios.empty()) {
    co_return out;
  }

  auto appendCachedSlicesForRatio =
    [&](const std::array<size_t, 3>& ratio, CachedSliceBatchResult batch, bool* fullyCachedOut) {
      if (fullyCachedOut) {
        *fullyCachedOut = batch.fullyCached;
      }

      const double tileScale = static_cast<double>(ratio[0]);
      for (size_t i = 0; i < batch.imgs.size(); ++i) {
        if (!batch.imgs[i]) {
          continue;
        }
        out.imgs.push_back(std::move(batch.imgs[i]));
        out.locs.push_back(batch.locs[i]);
        out.scales.push_back(tileScale);
        out.ratios.push_back(ratio);
      }
    };

  // First: attempt the targetRatio. If it is fully cached, return *only* that level.
  // Keeping coarser underlays would cause visible "blocky" blending artifacts when the image is rendered
  // with transparency (e.g. segmentation overlays), because QGraphicsItemGroup opacity makes lower layers
  // show through.
  {
    const auto scaleIndexOpt = scaleIndexForRatio(targetRatio);
    if (scaleIndexOpt) {
      const auto chunks = chunksIntersectingBaseBox(*scaleIndexOpt, boxStart, boxEnd);
      CachedSliceBatchResult batch = co_await buildCachedSliceBatchAsync(*this, targetRatio, chunks, z0);
      appendCachedSlicesForRatio(targetRatio, std::move(batch), &out.fullyCachedAtTargetRatio);
      if (out.fullyCachedAtTargetRatio) {
        co_return out;
      }
    }
  }

  // Fallback: collect cached chunks from coarser pyramid levels to cover as much of the viewport as possible.
  // Callers should use their scale/z-ordering policy to ensure finer tiles overwrite coarser ones.
  for (const auto& ratio : candidateRatios) {
    if (ratio[0] != ratio[1]) {
      // 2D view uses a uniform scale factor, so skip anisotropic XY levels rather than rendering incorrectly.
      continue;
    }
    if (ratio == targetRatio) {
      // We already appended any cached targetRatio tiles above. Only consider strictly-coarser levels here.
      continue;
    }

    auto scaleIndexOpt = scaleIndexForRatio(ratio);
    if (!scaleIndexOpt) {
      continue;
    }

    const auto chunks = chunksIntersectingBaseBox(*scaleIndexOpt, boxStart, boxEnd);
    if (chunks.empty()) {
      continue;
    }
    CachedSliceBatchResult batch = co_await buildCachedSliceBatchAsync(*this, ratio, chunks, z0);
    appendCachedSlicesForRatio(ratio, std::move(batch), /*fullyCachedOut=*/nullptr);
  }

  co_return out;
}

folly::coro::Task<ZNeuroglancerPrecomputedVolume::SliceTilePack>
ZNeuroglancerPrecomputedVolume::sliceTilePackFor2DViewportAsync(size_t z,
                                                                size_t t,
                                                                const QRectF& viewport,
                                                                double renderScale,
                                                                Slice2DRatioPolicy ratioPolicy) const
{
  SliceTilePack out;
  const SliceChunkRequests requests = sliceChunkRequestsFor2DViewport(z, t, viewport, renderScale, ratioPolicy);
  out.targetRatio = requests.targetRatio;
  if (requests.chunks.empty()) {
    co_return out;
  }

  const auto& targetRatio = requests.targetRatio;
  const int64_t z0 = static_cast<int64_t>(z);
  const int64_t ratioZ = static_cast<int64_t>(targetRatio[2]);
  CHECK(ratioZ > 0);

  auto toIntChecked = [](int64_t v) -> int {
    if (v < static_cast<int64_t>(std::numeric_limits<int>::min()) || v > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      throw ZException(fmt::format("Neuroglancer chunk coordinate {} is out of QPoint range", v));
    }
    return static_cast<int>(v);
  };

  // Keep one bounded async batch per 2D pass instead of launching one blockingWait() per chunk across
  // a worker pool. This preserves the existing "one preview/final pass per epoch" scheduling while
  // moving the remote I/O and decode path to coroutine-first chunk reads.
  const size_t recommendedChunkReadWindow = std::max<size_t>(1, ZCpuInfo::instance().nLogicalCores);
  const size_t chunkReadWindow = std::min(recommendedChunkReadWindow, requests.chunks.size());
  CHECK(chunkReadWindow > 0);

  std::vector<folly::coro::Task<folly::Try<std::shared_ptr<ZImg>>>> chunkTasks;
  chunkTasks.reserve(requests.chunks.size());
  for (const auto& chunk : requests.chunks) {
    chunkTasks.push_back(readChunkTryAsync(*this, chunk));
  }

  std::vector<folly::Try<std::shared_ptr<ZImg>>> chunkResults =
    co_await folly::coro::collectAllWindowed(std::move(chunkTasks), chunkReadWindow);

  const double tileScale = static_cast<double>(targetRatio[0]);
  size_t failedChunkCount = 0;
  folly::exception_wrapper firstChunkFailure;
  std::string firstChunkFailureMessage;

  out.imgs.reserve(requests.chunks.size());
  out.locs.reserve(requests.chunks.size());
  out.scales.reserve(requests.chunks.size());
  out.ratios.reserve(requests.chunks.size());

  for (size_t i = 0; i < chunkResults.size(); ++i) {
    const auto& chunk = requests.chunks[i];
    auto& chunkResult = chunkResults[i];
    if (chunkResult.hasException()) {
      folly::exception_wrapper error = std::move(chunkResult).exception();
      if (error.is_compatible_with<ZCancellationException>() || error.is_compatible_with<folly::OperationCancelled>()) {
        error.throw_exception();
      }
      ++failedChunkCount;
      if (!firstChunkFailure) {
        firstChunkFailure = error;
        try {
          error.throw_exception();
        }
        catch (const std::exception& e) {
          firstChunkFailureMessage = e.what();
        }
        catch (...) {
          firstChunkFailureMessage = "non-std exception";
        }
      }
      continue;
    }

    std::shared_ptr<ZImg> chunkImg = std::move(chunkResult).value();
    if (!chunkImg) {
      continue;
    }

    const int64_t localZBase = z0 - chunk.baseStart[2];
    CHECK(localZBase >= 0);
    const int64_t localZ = localZBase / ratioZ;
    CHECK(localZ >= 0);
    CHECK(static_cast<size_t>(localZ) < chunkImg->depth());

    ZImg sliceImg = chunkImg->crop(ZImgRegion(0,
                                              static_cast<index_t>(chunkImg->width()),
                                              0,
                                              static_cast<index_t>(chunkImg->height()),
                                              static_cast<index_t>(localZ),
                                              static_cast<index_t>(localZ + 1)));
    out.imgs.push_back(std::make_shared<ZImg>(std::move(sliceImg)));
    out.locs.push_back(QPoint(toIntChecked(chunk.baseStart[0]), toIntChecked(chunk.baseStart[1])));
    out.scales.push_back(tileScale);
    out.ratios.push_back(targetRatio);
  }

  if (out.imgs.empty() && firstChunkFailure) {
    firstChunkFailure.throw_exception();
  }

  if (failedChunkCount > 0) {
    VLOG(1) << "Neuroglancer 2D slice pass skipped " << failedChunkCount << " of " << requests.chunks.size()
            << " chunk(s) at ratio [" << targetRatio[0] << "," << targetRatio[1] << "," << targetRatio[2]
            << "]; first failure: " << firstChunkFailureMessage;
  }

  co_return out;
}

ZNeuroglancerPrecomputedVolume::SliceChunkRequests ZNeuroglancerPrecomputedVolume::sliceChunkRequestsFor2DViewport(
  size_t z,
  size_t t,
  const QRectF& viewport,
  double renderScale,
  Slice2DRatioPolicy ratioPolicy) const
{
  CHECK(t == 0) << "Neuroglancer precomputed volumes do not support time dimension yet (t must be 0)";
  CHECK(renderScale > 0);

  SliceChunkRequests out;

  std::vector<std::array<size_t, 3>> ratios = availableRatios();
  if (ratios.empty()) {
    return out;
  }

  std::array<size_t, 3> targetRatio{};
  switch (ratioPolicy) {
  case Slice2DRatioPolicy::BestForScale:
    targetRatio = bestRatioForIsotropicScale(ratios, renderScale);
    break;
  case Slice2DRatioPolicy::CoarsestXY:
    targetRatio = coarsestRatioFor2DPreview(ratios);
    break;
  }
  out.targetRatio = targetRatio;

  if (targetRatio[0] != targetRatio[1]) {
    throw ZException(fmt::format("Neuroglancer 2D display currently requires isotropic XY downsampling ratios, but got ratio [{},{},{}]",
                                 targetRatio[0],
                                 targetRatio[1],
                                 targetRatio[2]));
  }

  auto scaleIndexOpt = scaleIndexForRatio(targetRatio);
  if (!scaleIndexOpt) {
    throw ZException(fmt::format("Neuroglancer requested ratio [{},{},{}] is not available in this dataset",
                                 targetRatio[0],
                                 targetRatio[1],
                                 targetRatio[2]));
  }

  const int64_t z0 = static_cast<int64_t>(z);
  const std::array<int64_t, 3> boxStart{static_cast<int64_t>(std::floor(viewport.x())),
                                        static_cast<int64_t>(std::floor(viewport.y())),
                                        z0};
  const std::array<int64_t, 3> boxEnd{static_cast<int64_t>(std::ceil(viewport.right())),
                                      static_cast<int64_t>(std::ceil(viewport.bottom())),
                                      z0 + 1};

  out.chunks = chunksIntersectingBaseBox(*scaleIndexOpt, boxStart, boxEnd);
  return out;
}

bool ZNeuroglancerPrecomputedVolume::is2DViewportFullyCachedForCoarsestXY(size_t z, size_t t, const QRectF& viewport) const
{
  CHECK(t == 0) << "Neuroglancer precomputed volumes do not support time dimension yet (t must be 0)";

  std::vector<std::array<size_t, 3>> ratios = availableRatios();
  if (ratios.empty()) {
    return true;
  }

  const std::array<size_t, 3> ratio = coarsestRatioFor2DPreview(ratios);
  if (ratio[0] != ratio[1]) {
    // 2D view assumes an isotropic XY ratio; treat anisotropic levels as unsupported.
    return false;
  }

  auto scaleIndexOpt = scaleIndexForRatio(ratio);
  if (!scaleIndexOpt) {
    return false;
  }

  const int64_t z0 = static_cast<int64_t>(z);
  const std::array<int64_t, 3> boxStart{static_cast<int64_t>(std::floor(viewport.x())),
                                        static_cast<int64_t>(std::floor(viewport.y())),
                                        z0};
  const std::array<int64_t, 3> boxEnd{static_cast<int64_t>(std::ceil(viewport.right())),
                                      static_cast<int64_t>(std::ceil(viewport.bottom())),
                                      z0 + 1};

  const auto chunks = chunksIntersectingBaseBox(*scaleIndexOpt, boxStart, boxEnd);
  if (chunks.empty()) {
    return true;
  }

  for (const auto& c : chunks) {
    if (!tryGetCachedChunk(c)) {
      return false;
    }
  }
  return true;
}

} // namespace nim
