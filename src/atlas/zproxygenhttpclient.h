#pragma once

#include "zlog.h"

#include <folly/coro/Task.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <proxygen/lib/http/coro/client/HTTPClientConnectionCache.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace folly {
class SSLContext;
}

namespace nim {

class ZHttpDiskCache;

enum class ZHttpGetBytesSource : std::uint8_t
{
  Unknown = 0,
  Network = 1,
  DiskCache = 2,
};

struct ZHttpGetBytesResult
{
  uint16_t status = 0;
  std::string contentType;
  std::string contentEncoding;
  std::vector<uint8_t> body;

  // Size (in bytes) of the HTTP response body as received over the wire, before
  // applying Content-Encoding decompression. For disk-cache hits, this is 0
  // because no network I/O was performed.
  uint64_t encodedBodyBytes = 0;

  // Where the bytes were served from. This is primarily intended for telemetry
  // and debugging; core logic should use HTTP status codes and content fields.
  ZHttpGetBytesSource source = ZHttpGetBytesSource::Unknown;
};

class ZProxygenHttpClient
{
public:
  static ZProxygenHttpClient& instance();

  // Returns std::nullopt for missing resources. For Neuroglancer parity, both
  // HTTP 403 and 404 are normalized to "not found" because some object stores
  // report absent objects as 403.
  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(std::string url,
                                                                 std::chrono::milliseconds timeout,
                                                                 std::vector<std::pair<std::string, std::string>> requestHeaders = {});

private:
  ZProxygenHttpClient();

  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytesOnEventBase(
    std::string url,
    std::chrono::milliseconds timeout,
    std::vector<std::pair<std::string, std::string>> requestHeaders);

private:
  struct DirectDnsCacheEntry
  {
    std::vector<std::string> addresses;
    size_t nextIndex = 0;
  };

  folly::ScopedEventBaseThread m_eventBaseThread;
  std::shared_ptr<const folly::SSLContext> m_sslContext;
  std::string m_caBundlePath;

  std::unique_ptr<ZHttpDiskCache> m_diskCache;

  std::unique_ptr<proxygen::coro::HTTPClientConnectionCache> m_directConnCache;
  std::unordered_map<std::string, std::unique_ptr<proxygen::coro::HTTPClientConnectionCache>> m_proxyConnCaches;
  std::unordered_map<std::string, DirectDnsCacheEntry> m_directDnsCache;
};

} // namespace nim
