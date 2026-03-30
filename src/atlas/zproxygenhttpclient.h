#pragma once

#include "zhttpclient.h"
#include "zlog.h"

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

class QObject;
class QThread;

namespace folly {
class SSLContext;
}

namespace nim {

class ZHttpDiskCache;

class ZProxygenHttpClient
{
public:
  static ZProxygenHttpClient& instance();

  // Returns std::nullopt for missing resources. For Neuroglancer parity, both
  // HTTP 403 and 404 are normalized to "not found" because some object stores
  // report absent objects as 403.
  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request);

private:
  ZProxygenHttpClient();
  ~ZProxygenHttpClient();

  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytesOnEventBase(ZHttpGetRequest request);

private:
  struct DirectDnsCacheEntry
  {
    std::vector<std::string> addresses;
    size_t nextIndex = 0;
  };

  folly::ScopedEventBaseThread m_eventBaseThread;
  std::shared_ptr<const folly::SSLContext> m_sslContext;
  std::string m_caBundlePath;
  std::string m_trustSourceDescription;

  std::unique_ptr<ZHttpDiskCache> m_diskCache;

  std::unique_ptr<proxygen::coro::HTTPClientConnectionCache> m_directConnCache;
  std::unordered_map<std::string, std::unique_ptr<proxygen::coro::HTTPClientConnectionCache>> m_proxyConnCaches;
  std::unordered_map<std::string, DirectDnsCacheEntry> m_directDnsCache;

  std::unique_ptr<::QThread> m_hostLookupThread;
  ::QObject* m_hostLookupInvoker = nullptr;
};

} // namespace nim
