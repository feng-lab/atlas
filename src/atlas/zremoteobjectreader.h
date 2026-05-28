#pragma once

#include "zremoteobjectstore.h"
#include "zimgreadstats.h"
#include "zfolly.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

enum class ZRemoteRangeReadPolicy
{
  RequireExactLength,
  AllowFullResponseSlice,
};

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
getRemoteObjectResponseAsync(const ZRemoteObjectStore& objectStore,
                             ZHttpGetRequest request,
                             /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                             ZImgReadStatsContext statsContext = {});

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
getRemoteObjectResponseAsync(ZHttpGetRequest request,
                             /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                             ZImgReadStatsContext statsContext = {});

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectBytesAsync(const ZRemoteObjectStore& objectStore,
                          std::string url,
                          std::chrono::milliseconds timeout,
                          /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                          ZImgReadStatsContext statsContext = {});

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectBytesAsync(std::string url,
                          std::chrono::milliseconds timeout,
                          /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                          ZImgReadStatsContext statsContext = {});

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectRangeBytesAsync(const ZRemoteObjectStore& objectStore,
                               std::string url,
                               std::chrono::milliseconds timeout,
                               uint64_t offset,
                               uint64_t length,
                               ZRemoteRangeReadPolicy policy = ZRemoteRangeReadPolicy::RequireExactLength,
                               /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                               ZImgReadStatsContext statsContext = {});

folly::coro::Task<std::optional<std::vector<uint8_t>>>
getRemoteObjectRangeBytesAsync(std::string url,
                               std::chrono::milliseconds timeout,
                               uint64_t offset,
                               uint64_t length,
                               ZRemoteRangeReadPolicy policy = ZRemoteRangeReadPolicy::RequireExactLength,
                               /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                               ZImgReadStatsContext statsContext = {});

} // namespace nim
