#pragma once

#include <cstddef>
#include <cstdint>

namespace nim {

// Where a 3D paging region block was resolved from.
//
// - MemoryCache: hit in ZImgRegionCache's in-memory bucket (already materialized this run).
// - DiskCache: hit in the optional persistent disk backend (SQLite) and promoted to memory.
// - SourceRead: cache miss; block was produced by reading from the underlying dataset (file or network).
// - SkippedEmpty: block was determined to be empty without producing a materialized ZImg result.
enum class ZImgRegionBlockSource : std::uint8_t
{
  MemoryCache = 0,
  DiskCache = 1,
  SourceRead = 2,
  SkippedEmpty = 3,
};

// Identifies a 3D paging request within a user-visible render operation.
// These values are supplied by the renderer (e.g., channel and progressive round).
struct ZImgReadStatsContext
{
  std::uint32_t channel = 0;
  std::uint32_t round = 0;
};

// Summary of a single 3D paging round (missing-block discovery + cache/update decision).
// This is intended to be emitted once per call to Z3DImg::updateAndUploadPageDirectoryCaches.
struct ZImgPagingRoundSummary
{
  size_t missingBlocks = 0;
  size_t processedMissingBlocks = 0;
  size_t skippedMissingBlocks = 0;
  size_t alreadyMappedBlocks = 0;
  size_t emptyBlocksMarked = 0;
  size_t blocksQueuedForRead = 0;
  size_t emptyBlocksRead = 0;
  bool filledAllMissingBlocks = false;
};

// Optional sink for read/paging telemetry. Call sites should pass nullptr when stats
// are disabled so hot paths pay only a pointer check.
class ZImgReadStatsSink
{
public:
  virtual ~ZImgReadStatsSink() = default;

  // Called when a region block is resolved to a concrete image payload (or determined empty).
  // - bytes: size of the concrete payload (0 for SkippedEmpty or when no materialization occurred).
  // - empty: true if the logical block contains no signal (e.g., nullptr returned to callers).
  virtual void onImgRegionBlockResolved(const ZImgReadStatsContext& ctx,
                                        ZImgRegionBlockSource source,
                                        size_t bytes,
                                        bool empty) = 0;

  // Called once per paging round with a coarse summary of the round's block mapping decisions.
  // Default implementation is a no-op so sinks can opt into only per-block stats.
  virtual void on3dPagingRoundSummary(const ZImgReadStatsContext& ctx, const ZImgPagingRoundSummary& summary)
  {
    (void)ctx;
    (void)summary;
  }
};

} // namespace nim
