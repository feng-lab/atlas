#pragma once

#include "zhttpclient.h"

#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nim {

class ZSqliteDiskCacheBucket;

// Simple persistent on-disk cache for HTTP GET results (SQLite-backed).
//
// Notes:
// - This is intentionally a *byte cache* keyed by (URL + exact byte range) and stores the already-decoded
//   result bytes returned to callers (i.e. after any HTTP-level Content-Encoding has been handled).
// - Eviction is best-effort LRU-ish based on access timestamps (touched on read).
// - Multi-process: multiple Atlas processes may concurrently read/write the same cache DB.
//   Writes are best-effort and may be dropped under SQLite contention.
class ZHttpDiskCache
{
public:
  using Blob = std::vector<std::uint8_t>;

  struct KeyParts
  {
    std::string url;
    std::optional<ZHttpByteRange> exactByteRange;
    std::string cachePartition;
  };

  ZHttpDiskCache(QString rootDir, uint64_t maxBytes);
  ~ZHttpDiskCache();

  [[nodiscard]] bool isEnabled() const;

  [[nodiscard]] uint64_t maxBytes() const
  {
    return m_maxBytes;
  }

  // Returns cached bytes for the request if present (otherwise std::nullopt).
  [[nodiscard]] std::optional<ZHttpGetBytesResult> tryGet(const ZHttpGetRequest& request) const;

  // Async wrapper around tryGet() that offloads synchronous SQLite reads onto a
  // CPU executor. This prevents callers like the Proxygen EventBase thread from
  // serializing all concurrent requests on disk-cache hits.
  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>> tryGetAsync(ZHttpGetRequest request) const;

  // Stores a successful response in the cache. No-op if disabled.
  void put(const ZHttpGetRequest& request, const ZHttpGetBytesResult& result);

  // Waits until all queued async writes complete (or timeout). Intended for tests.
  [[nodiscard]] bool drainWrites(std::chrono::milliseconds timeout = std::chrono::seconds(5));

private:
  [[nodiscard]] static KeyParts keyPartsFrom(const ZHttpGetRequest& request);

  [[nodiscard]] static Blob sha256(const void* data, size_t bytes);

  [[nodiscard]] static Blob serializeEntry(const ZHttpGetBytesResult& result);

  [[nodiscard]] static std::optional<ZHttpGetBytesResult> parseEntry(std::span<const std::uint8_t> value);

private:
  QString m_rootDir;

  uint64_t m_maxBytes = 0;

  std::unique_ptr<ZSqliteDiskCacheBucket> m_bucket;
};

} // namespace nim
