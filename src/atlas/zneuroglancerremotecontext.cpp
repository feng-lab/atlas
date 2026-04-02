#include "zneuroglancerremotecontext.h"

namespace nim {

ZNeuroglancerRemoteContext::ZNeuroglancerRemoteContext(std::shared_ptr<const ZRemoteObjectStore> objectStore,
                                                       std::chrono::milliseconds timeout)
  : m_objectStore(objectStore ? std::move(objectStore) : ZRemoteObjectStore::sharedDefaultStore())
  , m_timeout(timeout.count() > 0 ? timeout : std::chrono::milliseconds{30000})
{}

std::shared_ptr<const ZNeuroglancerRemoteContext>
ZNeuroglancerRemoteContext::create(std::chrono::milliseconds timeout,
                                   std::shared_ptr<const ZRemoteObjectStore> objectStore)
{
  return std::make_shared<const ZNeuroglancerRemoteContext>(std::move(objectStore), timeout);
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
ZNeuroglancerRemoteContext::getResponseAsync(ZHttpGetRequest request,
                                             /*nullable*/ ZImgReadStatsSink* statsSink,
                                             ZImgReadStatsContext statsContext) const
{
  request.timeout = m_timeout;
  co_return co_await getRemoteObjectResponseAsync(objectStore(), std::move(request), statsSink, statsContext);
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
ZNeuroglancerRemoteContext::getResponseAsync(std::string url,
                                             /*nullable*/ ZImgReadStatsSink* statsSink,
                                             ZImgReadStatsContext statsContext) const
{
  co_return co_await getResponseAsync(
    ZHttpGetRequest{
      .url = std::move(url),
      .timeout = std::chrono::milliseconds{0},
      .headers = {},
      .exactByteRange = std::nullopt,
    },
    statsSink,
    statsContext);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
ZNeuroglancerRemoteContext::getBytesAsync(std::string url,
                                          /*nullable*/ ZImgReadStatsSink* statsSink,
                                          ZImgReadStatsContext statsContext) const
{
  co_return co_await getRemoteObjectBytesAsync(objectStore(), std::move(url), m_timeout, statsSink, statsContext);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
ZNeuroglancerRemoteContext::getRangeBytesAsync(std::string url,
                                               uint64_t offset,
                                               uint64_t length,
                                               ZRemoteRangeReadPolicy policy,
                                               /*nullable*/ ZImgReadStatsSink* statsSink,
                                               ZImgReadStatsContext statsContext) const
{
  co_return co_await getRemoteObjectRangeBytesAsync(objectStore(),
                                                    std::move(url),
                                                    m_timeout,
                                                    offset,
                                                    length,
                                                    policy,
                                                    statsSink,
                                                    statsContext);
}

} // namespace nim
