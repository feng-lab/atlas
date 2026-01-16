#pragma once

#include "zimgreadstats.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace nim {

// Aggregates 3D image paging read statistics for a single user-visible render operation.
//
// Threading:
// - onImgRegionBlockResolved() may be called from the global CPU executor threads.
// - on3dPagingRoundSummary() is called by the rendering thread after paging decisions are made.
class Z3DImgPagingFrameStats final : public ZImgReadStatsSink
{
public:
  struct CreateInfo
  {
    std::string label;
    size_t numChannels = 0;
    uint32_t maxRounds = 1;
    uint64_t streamKey = 0;
    uint32_t progressiveGeneration = 0;
  };

  explicit Z3DImgPagingFrameStats(CreateInfo info);

  void onImgRegionBlockResolved(const ZImgReadStatsContext& ctx,
                                ZImgRegionBlockSource source,
                                size_t bytes,
                                bool empty) override;

  void on3dPagingRoundSummary(const ZImgReadStatsContext& ctx, const ZImgPagingRoundSummary& summary) override;

  void onSourceLogicalBytes(const ZImgReadStatsContext& ctx, size_t bytes) override;

  void onUnderlyingIoBytes(const ZImgReadStatsContext& ctx, ZImgUnderlyingIoKind kind, size_t bytes) override;

  void onGpuUploadBytes(const ZImgReadStatsContext& ctx, ZImgGpuUploadKind kind, size_t bytes, size_t blocks) override;

  [[nodiscard]] bool hasActivity() const;

  // Returns a multi-line string suitable for one log entry.
  [[nodiscard]] std::string formatSummary(bool includeRoundBreakdown) const;

private:
  struct RoundStats
  {
    // Note: std::atomic is neither copyable nor movable. Some libstdc++ container
    // implementations (and some generic code paths) still instantiate element copy/move
    // operations even when we only ever "resize()" with default construction.
    //
    // These stats are purely diagnostic; when copied/moved we want a best-effort snapshot of
    // the counters rather than failing to compile.
    RoundStats() = default;

    RoundStats(const RoundStats& other) noexcept { copyFrom(other); }
    RoundStats& operator=(const RoundStats& other) noexcept
    {
      if (this == &other) {
        return *this;
      }
      copyFrom(other);
      return *this;
    }

    RoundStats(RoundStats&& other) noexcept { copyFrom(other); }
    RoundStats& operator=(RoundStats&& other) noexcept
    {
      if (this == &other) {
        return *this;
      }
      copyFrom(other);
      return *this;
    }

    // Block read resolution (per block returned to callers)
    std::atomic<uint64_t> blocksMemoryCache{0};
    std::atomic<uint64_t> blocksDiskCache{0};
    std::atomic<uint64_t> blocksSourceRead{0};
    std::atomic<uint64_t> blocksSkippedEmpty{0};

    std::atomic<uint64_t> bytesMemoryCache{0};
    std::atomic<uint64_t> bytesDiskCache{0};
    std::atomic<uint64_t> bytesSourceRead{0};

    std::atomic<uint64_t> emptyResults{0};

    // Paging round planning/execution (once per updateAndUploadPageDirectoryCaches)
    std::atomic<size_t> missingBlocks{0};
    std::atomic<size_t> processedMissingBlocks{0};
    std::atomic<size_t> skippedMissingBlocks{0};
    std::atomic<size_t> alreadyMappedBlocks{0};
    std::atomic<size_t> emptyBlocksMarked{0};
    std::atomic<size_t> blocksQueuedForRead{0};
    std::atomic<size_t> emptyBlocksRead{0};
    std::atomic<bool> filledAllMissingBlocks{false};

    // Underlying I/O (true bytes read from network/disk cache, independent of decoded block size).
    std::atomic<uint64_t> underlyingNetworkBytes{0};
    std::atomic<uint64_t> underlyingHttpDiskCacheBytes{0};
    std::atomic<uint64_t> underlyingImgRegionDiskCacheBytes{0};
    std::atomic<uint64_t> underlyingFileBytes{0};

    // Logical source bytes touched (decoded/uncompressed), e.g. source tiles/chunks that were materialized.
    std::atomic<uint64_t> sourceLogicalBytes{0};

    // GPU uploads (bytes pushed into paging GPU resources).
    std::atomic<uint64_t> gpuUploadBytesImageBlocks{0};
    std::atomic<uint64_t> gpuUploadBlocksImageBlocks{0};
    std::atomic<uint64_t> gpuUploadBytesPageDirectory{0};
    std::atomic<uint64_t> gpuUploadBytesPageTableCache{0};

  private:
    void copyFrom(const RoundStats& other) noexcept
    {
      blocksMemoryCache.store(other.blocksMemoryCache.load(std::memory_order_relaxed), std::memory_order_relaxed);
      blocksDiskCache.store(other.blocksDiskCache.load(std::memory_order_relaxed), std::memory_order_relaxed);
      blocksSourceRead.store(other.blocksSourceRead.load(std::memory_order_relaxed), std::memory_order_relaxed);
      blocksSkippedEmpty.store(other.blocksSkippedEmpty.load(std::memory_order_relaxed), std::memory_order_relaxed);

      bytesMemoryCache.store(other.bytesMemoryCache.load(std::memory_order_relaxed), std::memory_order_relaxed);
      bytesDiskCache.store(other.bytesDiskCache.load(std::memory_order_relaxed), std::memory_order_relaxed);
      bytesSourceRead.store(other.bytesSourceRead.load(std::memory_order_relaxed), std::memory_order_relaxed);

      emptyResults.store(other.emptyResults.load(std::memory_order_relaxed), std::memory_order_relaxed);

      missingBlocks.store(other.missingBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
      processedMissingBlocks.store(other.processedMissingBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
      skippedMissingBlocks.store(other.skippedMissingBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
      alreadyMappedBlocks.store(other.alreadyMappedBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
      emptyBlocksMarked.store(other.emptyBlocksMarked.load(std::memory_order_relaxed), std::memory_order_relaxed);
      blocksQueuedForRead.store(other.blocksQueuedForRead.load(std::memory_order_relaxed), std::memory_order_relaxed);
      emptyBlocksRead.store(other.emptyBlocksRead.load(std::memory_order_relaxed), std::memory_order_relaxed);
      filledAllMissingBlocks.store(other.filledAllMissingBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);

      underlyingNetworkBytes.store(other.underlyingNetworkBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
      underlyingHttpDiskCacheBytes.store(other.underlyingHttpDiskCacheBytes.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
      underlyingImgRegionDiskCacheBytes.store(other.underlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed),
                                             std::memory_order_relaxed);
      underlyingFileBytes.store(other.underlyingFileBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);

      sourceLogicalBytes.store(other.sourceLogicalBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);

      gpuUploadBytesImageBlocks.store(other.gpuUploadBytesImageBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
      gpuUploadBlocksImageBlocks.store(other.gpuUploadBlocksImageBlocks.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
      gpuUploadBytesPageDirectory.store(other.gpuUploadBytesPageDirectory.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
      gpuUploadBytesPageTableCache.store(other.gpuUploadBytesPageTableCache.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
    }
  };

  struct ChannelStats
  {
    std::deque<RoundStats> rounds;
  };

  std::vector<ChannelStats> m_channels;

  std::string m_label;
  uint64_t m_streamKey = 0;
  uint32_t m_progressiveGeneration = 0;
  std::chrono::steady_clock::time_point m_start;

  // Rolling totals (avoid scanning at log sites that only want a headline).
  std::atomic<uint64_t> m_totalBlocksMemoryCache{0};
  std::atomic<uint64_t> m_totalBlocksDiskCache{0};
  std::atomic<uint64_t> m_totalBlocksSourceRead{0};
  std::atomic<uint64_t> m_totalBlocksSkippedEmpty{0};
  std::atomic<uint64_t> m_totalBytesMemoryCache{0};
  std::atomic<uint64_t> m_totalBytesDiskCache{0};
  std::atomic<uint64_t> m_totalBytesSourceRead{0};
  std::atomic<uint64_t> m_totalEmptyResults{0};

  std::atomic<uint64_t> m_totalUnderlyingNetworkBytes{0};
  std::atomic<uint64_t> m_totalUnderlyingHttpDiskCacheBytes{0};
  std::atomic<uint64_t> m_totalUnderlyingImgRegionDiskCacheBytes{0};
  std::atomic<uint64_t> m_totalUnderlyingFileBytes{0};

  std::atomic<uint64_t> m_totalSourceLogicalBytes{0};

  std::atomic<uint64_t> m_totalGpuUploadBytesImageBlocks{0};
  std::atomic<uint64_t> m_totalGpuUploadBlocksImageBlocks{0};
  std::atomic<uint64_t> m_totalGpuUploadBytesPageDirectory{0};
  std::atomic<uint64_t> m_totalGpuUploadBytesPageTableCache{0};
};

} // namespace nim
