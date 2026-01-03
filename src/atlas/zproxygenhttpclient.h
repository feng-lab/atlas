#pragma once

#include "zlog.h"

#include <folly/coro/Task.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <proxygen/lib/http/coro/client/HTTPClientConnectionCache.h>

#include <chrono>
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

struct ZHttpGetBytesResult
{
  uint16_t status = 0;
  std::string contentType;
  std::string contentEncoding;
  std::vector<uint8_t> body;
};

class ZProxygenHttpClient
{
public:
  static ZProxygenHttpClient& instance();

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
  folly::ScopedEventBaseThread m_eventBaseThread;
  std::shared_ptr<const folly::SSLContext> m_sslContext;
  std::string m_caBundlePath;

  std::unique_ptr<proxygen::coro::HTTPClientConnectionCache> m_directConnCache;
  std::unordered_map<std::string, std::unique_ptr<proxygen::coro::HTTPClientConnectionCache>> m_proxyConnCaches;
};

} // namespace nim
