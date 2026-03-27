#include "zneuroglancershardedreader.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"

#include <folly/OperationCancelled.h>
#include <folly/compression/Compression.h>

#include <algorithm>
#include <cstring>

namespace nim {
namespace {

std::string toStdString(const QString& s)
{
  const auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

QString shardHexString(uint64_t shard, int digits)
{
  QString s = QString::number(shard, 16);
  if (digits > 0) {
    s = s.rightJustified(digits, QChar('0'));
  }
  return s;
}

std::vector<uint8_t> decompressGzipBytes(std::vector<uint8_t> bytes)
{
  if (bytes.empty()) {
    return bytes;
  }

  auto codec = folly::compression::getCodec(folly::compression::CodecType::GZIP);
  auto uncompressed = codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  std::vector<uint8_t> out(uncompressed.size());
  std::memcpy(out.data(), uncompressed.data(), out.size());
  return out;
}

} // namespace

ZNeuroglancerShardedUrls makeNeuroglancerShardedUrls(const QUrl& baseUrl,
                                                     const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                                     uint64_t shard)
{
  const QString shardHex = shardHexString(shard, sharding.shardHexDigits);
  return {
    .shardUrl = baseUrl.resolved(QUrl(shardHex + ".shard")),
    .indexUrl = baseUrl.resolved(QUrl(shardHex + ".index")),
    .dataUrl = baseUrl.resolved(QUrl(shardHex + ".data")),
  };
}

std::vector<uint8_t> decodeNeuroglancerShardedBytes(std::vector<uint8_t> bytes,
                                                    ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding enc)
{
  switch (enc) {
    case ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Raw:
      return bytes;
    case ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Gzip:
      return decompressGzipBytes(std::move(bytes));
  }
  throw ZException("Invalid sharded data encoding");
}

folly::coro::Task<std::optional<ZNeuroglancerShardedShardIndexEntry>>
getNeuroglancerShardIndexEntryAsync(const ZNeuroglancerRemoteContext& remoteContext,
                                    const QUrl& baseUrl,
                                    const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                    uint64_t shard,
                                    uint64_t minishard,
                                    ZRemoteRangeReadPolicy rangePolicy,
                                    /*nullable*/ ZImgReadStatsSink* statsSink,
                                    ZImgReadStatsContext statsContext)
{
  const ZNeuroglancerShardedUrls urls = makeNeuroglancerShardedUrls(baseUrl, sharding, shard);
  const uint64_t shardIndexEntryOffset = minishard << 4;

  auto readFrom = [&](const QUrl& url) -> folly::coro::Task<std::optional<std::vector<uint8_t>>> {
    co_return co_await remoteContext
      .getRangeBytesAsync(toStdString(url.toString()), shardIndexEntryOffset, 16, rangePolicy, statsSink, statsContext);
  };

  auto parseEntry = [&](std::vector<uint8_t> bytes, bool useSplit) -> ZNeuroglancerShardedShardIndexEntry {
    CHECK(bytes.size() == 16);
    ZNeuroglancerShardedShardIndexEntry out{};
    out.minishardIndexStart = ZNeuroglancerUint64Sharding::readU64LE(bytes.data());
    out.minishardIndexEnd = ZNeuroglancerUint64Sharding::readU64LE(bytes.data() + 8);
    if (useSplit) {
      out.dataUrl = toStdString(urls.dataUrl.toString());
      out.baseDataOffset = 0;
    } else {
      out.dataUrl = toStdString(urls.shardUrl.toString());
      out.baseDataOffset = sharding.shardIndexSize;
    }
    return out;
  };

  const int mode = sharding.shardFileMode.load();
  if (mode == 1) {
    auto bytesOpt = co_await readFrom(urls.shardUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    co_return parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
  }
  if (mode == 2) {
    auto bytesOpt = co_await readFrom(urls.indexUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    co_return parseEntry(std::move(*bytesOpt), /*useSplit=*/true);
  }

  auto bytesOpt = co_await readFrom(urls.shardUrl);
  if (bytesOpt) {
    sharding.shardFileMode.store(1);
    co_return parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
  }

  auto legacyBytesOpt = co_await readFrom(urls.indexUrl);
  if (legacyBytesOpt) {
    sharding.shardFileMode.store(2);
    co_return parseEntry(std::move(*legacyBytesOpt), /*useSplit=*/true);
  }

  co_return std::nullopt;
}

folly::coro::Task<std::optional<ZNeuroglancerUint64Sharding::DecodedMinishardIndex>>
getNeuroglancerDecodedMinishardIndexAsync(const ZNeuroglancerRemoteContext& remoteContext,
                                          const ZNeuroglancerShardedShardIndexEntry& entry,
                                          const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                          ZRemoteRangeReadPolicy rangePolicy,
                                          /*nullable*/ ZImgReadStatsSink* statsSink,
                                          ZImgReadStatsContext statsContext)
{
  if (entry.minishardIndexEnd < entry.minishardIndexStart) {
    // This comes from remote shard metadata, not an internal invariant. Treat a negative-length minishard slice as
    // a corrupt dataset error instead of a soft miss so callers do not silently reinterpret malformed sharding
    // metadata as "object not found".
    throw ZException("Invalid shard index entry: end offset < start offset");
  }
  if (entry.minishardIndexEnd == entry.minishardIndexStart) {
    co_return ZNeuroglancerUint64Sharding::DecodedMinishardIndex{};
  }

  const uint64_t absStart = entry.baseDataOffset + entry.minishardIndexStart;
  const uint64_t absEnd = entry.baseDataOffset + entry.minishardIndexEnd;
  CHECK(absEnd >= absStart);

  auto bytesOpt = co_await remoteContext.getRangeBytesAsync(entry.dataUrl,
                                                            absStart,
                                                            absEnd - absStart,
                                                            rangePolicy,
                                                            statsSink,
                                                            statsContext);
  if (!bytesOpt) {
    co_return std::nullopt;
  }

  auto decodedBytes = decodeNeuroglancerShardedBytes(std::move(*bytesOpt), sharding.minishardIndexEncoding);
  co_return ZNeuroglancerUint64Sharding::decodeMinishardIndex(
    std::span<const uint8_t>(decodedBytes.data(), decodedBytes.size()),
    entry.baseDataOffset);
}

std::optional<ZNeuroglancerShardedPayloadLocation>
findNeuroglancerShardedPayloadLocation(const ZNeuroglancerUint64Sharding::DecodedMinishardIndex& decodedIndex,
                                       uint64_t key)
{
  const auto it = std::lower_bound(decodedIndex.keys.begin(), decodedIndex.keys.end(), key);
  if (it == decodedIndex.keys.end() || *it != key) {
    return std::nullopt;
  }

  const size_t idx = static_cast<size_t>(it - decodedIndex.keys.begin());
  CHECK(idx < decodedIndex.starts.size());
  CHECK(idx < decodedIndex.ends.size());

  const uint64_t start = decodedIndex.starts[idx];
  const uint64_t end = decodedIndex.ends[idx];
  if (end <= start) {
    return std::nullopt;
  }

  return ZNeuroglancerShardedPayloadLocation{.start = start, .end = end};
}

folly::coro::Task<std::optional<ZNeuroglancerShardedPayloadBytes>>
getNeuroglancerDecodedShardedPayloadBytesAsync(const ZNeuroglancerRemoteContext& remoteContext,
                                               const std::string& dataUrl,
                                               const ZNeuroglancerShardedPayloadLocation& location,
                                               const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                               ZRemoteRangeReadPolicy rangePolicy,
                                               /*nullable*/ ZImgReadStatsSink* statsSink,
                                               ZImgReadStatsContext statsContext)
{
  CHECK(location.end >= location.start);
  const uint64_t payloadLength = location.end - location.start;

  std::optional<std::vector<uint8_t>> bytesOpt;
  try {
    bytesOpt = co_await remoteContext
                 .getRangeBytesAsync(dataUrl, location.start, payloadLength, rangePolicy, statsSink, statsContext);
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    throw ZException(
      fmt::format("Failed to read Neuroglancer sharded payload from '{}' [start={}, end={}, length={}]: {}",
                  dataUrl,
                  location.start,
                  location.end,
                  payloadLength,
                  e.what()));
  }
  if (!bytesOpt) {
    co_return std::nullopt;
  }

  ZNeuroglancerShardedPayloadBytes out;
  out.bytes = decodeNeuroglancerShardedBytes(std::move(*bytesOpt), sharding.dataEncoding);
  out.start = location.start;
  co_return out;
}

} // namespace nim
