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

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZHttpRemoteObjectStore::getBytes(ZHttpGetRequest request) const
{
  co_return co_await ZHttpClient::instance().getBytes(std::move(request));
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
