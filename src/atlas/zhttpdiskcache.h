#pragma once

#include <QByteArray>
#include <QLockFile>
#include <QString>

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

struct ZHttpGetBytesResult;

// Simple persistent on-disk cache for HTTP GET results.
//
// Notes:
// - This is intentionally a *byte cache* keyed by (URL + Range) and stores the already-decoded
//   result bytes returned to callers (i.e. after any HTTP-level Content-Encoding has been handled).
// - Eviction is best-effort LRU-ish based on file timestamps (mtime touched on read).
// - Multi-process safety: the cache directory is guarded by a lock file; if it cannot be acquired,
//   the cache is disabled for that process.
class ZHttpDiskCache
{
public:
  struct KeyParts
  {
    std::string url;
    std::string range; // empty for non-Range requests
  };

  ZHttpDiskCache(QString rootDir, uint64_t maxBytes);

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

  [[nodiscard]] static QByteArray sha256Hex(const QByteArray& bytes);

  [[nodiscard]] QString entryPathForKeyHash(const QByteArray& hexHash) const;

  [[nodiscard]] std::optional<ZHttpGetBytesResult> readEntryFile(const QString& path);

  void writeEntryFile(const QString& path, const ZHttpGetBytesResult& result);

  void updateTotalBytesFromDisk();

  void maybePrune();

private:
  QString m_rootDir;
  QString m_entriesDir;
  QString m_lockPath;

  uint64_t m_maxBytes = 0;
  uint64_t m_totalBytes = 0;

  bool m_enabled = false;
  bool m_sizeKnown = false;

  std::unique_ptr<QLockFile> m_lock;
  std::mutex m_mu;
  std::chrono::steady_clock::time_point m_lastPrune{};
};

} // namespace nim
