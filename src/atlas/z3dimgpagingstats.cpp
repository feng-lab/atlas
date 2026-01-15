#include "z3dimgpagingstats.h"

#include "zlog.h"

#include <gflags/gflags.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

DEFINE_bool(atlas_log_3d_paging_frame_stats,
            false,
            "Log per-frame 3D image paging read stats (bytes/blocks by cache tier: memory, disk, source).");

DEFINE_bool(atlas_log_3d_paging_round_stats,
            false,
            "Include per-round 3D image paging stats in the per-frame log entry (can be verbose).");

namespace nim {

namespace {

[[nodiscard]] std::string formatBytes(uint64_t bytes)
{
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;
  constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

  if (bytes < static_cast<uint64_t>(kKiB)) {
    return fmt::format("{} B", bytes);
  }
  if (bytes < static_cast<uint64_t>(kMiB)) {
    return fmt::format("{:.2f} KiB", static_cast<double>(bytes) / kKiB);
  }
  if (bytes < static_cast<uint64_t>(kGiB)) {
    return fmt::format("{:.2f} MiB", static_cast<double>(bytes) / kMiB);
  }
  return fmt::format("{:.2f} GiB", static_cast<double>(bytes) / kGiB);
}

struct AggregatedBytes
{
  uint64_t memBytes = 0;
  uint64_t diskBytes = 0;
  uint64_t sourceBytes = 0;
};

struct AggregatedBlocks
{
  uint64_t memBlocks = 0;
  uint64_t diskBlocks = 0;
  uint64_t sourceBlocks = 0;
  uint64_t skippedEmptyBlocks = 0;
  uint64_t emptyResults = 0;
};

struct AggregatedRoundSummary
{
  size_t missingBlocks = 0;
  size_t processedMissingBlocks = 0;
  size_t skippedMissingBlocks = 0;
  size_t alreadyMappedBlocks = 0;
  size_t emptyBlocksMarked = 0;
  size_t blocksQueuedForRead = 0;
  size_t emptyBlocksRead = 0;
  size_t roundsWithAnyMissing = 0;
  size_t roundsFilledAllMissing = 0;
};

} // namespace

Z3DImgPagingFrameStats::Z3DImgPagingFrameStats(CreateInfo info)
  : m_label(std::move(info.label))
  , m_streamKey(info.streamKey)
  , m_progressiveGeneration(info.progressiveGeneration)
  , m_start(std::chrono::steady_clock::now())
{
  const uint32_t maxRounds = std::max<uint32_t>(1u, info.maxRounds);
  m_channels.resize(info.numChannels);
  for (auto& ch : m_channels) {
    ch.rounds.resize(maxRounds);
  }
}

void Z3DImgPagingFrameStats::onImgRegionBlockResolved(const ZImgReadStatsContext& ctx,
                                                      ZImgRegionBlockSource source,
                                                      size_t bytes,
                                                      bool empty)
{
  const size_t channel = static_cast<size_t>(ctx.channel);
  CHECK_LT(channel, m_channels.size()) << "Paging stats channel out of range: channel=" << channel
                                       << " total=" << m_channels.size();
  const uint32_t round = ctx.round;
  CHECK_LT(static_cast<size_t>(round), m_channels[channel].rounds.size())
    << "Paging stats round out of range: round=" << round << " total=" << m_channels[channel].rounds.size();

  auto& rs = m_channels[channel].rounds[round];
  const uint64_t ubytes = static_cast<uint64_t>(bytes);

  if (empty) {
    rs.emptyResults.fetch_add(1, std::memory_order_relaxed);
    m_totalEmptyResults.fetch_add(1, std::memory_order_relaxed);
  }

  switch (source) {
    case ZImgRegionBlockSource::MemoryCache:
      rs.blocksMemoryCache.fetch_add(1, std::memory_order_relaxed);
      rs.bytesMemoryCache.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalBlocksMemoryCache.fetch_add(1, std::memory_order_relaxed);
      m_totalBytesMemoryCache.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgRegionBlockSource::DiskCache:
      rs.blocksDiskCache.fetch_add(1, std::memory_order_relaxed);
      rs.bytesDiskCache.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalBlocksDiskCache.fetch_add(1, std::memory_order_relaxed);
      m_totalBytesDiskCache.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgRegionBlockSource::SourceRead:
      rs.blocksSourceRead.fetch_add(1, std::memory_order_relaxed);
      rs.bytesSourceRead.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalBlocksSourceRead.fetch_add(1, std::memory_order_relaxed);
      m_totalBytesSourceRead.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgRegionBlockSource::SkippedEmpty:
      rs.blocksSkippedEmpty.fetch_add(1, std::memory_order_relaxed);
      m_totalBlocksSkippedEmpty.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void Z3DImgPagingFrameStats::on3dPagingRoundSummary(const ZImgReadStatsContext& ctx, const ZImgPagingRoundSummary& summary)
{
  const size_t channel = static_cast<size_t>(ctx.channel);
  CHECK_LT(channel, m_channels.size()) << "Paging stats channel out of range: channel=" << channel
                                       << " total=" << m_channels.size();
  const uint32_t round = ctx.round;
  CHECK_LT(static_cast<size_t>(round), m_channels[channel].rounds.size())
    << "Paging stats round out of range: round=" << round << " total=" << m_channels[channel].rounds.size();

  auto& rs = m_channels[channel].rounds[round];
  rs.missingBlocks.store(summary.missingBlocks, std::memory_order_relaxed);
  rs.processedMissingBlocks.store(summary.processedMissingBlocks, std::memory_order_relaxed);
  rs.skippedMissingBlocks.store(summary.skippedMissingBlocks, std::memory_order_relaxed);
  rs.alreadyMappedBlocks.store(summary.alreadyMappedBlocks, std::memory_order_relaxed);
  rs.emptyBlocksMarked.store(summary.emptyBlocksMarked, std::memory_order_relaxed);
  rs.blocksQueuedForRead.store(summary.blocksQueuedForRead, std::memory_order_relaxed);
  rs.emptyBlocksRead.store(summary.emptyBlocksRead, std::memory_order_relaxed);
  rs.filledAllMissingBlocks.store(summary.filledAllMissingBlocks, std::memory_order_relaxed);
}

bool Z3DImgPagingFrameStats::hasActivity() const
{
  return m_totalBlocksMemoryCache.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksDiskCache.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksSourceRead.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksSkippedEmpty.load(std::memory_order_relaxed) != 0 ||
         m_totalEmptyResults.load(std::memory_order_relaxed) != 0;
}

std::string Z3DImgPagingFrameStats::formatSummary(bool includeRoundBreakdown) const
{
  auto anyRoundActivity = [](const RoundStats& rs) {
    return rs.blocksMemoryCache.load(std::memory_order_relaxed) != 0 ||
           rs.blocksDiskCache.load(std::memory_order_relaxed) != 0 ||
           rs.blocksSourceRead.load(std::memory_order_relaxed) != 0 ||
           rs.blocksSkippedEmpty.load(std::memory_order_relaxed) != 0 ||
           rs.missingBlocks.load(std::memory_order_relaxed) != 0 || rs.blocksQueuedForRead.load(std::memory_order_relaxed) != 0;
  };

  auto sumBytesForChannel = [](const ChannelStats& ch) {
    AggregatedBytes out;
    for (const auto& rs : ch.rounds) {
      out.memBytes += rs.bytesMemoryCache.load(std::memory_order_relaxed);
      out.diskBytes += rs.bytesDiskCache.load(std::memory_order_relaxed);
      out.sourceBytes += rs.bytesSourceRead.load(std::memory_order_relaxed);
    }
    return out;
  };

  auto sumBlocksForChannel = [](const ChannelStats& ch) {
    AggregatedBlocks out;
    for (const auto& rs : ch.rounds) {
      out.memBlocks += rs.blocksMemoryCache.load(std::memory_order_relaxed);
      out.diskBlocks += rs.blocksDiskCache.load(std::memory_order_relaxed);
      out.sourceBlocks += rs.blocksSourceRead.load(std::memory_order_relaxed);
      out.skippedEmptyBlocks += rs.blocksSkippedEmpty.load(std::memory_order_relaxed);
      out.emptyResults += rs.emptyResults.load(std::memory_order_relaxed);
    }
    return out;
  };

  auto sumRoundSummaryForChannel = [](const ChannelStats& ch) {
    AggregatedRoundSummary out;
    for (const auto& rs : ch.rounds) {
      const size_t missing = rs.missingBlocks.load(std::memory_order_relaxed);
      out.missingBlocks += missing;
      out.processedMissingBlocks += rs.processedMissingBlocks.load(std::memory_order_relaxed);
      out.skippedMissingBlocks += rs.skippedMissingBlocks.load(std::memory_order_relaxed);
      out.alreadyMappedBlocks += rs.alreadyMappedBlocks.load(std::memory_order_relaxed);
      out.emptyBlocksMarked += rs.emptyBlocksMarked.load(std::memory_order_relaxed);
      out.blocksQueuedForRead += rs.blocksQueuedForRead.load(std::memory_order_relaxed);
      out.emptyBlocksRead += rs.emptyBlocksRead.load(std::memory_order_relaxed);
      if (missing != 0) {
        ++out.roundsWithAnyMissing;
        if (rs.filledAllMissingBlocks.load(std::memory_order_relaxed)) {
          ++out.roundsFilledAllMissing;
        }
      }
    }
    return out;
  };

  const uint64_t memBlocks = m_totalBlocksMemoryCache.load(std::memory_order_relaxed);
  const uint64_t diskBlocks = m_totalBlocksDiskCache.load(std::memory_order_relaxed);
  const uint64_t sourceBlocks = m_totalBlocksSourceRead.load(std::memory_order_relaxed);
  const uint64_t skippedEmptyBlocks = m_totalBlocksSkippedEmpty.load(std::memory_order_relaxed);

  const uint64_t memBytes = m_totalBytesMemoryCache.load(std::memory_order_relaxed);
  const uint64_t diskBytes = m_totalBytesDiskCache.load(std::memory_order_relaxed);
  const uint64_t sourceBytes = m_totalBytesSourceRead.load(std::memory_order_relaxed);

  const uint64_t emptyResults = m_totalEmptyResults.load(std::memory_order_relaxed);

  const auto now = std::chrono::steady_clock::now();
  const double elapsedMs =
    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - m_start).count();

  std::string out;
  out.reserve(4096);

  out += fmt::format("3D paging frame stats: '{}' stream=0x{:x} gen={} channels={} elapsedMs={:.3f}\n",
                     m_label,
                     m_streamKey,
                     m_progressiveGeneration,
                     m_channels.size(),
                     elapsedMs);

  const uint64_t totalResolvedBlocks = memBlocks + diskBlocks + sourceBlocks;
  const uint64_t totalResolvedBytes = memBytes + diskBytes + sourceBytes;

  out += fmt::format("  Resolved blocks: {} (mem={} disk={} source={} skipped_empty={})\n",
                     totalResolvedBlocks,
                     memBlocks,
                     diskBlocks,
                     sourceBlocks,
                     skippedEmptyBlocks);
  out += fmt::format("  Resolved bytes:  {} (mem={} disk={} source={})\n",
                     formatBytes(totalResolvedBytes),
                     formatBytes(memBytes),
                     formatBytes(diskBytes),
                     formatBytes(sourceBytes));
  out += fmt::format("  Empty results:   {}\n", emptyResults);

  AggregatedRoundSummary roundTotals;
  for (const auto& ch : m_channels) {
    auto s = sumRoundSummaryForChannel(ch);
    roundTotals.missingBlocks += s.missingBlocks;
    roundTotals.processedMissingBlocks += s.processedMissingBlocks;
    roundTotals.skippedMissingBlocks += s.skippedMissingBlocks;
    roundTotals.alreadyMappedBlocks += s.alreadyMappedBlocks;
    roundTotals.emptyBlocksMarked += s.emptyBlocksMarked;
    roundTotals.blocksQueuedForRead += s.blocksQueuedForRead;
    roundTotals.emptyBlocksRead += s.emptyBlocksRead;
    roundTotals.roundsWithAnyMissing += s.roundsWithAnyMissing;
    roundTotals.roundsFilledAllMissing += s.roundsFilledAllMissing;
  }

  if (roundTotals.missingBlocks > 0 || roundTotals.blocksQueuedForRead > 0) {
    out += fmt::format(
      "  Paging rounds: missing={} processed={} skipped={} mapped={} empty_marked={} queued_read={} empty_read={} filled_rounds={}/{}\n",
      roundTotals.missingBlocks,
      roundTotals.processedMissingBlocks,
      roundTotals.skippedMissingBlocks,
      roundTotals.alreadyMappedBlocks,
      roundTotals.emptyBlocksMarked,
      roundTotals.blocksQueuedForRead,
      roundTotals.emptyBlocksRead,
      roundTotals.roundsFilledAllMissing,
      roundTotals.roundsWithAnyMissing);
  }

  // Channel summaries (only channels with any activity)
  for (size_t c = 0; c < m_channels.size(); ++c) {
    const auto& ch = m_channels[c];
    const auto blocks = sumBlocksForChannel(ch);
    const auto bytes = sumBytesForChannel(ch);
    const auto rounds = sumRoundSummaryForChannel(ch);
    const bool active = (blocks.memBlocks + blocks.diskBlocks + blocks.sourceBlocks + blocks.skippedEmptyBlocks) != 0 ||
                        rounds.missingBlocks != 0 || rounds.blocksQueuedForRead != 0;
    if (!active) {
      continue;
    }

    out += fmt::format(
      "  ch{}: blocks={} (mem={} disk={} source={} skipped_empty={} empty={}) bytes={} (mem={} disk={} source={}) rounds_missing={} queued_read={} mapped={}\n",
      c,
      (blocks.memBlocks + blocks.diskBlocks + blocks.sourceBlocks),
      blocks.memBlocks,
      blocks.diskBlocks,
      blocks.sourceBlocks,
      blocks.skippedEmptyBlocks,
      blocks.emptyResults,
      formatBytes(bytes.memBytes + bytes.diskBytes + bytes.sourceBytes),
      formatBytes(bytes.memBytes),
      formatBytes(bytes.diskBytes),
      formatBytes(bytes.sourceBytes),
      rounds.missingBlocks,
      rounds.blocksQueuedForRead,
      rounds.alreadyMappedBlocks);

    if (includeRoundBreakdown) {
      for (size_t r = 0; r < ch.rounds.size(); ++r) {
        const auto& rs = ch.rounds[r];
        if (!anyRoundActivity(rs)) {
          continue;
        }
        out += fmt::format(
          "    r{}: missing={} processed={} skipped={} mapped={} empty_marked={} queued_read={} empty_read={} filled={} | blocks(mem/disk/source)=({}/{}/{}) bytes(mem/disk/source)=({}/{}/{})\n",
          r,
          rs.missingBlocks.load(std::memory_order_relaxed),
          rs.processedMissingBlocks.load(std::memory_order_relaxed),
          rs.skippedMissingBlocks.load(std::memory_order_relaxed),
          rs.alreadyMappedBlocks.load(std::memory_order_relaxed),
          rs.emptyBlocksMarked.load(std::memory_order_relaxed),
          rs.blocksQueuedForRead.load(std::memory_order_relaxed),
          rs.emptyBlocksRead.load(std::memory_order_relaxed),
          rs.filledAllMissingBlocks.load(std::memory_order_relaxed) ? 1 : 0,
          rs.blocksMemoryCache.load(std::memory_order_relaxed),
          rs.blocksDiskCache.load(std::memory_order_relaxed),
          rs.blocksSourceRead.load(std::memory_order_relaxed),
          formatBytes(rs.bytesMemoryCache.load(std::memory_order_relaxed)),
          formatBytes(rs.bytesDiskCache.load(std::memory_order_relaxed)),
          formatBytes(rs.bytesSourceRead.load(std::memory_order_relaxed)));
      }
    }
  }

  // Guidance for verbosity.
  if (!includeRoundBreakdown && FLAGS_atlas_log_3d_paging_round_stats) {
    // Should never happen: includeRoundBreakdown is derived from the same flag.
    out += "  NOTE: atlas_log_3d_paging_round_stats is set but round breakdown was not requested.\n";
  } else if (!includeRoundBreakdown) {
    out += "  (Enable --atlas_log_3d_paging_round_stats=true for per-round detail.)\n";
  }

  return out;
}

} // namespace nim
