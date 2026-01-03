#pragma once

#include "zimg.h"

#include <folly/coro/Task.h>

#include <QPoint>
#include <QUrl>
#include <QRectF>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nim {

class ZNeuroglancerPrecomputedVolume
{
public:
  enum class Slice2DRatioPolicy
  {
    // Choose a pyramid level whose effective on-screen resolution is closest to 1:1
    // for the given render scale.
    BestForScale,

    // Always choose the coarsest available XY pyramid level (largest downsample ratio),
    // preferring the smallest Z downsample when multiple levels share the same XY ratio.
    // This is intended for fast "fill the holes" preview rendering during interaction.
    CoarsestXY,
  };

  struct Scale
  {
    enum class ChunkEncoding
    {
      Raw,
      Jpeg,
      Png,
      Compresso,
      CompressedSegmentation,
    };

    struct Sharding
    {
      enum class Hash
      {
        Identity,
        MurmurHash3X86_128,
      };

      enum class DataEncoding
      {
        Raw,
        Gzip,
      };

      size_t preshiftBits = 0;
      Hash hash = Hash::Identity;
      size_t minishardBits = 0;
      size_t shardBits = 0;
      DataEncoding minishardIndexEncoding = DataEncoding::Raw;
      DataEncoding dataEncoding = DataEncoding::Raw;

      uint64_t shardIndexSize = 0;
      uint64_t minishardMask = 0;
      uint64_t shardMask = 0;
      uint64_t shardAndMinishardMask = 0;
      int shardHexDigits = 0;

      // 0: unknown, 1: <shard>.shard, 2: legacy <shard>.index + <shard>.data
      mutable std::atomic<int> shardFileMode{0};

      Sharding() = default;

      Sharding(const Sharding& other)
        : preshiftBits(other.preshiftBits)
        , hash(other.hash)
        , minishardBits(other.minishardBits)
        , shardBits(other.shardBits)
        , minishardIndexEncoding(other.minishardIndexEncoding)
        , dataEncoding(other.dataEncoding)
        , shardIndexSize(other.shardIndexSize)
        , minishardMask(other.minishardMask)
        , shardMask(other.shardMask)
        , shardAndMinishardMask(other.shardAndMinishardMask)
        , shardHexDigits(other.shardHexDigits)
        , shardFileMode(other.shardFileMode.load())
      {}

      Sharding& operator=(const Sharding& other)
      {
        if (this == &other) {
          return *this;
        }
        preshiftBits = other.preshiftBits;
        hash = other.hash;
        minishardBits = other.minishardBits;
        shardBits = other.shardBits;
        minishardIndexEncoding = other.minishardIndexEncoding;
        dataEncoding = other.dataEncoding;
        shardIndexSize = other.shardIndexSize;
        minishardMask = other.minishardMask;
        shardMask = other.shardMask;
        shardAndMinishardMask = other.shardAndMinishardMask;
        shardHexDigits = other.shardHexDigits;
        shardFileMode.store(other.shardFileMode.load());
        return *this;
      }

      Sharding(Sharding&& other) noexcept
        : preshiftBits(other.preshiftBits)
        , hash(other.hash)
        , minishardBits(other.minishardBits)
        , shardBits(other.shardBits)
        , minishardIndexEncoding(other.minishardIndexEncoding)
        , dataEncoding(other.dataEncoding)
        , shardIndexSize(other.shardIndexSize)
        , minishardMask(other.minishardMask)
        , shardMask(other.shardMask)
        , shardAndMinishardMask(other.shardAndMinishardMask)
        , shardHexDigits(other.shardHexDigits)
        , shardFileMode(other.shardFileMode.load())
      {}

      Sharding& operator=(Sharding&& other) noexcept
      {
        if (this == &other) {
          return *this;
        }
        preshiftBits = other.preshiftBits;
        hash = other.hash;
        minishardBits = other.minishardBits;
        shardBits = other.shardBits;
        minishardIndexEncoding = other.minishardIndexEncoding;
        dataEncoding = other.dataEncoding;
        shardIndexSize = other.shardIndexSize;
        minishardMask = other.minishardMask;
        shardMask = other.shardMask;
        shardAndMinishardMask = other.shardAndMinishardMask;
        shardHexDigits = other.shardHexDigits;
        shardFileMode.store(other.shardFileMode.load());
        return *this;
      }
    };

    QString key;
    QUrl url;
    std::array<double, 3> resolutionNm{};
    std::array<int64_t, 3> voxelOffset{};
    std::array<int64_t, 3> size{};
    std::vector<std::array<int64_t, 3>> chunkSizes;
    std::array<int64_t, 3> chunkSize{};
    std::array<uint64_t, 3> chunkGridSize{};
    std::array<size_t, 3> ratioToBase{};
    QString encoding;
    ChunkEncoding chunkEncoding = ChunkEncoding::Raw;
    std::optional<std::array<int64_t, 3>> compressedSegmentationBlockSize;
    bool hidden = false;
    std::optional<Sharding> sharding;
  };

  struct Chunk
  {
    size_t scaleIndex = 0;
    std::array<int64_t, 3> globalStart{};
    std::array<int64_t, 3> globalEnd{};
    std::array<int64_t, 3> baseStart{};
    std::array<int64_t, 3> baseEnd{};
  };

  struct SliceTilePack
  {
    std::vector<std::shared_ptr<ZImg>> imgs;
    std::vector<QPoint> locs;
    std::vector<double> scales;
    std::array<size_t, 3> targetRatio{1, 1, 1};
  };

  struct SliceChunkRequests
  {
    std::vector<Chunk> chunks;
    std::array<size_t, 3> targetRatio{1, 1, 1};
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedVolume> open(QString url, std::chrono::milliseconds timeout);

  [[nodiscard]] const QString& rootUrl() const
  {
    return m_rootUrl;
  }

  [[nodiscard]] const ZImgInfo& baseImgInfo() const
  {
    return m_baseImgInfo;
  }

  [[nodiscard]] const std::vector<Scale>& scales() const
  {
    return m_scales;
  }

  [[nodiscard]] std::optional<size_t> scaleIndexForRatio(const std::array<size_t, 3>& ratio) const;

  [[nodiscard]] std::vector<std::array<size_t, 3>> availableRatios() const;

  [[nodiscard]] std::vector<Chunk> chunksIntersectingBaseBox(size_t scaleIndex,
                                                             const std::array<int64_t, 3>& baseStart,
                                                             const std::array<int64_t, 3>& baseEnd) const;

  [[nodiscard]] std::shared_ptr<ZImg> tryGetCachedChunk(const Chunk& chunk) const;

  folly::coro::Task<std::shared_ptr<ZImg>> readChunkAsync(const Chunk& chunk) const;

  [[nodiscard]] std::shared_ptr<ZImg> readChunkBlocking(const Chunk& chunk) const;

  // Builds a 2D slice tile pack for display at the target on-screen scale. This will only use chunks already
  // present in the in-memory chunk cache, and will fall back to coarser pyramid levels as needed to cover the
  // viewport. The returned tiles are ordered arbitrarily; callers should use their scale/z-ordering policy to
  // ensure finer tiles overwrite coarser ones.
  [[nodiscard]] SliceTilePack sliceTilePackFor2DViewportCacheBestEffort(size_t z,
                                                                        size_t t,
                                                                        const QRectF& viewport,
                                                                        double renderScale) const;

  // Builds a 2D slice tile pack by reading the target pyramid level. This may perform network I/O.
  [[nodiscard]] SliceTilePack sliceTilePackFor2DViewportBlocking(size_t z,
                                                                 size_t t,
                                                                 const QRectF& viewport,
                                                                 double renderScale,
                                                                 Slice2DRatioPolicy ratioPolicy = Slice2DRatioPolicy::BestForScale) const;

  // Computes the set of chunk bounding boxes intersecting a 2D viewport at the selected pyramid level.
  // This does not perform any network I/O; callers can schedule chunk reads as needed (e.g. for incremental rendering).
  [[nodiscard]] SliceChunkRequests sliceChunkRequestsFor2DViewport(size_t z,
                                                                   size_t t,
                                                                   const QRectF& viewport,
                                                                   double renderScale,
                                                                   Slice2DRatioPolicy ratioPolicy = Slice2DRatioPolicy::BestForScale) const;

  // Returns true if all chunks needed to render the given 2D viewport at the coarsest available XY pyramid
  // level (Slice2DRatioPolicy::CoarsestXY) are already present in the in-memory chunk cache.
  [[nodiscard]] bool is2DViewportFullyCachedForCoarsestXY(size_t z, size_t t, const QRectF& viewport) const;

  [[nodiscard]] const std::array<int64_t, 3>& baseVoxelOffset() const
  {
    return m_baseVoxelOffset;
  }

private:
  ZNeuroglancerPrecomputedVolume() = default;

  static QString normalizeRootUrl(QString url);

  static int64_t floorDiv(int64_t a, int64_t b);
  static int64_t ceilDiv(int64_t a, int64_t b);

  [[nodiscard]] QUrl infoUrl() const;

  [[nodiscard]] QUrl chunkUrl(const Chunk& chunk) const;

private:
  struct ChunkCache;

  QString m_rootUrl;
  QUrl m_rootQUrl;

  ZImgInfo m_baseImgInfo;
  std::array<double, 3> m_baseResolutionNm{};
  std::array<int64_t, 3> m_baseVoxelOffset{};

  QString m_dataTypeString;
  QString m_volumeTypeString;
  size_t m_numChannels = 1;

  std::vector<Scale> m_scales;
  std::map<std::array<size_t, 3>, size_t> m_ratioToScaleIndex;

  std::chrono::milliseconds m_defaultTimeout{30000};
  mutable std::unique_ptr<ChunkCache> m_chunkCache;
};

} // namespace nim
