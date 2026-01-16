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

// Where bytes were read from at the true I/O layer (network or persistent caches).
//
// Note: This is intentionally decoupled from ZImgRegionBlockSource. For example,
// a ZImgRegionBlockSource::SourceRead for a Neuroglancer dataset may correspond
// to underlying I/O from either the network or Atlas' HTTP disk cache.
enum class ZImgUnderlyingIoKind : std::uint8_t
{
  Network = 0,
  HttpDiskCache = 1,
  File = 2,
  ImgRegionDiskCache = 3,
};

// What GPU resource was updated by a paging upload.
enum class ZImgGpuUploadKind : std::uint8_t
{
  ImageBlocks = 0,
  PageDirectory = 1,
  PageTableCache = 2,
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

  // Called when decoded source data (uncompressed) had to be materialized to satisfy a request.
  //
  // Intended use:
  // - File-backed datasets: bytes of decoded tiles/chunks read from the underlying files.
  // - Network-backed datasets: bytes of decoded chunk payload materialized into a ZImg.
  //
  // This is distinct from onImgRegionBlockResolved(): a single block read may touch many
  // source tiles, and those tiles may be larger than the requested block due to alignment
  // and available pyramid ratios.
  //
  // Default implementation is a no-op.
  virtual void onSourceLogicalBytes(const ZImgReadStatsContext& ctx, size_t bytes)
  {
    (void)ctx;
    (void)bytes;
  }

  // Called when bytes are read from the underlying I/O source (e.g., network / HTTP disk cache).
  // Default implementation is a no-op so sinks can opt in as needed.
  virtual void onUnderlyingIoBytes(const ZImgReadStatsContext& ctx, ZImgUnderlyingIoKind kind, size_t bytes)
  {
    (void)ctx;
    (void)kind;
    (void)bytes;
  }

  // Called when bytes are uploaded to GPU resources as part of full-resolution paging.
  //
  // - bytes: number of bytes uploaded (CPU→GPU transfer size as provided by the uploader call site).
  // - blocks: optional count of logical blocks uploaded (used for ImageBlocks; 0 for page caches).
  //
  // Default implementation is a no-op so sinks can opt in as needed.
  virtual void onGpuUploadBytes(const ZImgReadStatsContext& ctx, ZImgGpuUploadKind kind, size_t bytes, size_t blocks)
  {
    (void)ctx;
    (void)kind;
    (void)bytes;
    (void)blocks;
  }
};

} // namespace nim
