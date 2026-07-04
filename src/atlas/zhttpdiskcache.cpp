#include "zhttpdiskcache.h"

#include "zdiskcacheutils.h"
#include "zfolly.h"
#include "zlog.h"
#include "zsqlitediskcachebucket.h"
#include "zstructutils.h"

#include "zcommandlineflags.h"

#include <boost/hash2/sha2.hpp>

#include <folly/coro/CurrentExecutor.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_http_async_max_pending_bytes);

namespace nim {

namespace {

constexpr uint32_t kCacheVersion = 1;

constexpr char kMagic[8] = {'A', 'T', 'L', 'S', 'H', 'D', 'C', '1'};

struct HttpCacheEntryHeader
{
  std::array<char, 8> magic{};
  uint32_t version = 0;
  uint16_t status = 0;
  uint32_t contentTypeLen = 0;
  uint32_t contentEncLen = 0;
  uint64_t bodyLen = 0;
};

[[nodiscard]] std::string lowerAscii(std::string_view in)
{
  std::string out(in);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

[[nodiscard]] bool hasHeaderLowerKey(const std::vector<std::pair<std::string, std::string>>& headers,
                                     std::string_view keyLowerAscii)
{
  for (const auto& [k, v] : headers) {
    (void)v;
    std::string kLower = lowerAscii(k);
    if (kLower == keyLowerAscii) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string exactByteRangeKeyString(const std::optional<ZHttpByteRange>& exactByteRange)
{
  if (!exactByteRange.has_value()) {
    return {};
  }
  return formatHttpByteRangeHeaderValue(*exactByteRange);
}

[[nodiscard]] bool isCacheableExactRangeResponse(const ZHttpGetRequest& request, const ZHttpGetBytesResult& result)
{
  if (!request.exactByteRange.has_value()) {
    return true;
  }

  // Range-keyed HTTP cache entries must represent the exact requested bytes.
  // Do not persist partial 206 bodies or full-object 200 fallbacks under a
  // byte-range key, or retries can get poisoned by the cache.
  return result.status == 206 && static_cast<uint64_t>(result.body.size()) == request.exactByteRange->length;
}

[[nodiscard]] int64_t nowNs()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] uint64_t estimatedSerializedEntryBytes(const ZHttpGetBytesResult& result)
{
  const size_t headerBytes = compactSize(HttpCacheEntryHeader{});
  uint64_t total = headerBytes;

  if (result.contentType.size() > std::numeric_limits<uint64_t>::max() - total) {
    return std::numeric_limits<uint64_t>::max();
  }
  total += static_cast<uint64_t>(result.contentType.size());

  if (result.contentEncoding.size() > std::numeric_limits<uint64_t>::max() - total) {
    return std::numeric_limits<uint64_t>::max();
  }
  total += static_cast<uint64_t>(result.contentEncoding.size());

  if (result.body.size() > std::numeric_limits<uint64_t>::max() - total) {
    return std::numeric_limits<uint64_t>::max();
  }
  total += static_cast<uint64_t>(result.body.size());
  return total;
}

} // namespace

ZHttpDiskCache::ZHttpDiskCache(QString rootDir, uint64_t maxBytes)
  : m_rootDir(std::move(rootDir))
  , m_maxBytes(maxBytes)
{
  m_rootDir = m_rootDir.trimmed();
  if (m_rootDir.isEmpty() || m_maxBytes == 0) {
    return;
  }

  m_bucket =
    std::make_unique<ZSqliteDiskCacheBucket>(m_rootDir,
                                             QStringLiteral("http.sqlite"),
                                             m_maxBytes,
                                             absl::GetFlag(FLAGS_atlas_disk_cache_http_async_max_pending_bytes),
                                             QStringLiteral("http_disk_cache"));
}

ZHttpDiskCache::~ZHttpDiskCache() = default;

bool ZHttpDiskCache::isEnabled() const
{
  return m_bucket && m_bucket->isEnabled();
}

ZHttpDiskCache::KeyParts ZHttpDiskCache::keyPartsFrom(const ZHttpGetRequest& request)
{
  CHECK(!hasHeaderLowerKey(request.headers, "range"))
    << "ZHttpDiskCache expects typed exact-byte-range metadata instead of raw Range headers";

  KeyParts out;
  out.url = request.url;
  out.exactByteRange = request.exactByteRange;
  out.cachePartition = request.cachePartition;
  return out;
}

ZHttpDiskCache::Blob ZHttpDiskCache::sha256(const void* data, size_t bytes)
{
  boost::hash2::sha2_256 hash;
  if (bytes > 0) {
    CHECK(data != nullptr);
    hash.update(data, bytes);
  }
  const boost::hash2::sha2_256::result_type digest = hash.result();
  Blob out;
  out.resize(digest.size());
  CHECK(out.size() == digest.size());
  std::memcpy(out.data(), digest.data(), digest.size());
  return out;
}

ZHttpDiskCache::Blob ZHttpDiskCache::serializeEntry(const ZHttpGetBytesResult& result)
{
  if (std::endian::native != std::endian::little) {
    return {};
  }

  if (result.status < 0 || result.status > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  if (result.contentType.size() > std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  if (result.contentEncoding.size() > std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  if (result.body.size() > std::numeric_limits<uint64_t>::max()) {
    return {};
  }

  HttpCacheEntryHeader hdr{};
  std::memcpy(hdr.magic.data(), kMagic, sizeof(kMagic));
  hdr.version = kCacheVersion;
  hdr.status = static_cast<uint16_t>(result.status);
  hdr.contentTypeLen = static_cast<uint32_t>(result.contentType.size());
  hdr.contentEncLen = static_cast<uint32_t>(result.contentEncoding.size());
  hdr.bodyLen = static_cast<uint64_t>(result.body.size());

  const size_t headerBytes = compactSize(hdr);
  if (hdr.bodyLen > std::numeric_limits<size_t>::max()) {
    return {};
  }
  const size_t bodyBytes = static_cast<size_t>(hdr.bodyLen);
  size_t totalBytes = headerBytes;
  if (hdr.contentTypeLen > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += static_cast<size_t>(hdr.contentTypeLen);
  if (hdr.contentEncLen > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += static_cast<size_t>(hdr.contentEncLen);
  if (bodyBytes > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += bodyBytes;

  Blob out;
  try {
    out.resize(totalBytes);
  } catch (...) {
    return {};
  }
  if (out.empty()) {
    return {};
  }
  if (compactStructToMemory(out.data(), headerBytes, hdr) != headerBytes) {
    return {};
  }

  size_t offset = headerBytes;
  if (hdr.contentTypeLen > 0) {
    const char* p = result.contentType.data();
    std::memcpy(out.data() + offset, p, hdr.contentTypeLen);
    offset += hdr.contentTypeLen;
  }
  if (hdr.contentEncLen > 0) {
    const char* p = result.contentEncoding.data();
    std::memcpy(out.data() + offset, p, hdr.contentEncLen);
    offset += hdr.contentEncLen;
  }
  if (hdr.bodyLen > 0) {
    std::memcpy(out.data() + offset, result.body.data(), static_cast<size_t>(hdr.bodyLen));
    offset += static_cast<size_t>(hdr.bodyLen);
  }
  if (offset != totalBytes) {
    return {};
  }

  return out;
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::tryGet(const ZHttpGetRequest& request) const
{
  if (!isEnabled()) {
    return std::nullopt;
  }

  const KeyParts parts = keyPartsFrom(request);
  const std::string rangeKey = exactByteRangeKeyString(parts.exactByteRange);

  std::string keyBytes;
  keyBytes.reserve(32 + parts.cachePartition.size() + parts.url.size() + rangeKey.size());
  keyBytes.append("GET\n");
  keyBytes.append("partition=");
  keyBytes.append(parts.cachePartition);
  keyBytes.push_back('\n');
  keyBytes.append(parts.url);
  keyBytes.push_back('\n');
  keyBytes.append("range=");
  keyBytes.append(rangeKey);
  keyBytes.push_back('\n');

  const Blob keyHash = sha256(keyBytes.data(), keyBytes.size());
  if (keyHash.size() != 32) {
    return std::nullopt;
  }

  auto getOpt = m_bucket->tryGetNoTouch(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
  if (!getOpt.has_value()) {
    return std::nullopt;
  }

  auto resOpt = parseEntry(std::span<const std::uint8_t>(getOpt->value.data(), getOpt->value.size()));
  if (!resOpt.has_value()) {
    // Corrupt entry; best-effort cleanup (async).
    m_bucket->tryEnqueueErase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
    return std::nullopt;
  }

  m_bucket->tryEnqueueTouchIfStale(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
                                   getOpt->lastAccessNs,
                                   nowNs(),
                                   atlasDiskCacheTouchMinInterval());

  return resOpt;
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZHttpDiskCache::tryGetAsync(ZHttpGetRequest request) const
{
  if (!isEnabled()) {
    co_return std::nullopt;
  }

  co_return co_await folly::coro::co_withExecutor(
    getAtlasBackgroundExecutor(),
    [this, request = std::move(request)]() mutable -> folly::coro::Task<std::optional<ZHttpGetBytesResult>> {
      co_await folly::coro::co_reschedule_on_current_executor;
      co_return tryGet(request);
    }());
}

void ZHttpDiskCache::put(const ZHttpGetRequest& request, const ZHttpGetBytesResult& result)
{
  if (!isEnabled()) {
    return;
  }
  if (result.status != 200 && result.status != 206) {
    return;
  }
  if (!isCacheableExactRangeResponse(request, result)) {
    return;
  }
  if (static_cast<uint64_t>(result.body.size()) > m_maxBytes) {
    // An entry larger than the entire budget would cause immediate thrash; skip caching.
    return;
  }

  const KeyParts parts = keyPartsFrom(request);
  const std::string rangeKey = exactByteRangeKeyString(parts.exactByteRange);
  std::string keyBytes;
  keyBytes.reserve(32 + parts.cachePartition.size() + parts.url.size() + rangeKey.size());
  keyBytes.append("GET\n");
  keyBytes.append("partition=");
  keyBytes.append(parts.cachePartition);
  keyBytes.push_back('\n');
  keyBytes.append(parts.url);
  keyBytes.push_back('\n');
  keyBytes.append("range=");
  keyBytes.append(rangeKey);
  keyBytes.push_back('\n');

  const Blob keyHash = sha256(keyBytes.data(), keyBytes.size());
  if (keyHash.size() != 32) {
    return;
  }

  const uint64_t estimatedBytes = estimatedSerializedEntryBytes(result);
  if (estimatedBytes == std::numeric_limits<uint64_t>::max()) {
    return;
  }
  if (estimatedBytes > m_maxBytes) {
    // Entry would always be rejected by the underlying cache (value.size() > maxBytes).
    return;
  }

  // Copying the HTTP body can be expensive; avoid copying if the queue is full
  // by deferring the copy into the factory (executed only if accepted).
  m_bucket->tryEnqueueTaskWithFactory(
    std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
    estimatedBytes,
    [&result](const ZSqliteDiskCacheBucket::KeyHash32& keyArr) -> ZSqliteDiskCacheBucket::TaskFn {
      ZHttpGetBytesResult resultCopy = result;
      return [keyArr, result = std::move(resultCopy)](ZSqliteLRUCache& cache) {
        const Blob value = serializeEntry(result);
        if (value.empty()) {
          return;
        }
        cache.put(std::span<const std::uint8_t>(keyArr.data(), keyArr.size()),
                  std::span<const std::uint8_t>(value.data(), value.size()));
      };
    });
}

bool ZHttpDiskCache::drainWrites(std::chrono::milliseconds timeout)
{
  if (!isEnabled()) {
    return false;
  }
  return m_bucket->drainWrites(timeout);
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::parseEntry(std::span<const std::uint8_t> value)
{
  if (std::endian::native != std::endian::little) {
    return std::nullopt;
  }

  static const size_t headerBytes = compactSize(HttpCacheEntryHeader{});
  if (value.size() < headerBytes) {
    return std::nullopt;
  }

  HttpCacheEntryHeader hdr{};
  readStructFromCompactMemory(hdr, value.data(), headerBytes);

  if (std::memcmp(hdr.magic.data(), kMagic, sizeof(kMagic)) != 0) {
    return std::nullopt;
  }
  if (hdr.version != kCacheVersion) {
    return std::nullopt;
  }

  size_t offset = headerBytes;
  if (hdr.contentTypeLen > value.size() - offset) {
    return std::nullopt;
  }
  const std::string_view contentTypeView(reinterpret_cast<const char*>(value.data() + offset), hdr.contentTypeLen);
  offset += hdr.contentTypeLen;
  if (hdr.contentEncLen > value.size() - offset) {
    return std::nullopt;
  }
  const std::string_view contentEncView(reinterpret_cast<const char*>(value.data() + offset), hdr.contentEncLen);
  offset += hdr.contentEncLen;

  if (hdr.bodyLen > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  const size_t bodyLen = static_cast<size_t>(hdr.bodyLen);
  if (bodyLen > value.size() - offset) {
    return std::nullopt;
  }

  try {
    std::vector<uint8_t> body;
    body.resize(bodyLen);
    if (!body.empty()) {
      std::memcpy(body.data(), value.data() + offset, body.size());
    }

    ZHttpGetBytesResult out{};
    out.status = hdr.status;
    out.contentType.assign(contentTypeView.data(), contentTypeView.size());
    out.contentEncoding.assign(contentEncView.data(), contentEncView.size());
    out.body = std::move(body);
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace nim
