#include "zremoteobjectstore.h"

namespace nim {

const ZRemoteObjectStore& ZRemoteObjectStore::defaultStore()
{
  return *sharedDefaultStore();
}

std::shared_ptr<const ZRemoteObjectStore> ZRemoteObjectStore::sharedDefaultStore()
{
  return ZHttpRemoteObjectStore::sharedInstance();
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
ZHttpRemoteObjectStore::getBytes(std::string url,
                                 std::chrono::milliseconds timeout,
                                 std::vector<std::pair<std::string, std::string>> requestHeaders) const
{
  co_return co_await ZHttpClient::instance().getBytes(std::move(url), timeout, std::move(requestHeaders));
}

const void* ZHttpRemoteObjectStore::contentCacheScopeToken() const
{
  return sharedInstance().get();
}

std::shared_ptr<const ZRemoteObjectStore> ZHttpRemoteObjectStore::sharedInstance()
{
  static const std::shared_ptr<const ZRemoteObjectStore> instance = std::make_shared<ZHttpRemoteObjectStore>();
  return instance;
}

} // namespace nim
