#pragma once

#include "zneuroglancerremotecontext.h"
#include "zimg.h"
#include "zimgreadstats.h"

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
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nim {

class ZRemoteObjectStore;
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
    // Per-tile downsample ratio (relative to the base scale). Aligned with imgs/locs/scales.
    std::vector<std::array<size_t, 3>> ratios;
    std::array<size_t, 3> targetRatio{1, 1, 1};
    // True if the tile pack contains a complete, hole-free coverage at targetRatio using only cached chunks.
    // This is useful for callers that want to skip a more expensive "final" render when the requested LOD is
    // already fully available in the in-memory chunk cache.
    bool fullyCachedAtTargetRatio = false;
  };

  struct SliceChunkRequests
  {
    std::vector<Chunk> chunks;
    std::array<size_t, 3> targetRatio{1, 1, 1};
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedVolume>
  open(QString url, std::chrono::milliseconds timeout, std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Normalizes a dataset root URL for Neuroglancer precomputed volumes:
  // - Accepts optional "precomputed://" prefix.
  // - Maps "gs://bucket/path" to "https://storage.googleapis.com/bucket/path".
  // - Maps "s3://bucket/path" to an HTTPS S3 endpoint.
  // - Strips trailing "/info" and ensures the result ends with '/'.
  // This does not perform any network I/O and is safe to call for scene serialization/dedup.
  static QString normalizeRootUrl(QString url);

  [[nodiscard]] const QString& rootUrl() const
  {
    return m_rootUrl;
  }

  [[nodiscard]] const ZImgInfo& baseImgInfo() const
  {
    return m_baseImgInfo;
  }

  [[nodiscard]] std::chrono::milliseconds defaultTimeout() const
  {
    CHECK(m_remoteContext);
    return m_remoteContext->timeout();
  }

  // Reader-facing remote I/O policy for this volume and any child Neuroglancer sources reopened from it.
  // Callers that already hold a live volume should pass this context onward rather than rebuilding child
  // readers from timeout only, otherwise any non-default store/backend/session carried by the volume is lost.
  [[nodiscard]] const std::shared_ptr<const ZNeuroglancerRemoteContext>& sharedRemoteContext() const
  {
    CHECK(m_remoteContext);
    return m_remoteContext;
  }

  [[nodiscard]] const QString& dataTypeString() const
  {
    return m_dataTypeString;
  }

  [[nodiscard]] const QString& volumeTypeString() const
  {
    return m_volumeTypeString;
  }

  [[nodiscard]] bool isSegmentation() const
  {
    return m_volumeTypeString == "segmentation";
  }

  [[nodiscard]] bool hasSegmentPropertiesDirectory() const
  {
    return !m_segmentPropertiesKey.isEmpty();
  }

  [[nodiscard]] const QString& segmentPropertiesKey() const
  {
    return m_segmentPropertiesKey;
  }

  [[nodiscard]] QUrl segmentPropertiesDirUrl() const
  {
    CHECK(hasSegmentPropertiesDirectory());
    return m_segmentPropertiesDirUrl;
  }

  [[nodiscard]] std::shared_ptr<const class ZNeuroglancerPrecomputedSegmentProperties> segmentPropertiesShared() const;

  // Loads segment properties from the segment_properties directory specified in the volume info.
  // This performs network I/O and may take time; prefer calling from a worker thread.
  std::shared_ptr<const class ZNeuroglancerPrecomputedSegmentProperties> loadSegmentPropertiesBlocking() const;

  [[nodiscard]] bool hasMeshDirectory() const
  {
    return !m_meshKey.isEmpty();
  }

  [[nodiscard]] const QString& meshKey() const
  {
    return m_meshKey;
  }

  [[nodiscard]] QUrl meshDirUrl() const
  {
    CHECK(hasMeshDirectory());
    return m_meshDirUrl;
  }

  [[nodiscard]] std::shared_ptr<const class ZNeuroglancerPrecomputedMeshSource> meshSourceShared() const;

  // Loads mesh metadata from the mesh directory specified in the volume info.
  // This performs network I/O (reading mesh/info) and may take time; prefer calling from a worker thread.
  std::shared_ptr<const class ZNeuroglancerPrecomputedMeshSource> loadMeshSourceBlocking() const;

  [[nodiscard]] bool hasSkeletonDirectory() const
  {
    return !m_skeletonKey.isEmpty();
  }

  [[nodiscard]] const QString& skeletonKey() const
  {
    return m_skeletonKey;
  }

  [[nodiscard]] QUrl skeletonDirUrl() const
  {
    CHECK(hasSkeletonDirectory());
    return m_skeletonDirUrl;
  }

  [[nodiscard]] std::shared_ptr<const class ZNeuroglancerPrecomputedSkeletonSource> skeletonSourceShared() const;

  // Loads skeleton metadata from the skeletons directory specified in the volume info.
  // This performs network I/O (reading skeletons/info) and may take time; prefer calling from a worker thread.
  std::shared_ptr<const class ZNeuroglancerPrecomputedSkeletonSource> loadSkeletonSourceBlocking() const;

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

  folly::coro::Task<std::shared_ptr<ZImg>> readChunkAsync(const Chunk& chunk,
                                                          /*nullable*/ ZImgReadStatsSink* statsSink = nullptr,
                                                          ZImgReadStatsContext statsContext = {}) const;

  [[nodiscard]] std::shared_ptr<ZImg> readChunkBlocking(const Chunk& chunk) const;

  // Builds a 2D slice tile pack for display at the target on-screen scale. This will only use chunks already
  // present in the in-memory chunk cache, and will fall back to coarser pyramid levels as needed to cover the
  // viewport. The returned tiles are ordered arbitrarily; callers should use their scale/z-ordering policy to
  // ensure finer tiles overwrite coarser ones. If the target pyramid level is fully cached for the viewport,
  // the function returns only that level and sets SliceTilePack::fullyCachedAtTargetRatio.
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

  static int64_t floorDiv(int64_t a, int64_t b);
  static int64_t ceilDiv(int64_t a, int64_t b);

  [[nodiscard]] QUrl infoUrl() const;

  [[nodiscard]] QUrl chunkUrl(const Chunk& chunk) const;

private:
  struct ChunkCache;
  struct InFlightSegmentPropertiesLoad;
  struct InFlightMeshSourceLoad;
  struct InFlightSkeletonSourceLoad;

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

  std::shared_ptr<const ZNeuroglancerRemoteContext> m_remoteContext;
  mutable std::unique_ptr<ChunkCache> m_chunkCache;

  QString m_segmentPropertiesKey;
  QUrl m_segmentPropertiesDirUrl;
  mutable std::mutex m_segmentPropertiesMutex;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> m_segmentProperties;
  mutable std::shared_ptr<InFlightSegmentPropertiesLoad> m_segmentPropertiesInFlight;

  QString m_meshKey;
  QUrl m_meshDirUrl;
  mutable std::mutex m_meshMutex;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> m_meshSource;
  mutable std::shared_ptr<InFlightMeshSourceLoad> m_meshSourceInFlight;

  QString m_skeletonKey;
  QUrl m_skeletonDirUrl;
  mutable std::mutex m_skeletonMutex;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> m_skeletonSource;
  mutable std::shared_ptr<InFlightSkeletonSourceLoad> m_skeletonSourceInFlight;
};

} // namespace nim
