#include "zremoteobjectreader.h"

#include "zexception.h"

#include <cstring>

namespace nim {
namespace {

void recordUnderlyingIoStats(const ZHttpGetBytesResult& result,
                             /*nullable*/ ZImgReadStatsSink* statsSink,
                             ZImgReadStatsContext statsContext)
{
  if (statsSink == nullptr) {
    return;
  }

  switch (result.source) {
    case ZHttpGetBytesSource::Network:
      statsSink->onUnderlyingIoBytes(statsContext, ZImgUnderlyingIoKind::Network, result.encodedBodyBytes);
      break;
    case ZHttpGetBytesSource::DiskCache:
      statsSink->onUnderlyingIoBytes(statsContext,
                                     ZImgUnderlyingIoKind::HttpDiskCache,
                                     static_cast<uint64_t>(result.body.size()));
      break;
    case ZHttpGetBytesSource::Unknown:
      break;
  }
}

} // namespace

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
getRemoteObjectResponseAsync(const ZRemoteObjectStore& objectStore,
                             std::string url,
                             std::chrono::milliseconds timeout,
                             std::vector<std::pair<std::string, std::string>> requestHeaders,
                             /*nullable*/ ZImgReadStatsSink* statsSink,
                             ZImgReadStatsContext statsContext)
{
  auto resOpt = co_await objectStore.getBytes(std::move(url), timeout, std::move(requestHeaders));
  if (!resOpt) {
    co_return std::nullopt;
  }

  recordUnderlyingIoStats(*resOpt, statsSink, statsContext);
  co_return std::move(*resOpt);
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
getRemoteObjectResponseAsync(std::string url,
                             std::chrono::milliseconds timeout,
                             std::vector<std::pair<std::string, std::string>> requestHeaders,
                             /*nullable*/ ZImgReadStatsSink* statsSink,
                             ZImgReadStatsContext statsContext)
{
  co_return co_await getRemoteObjectResponseAsync(ZRemoteObjectStore::defaultStore(),
                                                  std::move(url),
                                                  timeout,
                                                  std::move(requestHeaders),
                                                  statsSink,
                                                  statsContext);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectBytesAsync(const ZRemoteObjectStore& objectStore,
                          std::string url,
                          std::chrono::milliseconds timeout,
                          /*nullable*/ ZImgReadStatsSink* statsSink,
                          ZImgReadStatsContext statsContext)
{
  auto resOpt = co_await getRemoteObjectResponseAsync(objectStore, url, timeout, {}, statsSink, statsContext);
  if (!resOpt) {
    co_return std::nullopt;
  }
  if (resOpt->status != 200) {
    throw ZException(fmt::format("HTTP GET failed for '{}' (status {})", url, resOpt->status));
  }

  co_return std::move(resOpt->body);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectBytesAsync(std::string url,
                          std::chrono::milliseconds timeout,
                          /*nullable*/ ZImgReadStatsSink* statsSink,
                          ZImgReadStatsContext statsContext)
{
  co_return co_await getRemoteObjectBytesAsync(ZRemoteObjectStore::defaultStore(),
                                               std::move(url),
                                               timeout,
                                               statsSink,
                                               statsContext);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectRangeBytesAsync(const ZRemoteObjectStore& objectStore,
                               std::string url,
                               std::chrono::milliseconds timeout,
                               uint64_t offset,
                               uint64_t length,
                               ZRemoteRangeReadPolicy policy,
                               /*nullable*/ ZImgReadStatsSink* statsSink,
                               ZImgReadStatsContext statsContext)
{
  if (length == 0) {
    co_return std::vector<uint8_t>{};
  }

  const uint64_t endInclusive = offset + length - 1;
  auto resOpt = co_await getRemoteObjectResponseAsync(objectStore,
                                                      url,
                                                      timeout,
                                                      {
                                                        {"range", fmt::format("bytes={}-{}", offset, endInclusive)}
  },
                                                      statsSink,
                                                      statsContext);
  if (!resOpt) {
    co_return std::nullopt;
  }

  if (resOpt->status != 206 && resOpt->status != 200) {
    throw ZException(fmt::format("HTTP range GET failed for '{}' (status {})", url, resOpt->status));
  }

  if (resOpt->body.size() == length) {
    co_return std::move(resOpt->body);
  }

  if (policy == ZRemoteRangeReadPolicy::AllowFullResponseSlice && resOpt->status == 200) {
    const uint64_t bodySize = static_cast<uint64_t>(resOpt->body.size());
    if (offset <= bodySize && length <= bodySize - offset) {
      std::vector<uint8_t> out(static_cast<size_t>(length));
      std::memcpy(out.data(), resOpt->body.data() + static_cast<size_t>(offset), static_cast<size_t>(length));
      co_return out;
    }
  }

  throw ZException(fmt::format("HTTP range GET size mismatch for '{}': got {} bytes, expected {} bytes",
                               url,
                               resOpt->body.size(),
                               length));
}

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectRangeBytesAsync(std::string url,
                               std::chrono::milliseconds timeout,
                               uint64_t offset,
                               uint64_t length,
                               ZRemoteRangeReadPolicy policy,
                               /*nullable*/ ZImgReadStatsSink* statsSink,
                               ZImgReadStatsContext statsContext)
{
  co_return co_await getRemoteObjectRangeBytesAsync(ZRemoteObjectStore::defaultStore(),
                                                    std::move(url),
                                                    timeout,
                                                    offset,
                                                    length,
                                                    policy,
                                                    statsSink,
                                                    statsContext);
}

} // namespace nim
