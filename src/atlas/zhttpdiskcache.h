#pragma once

#include <QLockFile>
#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nim {

struct ZHttpGetBytesResult;
class ZSqliteLRUCache;

// Simple persistent on-disk cache for HTTP GET results (SQLite-backed).
//
// Notes:
// - This is intentionally a *byte cache* keyed by (URL + Range) and stores the already-decoded
//   result bytes returned to callers (i.e. after any HTTP-level Content-Encoding has been handled).
// - Eviction is best-effort LRU-ish based on access timestamps (touched on read).
// - Multi-process safety: the cache directory is guarded by a lock file; if it cannot be acquired,
//   the cache is disabled for that process. (We keep a single-writer policy so size tracking is consistent.)
class ZHttpDiskCache
{
public:
  using Blob = std::vector<std::uint8_t>;

  struct KeyParts
  {
    std::string url;
    std::string range; // empty for non-Range requests
  };

  ZHttpDiskCache(QString rootDir, uint64_t maxBytes);
  ~ZHttpDiskCache();

  [[nodiscard]] bool isEnabled() const
  {
    return m_enabled;
  }

  [[nodiscard]] uint64_t maxBytes() const
  {
    return m_maxBytes;
  }

  // Returns cached bytes for the request if present (otherwise std::nullopt).
  [[nodiscard]] std::optional<ZHttpGetBytesResult> tryGet(const std::string& url,
                                                          const std::vector<std::pair<std::string, std::string>>& requestHeaders);

  // Stores a successful response in the cache. No-op if disabled.
  void put(const std::string& url,
           const std::vector<std::pair<std::string, std::string>>& requestHeaders,
           const ZHttpGetBytesResult& result);

private:
  [[nodiscard]] static KeyParts keyPartsFrom(const std::string& url,
                                            const std::vector<std::pair<std::string, std::string>>& requestHeaders);

  [[nodiscard]] static Blob sha256(const void* data, size_t bytes);

  [[nodiscard]] static Blob serializeEntry(const ZHttpGetBytesResult& result);

  [[nodiscard]] static std::optional<ZHttpGetBytesResult> parseEntry(std::span<const std::uint8_t> value);

private:
  QString m_rootDir;
  QString m_cacheDir;
  QString m_dbPath;
  QString m_lockPath;

  uint64_t m_maxBytes = 0;

  bool m_enabled = false;

  std::unique_ptr<QLockFile> m_lock;
  std::unique_ptr<ZSqliteLRUCache> m_cache;
  std::mutex m_mu;
};

} // namespace nim
