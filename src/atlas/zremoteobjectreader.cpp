#include "zremoteobjectreader.h"

#include "zexception.h"
#include "zhttpretrypolicy.h"
#include "zlog.h"

#include <gflags/gflags.h>

#include <folly/coro/Sleep.h>

#include <cstring>

DECLARE_uint32(atlas_http_max_retries);

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

[[nodiscard]] std::optional<std::vector<uint8_t>>
sliceAllowedFullResponse(const ZHttpGetBytesResult& result, uint64_t offset, uint64_t length)
{
  const uint64_t bodySize = static_cast<uint64_t>(result.body.size());
  if (offset > bodySize || length > bodySize - offset) {
    return std::nullopt;
  }

  std::vector<uint8_t> out(static_cast<size_t>(length));
  std::memcpy(out.data(), result.body.data() + static_cast<size_t>(offset), static_cast<size_t>(length));
  return out;
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
  const std::vector<std::pair<std::string, std::string>> requestHeaders = {
    {"range", fmt::format("bytes={}-{}", offset, endInclusive)}
  };

  const uint32_t maxRetries = FLAGS_atlas_http_max_retries;
  for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
    auto resOpt =
      co_await getRemoteObjectResponseAsync(objectStore, url, timeout, requestHeaders, statsSink, statsContext);
    if (!resOpt) {
      co_return std::nullopt;
    }

    if (resOpt->status != 206 && resOpt->status != 200) {
      throw ZException(fmt::format("HTTP range GET failed for '{}' (status {})", url, resOpt->status));
    }

    if (resOpt->status == 206) {
      if (resOpt->body.size() == length) {
        co_return std::move(resOpt->body);
      }
    } else if (policy == ZRemoteRangeReadPolicy::AllowFullResponseSlice) {
      if (auto sliced = sliceAllowedFullResponse(*resOpt, offset, length)) {
        co_return sliced;
      }
    } else {
      throw ZException(fmt::format("HTTP range GET unexpectedly returned 200 for strict range request '{}'", url));
    }

    if (attempt < maxRetries) {
      VLOG(1) << fmt::format("HTTP range GET size mismatch (attempt {}/{}): '{}' got {} bytes, expected {}; retrying",
                             attempt + 1,
                             maxRetries + 1,
                             url,
                             resOpt->body.size(),
                             length);
      co_await folly::coro::sleepReturnEarlyOnCancel(httpRetryBackoffForAttempt(attempt));
      continue;
    }

    throw ZException(fmt::format("HTTP range GET size mismatch for '{}': got {} bytes, expected {} bytes",
                                 url,
                                 resOpt->body.size(),
                                 length));
  }

  throw ZException(fmt::format("HTTP range GET failed for '{}': exhausted retries", url));
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
