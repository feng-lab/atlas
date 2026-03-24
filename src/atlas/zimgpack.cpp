#include "zimgpack.h"

#include "zcpuinfo.h"
#include "zlog.h"
#include "zimgcache.h"
#include "zimgregioncache.h"
#include "zimgpreviewdiskcache.h"
#include "zcancellation.h"
#include "zioreadstats.h"
#include "zimgreadstatsscope.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancersegmentationcolors.h"
#include <QDateTime>
#include <QFileInfo>
#include <QPoint>
#include <QMenu>
#include <boost/hash2/sha2.hpp>
#include <bit>
#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <tbb/parallel_for.h>
#include <boost/iterator/function_output_iterator.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <zbenchtimer.h>

DEFINE_bool(atlas_readRegionToImg_use_multithreaded_resize,
            false,
            "Whether readRegionToImg uses multithreaded resize, default is false");

// When true, large non-pyramidal sources avoid eager downsampled image generation at open,
// and instead precompute only tile descriptors (index) without I/O.
DEFINE_bool(atlas_imgpack_defer_pyramidal,
            true,
            "Defer pyramid building for large non-pyramidal images (index only, no image reads)");

// Quick window estimation flags
DEFINE_bool(atlas_imgpack_quick_window_enable,
            true,
            "Enable percentile-based quick window for float data");
DEFINE_double(atlas_imgpack_quick_window_lower_p, 0.01, "Lower percentile for quick window [0,1]");
DEFINE_double(atlas_imgpack_quick_window_upper_p, 0.99, "Upper percentile for quick window [0,1]");
DEFINE_uint32(atlas_imgpack_quick_window_tiles_per_axis, 2, "Tile samples per axis for quick window (>=1)");
DEFINE_uint32(atlas_imgpack_quick_window_sample_step, 8, "Pixel stride inside tile for sampling (>=1)");
DEFINE_uint32(atlas_imgpack_quick_window_max_samples,
              500000,
              "Max total samples across all tiles/channels for quick window");

// Defined in z3dimg.cpp; shared here so 3D preview assembly can reuse the same concurrency limit.
DECLARE_uint32(atlas_ng_precomputed_3d_max_concurrent_block_reads);

DEFINE_bool(
  atlas_ng_precomputed_use_batched_chunk_reads,
  false,
  "Experimental: replace Neuroglancer per-chunk fan-out waits with bounded batched chunk reads in 2D/synchronous paths.");

#if 0
DEFINE_uint32(atlas_readRegionToImg_version,
              1,
              "Which version of readRegionToImg to use, value can be 0-1, default is 1");
#endif

namespace {

struct MaxOp
{
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

[[nodiscard]] size_t neuroglancerChunkReadWindow(size_t chunkCount)
{
  CHECK(chunkCount > 0);
  const size_t logicalCores = std::max<size_t>(1, nim::ZCpuInfo::instance().nLogicalCores);
  return std::min(logicalCores, chunkCount);
}

folly::coro::Task<std::shared_ptr<nim::ZImg>>
readNeuroglancerChunkBestEffortAsync(const nim::ZNeuroglancerPrecomputedVolume& volume,
                                     nim::ZNeuroglancerPrecomputedVolume::Chunk chunk)
{
  try {
    co_return co_await volume.readChunkAsync(std::move(chunk));
  }
  catch (const std::exception&) {
    co_return std::shared_ptr<nim::ZImg>();
  }
  catch (...) {
    co_return std::shared_ptr<nim::ZImg>();
  }
}

template<typename ChunkFn>
void forEachNeuroglancerChunkBestEffortBlocking(const nim::ZNeuroglancerPrecomputedVolume& volume,
                                                const std::vector<nim::ZNeuroglancerPrecomputedVolume::Chunk>& chunks,
                                                ChunkFn&& onChunk)
{
  if (chunks.empty()) {
    return;
  }

  if (!FLAGS_atlas_ng_precomputed_use_batched_chunk_reads) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chunks.size()), [&](const tbb::blocked_range<size_t>& r) {
      for (size_t i = r.begin(); i != r.end(); ++i) {
        std::shared_ptr<nim::ZImg> chunkImg;
        try {
          chunkImg = volume.readChunkBlocking(chunks[i]);
        }
        catch (const std::exception&) {
          continue;
        }
        catch (...) {
          continue;
        }
        if (!chunkImg) {
          continue;
        }
        onChunk(i, chunks[i], *chunkImg);
      }
    });
    return;
  }

  const size_t maxConcurrent = neuroglancerChunkReadWindow(chunks.size());
  std::vector<folly::coro::Task<std::shared_ptr<nim::ZImg>>> tasks;
  tasks.reserve(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    tasks.push_back(readNeuroglancerChunkBestEffortAsync(volume, chunks[i]));
  }

  std::vector<std::shared_ptr<nim::ZImg>> chunkImgs =
    folly::coro::blockingWait(folly::coro::collectAllWindowed(std::move(tasks), maxConcurrent));
  for (size_t i = 0; i < chunkImgs.size(); ++i) {
    if (!chunkImgs[i]) {
      continue;
    }
    onChunk(i, chunks[i], *chunkImgs[i]);
  }
}

} // namespace

namespace nim {

namespace {

void reportFileReadBytesToStatsSink(void* user, const ZIoReadStatsContext& ctx, size_t bytes)
{
  auto* statsSink = static_cast<ZImgReadStatsSink*>(user);
  if (!statsSink) {
    return;
  }
  statsSink->onUnderlyingIoBytes(ZImgReadStatsContext{ctx.channel, ctx.round}, ZImgUnderlyingIoKind::File, bytes);
}

template<typename TValue>
std::optional<TValue> tryGetCachedNeuroglancerValueAtBaseVoxel(const ZNeuroglancerPrecomputedVolume& vol,
                                                               const std::set<std::array<size_t, 3>>& ratios,
                                                               const ZImgInfo& baseInfo,
                                                               size_t x,
                                                               size_t y,
                                                               size_t z,
                                                               size_t c)
{
  if (c >= baseInfo.numChannels) {
    return std::nullopt;
  }
  if (x >= baseInfo.width || y >= baseInfo.height || z >= baseInfo.depth) {
    return std::nullopt;
  }

  const int64_t ix = static_cast<int64_t>(x);
  const int64_t iy = static_cast<int64_t>(y);
  const int64_t iz = static_cast<int64_t>(z);

  for (const auto& ratio : ratios) {
    auto scaleIndexOpt = vol.scaleIndexForRatio(ratio);
    if (!scaleIndexOpt) {
      continue;
    }
    const auto chunks = vol.chunksIntersectingBaseBox(*scaleIndexOpt, {ix, iy, iz}, {ix + 1, iy + 1, iz + 1});
    for (const auto& chunk : chunks) {
      auto chunkImg = vol.tryGetCachedChunk(chunk);
      if (!chunkImg) {
        continue;
      }
      if (chunkImg->isEmpty()) {
        continue;
      }

      const int64_t lx = ix - chunk.baseStart[0];
      const int64_t ly = iy - chunk.baseStart[1];
      const int64_t lz = iz - chunk.baseStart[2];
      CHECK(lx >= 0 && ly >= 0 && lz >= 0);

      const int64_t rx = static_cast<int64_t>(ratio[0]);
      const int64_t ry = static_cast<int64_t>(ratio[1]);
      const int64_t rz = static_cast<int64_t>(ratio[2]);
      CHECK(rx > 0 && ry > 0 && rz > 0);

      const size_t vx = static_cast<size_t>(lx / rx);
      const size_t vy = static_cast<size_t>(ly / ry);
      const size_t vz = static_cast<size_t>(lz / rz);

      CHECK(vx < chunkImg->width());
      CHECK(vy < chunkImg->height());
      CHECK(vz < chunkImg->depth());
      return chunkImg->value<TValue>(vx, vy, vz, c, 0);
    }
  }

  return std::nullopt;
}

template<typename TVoxel>
void pasteNeuroglancerSegmentationChunkAsRgbTyped(const ZImg& src,
                                                  ZImg& dst,
                                                  const ZVoxelCoordinate& start /*dst coords*/)
{
  CHECK(dst.isType<uint8_t>());
  CHECK(dst.numChannels() == 3);
  CHECK(src.voxelFormat() == VoxelFormat::Unsigned);
  CHECK(src.numTimes() == 1);
  CHECK(src.numChannels() == 1);

  const index_t dstW = static_cast<index_t>(dst.width());
  const index_t dstH = static_cast<index_t>(dst.height());
  const index_t dstD = static_cast<index_t>(dst.depth());
  const index_t srcW = static_cast<index_t>(src.width());
  const index_t srcH = static_cast<index_t>(src.height());
  const index_t srcD = static_cast<index_t>(src.depth());

  const index_t dstX0 = std::max<index_t>(0, start.x);
  const index_t dstY0 = std::max<index_t>(0, start.y);
  const index_t dstZ0 = std::max<index_t>(0, start.z);
  const index_t dstX1 = std::min(dstW, start.x + srcW);
  const index_t dstY1 = std::min(dstH, start.y + srcH);
  const index_t dstZ1 = std::min(dstD, start.z + srcD);
  if (dstX0 >= dstX1 || dstY0 >= dstY1 || dstZ0 >= dstZ1) {
    return;
  }

  const index_t srcX0 = dstX0 - start.x;
  const index_t srcY0 = dstY0 - start.y;
  const index_t srcZ0 = dstZ0 - start.z;
  CHECK(srcX0 >= 0 && srcY0 >= 0 && srcZ0 >= 0);

  const auto dstPlaneStride = static_cast<size_t>(dst.width() * dst.height());
  const auto dstRowStride = static_cast<size_t>(dst.width());
  const auto srcPlaneStride = static_cast<size_t>(src.width() * src.height());
  const auto srcRowStride = static_cast<size_t>(src.width());

  uint8_t* dstR = dst.channelData<uint8_t>(0);
  uint8_t* dstG = dst.channelData<uint8_t>(1);
  uint8_t* dstB = dst.channelData<uint8_t>(2);
  const TVoxel* srcIds = src.channelData<TVoxel>(0);

  for (index_t z = dstZ0; z < dstZ1; ++z) {
    const index_t localZ = z - dstZ0;
    const size_t dstPlaneOff = static_cast<size_t>(z) * dstPlaneStride;
    const size_t srcPlaneOff = static_cast<size_t>(srcZ0 + localZ) * srcPlaneStride;

    for (index_t y = dstY0; y < dstY1; ++y) {
      const index_t localY = y - dstY0;
      const size_t dstRowOff = dstPlaneOff + static_cast<size_t>(y) * dstRowStride + static_cast<size_t>(dstX0);
      const size_t srcRowOff =
        srcPlaneOff + static_cast<size_t>(srcY0 + localY) * srcRowStride + static_cast<size_t>(srcX0);

      for (index_t x = 0; x < dstX1 - dstX0; ++x) {
        const uint64_t id = static_cast<uint64_t>(srcIds[srcRowOff + static_cast<size_t>(x)]);
        const col4 c = neuroglancer::labelColorForId(id);
        const size_t dstIdx = dstRowOff + static_cast<size_t>(x);
        dstR[dstIdx] = c.r;
        dstG[dstIdx] = c.g;
        dstB[dstIdx] = c.b;
      }
    }
  }
}

void pasteNeuroglancerSegmentationChunkAsRgb(const ZImg& src, ZImg& dst, const ZVoxelCoordinate& start)
{
  CHECK(src.voxelFormat() == VoxelFormat::Unsigned);
  switch (src.bytesPerVoxel()) {
    case 1:
      pasteNeuroglancerSegmentationChunkAsRgbTyped<uint8_t>(src, dst, start);
      return;
    case 2:
      pasteNeuroglancerSegmentationChunkAsRgbTyped<uint16_t>(src, dst, start);
      return;
    case 4:
      pasteNeuroglancerSegmentationChunkAsRgbTyped<uint32_t>(src, dst, start);
      return;
    case 8:
      pasteNeuroglancerSegmentationChunkAsRgbTyped<uint64_t>(src, dst, start);
      return;
    default:
      break;
  }
  CHECK(false) << "Unsupported voxel type for Neuroglancer segmentation conversion: bytesPerVoxel=" << src.bytesPerVoxel();
}

template<typename TVoxel>
void pasteNeuroglancerSegmentationChunkAsRgbComponentTyped(const ZImg& src,
                                                           ZImg& dst,
                                                           const ZVoxelCoordinate& start /*dst coords*/,
                                                           size_t component)
{
  CHECK(dst.isType<uint8_t>());
  CHECK(dst.numChannels() == 1);
  CHECK(component < 3);
  CHECK(src.voxelFormat() == VoxelFormat::Unsigned);
  CHECK(src.numTimes() == 1);
  CHECK(src.numChannels() == 1);

  const index_t dstW = static_cast<index_t>(dst.width());
  const index_t dstH = static_cast<index_t>(dst.height());
  const index_t dstD = static_cast<index_t>(dst.depth());
  const index_t srcW = static_cast<index_t>(src.width());
  const index_t srcH = static_cast<index_t>(src.height());
  const index_t srcD = static_cast<index_t>(src.depth());

  const index_t dstX0 = std::max<index_t>(0, start.x);
  const index_t dstY0 = std::max<index_t>(0, start.y);
  const index_t dstZ0 = std::max<index_t>(0, start.z);
  const index_t dstX1 = std::min(dstW, start.x + srcW);
  const index_t dstY1 = std::min(dstH, start.y + srcH);
  const index_t dstZ1 = std::min(dstD, start.z + srcD);
  if (dstX0 >= dstX1 || dstY0 >= dstY1 || dstZ0 >= dstZ1) {
    return;
  }

  const index_t srcX0 = dstX0 - start.x;
  const index_t srcY0 = dstY0 - start.y;
  const index_t srcZ0 = dstZ0 - start.z;
  CHECK(srcX0 >= 0 && srcY0 >= 0 && srcZ0 >= 0);

  const auto dstPlaneStride = static_cast<size_t>(dst.width() * dst.height());
  const auto dstRowStride = static_cast<size_t>(dst.width());
  const auto srcPlaneStride = static_cast<size_t>(src.width() * src.height());
  const auto srcRowStride = static_cast<size_t>(src.width());

  uint8_t* dstOut = dst.channelData<uint8_t>(0);
  const TVoxel* srcIds = src.channelData<TVoxel>(0);

  for (index_t z = dstZ0; z < dstZ1; ++z) {
    const index_t localZ = z - dstZ0;
    const size_t dstPlaneOff = static_cast<size_t>(z) * dstPlaneStride;
    const size_t srcPlaneOff = static_cast<size_t>(srcZ0 + localZ) * srcPlaneStride;

    for (index_t y = dstY0; y < dstY1; ++y) {
      const index_t localY = y - dstY0;
      const size_t dstRowOff = dstPlaneOff + static_cast<size_t>(y) * dstRowStride + static_cast<size_t>(dstX0);
      const size_t srcRowOff =
        srcPlaneOff + static_cast<size_t>(srcY0 + localY) * srcRowStride + static_cast<size_t>(srcX0);

      for (index_t x = 0; x < dstX1 - dstX0; ++x) {
        const uint64_t id = static_cast<uint64_t>(srcIds[srcRowOff + static_cast<size_t>(x)]);
        const col4 c = neuroglancer::labelColorForId(id);
        uint8_t v = 0_u8;
        switch (component) {
          case 0:
            v = c.r;
            break;
          case 1:
            v = c.g;
            break;
          case 2:
            v = c.b;
            break;
          default:
            break;
        }
        dstOut[dstRowOff + static_cast<size_t>(x)] = v;
      }
    }
  }
}

void pasteNeuroglancerSegmentationChunkAsRgbComponent(const ZImg& src,
                                                      ZImg& dst,
                                                      const ZVoxelCoordinate& start,
                                                      size_t component)
{
  CHECK(src.voxelFormat() == VoxelFormat::Unsigned);
  switch (src.bytesPerVoxel()) {
    case 1:
      pasteNeuroglancerSegmentationChunkAsRgbComponentTyped<uint8_t>(src, dst, start, component);
      return;
    case 2:
      pasteNeuroglancerSegmentationChunkAsRgbComponentTyped<uint16_t>(src, dst, start, component);
      return;
    case 4:
      pasteNeuroglancerSegmentationChunkAsRgbComponentTyped<uint32_t>(src, dst, start, component);
      return;
    case 8:
      pasteNeuroglancerSegmentationChunkAsRgbComponentTyped<uint64_t>(src, dst, start, component);
      return;
    default:
      break;
  }
  CHECK(false) << "Unsupported voxel type for Neuroglancer segmentation conversion: bytesPerVoxel=" << src.bytesPerVoxel();
}

} // namespace

ZImgPackSubBlock::ZImgPackSubBlock(const std::shared_ptr<ZImg>& img,
                                   size_t ratio,
                                   index_t t,
                                   index_t z,
                                   index_t x,
                                   index_t y,
                                   size_t width,
                                   size_t height)
  : ZImgSubBlock(t, x, y, z, width, height, 1, ratio, ratio, 1)
  , m_img(img)
{}

std::shared_ptr<ZImg> ZImgPackSubBlock::read() const
{
  return m_img;
}

ZImgInfo ZImgPackSubBlock::readInfo() const
{
  return m_img->info();
}

ZImgPack::ZImgPack(ZImgSource imgSource, ZImgInfo* pInfo, std::vector<std::shared_ptr<ZImgSubBlock>>* pSceneSubBlocks)
  : m_imgSource(std::move(imgSource))
  , m_hasUnsavedChange(false)
  , m_diskCached(true)
{
  std::vector<std::shared_ptr<ZImgSubBlock>> sceneSubBlock;
  if (pInfo && pSceneSubBlocks) {
    m_imgInfo.swap(*pInfo);
    sceneSubBlock.swap(*pSceneSubBlocks);
  } else {
    m_imgInfo = ZImg::readImgInfo(m_imgSource, &sceneSubBlock);
  }
  m_imgMetaData = ZImg::readImgMetadata(m_imgSource);

  m_minMaxState = MinMaxState::Invalid;

  bool hasPyramidal = false;
  for (const auto& b : sceneSubBlock) {
    if (b->xRatio > 1) {
      hasPyramidal = true;
      break;
    }
  }

  // bool needScale = Z3DGpuInfo::instance().needScaleDataForTexture(m_imgInfo.width, m_imgInfo.height,
  // m_imgInfo.depth);
  if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold) {
    VLOG(1) << "read all";
    m_diskCached = false;
    m_img = ZImg(m_imgSource);
    m_img.computeMinMax(m_minIntensity, m_maxIntensity);
    m_minMaxState = MinMaxState::Complete;
    buildFastReadIndex(sceneSubBlock);
  } else if (hasPyramidal) {
    VLOG(1) << fmt::format("has pyramidal: {}", hasPyramidal);
    buildFastReadIndex(sceneSubBlock);
  } else {
    if (FLAGS_atlas_imgpack_defer_pyramidal) {
      VLOG(1) << "building pyramidal index only (no I/O)";
      buildPyramidalIndexOnly();
    } else {
      VLOG(1) << "building pyramidal";
      buildPyramidal();
    }
  }
  // Derive a fast initial display window for float data when we don't have full min/max
  computeQuickWindowIfNeeded();
  updateDerivedData();
  VLOG(1) << "imgpack done";
}

ZImgPack::ZImgPack(std::shared_ptr<ZNeuroglancerPrecomputedVolume> ngVolume)
  : m_hasUnsavedChange(false)
  , m_ngVolume(std::move(ngVolume))
  , m_diskCached(true)
{
  CHECK(m_ngVolume);

  m_imgInfo = m_ngVolume->baseImgInfo();
  m_imgMetaData = ZImgMetadata{};
  m_minMaxState = MinMaxState::Invalid;

  m_imgSource = ZImgSource{};
  m_imgSource.filenames = QStringList{m_ngVolume->rootUrl()};
  m_imgSource.catDim = Dimension::Z;
  m_imgSource.catScenes = false;
  m_imgSource.scene = 0;
  m_imgSource.format = FileFormat::Unknown;
  m_imgSource.totalFileSize = 0;

  m_allTiles.clear();
  m_rtToTileIndice.clear();
  m_rtToTileBoxRTree.clear();
  m_pyramidalRatios.clear();
  for (const auto& ratio : m_ngVolume->availableRatios()) {
    m_pyramidalRatios.insert(ratio);
  }
  m_pyramidalRatios.insert({1, 1, 1});

  updateDerivedData();
  VLOG(1) << "imgpack neuroglancer done";
}


const QString& ZImgPack::sizeInfo() const
{
  if (m_sizeInfo.isEmpty()) {
    m_sizeInfo = QString("%1: (w:%2, h:%3, d:%4, c:%5")
                   .arg(m_imgInfo.typeAsQString())
                   .arg(m_imgInfo.width)
                   .arg(m_imgInfo.height)
                   .arg(m_imgInfo.depth)
                   .arg(m_imgInfo.numChannels);
    if (m_imgInfo.numTimes > 1) {
      m_sizeInfo += QString(", t:%1)").arg(m_imgInfo.numTimes);
    } else {
      m_sizeInfo += ")";
    }
  }
  return m_sizeInfo;
}

const QString& ZImgPack::detailedInfo() const
{
  if (m_detailedInfo.isEmpty()) {
    QStringList info;
    info << QString("Width: %1").arg(m_imgInfo.width);
    info << QString("Height: %1").arg(m_imgInfo.height);
    info << QString("Depth: %1").arg(m_imgInfo.depth);
    info << QString("Number of Channels: %1").arg(m_imgInfo.numChannels);
    info << QString("Number of Times: %1").arg(m_imgInfo.numTimes);
    info << QString("Bytes per Voxel: %1").arg(m_imgInfo.bytesPerVoxel);
    info << QString("Voxel Format: %1").arg(enumToQString(m_imgInfo.voxelFormat));
    info << QString("Voxel Size Unit: %1").arg(enumToQString(m_imgInfo.voxelSizeUnit));
    info << QString("Voxel Size X: %1").arg(m_imgInfo.voxelSizeX);
    info << QString("Voxel Size Y: %1").arg(m_imgInfo.voxelSizeY);
    info << QString("Voxel Size Z: %1").arg(m_imgInfo.voxelSizeZ);
    if (m_imgInfo.lastChannelIsAlphaChannel && m_imgInfo.numChannels > 0) {
      info << QString("Alpha Channel: %1").arg(m_imgInfo.numChannels - 1);
    }
    if (m_imgInfo.validBitCount > 0) {
      info << QString("Valid Bit Count: %1").arg(m_imgInfo.validBitCount);
    }
    m_detailedInfo = info.join("\n");
    m_detailedInfo += "\n\n";

    for (const auto& meta : m_imgMetaData.topLevelAttachments()) {
      m_detailedInfo += meta.toQString();
      m_detailedInfo += "\n";
    }
  }
  return m_detailedInfo;
}

QString ZImgPack::neuroglancerRootUrl() const
{
  CHECK(m_ngVolume);
  return m_ngVolume->rootUrl();
}

namespace {

[[nodiscard]] std::optional<QString> normalizeNeuroglancerExternalSourceDirUrl(const ZNeuroglancerPrecomputedVolume& vol,
                                                                              QString userText,
                                                                              QString* errorMsg)
{
  if (errorMsg) {
    errorMsg->clear();
  }

  QString s = userText.trimmed();
  if (s.isEmpty()) {
    if (errorMsg) {
      *errorMsg = QStringLiteral("URL/path is empty");
    }
    return std::nullopt;
  }

  // Accept:
  // - relative dir (e.g. "mesh" or "skeletons") resolved against the segmentation root url
  // - absolute urls like "precomputed://gs://..." or "https://..."
  if (s.contains("://") || s.startsWith("gs://", Qt::CaseInsensitive)) {
    try {
      // normalizeRootUrl enforces a trailing slash and strips a direct /info suffix.
      const QString normalized = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(s));
      return normalized;
    }
    catch (const std::exception& e) {
      if (errorMsg) {
        *errorMsg = QString::fromUtf8(e.what());
      }
      return std::nullopt;
    }
  }

  if (!s.endsWith('/')) {
    s += '/';
  }
  const QUrl base(vol.rootUrl());
  const QUrl dirUrl = base.resolved(QUrl(s));
  if (!dirUrl.isValid()) {
    if (errorMsg) {
      *errorMsg = QStringLiteral("Invalid URL/path");
    }
    return std::nullopt;
  }
  QString normalized = dirUrl.toString(QUrl::StripTrailingSlash);
  if (!normalized.endsWith('/')) {
    normalized += '/';
  }
  return normalized;
}

} // namespace

bool ZImgPack::hasNeuroglancerMeshSourceOverride() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return !m_ngMeshSourceOverrideUrl.trimmed().isEmpty();
}

QString ZImgPack::neuroglancerMeshSourceOverrideUrl() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return m_ngMeshSourceOverrideUrl.trimmed();
}

bool ZImgPack::hasNeuroglancerSkeletonSourceOverride() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return !m_ngSkeletonSourceOverrideUrl.trimmed().isEmpty();
}

QString ZImgPack::neuroglancerSkeletonSourceOverrideUrl() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return m_ngSkeletonSourceOverrideUrl.trimmed();
}

bool ZImgPack::hasNeuroglancerAnnotationsSourceOverride() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return !m_ngAnnotationsSourceOverrideUrl.trimmed().isEmpty();
}

QString ZImgPack::neuroglancerAnnotationsSourceOverrideUrl() const
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  return m_ngAnnotationsSourceOverrideUrl.trimmed();
}

bool ZImgPack::hasNeuroglancerMeshSourceConfigured() const
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    return false;
  }
  if (m_ngVolume->hasMeshDirectory()) {
    return true;
  }
  return hasNeuroglancerMeshSourceOverride();
}

bool ZImgPack::hasNeuroglancerSkeletonSourceConfigured() const
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    return false;
  }
  if (m_ngVolume->hasSkeletonDirectory()) {
    return true;
  }
  return hasNeuroglancerSkeletonSourceOverride();
}

bool ZImgPack::hasNeuroglancerAnnotationsSourceConfigured() const
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    return false;
  }
  return hasNeuroglancerAnnotationsSourceOverride();
}

bool ZImgPack::setNeuroglancerMeshSourceOverride(QString userText, QString* errorMsg)
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    if (errorMsg) {
      *errorMsg = QStringLiteral("Not a Neuroglancer segmentation dataset");
    }
    return false;
  }

  QString err;
  const auto urlOpt = normalizeNeuroglancerExternalSourceDirUrl(*m_ngVolume, std::move(userText), &err);
  if (!urlOpt) {
    if (errorMsg) {
      *errorMsg = err;
    }
    return false;
  }

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngMeshSourceOverrideUrl = *urlOpt;
  m_ngMeshSourceOverride.reset();
  return true;
}

bool ZImgPack::setNeuroglancerSkeletonSourceOverride(QString userText, QString* errorMsg)
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    if (errorMsg) {
      *errorMsg = QStringLiteral("Not a Neuroglancer segmentation dataset");
    }
    return false;
  }

  QString err;
  const auto urlOpt = normalizeNeuroglancerExternalSourceDirUrl(*m_ngVolume, std::move(userText), &err);
  if (!urlOpt) {
    if (errorMsg) {
      *errorMsg = err;
    }
    return false;
  }

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngSkeletonSourceOverrideUrl = *urlOpt;
  m_ngSkeletonSourceOverride.reset();
  return true;
}

bool ZImgPack::setNeuroglancerAnnotationsSourceOverride(QString userText, QString* errorMsg)
{
  if (!m_ngVolume || !m_ngVolume->isSegmentation()) {
    if (errorMsg) {
      *errorMsg = QStringLiteral("Not a Neuroglancer segmentation dataset");
    }
    return false;
  }

  QString err;
  const auto urlOpt = normalizeNeuroglancerExternalSourceDirUrl(*m_ngVolume, std::move(userText), &err);
  if (!urlOpt) {
    if (errorMsg) {
      *errorMsg = err;
    }
    return false;
  }

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngAnnotationsSourceOverrideUrl = *urlOpt;
  m_ngAnnotationsSourceOverride.reset();
  return true;
}

void ZImgPack::clearNeuroglancerMeshSourceOverride()
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngMeshSourceOverrideUrl.clear();
  m_ngMeshSourceOverride.reset();
}

void ZImgPack::clearNeuroglancerSkeletonSourceOverride()
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngSkeletonSourceOverrideUrl.clear();
  m_ngSkeletonSourceOverride.reset();
}

void ZImgPack::clearNeuroglancerAnnotationsSourceOverride()
{
  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  m_ngAnnotationsSourceOverrideUrl.clear();
  m_ngAnnotationsSourceOverride.reset();
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> ZImgPack::loadNeuroglancerMeshSourceBlocking() const
{
  CHECK(m_ngVolume);
  CHECK(m_ngVolume->isSegmentation());

  QString overrideUrl;
  {
    const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
    overrideUrl = m_ngMeshSourceOverrideUrl.trimmed();
    if (!overrideUrl.isEmpty() && m_ngMeshSourceOverride) {
      return m_ngMeshSourceOverride;
    }
  }

  if (overrideUrl.isEmpty()) {
    return m_ngVolume->loadMeshSourceBlocking();
  }

  const ZImgInfo& info = m_ngVolume->baseImgInfo();
  const std::array<double, 3> baseResolutionNm{info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ};

  auto loaded = ZNeuroglancerPrecomputedMeshSource::open(QUrl(overrideUrl),
                                                         baseResolutionNm,
                                                         m_ngVolume->baseVoxelOffset(),
                                                         m_ngVolume->sharedRemoteContext());
  CHECK(loaded);

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  if (m_ngMeshSourceOverrideUrl.trimmed() == overrideUrl) {
    m_ngMeshSourceOverride = loaded;
  }
  return loaded;
}

std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> ZImgPack::loadNeuroglancerSkeletonSourceBlocking() const
{
  CHECK(m_ngVolume);
  CHECK(m_ngVolume->isSegmentation());

  QString overrideUrl;
  {
    const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
    overrideUrl = m_ngSkeletonSourceOverrideUrl.trimmed();
    if (!overrideUrl.isEmpty() && m_ngSkeletonSourceOverride) {
      return m_ngSkeletonSourceOverride;
    }
  }

  if (overrideUrl.isEmpty()) {
    return m_ngVolume->loadSkeletonSourceBlocking();
  }

  const ZImgInfo& info = m_ngVolume->baseImgInfo();
  const std::array<double, 3> baseResolutionNm{info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ};

  auto loaded = ZNeuroglancerPrecomputedSkeletonSource::open(QUrl(overrideUrl),
                                                             baseResolutionNm,
                                                             m_ngVolume->baseVoxelOffset(),
                                                             m_ngVolume->sharedRemoteContext());
  CHECK(loaded);

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  if (m_ngSkeletonSourceOverrideUrl.trimmed() == overrideUrl) {
    m_ngSkeletonSourceOverride = loaded;
  }
  return loaded;
}

std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> ZImgPack::loadNeuroglancerAnnotationsSourceBlocking() const
{
  CHECK(m_ngVolume);
  CHECK(m_ngVolume->isSegmentation());

  QString overrideUrl;
  {
    const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
    overrideUrl = m_ngAnnotationsSourceOverrideUrl.trimmed();
    if (!overrideUrl.isEmpty() && m_ngAnnotationsSourceOverride) {
      return m_ngAnnotationsSourceOverride;
    }
  }

  CHECK(!overrideUrl.isEmpty()) << "Neuroglancer annotations source is not configured";

  const ZImgInfo& info = m_ngVolume->baseImgInfo();
  const std::array<double, 3> baseResolutionNm{info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ};

  auto loaded = ZNeuroglancerPrecomputedAnnotationsSource::open(QUrl(overrideUrl),
                                                                baseResolutionNm,
                                                                m_ngVolume->baseVoxelOffset(),
                                                                m_ngVolume->sharedRemoteContext());
  CHECK(loaded);

  const std::lock_guard<std::mutex> lock(m_ngExternalSourcesMutex);
  if (m_ngAnnotationsSourceOverrideUrl.trimmed() == overrideUrl) {
    m_ngAnnotationsSourceOverride = loaded;
  }
  return loaded;
}

std::unique_ptr<ZImgPack> ZImgPack::makeNeuroglancerSegmentationRgbFor3D() const
{
  CHECK(m_ngVolume);
  CHECK(m_ngVolume->isSegmentation());

  auto out = std::make_unique<ZImgPack>(m_ngVolume);
  out->m_ngSegmentationRgbFor3D = true;

  // Present a 3-channel uint8 RGB view to 3D rendering code.
  out->m_imgInfo.numChannels = 3;
  out->m_imgInfo.bytesPerVoxel = 1;
  out->m_imgInfo.voxelFormat = VoxelFormat::Unsigned;
  out->m_imgInfo.validBitCount = 8;
  out->m_imgInfo.lastChannelIsAlphaChannel = false;
  out->m_imgInfo.createDefaultDescriptions();

  // Make the intent explicit in the UI and use standard RGB channel colors.
  out->m_imgInfo.channelNames = {"R", "G", "B"};
  out->m_imgInfo.channelColors = {col4{255, 0, 0}, col4{0, 255, 0}, col4{0, 0, 255}};

  out->updateDerivedData();
  return out;
}

void ZImgPack::setChannelColor(size_t c, col4 col)
{
  CHECK(c < m_imgInfo.numChannels);
  m_imgInfo.channelColors[c] = col;
  if (!m_diskCached) {
    m_img.infoRef().channelColors[c] = col;
  }
}

void ZImgPack::save(const QString& fileName, FileFormat format, const ZImgWriteParameters& paras)
{
  if (m_ngVolume) {
    throw ZException("Saving Neuroglancer precomputed volumes is not supported (read-only dataset)");
  }
  if (m_diskCached) {
    ZImg::writeImg(fileName, *this, format, paras);
  } else {
    m_img.save(fileName, format, paras);
    m_diskCached = true;
  }
  m_imgSource = ZImgSource(fileName);
  m_hasUnsavedChange = false;

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    ZImgCache::instance().remove(ImageCacheHashKeyType(this, i));
  }

  // Invalidate the dataset fingerprint so any future ZImgRegionCache keys use the new
  // (post-save) dataset identity. ZImgRegionCache is keyed by this fingerprint, so we do
  // not need to clear the global cache for correctness.
  {
    const std::lock_guard<std::mutex> lock(m_datasetFingerprintMutex);
    m_datasetFingerprintValid.store(false, std::memory_order_release);
    m_datasetFingerprint.fill(0);
  }

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  std::vector<ZImgInfo> infos = ZImg::readImgInfos(m_imgSource.filenames[0], &subBlocks, m_imgSource.format);
  CHECK(!infos.empty() && !subBlocks.empty());
  m_imgInfo = infos[0];
  m_imgMetaData = ZImg::readImgMetadata(m_imgSource);
  buildFastReadIndex(subBlocks[0]);

  updateDerivedData();
}

bool ZImgPack::needUpdate(const QRectF& viewport,
                          double scale,
                          const QRectF& oldViewport,
                          double oldScale,
                          size_t t,
                          size_t z,
                          bool mip) const
{
  if (!m_diskCached) {
    return false;
  }

  // The first viewportChanged() after loading a dataset can arrive before the
  // caller has a valid "previous" scale (e.g. m_lastScale defaults to 0).
  // Treat that as needing an update instead of crashing in ratioForScale(),
  // which requires strictly-positive scales.
  // Use `!(oldScale > 0)` so NaNs are treated as invalid too.
  if (!(oldScale > 0)) {
    return true;
  }

  const double zScale = m_ngVolume ? scale : 1.0;
  const double oldZScale = m_ngVolume ? oldScale : 1.0;
  auto readRatio = ratioForScale(scale, scale, zScale);
  auto oldReadRatio = ratioForScale(oldScale, oldScale, oldZScale);
  if (readRatio != oldReadRatio) {
    return true;
  }

  if (m_imgInfo.depth == 1) {
    mip = false;
  }

  if (mip) { // for now mip is always full-resolution
    return false;
  }

  if (m_ngVolume) {
    CHECK(t == 0);
    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(readRatio);
    CHECK(scaleIndexOpt);

    const int64_t z0 = static_cast<int64_t>(z);
    const std::array<int64_t, 3> box1Start{static_cast<int64_t>(std::floor(viewport.x())),
                                           static_cast<int64_t>(std::floor(viewport.y())),
                                           z0};
    const std::array<int64_t, 3> box1End{static_cast<int64_t>(std::ceil(viewport.right())),
                                         static_cast<int64_t>(std::ceil(viewport.bottom())),
                                         z0 + 1};
    const std::array<int64_t, 3> box2Start{static_cast<int64_t>(std::floor(oldViewport.x())),
                                           static_cast<int64_t>(std::floor(oldViewport.y())),
                                           z0};
    const std::array<int64_t, 3> box2End{static_cast<int64_t>(std::ceil(oldViewport.right())),
                                         static_cast<int64_t>(std::ceil(oldViewport.bottom())),
                                         z0 + 1};

    using ChunkKey = std::tuple<size_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t>;
    auto chunkKeyOf = [](const ZNeuroglancerPrecomputedVolume::Chunk& c) -> ChunkKey {
      return std::make_tuple(c.scaleIndex,
                             c.globalStart[0],
                             c.globalEnd[0],
                             c.globalStart[1],
                             c.globalEnd[1],
                             c.globalStart[2],
                             c.globalEnd[2]);
    };

    const auto chunks1 = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt, box1Start, box1End);
    const auto chunks2 = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt, box2Start, box2End);

    std::set<ChunkKey> keys1;
    std::set<ChunkKey> keys2;
    for (const auto& c : chunks1) {
      keys1.insert(chunkKeyOf(c));
    }
    for (const auto& c : chunks2) {
      keys2.insert(chunkKeyOf(c));
    }
    return keys1 != keys2;
  }

#if 1
  auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
  if (tiit != m_rtToTileBoxRTree.end()) {
    TileBoxType queryBox1(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y()), z),
                          TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom()), z));
    TileBoxType queryBox2(TileCornerType(std::floor(oldViewport.x()), std::floor(oldViewport.y()), z),
                          TileCornerType(std::ceil(oldViewport.right()), std::ceil(oldViewport.bottom()), z));
    std::set<size_t> queryResult1;
    std::set<size_t> queryResult2;
    tiit->second->query(bgi::intersects(queryBox1),
                        boost::make_function_output_iterator([&queryResult1](const auto& val) {
                          queryResult1.insert(val.second);
                        }));
    tiit->second->query(bgi::intersects(queryBox2),
                        boost::make_function_output_iterator([&queryResult2](const auto& val) {
                          queryResult2.insert(val.second);
                        }));
    return queryResult1 != queryResult2;
  }
#else
  auto tiit = m_rtzToTileIndice.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    for (size_t i = 0; i < tileIndice.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
      QRectF tileRect(tile.x, tile.y, tile.width, tile.height);
      bool vpIntersect = tileRect.intersects(viewport);
      bool oldVpIntersect = tileRect.intersects(oldViewport);
      if (vpIntersect != oldVpIntersect) {
        return true;
      }
    }
  }
#endif

  return false;
}

void ZImgPack::retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>>& imgs,
                                   std::vector<QPoint>& locs,
                                   std::vector<double>& scales,
                                   size_t z,
                                   size_t t,
                                   const QRectF& viewport,
                                   double scale) const
{
  CHECK(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  const double zScale = m_ngVolume ? scale : 1.0;
  auto readRatio = ratioForScale(scale, scale, zScale);

  if (m_ngVolume) {
    CHECK(t == 0);
    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(readRatio);
    CHECK(scaleIndexOpt);

    const int64_t z0 = static_cast<int64_t>(z);
    const std::array<int64_t, 3> boxStart{static_cast<int64_t>(std::floor(viewport.x())),
                                          static_cast<int64_t>(std::floor(viewport.y())),
                                          z0};
    const std::array<int64_t, 3> boxEnd{static_cast<int64_t>(std::ceil(viewport.right())),
                                        static_cast<int64_t>(std::ceil(viewport.bottom())),
                                        z0 + 1};

    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt, boxStart, boxEnd);
    if (chunks.empty()) {
      return;
    }

    const double tileScale = static_cast<double>(readRatio[0]);
    const int64_t ratioZ = static_cast<int64_t>(readRatio[2]);
    CHECK(ratioZ > 0);

    std::vector<std::shared_ptr<ZImg>> tmpImgs(chunks.size());
    std::vector<QPoint> tmpLocs(chunks.size());

    auto toIntChecked = [](int64_t v) -> int {
      if (v < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
          v > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        throw ZException(fmt::format("Neuroglancer chunk coordinate {} is out of QPoint range", v));
      }
      return static_cast<int>(v);
    };

    forEachNeuroglancerChunkBestEffortBlocking(
      *m_ngVolume,
      chunks,
      [&](size_t i, const auto& chunk, const ZImg& chunkImg) {
        const int64_t localZBase = z0 - chunk.baseStart[2];
        CHECK(localZBase >= 0);
        const int64_t localZ = localZBase / ratioZ;
        CHECK(localZ >= 0);
        CHECK(static_cast<size_t>(localZ) < chunkImg.depth());

        ZImg sliceImg = chunkImg.crop(ZImgRegion(0,
                                                 static_cast<index_t>(chunkImg.width()),
                                                 0,
                                                 static_cast<index_t>(chunkImg.height()),
                                                 static_cast<index_t>(localZ),
                                                 static_cast<index_t>(localZ + 1)));
        tmpImgs[i] = std::make_shared<ZImg>(std::move(sliceImg));
        tmpLocs[i] = QPoint(toIntChecked(chunk.baseStart[0]), toIntChecked(chunk.baseStart[1]));
      });

    for (size_t i = 0; i < tmpImgs.size(); ++i) {
      if (!tmpImgs[i]) {
        continue;
      }
      imgs.push_back(std::move(tmpImgs[i]));
      locs.push_back(tmpLocs[i]);
      scales.push_back(tileScale);
    }
    return;
  }

#if 1
  bool finish =
    false; // in case of multiple image with pyramidal levels (like czi) concated together to form
           // one stack, some z slice might have less pyramidal levels so we can not trust readRatio in such case
  while (!finish) {
    auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
    if (tiit != m_rtToTileBoxRTree.end()) {
      TileBoxType queryBox(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y()), z),
                           TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom()), z));
      std::vector<RTreeValueType> queryResult;
      tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
      if (!queryResult.empty()) {
        VLOG(1) << "read start " << queryResult.size();
        imgs.resize(queryResult.size());
        locs.resize(queryResult.size());
        scales.resize(queryResult.size(), readRatio[0]);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, queryResult.size()), [&](const tbb::blocked_range<size_t>& r) {
          for (size_t i = r.begin(); i != r.end(); ++i) {
            const ZImgSubBlock& tile = *m_allTiles[queryResult[i].second].get();
            locs[i] = QPoint(tile.x, tile.y);
            imgs[i] = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, queryResult[i].second), tile);
          }
        });
        finish = true;
        VLOG(1) << "read end";
      } else {
        // move to previous readRatio
        finish = true;
        for (auto rit = m_pyramidalRatios.rbegin(); rit != m_pyramidalRatios.rend(); ++rit) {
          if (*rit == readRatio) {
            ++rit;
            if (rit != m_pyramidalRatios.rend()) {
              readRatio = *rit;
              finish = false;
            }
            break;
          }
        }
      }
    } else {
      finish = true;
    }
  }
#else
  auto tiit = m_rtzToTileIndice.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    for (size_t i = 0; i < tileIndice.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
      QRectF tileRect(tile.x, tile.y, tile.width, tile.height);
      if (tileRect.intersects(viewport)) {
        std::shared_ptr<ZImg>* imgPtr =
          ZImgCache::instance().getOrRead(boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
        imgs.push_back(*imgPtr);
        locs.push_back(QPoint(tile.x, tile.y));
        scales.push_back(readRatio);
        // VLOG(1) << level << " " << (1<<level) << " " << x << " " << y << " " << width << " " << height;
      }
    }
  }
#endif
}

void ZImgPack::retrieveCoveredMIPImgs(std::vector<std::shared_ptr<ZImg>>& imgs,
                                      std::vector<QPoint>& locs,
                                      std::vector<double>& scales,
                                      size_t zStart,
                                      size_t zEnd,
                                      size_t t,
                                      const QRectF&,
                                      double) const
{
  CHECK(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  if (m_ngVolume) {
    throw ZException("MIP rendering for Neuroglancer precomputed volumes is not supported yet");
  }

  if (m_mipImgs.empty()) {
    m_mipImgs.resize(m_imgInfo.numTimes);
  }
  if (m_mipImgs[t] && zStart == m_mipZStart && zEnd == m_mipZEnd) {
    imgs.push_back(m_mipImgs[t]);
    locs.emplace_back(0, 0);
    scales.push_back(1);
    return;
  }

  m_mipImgs[t] = std::make_shared<ZImg>(assembleImg(std::array<size_t, 3>{1, 1, 1}, zStart, /*c*/ -1, t));
  for (size_t z = zStart + 1; z <= zEnd; ++z) {
    m_mipImgs[t]->binaryOperation(assembleImg(std::array<size_t, 3>{1, 1, 1}, z, /*c*/ -1, t), MaxOp());
  }
  m_mipZStart = zStart;
  m_mipZEnd = zEnd;
  LOG(INFO) << "MIP: " << m_mipZStart << " " << m_mipZEnd;

  imgs.push_back(m_mipImgs[t]);
  locs.emplace_back(0, 0);
  scales.push_back(1);
}

double ZImgPack::value(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_ngVolume) {
      CHECK(t == 0);
      if (c >= m_imgInfo.numChannels) {
        return 0;
      }
      if (m_imgInfo.depth == 1) {
        mip = false;
      }
      if (mip) {
        return 0;
      }
      if (x >= m_imgInfo.width || y >= m_imgInfo.height || z >= m_imgInfo.depth) {
        return 0;
      }

      auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(std::array<size_t, 3>{1, 1, 1});
      CHECK(scaleIndexOpt);

      const int64_t ix = static_cast<int64_t>(x);
      const int64_t iy = static_cast<int64_t>(y);
      const int64_t iz = static_cast<int64_t>(z);
      const auto chunks =
        m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt, {ix, iy, iz}, {ix + 1, iy + 1, iz + 1});
      if (chunks.empty()) {
        return 0;
      }

      // Keep value() fast and non-blocking for network-backed datasets.
      // 3D paging (readRegionToImgAsync) is responsible for pulling data over the network.
      auto chunkImg = m_ngVolume->tryGetCachedChunk(chunks.front());
      if (!chunkImg) return 0;

      const int64_t lx = ix - chunks.front().baseStart[0];
      const int64_t ly = iy - chunks.front().baseStart[1];
      const int64_t lz = iz - chunks.front().baseStart[2];
      CHECK(lx >= 0 && ly >= 0 && lz >= 0);
      CHECK(static_cast<size_t>(lx) < chunkImg->width());
      CHECK(static_cast<size_t>(ly) < chunkImg->height());
      CHECK(static_cast<size_t>(lz) < chunkImg->depth());
      return chunkImg->value<double>(static_cast<size_t>(lx), static_cast<size_t>(ly), static_cast<size_t>(lz), c, 0);
    }

    if (m_imgInfo.depth == 1) {
      mip = false;
    }
    if (mip) {
      CHECK(m_mipImgs[t] && !m_mipImgs[t]->isEmpty());
      return m_mipImgs[t]->value<double>(x, y, 0, c, 0);
    } else {
      if (auto tiit = m_rtToTileIndice.find(std::make_tuple(1_uz, 1_uz, 1_uz, t)); tiit != m_rtToTileIndice.end()) {
        for (const std::vector<size_t>& tileIndice = tiit->second; auto tileIndex : tileIndice) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
          CHECK(tile.x >= 0 && tile.y >= 0 && tile.z >= 0);
          if (static_cast<index_t>(x) >= tile.x && static_cast<index_t>(x) < tile.x + tile.width &&
              static_cast<index_t>(y) >= tile.y && static_cast<index_t>(y) < tile.y + tile.height &&
              static_cast<index_t>(z) >= tile.z && static_cast<index_t>(z) < tile.z + tile.depth) {
            std::shared_ptr<ZImg> imgPtr =
              ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), tile);
            return imgPtr->value<double>(x - tile.x, y - tile.y, z - tile.z, c, 0);
          }
        }
      }
    }
    return 0;
  }
  if (mip) {
    CHECK(!m_maximumProjectedAlongZImg.isEmpty());
    return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
  }
  return m_img.value<double>(x, y, z, c, t);
}

std::optional<size_t> ZImgPack::tryFindBaseTileIndexForVoxel(size_t x, size_t y, size_t z, size_t t) const
{
  if (!m_diskCached) {
    return std::nullopt;
  }

  if (x >= m_imgInfo.width || y >= m_imgInfo.height || z >= m_imgInfo.depth) {
    return std::nullopt;
  }

  auto it = m_rtToTileBoxRTree.find(std::make_tuple(1_uz, 1_uz, 1_uz, t));
  if (it == m_rtToTileBoxRTree.end() || !it->second) {
    return std::nullopt;
  }

  const TileCornerType p(static_cast<index_t>(x), static_cast<index_t>(y), static_cast<index_t>(z));
  const TileBoxType queryBox(p, p);

  std::optional<size_t> tileIndexOpt;
  it->second->query(bgi::intersects(queryBox), boost::make_function_output_iterator([&](const auto& val) {
                      if (!tileIndexOpt.has_value()) {
                        tileIndexOpt = val.second;
                      }
                    }));
  return tileIndexOpt;
}

const ZImgSubBlock& ZImgPack::tileByIndex(size_t tileIndex) const
{
  CHECK(tileIndex < m_allTiles.size());
  CHECK(m_allTiles[tileIndex] != nullptr);
  return *m_allTiles[tileIndex].get();
}

std::shared_ptr<ZImg> ZImgPack::readTileBlocking(size_t tileIndex) const
{
  CHECK(m_diskCached);
  const ZImgSubBlock& tile = tileByIndex(tileIndex);
  return ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), tile);
}

double ZImgPack::displayValue(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_ngVolume) {
      CHECK(t == 0);
      if (m_imgInfo.depth == 1) {
        mip = false;
      }
      if (mip) {
        return 0;
      }
      // Prefer already-cached data; never trigger network I/O from displayValue()
      // (called on mouse-move, and in other hot UI paths).
      const auto vOpt =
        tryGetCachedNeuroglancerValueAtBaseVoxel<double>(*m_ngVolume, m_pyramidalRatios, m_imgInfo, x, y, z, c);
      return vOpt.value_or(0.0);
    }

    if (m_imgInfo.depth == 1) {
      mip = false;
    }
    bool hasTile = false;
    index_t ix = x;
    index_t iy = y;
    index_t iz = z;

    if (mip) {
      hasTile = true;
    } else {
      for (const auto& ratio : m_pyramidalRatios) {
        if (auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
            tiit != m_rtToTileIndice.end()) {
          for (const std::vector<size_t>& tileIndice = tiit->second; auto tileIndex : tileIndice) {
            const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
            if (ix >= tile.x && ix < tile.x + tile.width && iy >= tile.y && iy < tile.y + tile.height && iz >= tile.z &&
                iz < tile.z + tile.depth) {
              if (ratio[0] == 1 && ratio[1] == 1 && ratio[2] == 1) {
                hasTile = true;
              }

              if (std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().get(ImageCacheHashKeyType(this, tileIndex));
                  imgPtr) {
                return imgPtr->value<double>((ix - tile.x) / (ratio[0]),
                                             (iy - tile.y) / (ratio[1]),
                                             (iz - tile.z) / (ratio[2]),
                                             c,
                                             0);
              }
            }
          }
        }
      }
    }

    return hasTile ? value(x, y, z, c, t, mip) : 0;
  }
  if (mip) {
    CHECK(!m_maximumProjectedAlongZImg.isEmpty());
    return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
  }
  return m_img.value<double>(x, y, z, c, t);
}

std::optional<uint64_t> ZImgPack::tryGetCachedNeuroglancerSegmentationId(size_t x, size_t y, size_t z) const
{
  if (!m_diskCached || !m_ngVolume) {
    return std::nullopt;
  }
  CHECK(m_ngVolume);
  if (!m_ngVolume->isSegmentation()) {
    return std::nullopt;
  }
  return tryGetCachedNeuroglancerValueAtBaseVoxel<uint64_t>(*m_ngVolume, m_pyramidalRatios, m_imgInfo, x, y, z, 0);
}

ZImg ZImgPack::crop(const ZImgRegion& region) const
{
  ZImg res;

  if (!m_diskCached) {
    res = m_img.crop(region);
    return res;
  }

  if (region.isEmpty()) {
    return res;
  }

  ZImgRegion rgn = region;
  if (!rgn.isValid(m_imgInfo)) {
    throw ZException(fmt::format("Try to crop img <{}> with invalid region <{}>", m_imgInfo, rgn));
  }

  rgn.resolveRegionEnd(m_imgInfo);
  ZImgInfo resInfo = rgn.clip(m_imgInfo);
  // create destination
  res = ZImg(resInfo);

  if (m_ngVolume) {
    if (rgn.tStart() != 0 || rgn.tEnd() != 1) {
      throw ZException("Neuroglancer precomputed volumes do not support time dimension yet (t must be 0)");
    }

    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(std::array<size_t, 3>{1, 1, 1});
    CHECK(scaleIndexOpt);

    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                             {static_cast<int64_t>(rgn.start.x),
                                                              static_cast<int64_t>(rgn.start.y),
                                                              static_cast<int64_t>(rgn.start.z)},
                                                             {static_cast<int64_t>(rgn.end.x),
                                                              static_cast<int64_t>(rgn.end.y),
                                                              static_cast<int64_t>(rgn.end.z)});
    if (chunks.empty()) {
      return res;
    }

    forEachNeuroglancerChunkBestEffortBlocking(*m_ngVolume,
                                               chunks,
                                               [&](size_t, const auto& chunk, const ZImg& chunkImg) {
                                                 ZVoxelCoordinate tileStart(static_cast<index_t>(chunk.baseStart[0]),
                                                                            static_cast<index_t>(chunk.baseStart[1]),
                                                                            static_cast<index_t>(chunk.baseStart[2]),
                                                                            0,
                                                                            0);
                                                 ZVoxelCoordinate start = tileStart - rgn.start;
                                                 res.pasteImg(chunkImg, start, false);
                                               });

    return res;
  }
  // start copy data
  for (auto t = rgn.tStart(); t < rgn.tEnd(); ++t) {
    if (auto tiit = m_rtToTileIndice.find(std::make_tuple(1, 1, 1, t)); tiit != m_rtToTileIndice.end()) {
      for (const std::vector<size_t>& tileIndice = tiit->second; auto tileIndex : tileIndice) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
        ZVoxelCoordinate tileStart(tile.x, tile.y, tile.z, 0, t);
        ZVoxelCoordinate start = tileStart - rgn.start;
        if ((start.x < 0 && start.x + tile.width <= 0) || start.x >= res.sWidth() ||
            (start.y < 0 && start.y + tile.height <= 0) || start.y >= res.sHeight() ||
            (start.z < 0 && start.z + tile.depth <= 0) || start.z >= res.sDepth()) {
          continue;
        }

        std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), tile);
        res.pasteImg(*imgPtr, start);
      }
    }
  }

  return res;
}

ZImg ZImgPack::resizedImg(size_t width, size_t height, size_t depth, size_t t) const
{
  // VLOG(1) << width << " " << height << " " << depth;
  CHECK(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth && width > 0 && height > 0 &&
        depth > 0);
  ZImg res;

  if (!m_diskCached) {
    res = m_img.createView(-1, t).resized(width, height, depth);
    return res;
  }

  std::array<size_t, 3> ratio = {1, 1, 1};
  if (m_ngVolume) {
    // For Neuroglancer sources, prefer downloading a pyramid level whose voxel grid already fits within
    // the requested output dimensions (w/h/d). Some datasets skip intermediate scales, and choosing the
    // nearest finer level can explode network I/O (download a huge volume just to downsample again).
    //
    // We therefore choose the *least* coarse ratio that still makes the volume <= target dims, and only
    // fall back to client-side downsampling if even the coarsest server-side level is still too large.
    auto ceilDiv = [](size_t a, size_t b) -> size_t {
      CHECK(b > 0);
      return (a + b - 1) / b;
    };

    const std::array<size_t, 3> required = {
      ceilDiv(m_imgInfo.width, width),
      ceilDiv(m_imgInfo.height, height),
      ceilDiv(m_imgInfo.depth, depth),
    };

    bool foundFit = false;
    size_t bestSum = std::numeric_limits<size_t>::max();
    for (const auto& r : m_pyramidalRatios) {
      if (r[0] >= required[0] && r[1] >= required[1] && r[2] >= required[2]) {
        const size_t sum = r[0] + r[1] + r[2];
        if (!foundFit || sum < bestSum) {
          ratio = r;
          bestSum = sum;
          foundFit = true;
        }
      }
    }

    if (!foundFit) {
      // No server-side level is coarse enough; fall back to downloading the coarsest available
      // level (minimum data) and downsampling locally.
      size_t maxSum = 0;
      for (const auto& r : m_pyramidalRatios) {
        const size_t sum = r[0] + r[1] + r[2];
        if (sum > maxSum) {
          ratio = r;
          maxSum = sum;
        }
      }
    }

    if (VLOG_IS_ON(1)) {
      QString scaleKey = QStringLiteral("<unknown>");
      if (auto idxOpt = m_ngVolume->scaleIndexForRatio(ratio)) {
        scaleKey = m_ngVolume->scales().at(*idxOpt).key;
      }
      VLOG(1) << fmt::format(
        "ZImgPack resizedImg (Neuroglancer): target {}x{}x{} required ratio >= [{},{},{}] chose [{},{},{}] (scale '{}')",
        width,
        height,
        depth,
        required[0],
        required[1],
        required[2],
        ratio[0],
        ratio[1],
        ratio[2],
        scaleKey.toStdString());
    }
  } else {
    ratio = readRatioOf(std::max(1.0, std::floor(static_cast<double>(m_imgInfo.width) / width)),
                        std::max(1.0, std::floor(static_cast<double>(m_imgInfo.height) / height)),
                        std::max(1.0, std::floor(static_cast<double>(m_imgInfo.depth) / depth)));
  }

  res = assembleImg(ratio, t);
  if (res.width() != width || res.height() != height || res.depth() != depth) {
    if (m_ngSegmentationRgbFor3D) {
      res.resize(width, height, depth, Interpolant::Nearest, false, false, FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
    } else {
      res.resize(width, height, depth);
    }
  }
  return res;
}

std::shared_ptr<const ZImg> ZImgPack::resizedImgCached(size_t width, size_t height, size_t depth, size_t t) const
{
  CHECK(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth && width > 0 && height > 0 &&
        depth > 0);

  // Cache is intended for disk-backed, file-based datasets. For in-memory images (already loaded) and
  // network-backed sources, fall back to the regular resizedImg path.
  if (!m_diskCached || m_ngVolume) {
    auto img = std::make_shared<ZImg>(resizedImg(width, height, depth, t));
    return std::shared_ptr<const ZImg>(img);
  }

  const auto fingerprint = datasetFingerprintForCache();
  if (auto hit = ZImgPreviewDiskCache::instance().tryGetFilePreview(fingerprint, width, height, depth, t); hit) {
    return std::shared_ptr<const ZImg>(std::move(hit));
  }

  auto img = std::make_shared<ZImg>(resizedImg(width, height, depth, t));
  ZImgPreviewDiskCache::instance().tryPutFilePreview(fingerprint, width, height, depth, t, img);
  return std::shared_ptr<const ZImg>(img);
}

folly::coro::Task<std::shared_ptr<const ZImg>> ZImgPack::resizedImgCachedAsync(size_t width,
                                                                               size_t height,
                                                                               size_t depth,
                                                                               size_t t) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  CHECK(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth && width > 0 && height > 0 &&
        depth > 0);

  // For file-backed sources, preserve the existing preview-disk-cache behavior while making the
  // build path cancellation-aware (see below).
  if (!m_ngVolume) {
    maybeCancel(cancellationToken);

    // The coroutine wrapper makes Neuroglancer (network-backed) preview builds cancellable because the call-stack
    // naturally suspends while awaiting HTTP reads. For file-backed (local) sources, however, the old path called
    // the fully synchronous resizedImgCached() implementation, which does not suspend and therefore could not observe
    // cancellation once work had started.
    //
    // Make the local path cancellation-aware by explicitly polling the coroutine cancellation token while assembling
    // tiles into the preview volume. This preserves output equivalence with the synchronous path when not cancelled.
    if (!m_diskCached) {
      maybeCancel(cancellationToken);
      auto img = std::make_shared<ZImg>(resizedImg(width, height, depth, t));
      maybeCancel(cancellationToken);
      co_return std::shared_ptr<const ZImg>(std::move(img));
    }

    const auto fingerprint = datasetFingerprintForCache();
    if (auto hit = ZImgPreviewDiskCache::instance().tryGetFilePreview(fingerprint, width, height, depth, t); hit) {
      maybeCancel(cancellationToken);
      co_return std::shared_ptr<const ZImg>(std::move(hit));
    }

    maybeCancel(cancellationToken);

    const std::array<size_t, 3> ratio =
      readRatioOf(std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(m_imgInfo.width) / width))),
                  std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(m_imgInfo.height) / height))),
                  std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(m_imgInfo.depth) / depth))));

    ZImgInfo info = m_imgInfo;
    info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
    info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
    info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
    info.numTimes = 1;

    ZImg res(info);

    auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          maybeCancel(cancellationToken);

          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
          const ZVoxelCoordinate start(std::round(static_cast<double>(tile.x) / ratio[0]),
                                       std::round(static_cast<double>(tile.y) / ratio[1]),
                                       std::round(static_cast<double>(tile.z) / ratio[2]),
                                       0,
                                       0);

          maybeCancel(cancellationToken);

          std::shared_ptr<ZImg> imgPtr =
            ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);

          maybeCancel(cancellationToken);

          res.pasteImg(*imgPtr, start);
        }
      });
    }

    maybeCancel(cancellationToken);

    if (res.width() != width || res.height() != height || res.depth() != depth) {
      maybeCancel(cancellationToken);
      res.resize(width, height, depth);
    }

    auto img = std::make_shared<ZImg>(std::move(res));
    ZImgPreviewDiskCache::instance().tryPutFilePreview(fingerprint, width, height, depth, t, img);
    co_return std::shared_ptr<const ZImg>(std::move(img));
  }

  // Neuroglancer precomputed (network-backed): build the preview volume via coroutines so cancellation
  // propagates implicitly to HTTP reads (co_withCancellation at the call site).
  CHECK(m_diskCached);
  CHECK(t == 0);
  CHECK(m_ngVolume);

  std::array<size_t, 3> ratio = {1, 1, 1};
  {
    // Match resizedImg() ratio selection: choose the least coarse pyramid level that still fits <= target dims,
    // falling back to the coarsest available level when none fit.
    auto ceilDiv = [](size_t a, size_t b) -> size_t {
      CHECK(b > 0);
      return (a + b - 1) / b;
    };

    const std::array<size_t, 3> required = {
      ceilDiv(m_imgInfo.width, width),
      ceilDiv(m_imgInfo.height, height),
      ceilDiv(m_imgInfo.depth, depth),
    };

    bool foundFit = false;
    size_t bestSum = std::numeric_limits<size_t>::max();
    for (const auto& r : m_pyramidalRatios) {
      if (r[0] >= required[0] && r[1] >= required[1] && r[2] >= required[2]) {
        const size_t sum = r[0] + r[1] + r[2];
        if (!foundFit || sum < bestSum) {
          ratio = r;
          bestSum = sum;
          foundFit = true;
        }
      }
    }

    if (!foundFit) {
      size_t maxSum = 0;
      for (const auto& r : m_pyramidalRatios) {
        const size_t sum = r[0] + r[1] + r[2];
        if (sum > maxSum) {
          ratio = r;
          maxSum = sum;
        }
      }
    }
  }

  maybeCancel(cancellationToken);

  auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(ratio);
  CHECK(scaleIndexOpt);

  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  info.numTimes = 1;
  const auto& scale = m_ngVolume->scales().at(*scaleIndexOpt);
  info.voxelSizeX = scale.resolutionNm[0];
  info.voxelSizeY = scale.resolutionNm[1];
  info.voxelSizeZ = scale.resolutionNm[2];

  ZImg res(info);

  const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                           {0, 0, 0},
                                                           {static_cast<int64_t>(m_imgInfo.width),
                                                            static_cast<int64_t>(m_imgInfo.height),
                                                            static_cast<int64_t>(m_imgInfo.depth)});
  if (!chunks.empty()) {
    // Read chunks in batches so we don't keep shared_ptr references to *all* chunks alive at once; this avoids
    // defeating LRU eviction in the Neuroglancer chunk cache under memory pressure.
    const size_t maxConcurrent =
      std::max<size_t>(1, static_cast<size_t>(FLAGS_atlas_ng_precomputed_3d_max_concurrent_block_reads));

    // Each chunk task both reads and pastes its result. The chunk grid is non-overlapping by construction, so
    // concurrent writes to disjoint regions of `res` are safe and avoids serializing the CPU-heavy paste step
    // (notably segmentation ID→RGB conversion).
    auto readChunkBestEffort = [&](ZNeuroglancerPrecomputedVolume::Chunk chunk) -> folly::coro::Task<void> {
      std::shared_ptr<ZImg> chunkImg;
      try {
        chunkImg = co_await m_ngVolume->readChunkAsync(chunk);
      } catch (const ZCancellationException&) {
        throw;
      } catch (const folly::OperationCancelled&) {
        throw;
      } catch (const std::exception&) {
        co_return;
      }
      if (!chunkImg) {
        co_return;
      }

      maybeCancel(cancellationToken);

      const ZVoxelCoordinate start(std::round(static_cast<double>(chunk.baseStart[0]) / ratio[0]),
                                   std::round(static_cast<double>(chunk.baseStart[1]) / ratio[1]),
                                   std::round(static_cast<double>(chunk.baseStart[2]) / ratio[2]),
                                   0,
                                   0);
      if (m_ngSegmentationRgbFor3D) {
        CHECK(m_ngVolume->isSegmentation());
        pasteNeuroglancerSegmentationChunkAsRgb(*chunkImg, res, start);
      } else {
        res.pasteImg(*chunkImg, start, false);
      }
      co_return;
    };

    for (size_t batchStart = 0; batchStart < chunks.size(); batchStart += maxConcurrent) {
      maybeCancel(cancellationToken);

      const size_t batchEnd = std::min(batchStart + maxConcurrent, chunks.size());
      std::vector<folly::coro::Task<void>> tasks;
      tasks.reserve(batchEnd - batchStart);
      for (size_t i = batchStart; i < batchEnd; ++i) {
        tasks.push_back(readChunkBestEffort(chunks[i]));
      }

      co_await folly::coro::collectAllRange(std::move(tasks));
      maybeCancel(cancellationToken);
    }
  }

  maybeCancel(cancellationToken);

  if (res.width() != width || res.height() != height || res.depth() != depth) {
    if (m_ngSegmentationRgbFor3D) {
      res.resize(width, height, depth, Interpolant::Nearest, false, false, FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
    } else {
      res.resize(width, height, depth);
    }
  }

  auto img = std::make_shared<ZImg>(std::move(res));
  co_return std::shared_ptr<const ZImg>(std::move(img));
}

#if 0
folly::Future<std::shared_ptr<ZImg>> ZImgPack::readRegionToImg(index_t xyRatio,
                                                               index_t zRatio,
                                                               index_t sx,
                                                               index_t sy,
                                                               index_t sz,
                                                               size_t sc,
                                                               size_t t,
                                                               const ZImgInfo& resInfo,
                                                               double displayRangeMin,
                                                               double displayRangeMax,
                                                               const folly::CancellationToken& cancellationToken) const
{
  CHECK(xyRatio >= 1 && zRatio >= 1);
  auto cpuExecutor = folly::getGlobalCPUExecutor();
  if (FLAGS_atlas_readRegionToImg_version == 0) {
    return folly::via(cpuExecutor, [=, this, &resInfo]() {
      maybeCancel(cancellationToken);

      bool needToUpdateBlockInfo = false;
      if (auto it = m_blockInfo.find(
            std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth));
          it != m_blockInfo.end()) {
        const auto [minv, maxv] = it->second;
        if (maxv <= displayRangeMin) {
          return folly::makeFuture(std::shared_ptr<ZImg>());
        }
      } else {
        needToUpdateBlockInfo = true;
      }

      maybeCancel(cancellationToken);

      auto img = ZImgRegionCache::instance().get(ImageRegionCacheHashKeyType(this,
                                                                             xyRatio,
                                                                             zRatio,
                                                                             sx,
                                                                             sy,
                                                                             sz,
                                                                             sc,
                                                                             t,
                                                                             resInfo.width,
                                                                             resInfo.height,
                                                                             resInfo.depth,
                                                                             displayRangeMin,
                                                                             displayRangeMax));
      if (img) {
        return folly::makeFuture(img);
      }

      maybeCancel(cancellationToken);

      auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);
      std::vector<size_t> queryResult;
      if (auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
          tiit != m_rtToTileBoxRTree.end()) {
        TileBoxType queryBox(TileCornerType(sx * xyRatio, sy * xyRatio, sz * zRatio),
                             TileCornerType((sx + static_cast<index_t>(resInfo.width)) * xyRatio - 1,
                                            (sy + static_cast<index_t>(resInfo.height)) * xyRatio - 1,
                                            (sz + static_cast<index_t>(resInfo.depth)) * zRatio - 1));
        tiit->second->query(bgi::intersects(queryBox),
                            boost::make_function_output_iterator([&queryResult](const auto& value) {
                              queryResult.push_back(value.second);
                            }));
      }

      if (queryResult.empty()) {
        return folly::makeFuture(std::shared_ptr<ZImg>());
      }

      maybeCancel(cancellationToken);

      auto tmpResInfo = resInfo;
      tmpResInfo.width = std::ceil(resInfo.width * xyRatio * 1.0 / readRatio[0]);
      tmpResInfo.height = std::ceil(resInfo.height * xyRatio * 1.0 / readRatio[1]);
      tmpResInfo.depth = std::ceil(resInfo.depth * zRatio * 1.0 / readRatio[2]);
      tmpResInfo.voxelFormat = m_imgInfo.voxelFormat;
      tmpResInfo.bytesPerVoxel = m_imgInfo.bytesPerVoxel;
      auto rres = std::make_shared<ZImg>(tmpResInfo); // will be captured by value to keep the image alive
      auto res = rres.get();

      maybeCancel(cancellationToken);

      std::vector<folly::Future<folly::Unit>> tileFutures;
      for (auto tileIndex : queryResult) {
        const ZImgSubBlock* tile = m_allTiles[tileIndex].get();
        tileFutures.push_back(folly::via(cpuExecutor).then([=, this](auto&&) {
          maybeCancel(cancellationToken);
          auto imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), *tile);
          ZVoxelCoordinate start(std::round((tile->x * 1.0 / xyRatio - sx) * xyRatio / readRatio[0]),
                                 std::round((tile->y * 1.0 / xyRatio - sy) * xyRatio / readRatio[1]),
                                 std::round((tile->z * 1.0 / zRatio - sz) * zRatio / readRatio[2]),
                                 -ZVoxelCoordinate::value_type(sc),
                                 0);
          maybeCancel(cancellationToken);
          res->pasteImg(*imgPtr, start);
        }));
      }

      return folly::collect(tileFutures).via(cpuExecutor).thenValue([=, this, &resInfo](auto&&) {
        maybeCancel(cancellationToken);

        if (needToUpdateBlockInfo) {
          maybeCancel(cancellationToken);

          double minv;
          double maxv;
          res->computeMinMax(minv, maxv);
          m_blockInfo.emplace(
            std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth),
            std::make_pair(minv, maxv));
          if (maxv <= displayRangeMin) {
            return std::shared_ptr<ZImg>();
          }
        }

        if (res->isSameType(resInfo)) {
          if (displayRangeMin != m_imgInfo.dataRangeMin() || displayRangeMax != m_imgInfo.dataRangeMax() ||
              resInfo.validBitCount != m_imgInfo.validBitCount) {
            maybeCancel(cancellationToken);
            res->normalize(displayRangeMin, displayRangeMax);
          }
        } else {
          maybeCancel(cancellationToken);
          *res = res->convertTo(displayRangeMin, displayRangeMax, resInfo);
        }

        if (res->width() != resInfo.width || res->height() != resInfo.height || res->depth() != resInfo.depth) {
          maybeCancel(cancellationToken);
          res->resize(resInfo.width,
                      resInfo.height,
                      resInfo.depth,
                      Interpolant::Cubic,
                      true,
                      false,
                      FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
        }

        maybeCancel(cancellationToken);

        ZImgRegionCache::instance().insert(ImageRegionCacheHashKeyType(this,
                                                                       xyRatio,
                                                                       zRatio,
                                                                       sx,
                                                                       sy,
                                                                       sz,
                                                                       sc,
                                                                       t,
                                                                       resInfo.width,
                                                                       resInfo.height,
                                                                       resInfo.depth,
                                                                       displayRangeMin,
                                                                       displayRangeMax),
                                           rres);
        return rres;
      });
    });
  } else {
    maybeCancel(cancellationToken);

    bool needToUpdateBlockInfo = false;
    if (auto it = m_blockInfo.find(
          std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth));
        it != m_blockInfo.end()) {
      const auto [minv, maxv] = it->second;
      if (maxv <= displayRangeMin) {
        return folly::makeFuture(std::shared_ptr<ZImg>());
      }
    } else {
      needToUpdateBlockInfo = true;
    }

    maybeCancel(cancellationToken);

    auto img = ZImgRegionCache::instance().get(ImageRegionCacheHashKeyType(this,
                                                                           xyRatio,
                                                                           zRatio,
                                                                           sx,
                                                                           sy,
                                                                           sz,
                                                                           sc,
                                                                           t,
                                                                           resInfo.width,
                                                                           resInfo.height,
                                                                           resInfo.depth,
                                                                           displayRangeMin,
                                                                           displayRangeMax));
    if (img) {
      return folly::makeFuture(img);
    }

    maybeCancel(cancellationToken);

    auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);
    return folly::via(cpuExecutor,
                      [=, this]() {
                        maybeCancel(cancellationToken);

                        std::vector<folly::Future<std::tuple<ZVoxelCoordinate, std::shared_ptr<ZImg>>>> tileFutures;
                        if (auto tiit =
                              m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
                            tiit != m_rtToTileBoxRTree.end()) {
                          std::vector<size_t> queryResult;
                          TileBoxType queryBox(TileCornerType(sx * xyRatio, sy * xyRatio, sz * zRatio),
                                               TileCornerType((sx + static_cast<index_t>(resInfo.width)) * xyRatio - 1,
                                                              (sy + static_cast<index_t>(resInfo.height)) * xyRatio - 1,
                                                              (sz + static_cast<index_t>(resInfo.depth)) * zRatio - 1));
                          tiit->second->query(bgi::intersects(queryBox),
                                              boost::make_function_output_iterator([&queryResult](const auto& value) {
                                                queryResult.push_back(value.second);
                                              }));

                          maybeCancel(cancellationToken);

                          for (auto tileIndex : queryResult) {
                            maybeCancel(cancellationToken);
                            const ZImgSubBlock* tile = m_allTiles[tileIndex].get();
                            tileFutures.push_back(folly::via(cpuExecutor, [=, this]() {
                              maybeCancel(cancellationToken);
                              return std::make_tuple(
                                ZVoxelCoordinate(std::round((tile->x * 1.0 / xyRatio - sx) * xyRatio / readRatio[0]),
                                                 std::round((tile->y * 1.0 / xyRatio - sy) * xyRatio / readRatio[1]),
                                                 std::round((tile->z * 1.0 / zRatio - sz) * zRatio / readRatio[2]),
                                                 -ZVoxelCoordinate::value_type(sc),
                                                 0),
                                ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), *tile));
                            }));
                          }
                        }
                        return folly::collect(tileFutures);
                      })
      .via(cpuExecutor)
      .thenValue([=, this, &resInfo](auto&& tiles) {
        maybeCancel(cancellationToken);

        if (tiles.empty()) {
          return std::shared_ptr<ZImg>();
        }
        auto tmpResInfo = resInfo;
        tmpResInfo.width = std::ceil(resInfo.width * xyRatio * 1.0 / readRatio[0]);
        tmpResInfo.height = std::ceil(resInfo.height * xyRatio * 1.0 / readRatio[1]);
        tmpResInfo.depth = std::ceil(resInfo.depth * zRatio * 1.0 / readRatio[2]);
        tmpResInfo.voxelFormat = m_imgInfo.voxelFormat;
        tmpResInfo.bytesPerVoxel = m_imgInfo.bytesPerVoxel;
        auto res = std::make_shared<ZImg>(tmpResInfo);

        maybeCancel(cancellationToken);

        for (auto& [start, imgPtr] : tiles) {
          maybeCancel(cancellationToken);
          res->pasteImg(*imgPtr, start);
          imgPtr.reset();
        }

        if (needToUpdateBlockInfo) {
          maybeCancel(cancellationToken);

          double minv;
          double maxv;
          res->computeMinMax(minv, maxv);
          m_blockInfo.emplace(
            std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth),
            std::make_pair(minv, maxv));
          if (maxv <= displayRangeMin) {
            return std::shared_ptr<ZImg>();
          }
        }

        if (res->isSameType(resInfo)) {
          if (displayRangeMin != m_imgInfo.dataRangeMin() || displayRangeMax != m_imgInfo.dataRangeMax() ||
              resInfo.validBitCount != m_imgInfo.validBitCount) {
            maybeCancel(cancellationToken);
            res->normalize(displayRangeMin, displayRangeMax);
          }
        } else {
          maybeCancel(cancellationToken);
          *res = res->convertTo(displayRangeMin, displayRangeMax, resInfo);
        }

        if (res->width() != resInfo.width || res->height() != resInfo.height || res->depth() != resInfo.depth) {
          maybeCancel(cancellationToken);
          res->resize(resInfo.width,
                      resInfo.height,
                      resInfo.depth,
                      Interpolant::Cubic,
                      true,
                      false,
                      FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
        }

        maybeCancel(cancellationToken);

        ZImgRegionCache::instance().insert(ImageRegionCacheHashKeyType(this,
                                                                       xyRatio,
                                                                       zRatio,
                                                                       sx,
                                                                       sy,
                                                                       sz,
                                                                       sc,
                                                                       t,
                                                                       resInfo.width,
                                                                       resInfo.height,
                                                                       resInfo.depth,
                                                                       displayRangeMin,
                                                                       displayRangeMax),
                                           res);
        return res;
      });
  }
}
#endif

folly::coro::Task<void> ZImgPack::readTileToImgAsync(size_t tileIndex,
                                                     ZImg* img,
                                                     index_t xyRatio,
                                                     index_t zRatio,
                                                     index_t sx,
                                                     index_t sy,
                                                     index_t sz,
                                                     size_t sc,
                                                     std::array<size_t, 3> readRatio,
                                                     ZImgReadStatsSink* statsSink,
                                                     ZImgReadStatsContext statsContext) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  const ZImgSubBlock* tile = m_allTiles[tileIndex].get();
  std::optional<ZIoReadStatsScope> ioScope;
  if (statsSink) {
    ZIoReadStatsHooks hooks;
    hooks.user = statsSink;
    hooks.onFileReadBytes = reportFileReadBytesToStatsSink;
    const ZIoReadStatsContext ioCtx{statsContext.channel, statsContext.round};
    ioScope.emplace(hooks, ioCtx);
  }

  bool didRead = false;
  auto imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex),
                                                *tile,
                                                ZImgCache::FindStategy::UpdateLRUList,
                                                &didRead);
  if (didRead && statsSink) {
    statsSink->onSourceLogicalBytes(statsContext, imgPtr ? imgPtr->byteNumber() : 0);
  }
  ZVoxelCoordinate start(std::round((static_cast<double>(tile->x) / xyRatio - sx) * xyRatio / readRatio[0]),
                         std::round((static_cast<double>(tile->y) / xyRatio - sy) * xyRatio / readRatio[1]),
                         std::round((static_cast<double>(tile->z) / zRatio - sz) * zRatio / readRatio[2]),
                         -static_cast<index_t>(sc),
                         0);
  maybeCancel(cancellationToken);
  img->pasteImg(*imgPtr, start);

  co_return;
}

folly::coro::Task<std::shared_ptr<ZImg>> ZImgPack::readRegionToImgAsync(index_t xyRatio,
                                                                        index_t zRatio,
                                                                        index_t sx,
                                                                        index_t sy,
                                                                        index_t sz,
                                                                        size_t sc,
                                                                        size_t t,
                                                                        const ZImgInfo& resInfo,
                                                                        double displayRangeMin,
                                                                        double displayRangeMax,
                                                                        ZImgReadStatsSink* statsSink,
                                                                        ZImgReadStatsContext statsContext,
                                                                        ReadRegionCachePolicy regionCachePolicy) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  CHECK(xyRatio > 0);
  CHECK(zRatio > 0);
  CHECK(sc < m_imgInfo.numChannels);
  if (m_ngVolume) {
    CHECK(t == 0);
  }

  bool needToUpdateBlockInfo = false;
  double maxIntensity;
  if (m_blockInfo.cvisit(
        std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth),
        [&](const auto& x) {
          maxIntensity = x.second.second;
        })) {
    if (maxIntensity <= displayRangeMin) {
      if (statsSink) {
        statsSink->onImgRegionBlockResolved(statsContext, ZImgRegionBlockSource::SkippedEmpty, 0, /*empty=*/true);
      }
      co_return std::shared_ptr<ZImg>();
    }
  } else {
    needToUpdateBlockInfo = true;
  }

  maybeCancel(cancellationToken);

  std::optional<ImageRegionCacheHashKeyType> memKey;
  if (regionCachePolicy == ReadRegionCachePolicy::Use) {
    const ZImgRegionCacheSourceKind sourceKind =
      m_ngVolume ? ZImgRegionCacheSourceKind::Neuroglancer : ZImgRegionCacheSourceKind::File;
    memKey.emplace(datasetFingerprintForCache(),
                   sourceKind,
                   xyRatio,
                   zRatio,
                   sx,
                   sy,
                   sz,
                   sc,
                   t,
                   resInfo.width,
                   resInfo.height,
                   resInfo.depth,
                   resInfo.numChannels,
                   resInfo.numTimes,
                   static_cast<uint32_t>(resInfo.bytesPerVoxel),
                   static_cast<uint32_t>(std::to_underlying(resInfo.voxelFormat)),
                   static_cast<uint32_t>(resInfo.validBitCount),
                   displayRangeMin,
                   displayRangeMax);

    const auto hit = [&]() {
      if (statsSink) {
        ZImgReadStatsScope statsScope(statsSink, statsContext);
        return ZImgRegionCache::instance().findWithSource(*memKey);
      }
      return ZImgRegionCache::instance().findWithSource(*memKey);
    }();

    if (hit.has_value()) {
      if (statsSink) {
        const ZImgRegionBlockSource source = (hit->source == ZImgRegionCache::FindSource::Memory)
                                               ? ZImgRegionBlockSource::MemoryCache
                                               : ZImgRegionBlockSource::DiskCache;
        const size_t bytes = hit->value ? hit->value->byteNumber() : 0;
        statsSink->onImgRegionBlockResolved(statsContext, source, bytes, /*empty=*/false);
      }
      co_return hit->value;
    }
  }

  maybeCancel(cancellationToken);

  auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);

  if (m_ngVolume) {
    const std::array<size_t, 3> requestedRatio = {static_cast<size_t>(xyRatio),
                                                  static_cast<size_t>(xyRatio),
                                                  static_cast<size_t>(zRatio)};
    if (VLOG_IS_ON(1) && readRatio != requestedRatio) {
      VLOG(1) << fmt::format("Neuroglancer readRegion requested ratio [{},{},{}] is not available; using [{},{},{}] and "
                             "resizing (this may increase network I/O).",
                             requestedRatio[0],
                             requestedRatio[1],
                             requestedRatio[2],
                             readRatio[0],
                             readRatio[1],
                             readRatio[2]);
    }

    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(readRatio);
    CHECK(scaleIndexOpt);

    const int64_t baseStartX = static_cast<int64_t>(sx) * static_cast<int64_t>(xyRatio);
    const int64_t baseStartY = static_cast<int64_t>(sy) * static_cast<int64_t>(xyRatio);
    const int64_t baseStartZ = static_cast<int64_t>(sz) * static_cast<int64_t>(zRatio);
    const int64_t baseEndX =
      (static_cast<int64_t>(sx) + static_cast<int64_t>(resInfo.sWidth())) * static_cast<int64_t>(xyRatio);
    const int64_t baseEndY =
      (static_cast<int64_t>(sy) + static_cast<int64_t>(resInfo.sHeight())) * static_cast<int64_t>(xyRatio);
    const int64_t baseEndZ =
      (static_cast<int64_t>(sz) + static_cast<int64_t>(resInfo.sDepth())) * static_cast<int64_t>(zRatio);

    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                             {baseStartX, baseStartY, baseStartZ},
                                                             {baseEndX, baseEndY, baseEndZ});
    if (chunks.empty()) {
      if (statsSink) {
        statsSink->onImgRegionBlockResolved(statsContext, ZImgRegionBlockSource::SkippedEmpty, 0, /*empty=*/true);
      }
      co_return std::shared_ptr<ZImg>();
    }

    auto tmpResInfo = resInfo;
    tmpResInfo.width = std::ceil(static_cast<double>(resInfo.width) * static_cast<double>(xyRatio) / readRatio[0]);
    tmpResInfo.height = std::ceil(static_cast<double>(resInfo.height) * static_cast<double>(xyRatio) / readRatio[1]);
    tmpResInfo.depth = std::ceil(static_cast<double>(resInfo.depth) * static_cast<double>(zRatio) / readRatio[2]);
    tmpResInfo.voxelFormat = m_imgInfo.voxelFormat;
    tmpResInfo.bytesPerVoxel = m_imgInfo.bytesPerVoxel;
    auto res = std::make_shared<ZImg>(tmpResInfo);

    // For Neuroglancer, a single 3D paging block read can intersect multiple
    // underlying chunks (depending on the dataset chunkSize). If we read all of
    // them concurrently, total in-flight HTTP requests can explode during
    // full-resolution 3D paging (many blocks * many chunks per block). This
    // hurts cancellation responsiveness and can amplify transient socket/TLS
    // errors.
    //
    // Limit per-block chunk concurrency; overall concurrency is already bounded
    // at the block level by Z3DImg.
    constexpr size_t maxConcurrentChunkReadsPerBlock = 1;

    std::vector<folly::coro::Task<std::shared_ptr<ZImg>>> chunkTasks;
    chunkTasks.reserve(chunks.size());
    for (const auto& chunk : chunks) {
      chunkTasks.push_back(m_ngVolume->readChunkAsync(chunk, statsSink, statsContext));
    }
    std::vector<std::shared_ptr<ZImg>> chunkImgs =
      co_await folly::coro::collectAllWindowed(std::move(chunkTasks), maxConcurrentChunkReadsPerBlock);

    maybeCancel(cancellationToken);

    for (size_t i = 0; i < chunks.size(); ++i) {
      const auto& chunk = chunks[i];
      const auto& chunkImg = chunkImgs[i];
      if (!chunkImg) {
        continue;
      }

      const index_t dx = static_cast<index_t>(
        std::round(static_cast<double>(chunk.baseStart[0] - baseStartX) / static_cast<double>(readRatio[0])));
      const index_t dy = static_cast<index_t>(
        std::round(static_cast<double>(chunk.baseStart[1] - baseStartY) / static_cast<double>(readRatio[1])));
      const index_t dz = static_cast<index_t>(
        std::round(static_cast<double>(chunk.baseStart[2] - baseStartZ) / static_cast<double>(readRatio[2])));
      if (m_ngSegmentationRgbFor3D) {
        CHECK(m_ngVolume->isSegmentation());
        CHECK(sc < 3);
        CHECK(res->numChannels() == 1);
        const ZVoxelCoordinate start(dx, dy, dz, 0, 0);
        pasteNeuroglancerSegmentationChunkAsRgbComponent(*chunkImg, *res, start, sc);
      } else {
        const ZVoxelCoordinate start(dx, dy, dz, -static_cast<index_t>(sc), 0);
        res->pasteImg(*chunkImg, start, false);
      }
    }

    if (needToUpdateBlockInfo) {
      maybeCancel(cancellationToken);

      double minv;
      double maxv;
      res->computeMinMax(minv, maxv);
      m_blockInfo.emplace(
        std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth),
        std::make_pair(minv, maxv));
      if (maxv <= displayRangeMin) {
        if (statsSink) {
          statsSink->onImgRegionBlockResolved(statsContext,
                                              ZImgRegionBlockSource::SourceRead,
                                              res ? res->byteNumber() : 0,
                                              /*empty=*/true);
        }
        co_return std::shared_ptr<ZImg>();
      }
    }

    if (res->isSameType(resInfo)) {
      if (displayRangeMin != m_imgInfo.dataRangeMin() || displayRangeMax != m_imgInfo.dataRangeMax() ||
          resInfo.validBitCount != m_imgInfo.validBitCount) {
        maybeCancel(cancellationToken);
        res->normalize(displayRangeMin, displayRangeMax);
      }
    } else {
      maybeCancel(cancellationToken);
      *res = res->convertTo(displayRangeMin, displayRangeMax, resInfo);
    }

    if (res->width() != resInfo.width || res->height() != resInfo.height || res->depth() != resInfo.depth) {
      maybeCancel(cancellationToken);
      const Interpolant interpolant = m_ngSegmentationRgbFor3D ? Interpolant::Nearest : Interpolant::Cubic;
      const bool antialiasing = m_ngSegmentationRgbFor3D ? false : true;
      res->resize(resInfo.width,
                  resInfo.height,
                  resInfo.depth,
                  interpolant,
                  antialiasing,
                  false,
                  FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
    }

    maybeCancel(cancellationToken);

    if (statsSink) {
      statsSink->onImgRegionBlockResolved(statsContext,
                                          ZImgRegionBlockSource::SourceRead,
                                          res ? res->byteNumber() : 0,
                                          /*empty=*/false);
    }
    if (memKey.has_value()) {
      ZImgRegionCache::instance().insert(*memKey, res);
    }
    co_return res;
  }

  std::vector<size_t> queryResult;
  if (auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
      tiit != m_rtToTileBoxRTree.end()) {
    TileBoxType queryBox(TileCornerType(sx * xyRatio, sy * xyRatio, sz * zRatio),
                         TileCornerType((sx + resInfo.sWidth()) * xyRatio - 1,
                                        (sy + resInfo.sHeight()) * xyRatio - 1,
                                        (sz + resInfo.sDepth()) * zRatio - 1));
    tiit->second->query(bgi::intersects(queryBox),
                        boost::make_function_output_iterator([&queryResult](const auto& value) {
                          queryResult.push_back(value.second);
                        }));
  }

  if (queryResult.empty()) {
    if (statsSink) {
      statsSink->onImgRegionBlockResolved(statsContext, ZImgRegionBlockSource::SkippedEmpty, 0, /*empty=*/true);
    }
    co_return std::shared_ptr<ZImg>();
  }

  maybeCancel(cancellationToken);

  auto tmpResInfo = resInfo;
  tmpResInfo.width = std::ceil(static_cast<double>(resInfo.width) * xyRatio / readRatio[0]);
  tmpResInfo.height = std::ceil(static_cast<double>(resInfo.height) * xyRatio / readRatio[1]);
  tmpResInfo.depth = std::ceil(static_cast<double>(resInfo.depth) * zRatio / readRatio[2]);
  tmpResInfo.voxelFormat = m_imgInfo.voxelFormat;
  tmpResInfo.bytesPerVoxel = m_imgInfo.bytesPerVoxel;
  auto res = std::make_shared<ZImg>(tmpResInfo);

  std::vector<folly::coro::TaskWithExecutor<void>> tileTasks;
  tileTasks.reserve(queryResult.size());
  for (auto tileIndex : queryResult) {
    tileTasks.push_back(folly::coro::co_withExecutor(
      folly::getGlobalCPUExecutor(),
      readTileToImgAsync(tileIndex, res.get(), xyRatio, zRatio, sx, sy, sz, sc, readRatio, statsSink, statsContext)));
  }
  co_await folly::coro::collectAllRange(std::move(tileTasks));

  if (needToUpdateBlockInfo) {
    maybeCancel(cancellationToken);

    double minv;
    double maxv;
    res->computeMinMax(minv, maxv);
    m_blockInfo.emplace(
      std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, resInfo.width, resInfo.height, resInfo.depth),
      std::make_pair(minv, maxv));
    if (maxv <= displayRangeMin) {
      if (statsSink) {
        statsSink->onImgRegionBlockResolved(statsContext,
                                            ZImgRegionBlockSource::SourceRead,
                                            res ? res->byteNumber() : 0,
                                            /*empty=*/true);
      }
      co_return std::shared_ptr<ZImg>();
    }
  }

  if (res->isSameType(resInfo)) {
    if (displayRangeMin != m_imgInfo.dataRangeMin() || displayRangeMax != m_imgInfo.dataRangeMax() ||
        resInfo.validBitCount != m_imgInfo.validBitCount) {
      maybeCancel(cancellationToken);
      res->normalize(displayRangeMin, displayRangeMax);
    }
  } else {
    maybeCancel(cancellationToken);
    *res = res->convertTo(displayRangeMin, displayRangeMax, resInfo);
  }

  if (res->width() != resInfo.width || res->height() != resInfo.height || res->depth() != resInfo.depth) {
    maybeCancel(cancellationToken);
    res->resize(resInfo.width,
                resInfo.height,
                resInfo.depth,
                Interpolant::Cubic,
                true,
                false,
                FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
  }

  maybeCancel(cancellationToken);

  if (statsSink) {
    statsSink->onImgRegionBlockResolved(statsContext,
                                        ZImgRegionBlockSource::SourceRead,
                                        res ? res->byteNumber() : 0,
                                        /*empty=*/false);
  }
  if (memKey.has_value()) {
    ZImgRegionCache::instance().insert(*memKey, res);
  }
  co_return res;
}

const ZImg& ZImgPack::maxZProjectedImg(size_t zStart, size_t zEnd) const
{
  CHECK(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty() || zStart != m_mipZStart || zEnd != m_mipZEnd) {
    m_img.maximumZProjection(zStart, zEnd).swap(m_maximumProjectedAlongZImg);
    m_mipZStart = zStart;
    m_mipZEnd = zEnd;
  }
  return m_maximumProjectedAlongZImg;
}

void ZImgPack::show3DImgContextMenu(QPoint globalPos, float x, float y, float z, bool enter, bool exit)
{
  QMenu menu;
  QAction* enterSubregionViewAction = enter ? menu.addAction("Enter Subregion View") : nullptr;
  QAction* exitSubregionViewAction = exit ? menu.addAction("Exit Subregion View") : nullptr;
  if (menu.isEmpty()) {
    return;
  }
  QAction* selectedAction = menu.exec(globalPos);
  if (enterSubregionViewAction && selectedAction == enterSubregionViewAction) {
    Q_EMIT enterSubregionView(x, y, z);
  } else if (exitSubregionViewAction && selectedAction == exitSubregionViewAction) {
    Q_EMIT exitSubregionView();
  }
}

ZImg ZImgPack::slice(size_t z, size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1}, z, /*c*/ -1, t);
}

ZImg ZImgPack::allSlices(size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1}, t);
}

ZImg ZImgPack::wholeImg() const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1});
}

ZImg ZImgPack::assembleChannelTime(std::array<size_t, 3> ratio, size_t c, size_t t) const
{
  const ZImgInfo info = imgInfo();
  if (c >= info.numChannels || t >= info.numTimes) {
    throw ZException(fmt::format("assembleChannelTime: invalid channel/time selection (c={}, t={}, ratio=[{},{},{}]).",
                                 c,
                                 t,
                                 ratio[0],
                                 ratio[1],
                                 ratio[2]));
  }
  if (ratio[0] == 0 || ratio[1] == 0 || ratio[2] == 0) {
    throw ZException(fmt::format("assembleChannelTime: ratio values must be > 0, got ratio=[{},{},{}].",
                                 ratio[0],
                                 ratio[1],
                                 ratio[2]));
  }

  if (!m_diskCached) {
    ZImg res = m_img.extractChannel(c, static_cast<index_t>(t));
    if (ratio != std::array<size_t, 3>{1, 1, 1}) {
      res = res.blockDownsampled(ratio[0], ratio[1], ratio[2], ImgMergeMode::Interpolation);
    }
    return res;
  }

  if (m_ngVolume) {
    CHECK(t == 0);
  }

  std::array<size_t, 3> readRatio = ratio;
  if (!m_pyramidalRatios.contains(readRatio)) {
    readRatio = readRatioOf(ratio[0], ratio[1], ratio[2]);
  }
  CHECK(m_pyramidalRatios.contains(readRatio));

  ZImg res = assembleImg(readRatio, c, t);

  const size_t desWidth = (info.width + ratio[0] - 1) / ratio[0];
  const size_t desHeight = (info.height + ratio[1] - 1) / ratio[1];
  const size_t desDepth = (info.depth + ratio[2] - 1) / ratio[2];

  if (res.width() != desWidth || res.height() != desHeight || res.depth() != desDepth) {
    const Interpolant interpolant = m_ngSegmentationRgbFor3D ? Interpolant::Nearest : Interpolant::Cubic;
    const bool antialiasing = m_ngSegmentationRgbFor3D ? false : true;
    res.resize(desWidth,
               desHeight,
               desDepth,
               interpolant,
               antialiasing,
               false,
               FLAGS_atlas_readRegionToImg_use_multithreaded_resize);
  }

  return res;
}

void ZImgPack::createSliceTiles(ZImg* img, size_t z, size_t t)
{
  size_t ratio = 1;
  while (true) {
    size_t numX = (img->width() + m_tileSize - 1) / m_tileSize;
    size_t numY = (img->height() + m_tileSize - 1) / m_tileSize;
    if (numX == 1 && numY == 1) {
      size_t width = m_imgInfo.width;
      size_t height = m_imgInfo.height;
      if (img->width() <= 64 && img->height() <= 64) {
        std::shared_ptr<ZImg> simg(img);
        m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));

        ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), simg);
        break;
      } else {
        std::shared_ptr<ZImg> simg(new ZImg(*img));
        m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));

        ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), simg);

        img->zoom(0.5, 0.5);
        ratio *= 2;
      }
    } else {
      for (size_t x = 0; x < numX; ++x) {
        for (size_t y = 0; y < numY; ++y) {
          size_t startX = x * m_tileSize;
          size_t endX = std::min(img->width(), startX + m_tileSize);
          size_t startY = y * m_tileSize;
          size_t endY = std::min(img->height(), startY + m_tileSize);
          size_t width = std::min(m_imgInfo.width, (endX - startX) * ratio);
          size_t height = std::min(m_imgInfo.height, (endY - startY) * ratio);
          std::shared_ptr<ZImg> cropped(new ZImg());
          img->crop(ZImgRegion(startX, endX, startY, endY, 0, 1, 0, -1, 0, 1)).swap(*cropped);
          startX = startX * ratio;
          startY = startY * ratio;

          m_allTiles.emplace_back(new ZImgPackSubBlock(cropped, ratio, t, z, startX, startY, width, height));

          ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), cropped);
        }
      }

      img->zoom(0.5, 0.5);
      ratio *= 2;
    }
  }
}

void ZImgPack::buildPyramidal(ZImg& img)
{
  img.computeMinMax(m_minIntensity, m_maxIntensity);
  m_minMaxState = MinMaxState::Complete;

  if (m_imgInfo.depth == 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      if (m_imgInfo.numTimes == 1) {
        auto tImg = new ZImg();
        tImg->swap(img);
        createSliceTiles(tImg, 0, 0);
      } else {
        ZImgRegion rgn;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        createSliceTiles(tImg, 0, t);
      }
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        img.crop(rgn).swap(*tImg);

        createSliceTiles(tImg, z, t);
      }
    }
  }

  createTileIndexStructure();
}

void ZImgPack::buildPyramidal()
{
  if (m_imgInfo.byteNumber() <= ZCpuInfo::instance().nPhysicalRAM / 2) {
    ZImg tImg;
    tImg.load(m_imgSource);
    buildPyramidal(tImg);
    return;
  }

  double minV;
  double maxV;
  m_minIntensity = std::numeric_limits<double>::max();
  m_maxIntensity = std::numeric_limits<double>::lowest();

  if (m_imgInfo.depth == 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      ZImgRegion rgn;
      rgn.start.t = t;
      rgn.end.t = t + 1;
      auto tImg = new ZImg();
      tImg->load(m_imgSource.filenames,
                 m_imgSource.catDim,
                 m_imgSource.catScenes,
                 rgn,
                 m_imgSource.scene,
                 1,
                 1,
                 1,
                 m_imgSource.format);
      tImg->computeMinMax(minV, maxV);
      m_minIntensity = std::min(m_minIntensity, minV);
      m_maxIntensity = std::max(m_maxIntensity, maxV);
      createSliceTiles(tImg, 0, t);
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        tImg->load(m_imgSource.filenames,
                   m_imgSource.catDim,
                   m_imgSource.catScenes,
                   rgn,
                   m_imgSource.scene,
                   1,
                   1,
                   1,
                   m_imgSource.format);
        tImg->computeMinMax(minV, maxV);
        m_minIntensity = std::min(m_minIntensity, minV);
        m_maxIntensity = std::max(m_maxIntensity, maxV);

        createSliceTiles(tImg, z, t);
      }
    }
  }

  m_minMaxState = MinMaxState::Complete;

  createTileIndexStructure();
}

void ZImgPack::buildPyramidalIndexOnly()
{
  // Do not compute global min/max here; keep Invalid to avoid long stalls.
  m_minMaxState = MinMaxState::Invalid;

  m_allTiles.clear();
  m_rtToTileIndice.clear();
  m_rtToTileBoxRTree.clear();
  m_pyramidalRatios.clear();

  const size_t width = m_imgInfo.width;
  const size_t height = m_imgInfo.height;
  const size_t depth = m_imgInfo.depth;
  const size_t times = m_imgInfo.numTimes;

  // Iterate times and Z slices, generate XY tiles for powers-of-two ratios.
  for (size_t t = 0; t < times; ++t) {
    for (size_t z = 0; z < std::max<size_t>(depth, 1); ++z) {
      for (uint32_t r = 1;; r <<= 1U) {
        const size_t tileSpanX = m_tileSize * static_cast<size_t>(r);
        const size_t tileSpanY = m_tileSize * static_cast<size_t>(r);
        const size_t numX = (width + tileSpanX - 1) / tileSpanX;
        const size_t numY = (height + tileSpanY - 1) / tileSpanY;
        for (size_t xi = 0; xi < numX; ++xi) {
          for (size_t yi = 0; yi < numY; ++yi) {
            const size_t startX = xi * tileSpanX;
            const size_t startY = yi * tileSpanY;
            const size_t endX = std::min(width, startX + tileSpanX);
            const size_t endY = std::min(height, startY + tileSpanY);

            ZImgSource src = m_imgSource;
            src.region = ZImgRegion(ZVoxelCoordinate(static_cast<index_t>(startX),
                                                    static_cast<index_t>(startY),
                                                    static_cast<index_t>(z),
                                                    0,
                                                    static_cast<index_t>(t)),
                                    ZVoxelCoordinate(static_cast<index_t>(endX),
                                                    static_cast<index_t>(endY),
                                                    static_cast<index_t>(z + 1),
                                                    -1,
                                                    static_cast<index_t>(t + 1)));
            auto tile = std::make_shared<ZImgTileSubBlock>(src, r, r, 1, ImgMergeMode::Interpolation);
            m_allTiles.emplace_back(std::move(tile));
          }
        }
        // Stop when the downsampled image at this ratio would be small (<=64) in both axes
        if ((width + r - 1) / r <= 64 && (height + r - 1) / r <= 64) {
          break;
        }
      }
    }
  }

  createTileIndexStructure();
}

void ZImgPack::computeQuickWindowIfNeeded()
{
  if (!FLAGS_atlas_imgpack_quick_window_enable) {
    return;
  }
  // Only for disk-cached float images without full min/max
  if (!m_diskCached || m_minMaxState == MinMaxState::Complete || m_imgInfo.voxelFormat != VoxelFormat::Float) {
    return;
  }
  // Must have base-ratio tiles indexed
  if (m_rtToTileIndice.empty()) {
    return;
  }

  const uint32_t K = std::max<uint32_t>(1, FLAGS_atlas_imgpack_quick_window_tiles_per_axis);
  const uint32_t step = std::max<uint32_t>(1, FLAGS_atlas_imgpack_quick_window_sample_step);
  const uint32_t maxSamples = std::max<uint32_t>(1, FLAGS_atlas_imgpack_quick_window_max_samples);
  const double lp = std::clamp(FLAGS_atlas_imgpack_quick_window_lower_p, 0.0, 1.0);
  const double up = std::clamp(FLAGS_atlas_imgpack_quick_window_upper_p, 0.0, 1.0);
  if (!(lp < up)) {
    return;
  }

  // Determine sampled z,t slices
  std::vector<size_t> zSamples;
  if (m_imgInfo.depth <= 1) {
    zSamples = {0};
  } else {
    zSamples = {static_cast<size_t>(m_imgInfo.depth * 1 / 4), static_cast<size_t>(m_imgInfo.depth * 2 / 4),
                static_cast<size_t>(m_imgInfo.depth * 3 / 4)};
    for (auto& z : zSamples) {
      if (z >= m_imgInfo.depth) z = m_imgInfo.depth - 1;
    }
  }
  zSamples.erase(std::unique(zSamples.begin(), zSamples.end()), zSamples.end());
  if (zSamples.empty()) zSamples.push_back(0);

  std::vector<size_t> tSamples;
  if (m_imgInfo.numTimes <= 1) {
    tSamples = {0};
  } else {
    tSamples = {0u, m_imgInfo.numTimes - 1};
    if (m_imgInfo.numTimes > 2) {
      tSamples.push_back(m_imgInfo.numTimes / 2);
    }
    std::sort(tSamples.begin(), tSamples.end());
    tSamples.erase(std::unique(tSamples.begin(), tSamples.end()), tSamples.end());
  }

  // Base ratio and tiling info
  const size_t ratio = 1;
  const size_t tileSpanX = m_tileSize * ratio;
  const size_t tileSpanY = m_tileSize * ratio;
  const size_t numX = (m_imgInfo.width + tileSpanX - 1) / tileSpanX;
  const size_t numY = (m_imgInfo.height + tileSpanY - 1) / tileSpanY;

  auto pickIndex = [&](size_t n, uint32_t i, uint32_t k) -> size_t {
    if (k <= 1 || n <= 1) return 0;
    if (k == 2) return (i == 0 ? 0 : (n - 1));
    double pos = static_cast<double>(i) * (static_cast<double>(n - 1) / static_cast<double>(k - 1));
    size_t idx = static_cast<size_t>(std::round(pos));
    return std::min(idx, n - 1);
  };

  // Sample vectors per channel
  std::vector<std::vector<double>> channelSamples(m_imgInfo.numChannels);
  uint32_t totalSamples = 0;

  for (size_t t : tSamples) {
    for (size_t z : zSamples) {
      // Gather K×K tiles spread across the image
      std::vector<size_t> tileIndices;
      tileIndices.reserve(K * K);
      auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio, ratio, 1, t));
      if (tiit == m_rtToTileIndice.end()) {
        continue;
      }
      for (uint32_t xi = 0; xi < K; ++xi) {
        for (uint32_t yi = 0; yi < K; ++yi) {
          size_t tx = pickIndex(numX, xi, K);
          size_t ty = pickIndex(numY, yi, K);
          index_t startX = static_cast<index_t>(tx * tileSpanX);
          index_t startY = static_cast<index_t>(ty * tileSpanY);
          index_t endX = static_cast<index_t>(std::min(m_imgInfo.width, static_cast<size_t>(startX) + tileSpanX));
          index_t endY = static_cast<index_t>(std::min(m_imgInfo.height, static_cast<size_t>(startY) + tileSpanY));
          TileBoxType tileBox(TileCornerType(startX, startY, static_cast<index_t>(z)),
                              TileCornerType(endX - 1, endY - 1, static_cast<index_t>(z)));
          std::vector<RTreeValueType> q;
          if (auto rtreeIt = m_rtToTileBoxRTree.find(std::make_tuple(ratio, ratio, 1, t)); rtreeIt != m_rtToTileBoxRTree.end()) {
            rtreeIt->second->query(bgi::intersects(tileBox), std::back_inserter(q));
          }
          if (!q.empty()) {
            tileIndices.push_back(q.front().second);
          }
        }
      }
      // Read tiles and sample
      for (size_t tileIdx : tileIndices) {
        const ZImgSubBlock* tile = m_allTiles[tileIdx].get();
        auto imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIdx), *tile);
        if (!imgPtr || imgPtr->isEmpty()) continue;
        // Sample across XY with stride
        for (size_t c = 0; c < m_imgInfo.numChannels; ++c) {
          if (imgPtr->isType<float>()) {
            for (size_t y = 0; y < imgPtr->height() && totalSamples < maxSamples; y += step) {
              for (size_t x = 0; x < imgPtr->width() && totalSamples < maxSamples; x += step) {
                float* p = imgPtr->data<float>(x, y, 0, c, 0);
                channelSamples[c].push_back(static_cast<double>(*p));
                ++totalSamples;
              }
            }
          } else if (imgPtr->isType<double>()) {
            for (size_t y = 0; y < imgPtr->height() && totalSamples < maxSamples; y += step) {
              for (size_t x = 0; x < imgPtr->width() && totalSamples < maxSamples; x += step) {
                double* p = imgPtr->data<double>(x, y, 0, c, 0);
                channelSamples[c].push_back(*p);
                ++totalSamples;
              }
            }
          } else {
            // Fallback: fetch value converted to double
            for (size_t y = 0; y < imgPtr->height() && totalSamples < maxSamples; y += step) {
              for (size_t x = 0; x < imgPtr->width() && totalSamples < maxSamples; x += step) {
                double v = imgPtr->value<double>(x, y, 0, c, 0);
                channelSamples[c].push_back(v);
                ++totalSamples;
              }
            }
          }
          if (totalSamples >= maxSamples) break;
        }
        if (totalSamples >= maxSamples) break;
      }
      if (totalSamples >= maxSamples) break;
    }
    if (totalSamples >= maxSamples) break;
  }

  if (totalSamples == 0) {
    return;
  }

  auto percentile = [](std::vector<double>& v, double p) -> double {
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(std::floor(p * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
  };

  double lower = std::numeric_limits<double>::infinity();
  double upper = -std::numeric_limits<double>::infinity();
  for (auto& vc : channelSamples) {
    if (vc.empty()) continue;
    double l = percentile(vc, lp);
    double u = percentile(vc, up);
    if (u < l) std::swap(u, l);
    lower = std::min(lower, l);
    upper = std::max(upper, u);
  }
  if (!std::isfinite(lower) || !std::isfinite(upper) || lower == upper) {
    return;
  }

  m_minIntensity = lower;
  m_maxIntensity = upper;
  m_minMaxState = MinMaxState::Partial;
}

void ZImgPack::buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  // VLOG(1) << "here";
  m_allTiles = subBlocks;

  createTileIndexStructure();

#if 0
  // ZBenchTimer bt;
  // bt.start();

  // get estimation of minmax
  auto ratio = *m_pyramidalRatios.rbegin();
  VLOG(1) << ratio[0] << " " << ratio[1] << " " <<  ratio[2];

  double minV;
  double maxV;
  m_minIntensity = std::numeric_limits<double>::max();
  m_maxIntensity = std::numeric_limits<double>::lowest();

  for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
    auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      for (auto idx : tileIndice) {
        const ZImgSubBlock& tile = *m_allTiles[idx].get();
        std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, idx), tile);
        imgPtr->computeMinMax(minV, maxV);
        m_minIntensity = std::min(m_minIntensity, minV);
        m_maxIntensity = std::max(m_maxIntensity, maxV);
      }
    }
  }

  m_minMaxState = ratio == std::array<size_t, 3>{1, 1, 1} ? MinMaxState::Complete : MinMaxState::Partial;

  // bt.stop();
  // VLOG(1) << bt;
#endif
}

void ZImgPack::createTileIndexStructure()
{
  m_pyramidalRatios.clear();
  m_rtToTileIndice.clear();
  m_rtToTileBoxRTree.clear();

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    const ZImgSubBlock& tile = *m_allTiles[i].get();
    m_pyramidalRatios.insert(std::array<size_t, 3>{tile.xRatio, tile.yRatio, tile.zRatio});
    m_rtToTileIndice[std::make_tuple(tile.xRatio, tile.yRatio, tile.zRatio, tile.t)].push_back(i);
  }
  for (const auto& rtTileIndice : m_rtToTileIndice) {
    std::vector<RTreeValueType> values(rtTileIndice.second.size());
    for (size_t i = 0; i < rtTileIndice.second.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[rtTileIndice.second[i]].get();
      values[i] = std::make_pair(
        TileBoxType(TileCornerType(tile.x, tile.y, tile.z),
                    TileCornerType(tile.x + tile.width - 1, tile.y + tile.height - 1, tile.z + tile.depth - 1)),
        rtTileIndice.second[i]);
    }
    m_rtToTileBoxRTree.emplace_hint(m_rtToTileBoxRTree.end(), rtTileIndice.first, std::make_unique<RTreeType>(values));
  }
}

namespace {

void setInfoToSingleTime(ZImgInfo* info, const ZImgInfo& baseInfo, size_t t)
{
  CHECK(info);
  CHECK(t < baseInfo.numTimes);
  CHECK(baseInfo.timeStamps.size() == baseInfo.numTimes);
  info->numTimes = 1;
  info->timeStamps = {baseInfo.timeStamps[t]};
}

void setInfoToSingleChannel(ZImgInfo* info, const ZImgInfo& baseInfo, size_t c)
{
  CHECK(info);
  CHECK(c < baseInfo.numChannels);
  CHECK(baseInfo.channelNames.size() == baseInfo.numChannels);
  CHECK(baseInfo.channelColors.size() == baseInfo.numChannels);
  info->numChannels = 1;
  info->channelNames = {baseInfo.channelNames[c]};
  info->channelColors = {baseInfo.channelColors[c]};
  info->lastChannelIsAlphaChannel = baseInfo.lastChannelIsAlphaChannel && (c + 1 == baseInfo.numChannels);
}

void setInfoToSingleChannelTime(ZImgInfo* info, const ZImgInfo& baseInfo, size_t c, size_t t)
{
  setInfoToSingleChannel(info, baseInfo, c);
  setInfoToSingleTime(info, baseInfo, t);
}

} // namespace

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio) const
{
  CHECK(m_pyramidalRatios.contains(ratio));
  if (m_ngVolume) {
    CHECK(m_imgInfo.numTimes == 1);
    return assembleImg(ratio, 0);
  }
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  info.voxelSizeX *= static_cast<double>(ratio[0]);
  info.voxelSizeY *= static_cast<double>(ratio[1]);
  info.voxelSizeZ *= static_cast<double>(ratio[2]);
  ZImg res(info);

  for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
    auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
          ZVoxelCoordinate start(std::round(static_cast<double>(tile.x) / ratio[0]),
                                 std::round(static_cast<double>(tile.y) / ratio[1]),
                                 std::round(static_cast<double>(tile.z) / ratio[2]),
                                 0,
                                 t);

          std::shared_ptr<ZImg> imgPtr =
            ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
          res.pasteImg(*imgPtr, start);
        }
      });
    }
  }

  // VLOG(1) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio, size_t t) const
{
  CHECK(m_pyramidalRatios.contains(ratio));
  if (m_ngVolume) {
    CHECK(t == 0);
    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(ratio);
    CHECK(scaleIndexOpt);

    ZImgInfo info = m_imgInfo;
    info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
    info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
    info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
    setInfoToSingleTime(&info, m_imgInfo, t);
    const auto& scale = m_ngVolume->scales().at(*scaleIndexOpt);
    info.voxelSizeX = scale.resolutionNm[0];
    info.voxelSizeY = scale.resolutionNm[1];
    info.voxelSizeZ = scale.resolutionNm[2];

    ZImg res(info);

    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                             {0, 0, 0},
                                                             {static_cast<int64_t>(m_imgInfo.width),
                                                              static_cast<int64_t>(m_imgInfo.height),
                                                              static_cast<int64_t>(m_imgInfo.depth)});
    if (chunks.empty()) {
      return res;
    }

    forEachNeuroglancerChunkBestEffortBlocking(*m_ngVolume,
                                               chunks,
                                               [&](size_t, const auto& chunk, const ZImg& chunkImg) {
                                                 ZVoxelCoordinate start(
                                                   std::round(static_cast<double>(chunk.baseStart[0]) / ratio[0]),
                                                   std::round(static_cast<double>(chunk.baseStart[1]) / ratio[1]),
                                                   std::round(static_cast<double>(chunk.baseStart[2]) / ratio[2]),
                                                   0,
                                                   0);
                                                 if (m_ngSegmentationRgbFor3D) {
                                                   CHECK(m_ngVolume->isSegmentation());
                                                   pasteNeuroglancerSegmentationChunkAsRgb(chunkImg, res, start);
                                                 } else {
                                                   res.pasteImg(chunkImg, start, false);
                                                 }
                                               });

    return res;
  }
  CHECK(t < m_imgInfo.numTimes);
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  info.voxelSizeX *= static_cast<double>(ratio[0]);
  info.voxelSizeY *= static_cast<double>(ratio[1]);
  info.voxelSizeZ *= static_cast<double>(ratio[2]);
  setInfoToSingleTime(&info, m_imgInfo, t);
  ZImg res(info);

  auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
  if (tiit != m_rtToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()), [&](const tbb::blocked_range<size_t>& r) {
      for (size_t i = r.begin(); i != r.end(); ++i) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
        ZVoxelCoordinate start(std::round(static_cast<double>(tile.x) / ratio[0]),
                               std::round(static_cast<double>(tile.y) / ratio[1]),
                               std::round(static_cast<double>(tile.z) / ratio[2]),
                               0,
                               0);

        std::shared_ptr<ZImg> imgPtr =
          ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
        res.pasteImg(*imgPtr, start);
      }
    });
  }

  // VLOG(1) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio, size_t c, size_t t) const
{
  CHECK(m_pyramidalRatios.contains(ratio));

  if (m_ngVolume) {
    CHECK(t == 0);
    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(ratio);
    CHECK(scaleIndexOpt);
    CHECK(c < m_imgInfo.numChannels);

    ZImgInfo info = m_imgInfo;
    info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
    info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
    info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
    setInfoToSingleChannelTime(&info, m_imgInfo, c, t);
    const auto& scale = m_ngVolume->scales().at(*scaleIndexOpt);
    info.voxelSizeX = scale.resolutionNm[0];
    info.voxelSizeY = scale.resolutionNm[1];
    info.voxelSizeZ = scale.resolutionNm[2];

    ZImg res(info);

    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                              {0, 0, 0},
                                                              {static_cast<int64_t>(m_imgInfo.width),
                                                               static_cast<int64_t>(m_imgInfo.height),
                                                               static_cast<int64_t>(m_imgInfo.depth)});
    if (chunks.empty()) {
      return res;
    }

    forEachNeuroglancerChunkBestEffortBlocking(
      *m_ngVolume,
      chunks,
      [&](size_t, const auto& chunk, const ZImg& chunkImg) {
        ZVoxelCoordinate start(std::round(static_cast<double>(chunk.baseStart[0]) / ratio[0]),
                               std::round(static_cast<double>(chunk.baseStart[1]) / ratio[1]),
                               std::round(static_cast<double>(chunk.baseStart[2]) / ratio[2]),
                               0,
                               0);
        if (m_ngSegmentationRgbFor3D) {
          CHECK(m_ngVolume->isSegmentation());
          CHECK(c < 3);
          pasteNeuroglancerSegmentationChunkAsRgbComponent(chunkImg, res, start, c);
        } else {
          start.c = -static_cast<index_t>(c);
          res.pasteImg(chunkImg, start, false);
        }
      });

    return res;
  }

  CHECK(t < m_imgInfo.numTimes);
  CHECK(c < m_imgInfo.numChannels);

  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  info.voxelSizeX *= static_cast<double>(ratio[0]);
  info.voxelSizeY *= static_cast<double>(ratio[1]);
  info.voxelSizeZ *= static_cast<double>(ratio[2]);
  setInfoToSingleChannelTime(&info, m_imgInfo, c, t);
  ZImg res(info);

  auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
  if (tiit != m_rtToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()), [&](const tbb::blocked_range<size_t>& r) {
      for (size_t i = r.begin(); i != r.end(); ++i) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
        ZVoxelCoordinate start(std::round(static_cast<double>(tile.x) / ratio[0]),
                               std::round(static_cast<double>(tile.y) / ratio[1]),
                               std::round(static_cast<double>(tile.z) / ratio[2]),
                               -static_cast<index_t>(c),
                               0);

        std::shared_ptr<ZImg> imgPtr =
          ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
        res.pasteImg(*imgPtr, start);
      }
    });
  }

  return res;
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio, size_t z, int c, size_t t) const
{
  CHECK(m_pyramidalRatios.contains(ratio) && ratio[2] == 1);
  const bool allChannels = c < 0;
  const size_t sc = allChannels ? 0_uz : static_cast<size_t>(c);
  if (!allChannels) {
    CHECK(c >= 0);
    CHECK(sc < m_imgInfo.numChannels) << "Invalid channel index c=" << sc
                                      << " for numChannels=" << m_imgInfo.numChannels;
  }

  if (m_ngVolume) {
    CHECK(t == 0);
    auto scaleIndexOpt = m_ngVolume->scaleIndexForRatio(ratio);
    CHECK(scaleIndexOpt);

    ZImgInfo info = m_imgInfo;
    info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
    info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
    info.depth = 1;
    if (allChannels) {
      setInfoToSingleTime(&info, m_imgInfo, t);
    } else {
      setInfoToSingleChannelTime(&info, m_imgInfo, sc, t);
    }
    const auto& scale = m_ngVolume->scales().at(*scaleIndexOpt);
    info.voxelSizeX = scale.resolutionNm[0];
    info.voxelSizeY = scale.resolutionNm[1];
    info.voxelSizeZ = scale.resolutionNm[2];

    ZImg res(info);

    const int64_t iz = static_cast<int64_t>(z);
    const auto chunks = m_ngVolume->chunksIntersectingBaseBox(*scaleIndexOpt,
                                                             {0, 0, iz},
                                                             {static_cast<int64_t>(m_imgInfo.width),
                                                              static_cast<int64_t>(m_imgInfo.height),
                                                              iz + 1});
    if (chunks.empty()) {
      return res;
    }

    forEachNeuroglancerChunkBestEffortBlocking(
      *m_ngVolume,
      chunks,
      [&](size_t, const auto& chunk, const ZImg& chunkImg) {
        const ZVoxelCoordinate start(std::round(static_cast<double>(chunk.baseStart[0]) / ratio[0]),
                                     std::round(static_cast<double>(chunk.baseStart[1]) / ratio[1]),
                                     static_cast<index_t>(chunk.baseStart[2] - iz),
                                     0,
                                     0);
        if (m_ngSegmentationRgbFor3D) {
          CHECK(m_ngVolume->isSegmentation());
          if (allChannels) {
            pasteNeuroglancerSegmentationChunkAsRgb(chunkImg, res, start);
          } else {
            CHECK(sc < 3);
            pasteNeuroglancerSegmentationChunkAsRgbComponent(chunkImg, res, start, sc);
          }
        } else {
          if (allChannels) {
            res.pasteImg(chunkImg, start, false);
          } else {
            ZVoxelCoordinate startWithChannel = start;
            startWithChannel.c = -static_cast<index_t>(sc);
            res.pasteImg(chunkImg, startWithChannel, false);
          }
        }
      });
    return res;
  }
  CHECK(t < m_imgInfo.numTimes);
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = 1;
  info.voxelSizeX *= static_cast<double>(ratio[0]);
  info.voxelSizeY *= static_cast<double>(ratio[1]);
  info.voxelSizeZ *= static_cast<double>(ratio[2]);
  if (allChannels) {
    setInfoToSingleTime(&info, m_imgInfo, t);
  } else {
    setInfoToSingleChannelTime(&info, m_imgInfo, sc, t);
  }
  ZImg res(info);

  auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
  if (tiit != m_rtToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()), [&](const tbb::blocked_range<size_t>& r) {
      for (size_t i = r.begin(); i != r.end(); ++i) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
        if (static_cast<index_t>(z) >= tile.z && static_cast<index_t>(z) < tile.z + tile.depth) {
          ZVoxelCoordinate start(std::round(static_cast<double>(tile.x) / ratio[0]),
                                 std::round(static_cast<double>(tile.y) / ratio[1]),
                                 tile.z - static_cast<index_t>(z),
                                 allChannels ? 0 : -static_cast<index_t>(sc),
                                 0);

          std::shared_ptr<ZImg> imgPtr =
            ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
          res.pasteImg(*imgPtr, start);
        }
      }
    });
  }
  return res;
}

void ZImgPack::updateDerivedData()
{
  // VLOG(1) << m_imgInfo;
  if (!m_diskCached) {
    m_maximumProjectedAlongZImg.clear();
    if (m_imgInfo.depth == 1) {
      m_maximumProjectedAlongZImg = m_img.createView();
    }
  } else {
    m_img.clear();
    m_maximumProjectedAlongZImg.clear();
  }

  // CHECK(m_minMaxState != MinMaxState::Invalid);
  if (m_minMaxState == MinMaxState::Complete) {
    if (m_imgInfo.validBitCount != 12 && m_imgInfo.voxelByteNumber() == 2 &&
        m_imgInfo.voxelFormat == VoxelFormat::Unsigned && m_maxIntensity < 4096) {
      m_imgInfo.validBitCount = 12;
      if (!m_diskCached) {
        m_img.infoRef().validBitCount = 12;
      }
    }
    if (m_imgInfo.validBitCount != 1 && m_imgInfo.voxelByteNumber() == 1 &&
        m_imgInfo.voxelFormat == VoxelFormat::Unsigned && m_maxIntensity < 2) {
      m_imgInfo.validBitCount = 1;
      if (!m_diskCached) {
        m_img.infoRef().validBitCount = 1;
      }
    }
  }
  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();
  // For float data, prefer computed min/max when available (Partial or Complete)
  if (m_imgInfo.voxelFormat == VoxelFormat::Float && m_minMaxState != MinMaxState::Invalid) {
    m_rangeMin = m_minIntensity;
    m_rangeMax = m_maxIntensity;
  }

  m_sizeInfo.clear();
  m_detailedInfo.clear();

  updateNameTootip();
}

void ZImgPack::updateNameTootip()
{
  if (m_ngVolume) {
    const QUrl url(m_ngVolume->rootUrl());
    QString display = m_ngVolume->rootUrl();
    if (url.isValid() && !url.host().isEmpty()) {
      display = url.host() + url.path();
    }
    m_name = QString("Neuroglancer Precomputed: %1").arg(display);
    m_tooltip = m_ngVolume->rootUrl();
    return;
  }

  if (isSequence()) {
    m_name =
      QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" %1 Sequence").arg(enumToQString(m_imgSource.catDim));
    m_tooltip = m_imgSource.filenames.join("\n");
  } else {
    m_name = QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" scene %1").arg(m_imgSource.scene + 1);
    m_tooltip = m_imgSource.filenames[0] + QString(" scene %1").arg(m_imgSource.scene + 1);
  }
}

std::array<uint8_t, 32> ZImgPack::datasetFingerprintForCache() const
{
  if (m_datasetFingerprintValid.load(std::memory_order_acquire)) {
    return m_datasetFingerprint;
  }

  const std::lock_guard<std::mutex> lock(m_datasetFingerprintMutex);
  if (m_datasetFingerprintValid.load(std::memory_order_relaxed)) {
    return m_datasetFingerprint;
  }

  CHECK(std::endian::native == std::endian::little);

  boost::hash2::sha2_256 hash;
  static constexpr char kPrefix[] = "atlas_cache_imgpack_fingerprint_v1\n";
  hash.update(kPrefix, sizeof(kPrefix) - 1);

  auto updateU64 = [&hash](uint64_t v) { hash.update(&v, sizeof(v)); };

  auto updateI64 = [&updateU64](qint64 v) { updateU64(static_cast<uint64_t>(v)); };

  auto updateBytesWithLen = [&hash, &updateU64](const void* data, size_t bytes) {
    updateU64(static_cast<uint64_t>(bytes));
    if (bytes == 0) {
      return;
    }
    CHECK(data != nullptr);
    hash.update(data, bytes);
  };

  if (m_ngVolume) {
    // Neuroglancer sources are identified by their root URL.
    const QString url = m_ngVolume->rootUrl();
    const QByteArray urlBytes = url.toUtf8();
    updateBytesWithLen(urlBytes.constData(), static_cast<size_t>(urlBytes.size()));
  } else {
    // Include logical source parameters (region, scene, catDim, etc.).
    {
      const std::string sourceJson = jsonToString(m_imgSource);
      updateBytesWithLen(sourceJson.data(), sourceJson.size());
    }

    // Include file mtimes/sizes for correctness.
    updateU64(static_cast<uint64_t>(m_imgSource.filenames.size()));
    for (int i = 0; i < m_imgSource.filenames.size(); ++i) {
      const QString fn = m_imgSource.filenames[i];
      QFileInfo fi(fn);

      QString canonical = fi.canonicalFilePath();
      if (canonical.isEmpty()) {
        canonical = fi.absoluteFilePath();
      }

      const QByteArray pathBytes = canonical.toUtf8();
      updateBytesWithLen(pathBytes.constData(), static_cast<size_t>(pathBytes.size()));

      const qint64 sizeBytes = fi.size();
      updateI64(sizeBytes);

      const qint64 mtimeMs = fi.lastModified().toUTC().toMSecsSinceEpoch();
      updateI64(mtimeMs);
    }
  }

  const boost::hash2::sha2_256::result_type digest = hash.result();
  CHECK(digest.size() == m_datasetFingerprint.size());
  std::memcpy(m_datasetFingerprint.data(), digest.data(), m_datasetFingerprint.size());
  m_datasetFingerprintValid.store(true, std::memory_order_release);

  return m_datasetFingerprint;
}

std::array<size_t, 3> ZImgPack::ratioForScale(double xScale, double yScale, double zScale) const
{
  CHECK(!m_pyramidalRatios.empty());
  CHECK(xScale > 0);
  CHECK(yScale > 0);
  CHECK(zScale > 0);

  // For network-backed Neuroglancer sources, prefer a pyramid level whose effective
  // on-screen resolution is closest to 1:1. This avoids oversampling (which can cause
  // hundreds of small HTTP requests when zoomed out) while still preserving detail
  // when zoomed in.
  if (m_ngVolume) {
    std::array<size_t, 3> best = {1, 1, 1};
    double bestErr = std::numeric_limits<double>::infinity();
    size_t bestRatioSum = 0;

    for (const auto& ratio : m_pyramidalRatios) {
      const double ex = std::abs(static_cast<double>(ratio[0]) * xScale - 1.0);
      const double ey = std::abs(static_cast<double>(ratio[1]) * yScale - 1.0);
      const double ez = std::abs(static_cast<double>(ratio[2]) * zScale - 1.0);
      const double err = ex + ey + ez;
      const size_t ratioSum = ratio[0] + ratio[1] + ratio[2];

      // Tie-break toward coarser levels (larger ratios) to reduce network I/O.
      if (err < bestErr - 1e-12 || (std::abs(err - bestErr) <= 1e-12 && ratioSum > bestRatioSum)) {
        best = ratio;
        bestErr = err;
        bestRatioSum = ratioSum;
      }
    }

    return best;
  }

  return readRatioOf(std::max(1.0, std::floor(1.0 / xScale)),
                     std::max(1.0, std::floor(1.0 / yScale)),
                     std::max(1.0, std::floor(1.0 / zScale)));
}

std::array<size_t, 3> ZImgPack::readRatioOf(size_t xRatio, size_t yRatio, size_t zRatio) const
{
  std::array<size_t, 3> readRatio = {1, 1, 1};
  size_t lastRatioSum = 3;
  for (const auto& ratio : m_pyramidalRatios) {
    if (ratio[0] <= xRatio && ratio[1] <= yRatio && ratio[2] <= zRatio) {
      size_t ratioSum = ratio[0] + ratio[1] + ratio[2];
      if (ratioSum > lastRatioSum) {
        lastRatioSum = ratioSum;
        readRatio = ratio;
      }
    }
  }
  return readRatio;
}

} // namespace nim
