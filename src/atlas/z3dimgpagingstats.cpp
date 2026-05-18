#include "z3dimgpagingstats.h"

#include "zlog.h"

#include "zcommandlineflags.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

ABSL_FLAG(
  bool,
  atlas_log_3d_paging_frame_stats,
  false,
  "Log per-frame 3D image paging stats (resolved blocks/bytes by cache tier, underlying I/O bytes, source decoded bytes, GPU upload bytes).");

ABSL_FLAG(bool,
          atlas_log_3d_paging_round_stats,
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

struct AggregatedUnderlyingIoBytes
{
  uint64_t networkBytes = 0;
  uint64_t httpDiskCacheBytes = 0;
  uint64_t imgRegionDiskCacheBytes = 0;
  uint64_t fileBytes = 0;
};

struct AggregatedGpuUploads
{
  uint64_t imageBlocksBytes = 0;
  uint64_t imageBlocks = 0;
  uint64_t pageDirectoryBytes = 0;
  uint64_t pageTableCacheBytes = 0;
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

void Z3DImgPagingFrameStats::onSourceLogicalBytes(const ZImgReadStatsContext& ctx, size_t bytes)
{
  const size_t channel = static_cast<size_t>(ctx.channel);
  CHECK_LT(channel, m_channels.size()) << "Paging stats channel out of range: channel=" << channel
                                       << " total=" << m_channels.size();
  const uint32_t round = ctx.round;
  CHECK_LT(static_cast<size_t>(round), m_channels[channel].rounds.size())
    << "Paging stats round out of range: round=" << round << " total=" << m_channels[channel].rounds.size();

  auto& rs = m_channels[channel].rounds[round];
  const uint64_t ubytes = static_cast<uint64_t>(bytes);
  rs.sourceLogicalBytes.fetch_add(ubytes, std::memory_order_relaxed);
  m_totalSourceLogicalBytes.fetch_add(ubytes, std::memory_order_relaxed);
}

void Z3DImgPagingFrameStats::onUnderlyingIoBytes(const ZImgReadStatsContext& ctx, ZImgUnderlyingIoKind kind, size_t bytes)
{
  const size_t channel = static_cast<size_t>(ctx.channel);
  CHECK_LT(channel, m_channels.size()) << "Paging stats channel out of range: channel=" << channel
                                       << " total=" << m_channels.size();
  const uint32_t round = ctx.round;
  CHECK_LT(static_cast<size_t>(round), m_channels[channel].rounds.size())
    << "Paging stats round out of range: round=" << round << " total=" << m_channels[channel].rounds.size();

  auto& rs = m_channels[channel].rounds[round];
  const uint64_t ubytes = static_cast<uint64_t>(bytes);

  switch (kind) {
    case ZImgUnderlyingIoKind::Network:
      rs.underlyingNetworkBytes.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalUnderlyingNetworkBytes.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgUnderlyingIoKind::HttpDiskCache:
      rs.underlyingHttpDiskCacheBytes.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalUnderlyingHttpDiskCacheBytes.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgUnderlyingIoKind::ImgRegionDiskCache:
      rs.underlyingImgRegionDiskCacheBytes.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalUnderlyingImgRegionDiskCacheBytes.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgUnderlyingIoKind::File:
      rs.underlyingFileBytes.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalUnderlyingFileBytes.fetch_add(ubytes, std::memory_order_relaxed);
      break;
  }
}

void Z3DImgPagingFrameStats::onGpuUploadBytes(const ZImgReadStatsContext& ctx,
                                              ZImgGpuUploadKind kind,
                                              size_t bytes,
                                              size_t blocks)
{
  const size_t channel = static_cast<size_t>(ctx.channel);
  CHECK_LT(channel, m_channels.size()) << "Paging stats channel out of range: channel=" << channel
                                       << " total=" << m_channels.size();
  const uint32_t round = ctx.round;
  CHECK_LT(static_cast<size_t>(round), m_channels[channel].rounds.size())
    << "Paging stats round out of range: round=" << round << " total=" << m_channels[channel].rounds.size();

  auto& rs = m_channels[channel].rounds[round];
  const uint64_t ubytes = static_cast<uint64_t>(bytes);
  const uint64_t ublocks = static_cast<uint64_t>(blocks);

  switch (kind) {
    case ZImgGpuUploadKind::ImageBlocks:
      rs.gpuUploadBytesImageBlocks.fetch_add(ubytes, std::memory_order_relaxed);
      rs.gpuUploadBlocksImageBlocks.fetch_add(ublocks, std::memory_order_relaxed);
      m_totalGpuUploadBytesImageBlocks.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalGpuUploadBlocksImageBlocks.fetch_add(ublocks, std::memory_order_relaxed);
      break;
    case ZImgGpuUploadKind::PageDirectory:
      rs.gpuUploadBytesPageDirectory.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalGpuUploadBytesPageDirectory.fetch_add(ubytes, std::memory_order_relaxed);
      break;
    case ZImgGpuUploadKind::PageTableCache:
      rs.gpuUploadBytesPageTableCache.fetch_add(ubytes, std::memory_order_relaxed);
      m_totalGpuUploadBytesPageTableCache.fetch_add(ubytes, std::memory_order_relaxed);
      break;
  }
}

bool Z3DImgPagingFrameStats::hasActivity() const
{
  return m_totalBlocksMemoryCache.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksDiskCache.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksSourceRead.load(std::memory_order_relaxed) != 0 ||
         m_totalBlocksSkippedEmpty.load(std::memory_order_relaxed) != 0 ||
         m_totalEmptyResults.load(std::memory_order_relaxed) != 0 ||
         m_totalSourceLogicalBytes.load(std::memory_order_relaxed) != 0 ||
         m_totalUnderlyingNetworkBytes.load(std::memory_order_relaxed) != 0 ||
         m_totalUnderlyingHttpDiskCacheBytes.load(std::memory_order_relaxed) != 0 ||
         m_totalUnderlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed) != 0 ||
         m_totalUnderlyingFileBytes.load(std::memory_order_relaxed) != 0 ||
         m_totalGpuUploadBytesImageBlocks.load(std::memory_order_relaxed) != 0 ||
         m_totalGpuUploadBytesPageDirectory.load(std::memory_order_relaxed) != 0 ||
         m_totalGpuUploadBytesPageTableCache.load(std::memory_order_relaxed) != 0;
}

std::string Z3DImgPagingFrameStats::formatSummary(bool includeRoundBreakdown) const
{
  auto anyRoundActivity = [](const RoundStats& rs) {
    return rs.blocksMemoryCache.load(std::memory_order_relaxed) != 0 ||
           rs.blocksDiskCache.load(std::memory_order_relaxed) != 0 ||
           rs.blocksSourceRead.load(std::memory_order_relaxed) != 0 ||
           rs.blocksSkippedEmpty.load(std::memory_order_relaxed) != 0 ||
           rs.missingBlocks.load(std::memory_order_relaxed) != 0 ||
           rs.blocksQueuedForRead.load(std::memory_order_relaxed) != 0 ||
           rs.underlyingNetworkBytes.load(std::memory_order_relaxed) != 0 ||
           rs.underlyingHttpDiskCacheBytes.load(std::memory_order_relaxed) != 0 ||
           rs.underlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed) != 0 ||
           rs.underlyingFileBytes.load(std::memory_order_relaxed) != 0 ||
           rs.sourceLogicalBytes.load(std::memory_order_relaxed) != 0 ||
           rs.gpuUploadBytesImageBlocks.load(std::memory_order_relaxed) != 0 ||
           rs.gpuUploadBytesPageDirectory.load(std::memory_order_relaxed) != 0 ||
           rs.gpuUploadBytesPageTableCache.load(std::memory_order_relaxed) != 0;
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

  auto sumUnderlyingIoForChannel = [](const ChannelStats& ch) {
    AggregatedUnderlyingIoBytes out;
    for (const auto& rs : ch.rounds) {
      out.networkBytes += rs.underlyingNetworkBytes.load(std::memory_order_relaxed);
      out.httpDiskCacheBytes += rs.underlyingHttpDiskCacheBytes.load(std::memory_order_relaxed);
      out.imgRegionDiskCacheBytes += rs.underlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed);
      out.fileBytes += rs.underlyingFileBytes.load(std::memory_order_relaxed);
    }
    return out;
  };

  auto sumSourceLogicalForChannel = [](const ChannelStats& ch) -> uint64_t {
    uint64_t out = 0;
    for (const auto& rs : ch.rounds) {
      out += rs.sourceLogicalBytes.load(std::memory_order_relaxed);
    }
    return out;
  };

  auto sumGpuUploadsForChannel = [](const ChannelStats& ch) {
    AggregatedGpuUploads out;
    for (const auto& rs : ch.rounds) {
      out.imageBlocksBytes += rs.gpuUploadBytesImageBlocks.load(std::memory_order_relaxed);
      out.imageBlocks += rs.gpuUploadBlocksImageBlocks.load(std::memory_order_relaxed);
      out.pageDirectoryBytes += rs.gpuUploadBytesPageDirectory.load(std::memory_order_relaxed);
      out.pageTableCacheBytes += rs.gpuUploadBytesPageTableCache.load(std::memory_order_relaxed);
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

  const uint64_t underlyingNetworkBytes = m_totalUnderlyingNetworkBytes.load(std::memory_order_relaxed);
  const uint64_t underlyingHttpDiskCacheBytes = m_totalUnderlyingHttpDiskCacheBytes.load(std::memory_order_relaxed);
  const uint64_t underlyingImgRegionDiskCacheBytes =
    m_totalUnderlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed);
  const uint64_t underlyingFileBytes = m_totalUnderlyingFileBytes.load(std::memory_order_relaxed);

  const uint64_t gpuUploadBytesImageBlocks = m_totalGpuUploadBytesImageBlocks.load(std::memory_order_relaxed);
  const uint64_t gpuUploadBlocksImageBlocks = m_totalGpuUploadBlocksImageBlocks.load(std::memory_order_relaxed);
  const uint64_t gpuUploadBytesPageDirectory = m_totalGpuUploadBytesPageDirectory.load(std::memory_order_relaxed);
  const uint64_t gpuUploadBytesPageTableCache = m_totalGpuUploadBytesPageTableCache.load(std::memory_order_relaxed);
  const uint64_t gpuUploadBytesTotal = gpuUploadBytesImageBlocks + gpuUploadBytesPageDirectory + gpuUploadBytesPageTableCache;

  const uint64_t sourceLogicalBytes = m_totalSourceLogicalBytes.load(std::memory_order_relaxed);

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
  out += fmt::format("  Underlying I/O:  {} (network={} http_disk_cache={} imgregion_disk_cache={} file={})\n",
                     formatBytes(underlyingNetworkBytes + underlyingHttpDiskCacheBytes + underlyingImgRegionDiskCacheBytes +
                                 underlyingFileBytes),
                     formatBytes(underlyingNetworkBytes),
                     formatBytes(underlyingHttpDiskCacheBytes),
                     formatBytes(underlyingImgRegionDiskCacheBytes),
                     formatBytes(underlyingFileBytes));
  out += fmt::format("  Source decoded:  {} (uncompressed; tile/chunk cache misses)\n", formatBytes(sourceLogicalBytes));
  out += fmt::format("  GPU uploads:     {} (img_blocks={} blocks={} page_directory={} page_table={})\n",
                     formatBytes(gpuUploadBytesTotal),
                     formatBytes(gpuUploadBytesImageBlocks),
                     gpuUploadBlocksImageBlocks,
                     formatBytes(gpuUploadBytesPageDirectory),
                     formatBytes(gpuUploadBytesPageTableCache));

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
    const auto io = sumUnderlyingIoForChannel(ch);
    const auto gpu = sumGpuUploadsForChannel(ch);
    const uint64_t logical = sumSourceLogicalForChannel(ch);
    const auto rounds = sumRoundSummaryForChannel(ch);
    const bool active =
      (blocks.memBlocks + blocks.diskBlocks + blocks.sourceBlocks + blocks.skippedEmptyBlocks) != 0 ||
      rounds.missingBlocks != 0 || rounds.blocksQueuedForRead != 0 || io.networkBytes != 0 || io.httpDiskCacheBytes != 0 ||
      io.imgRegionDiskCacheBytes != 0 || io.fileBytes != 0 ||
      gpu.imageBlocksBytes != 0 || gpu.pageDirectoryBytes != 0 || gpu.pageTableCacheBytes != 0 || logical != 0;
    if (!active) {
      continue;
    }

    out += fmt::format(
      "  ch{}: blocks={} (mem={} disk={} source={} skipped_empty={} empty={}) bytes={} (mem={} disk={} source={}) "
      "io(net/http/imgregion/file)=({}/{}/{}/{}) src_decoded={} gpu(img/pd/pt)=({}/{}/{}) img_blocks_uploaded={} rounds_missing={} queued_read={} mapped={}\n",
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
      formatBytes(io.networkBytes),
      formatBytes(io.httpDiskCacheBytes),
      formatBytes(io.imgRegionDiskCacheBytes),
      formatBytes(io.fileBytes),
      formatBytes(logical),
      formatBytes(gpu.imageBlocksBytes),
      formatBytes(gpu.pageDirectoryBytes),
      formatBytes(gpu.pageTableCacheBytes),
      gpu.imageBlocks,
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
          "    r{}: missing={} processed={} skipped={} mapped={} empty_marked={} queued_read={} empty_read={} filled={} | "
          "blocks(mem/disk/source)=({}/{}/{}) bytes(mem/disk/source)=({}/{}/{}) io(net/http/imgregion/file)=({}/{}/{}/{}) src_decoded={} "
          "gpu(img/pd/pt)=({}/{}/{}) img_blocks_uploaded={}\n",
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
          formatBytes(rs.bytesSourceRead.load(std::memory_order_relaxed)),
          formatBytes(rs.underlyingNetworkBytes.load(std::memory_order_relaxed)),
          formatBytes(rs.underlyingHttpDiskCacheBytes.load(std::memory_order_relaxed)),
          formatBytes(rs.underlyingImgRegionDiskCacheBytes.load(std::memory_order_relaxed)),
          formatBytes(rs.underlyingFileBytes.load(std::memory_order_relaxed)),
          formatBytes(rs.sourceLogicalBytes.load(std::memory_order_relaxed)),
          formatBytes(rs.gpuUploadBytesImageBlocks.load(std::memory_order_relaxed)),
          formatBytes(rs.gpuUploadBytesPageDirectory.load(std::memory_order_relaxed)),
          formatBytes(rs.gpuUploadBytesPageTableCache.load(std::memory_order_relaxed)),
          rs.gpuUploadBlocksImageBlocks.load(std::memory_order_relaxed));
      }
    }
  }

  // Guidance for verbosity.
  if (!includeRoundBreakdown && absl::GetFlag(FLAGS_atlas_log_3d_paging_round_stats)) {
    // Should never happen: includeRoundBreakdown is derived from the same flag.
    out += "  NOTE: atlas_log_3d_paging_round_stats is set but round breakdown was not requested.\n";
  } else if (!includeRoundBreakdown) {
    out += "  (Enable --atlas_log_3d_paging_round_stats=true for per-round detail.)\n";
  }

  return out;
}

} // namespace nim
