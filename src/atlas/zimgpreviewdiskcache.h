#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nim {

class ZImg;

// Dedicated persistent on-disk cache for raw downsampled preview volumes.
//
// Motivation:
// - Z3DImg::readVolumes() builds a downsampled "fast preview" volume for rendering.
// - This operation can be expensive for tiled/disk-backed datasets.
// - Unlike paging bricks (ZImgRegionCache), the preview volume is a whole-volume read.
//
// Design:
// - Disk-only cache (no global RAM LRU): Z3DImg already holds the preview in memory for rendering.
// - Stable keys based on ZImgPack::datasetFingerprintForCache() + target dimensions + time index.
// - Best-effort: failures degrade to cache misses / skipped writes.
class ZImgPreviewDiskCache
{
public:
  static ZImgPreviewDiskCache& instance();

  // File-backed datasets only (stable fingerprints include file paths + mtimes).
  [[nodiscard]] std::shared_ptr<ZImg> tryGetFilePreview(const std::array<std::uint8_t, 32>& datasetFingerprint,
                                                        size_t width,
                                                        size_t height,
                                                        size_t depth,
                                                        size_t t) const;

  void tryPutFilePreview(const std::array<std::uint8_t, 32>& datasetFingerprint,
                         size_t width,
                         size_t height,
                         size_t depth,
                         size_t t,
                         std::shared_ptr<const ZImg> img);

private:
  ZImgPreviewDiskCache();
  ~ZImgPreviewDiskCache();

  ZImgPreviewDiskCache(const ZImgPreviewDiskCache&) = delete;
  ZImgPreviewDiskCache& operator=(const ZImgPreviewDiskCache&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace nim
