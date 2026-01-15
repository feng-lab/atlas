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

  [[nodiscard]] bool hasActivity() const;

  // Returns a multi-line string suitable for one log entry.
  [[nodiscard]] std::string formatSummary(bool includeRoundBreakdown) const;

private:
  struct RoundStats
  {
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
};

} // namespace nim

