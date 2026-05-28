#pragma once

#include "zneuroglancerremotecontext.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglanceruint64sharding.h"
#include "zfolly.h"

#include <QUrl>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nim {

struct ZNeuroglancerShardedUrls
{
  QUrl shardUrl;
  QUrl indexUrl;
  QUrl dataUrl;
};

struct ZNeuroglancerShardedShardIndexEntry
{
  std::string dataUrl;
  uint64_t baseDataOffset = 0;
  uint64_t minishardIndexStart = 0;
  uint64_t minishardIndexEnd = 0;
};

struct ZNeuroglancerShardedPayloadLocation
{
  uint64_t start = 0;
  uint64_t end = 0;
};

struct ZNeuroglancerShardedPayloadBytes
{
  std::vector<uint8_t> bytes;
  uint64_t start = 0;
};

[[nodiscard]] ZNeuroglancerShardedUrls
makeNeuroglancerShardedUrls(const QUrl& baseUrl,
                            const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                            uint64_t shard);

[[nodiscard]] std::vector<uint8_t>
decodeNeuroglancerShardedBytes(std::vector<uint8_t> bytes,
                               ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding enc);

[[nodiscard]] folly::coro::Task<std::optional<ZNeuroglancerShardedShardIndexEntry>>
getNeuroglancerShardIndexEntryAsync(const ZNeuroglancerRemoteContext& remoteContext,
                                    const QUrl& baseUrl,
                                    const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                    uint64_t shard,
                                    uint64_t minishard,
                                    ZRemoteRangeReadPolicy rangePolicy = ZRemoteRangeReadPolicy::RequireExactLength,
                                    /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                                    ZImgReadStatsContext statsContext = {});

[[nodiscard]] folly::coro::Task<std::optional<ZNeuroglancerUint64Sharding::DecodedMinishardIndex>>
getNeuroglancerDecodedMinishardIndexAsync(
  const ZNeuroglancerRemoteContext& remoteContext,
  const ZNeuroglancerShardedShardIndexEntry& entry,
  const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
  ZRemoteRangeReadPolicy rangePolicy = ZRemoteRangeReadPolicy::RequireExactLength,
  /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
  ZImgReadStatsContext statsContext = {});

[[nodiscard]] std::optional<ZNeuroglancerShardedPayloadLocation>
findNeuroglancerShardedPayloadLocation(const ZNeuroglancerUint64Sharding::DecodedMinishardIndex& decodedIndex,
                                       uint64_t key);

[[nodiscard]] folly::coro::Task<std::optional<ZNeuroglancerShardedPayloadBytes>>
getNeuroglancerDecodedShardedPayloadBytesAsync(
  const ZNeuroglancerRemoteContext& remoteContext,
  const std::string& dataUrl,
  const ZNeuroglancerShardedPayloadLocation& location,
  const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
  ZRemoteRangeReadPolicy rangePolicy = ZRemoteRangeReadPolicy::RequireExactLength,
  /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
  ZImgReadStatsContext statsContext = {});

} // namespace nim
