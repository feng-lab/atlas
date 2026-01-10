#pragma once

#include "zimg.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include <QRectF>
#include <QObject>
#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <tuple>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

namespace nim {

class ZNeuroglancerPrecomputedVolume;
class ZNeuroglancerPrecomputedMeshSource;
class ZNeuroglancerPrecomputedAnnotationsSource;
class ZNeuroglancerPrecomputedSkeletonSource;

class ZImgPackSubBlock : public ZImgSubBlock
{
public:
  ZImgPackSubBlock(const std::shared_ptr<ZImg>& img,
                   size_t ratio,
                   index_t t,
                   index_t z,
                   index_t x,
                   index_t y,
                   size_t width,
                   size_t height);

  [[nodiscard]] std::shared_ptr<ZImg> read() const override;

  [[nodiscard]] ZImgInfo readInfo() const override;

protected:
  std::shared_ptr<ZImg> m_img;
};

// might throw Exception
class ZImgPack
  : public QObject
  , public ZImgSliceProvider
{
  Q_OBJECT

public:
  enum class MinMaxState
  {
    Invalid,
    Partial,
    Complete
  };

  explicit ZImgPack(ZImgSource imgSource,
                    ZImgInfo* pInfo = nullptr,
                    std::vector<std::shared_ptr<ZImgSubBlock>>* pSceneSubBlocks = nullptr);

  explicit ZImgPack(std::shared_ptr<ZNeuroglancerPrecomputedVolume> ngVolume);

  ~ZImgPack() override = default;

  const QString& sizeInfo() const;

  const QString& detailedInfo() const;

  double rangeMin() const
  {
    return m_rangeMin;
  }

  double rangeMax() const
  {
    return m_rangeMax;
  }

  bool hasMinMax() const
  {
    return m_minMaxState != MinMaxState::Invalid;
  }

  double minIntensity() const
  {
    CHECK(hasMinMax());
    return m_minIntensity;
  }

  double maxIntensity() const
  {
    CHECK(hasMinMax());
    return m_maxIntensity;
  }

  bool isSequence() const
  {
    return m_imgSource.filenames.size() > 1;
  }

  const QString& name() const
  {
    return m_name;
  }

  const QString& tooltip() const
  {
    return m_tooltip;
  }

  // Returns a stable fingerprint suitable for persistent cache keys.
  //
  // For file-based sources, this includes the ordered file list plus each file's
  // canonical path, size, and modification time. The result is memoized for the
  // lifetime of this ZImgPack instance.
  [[nodiscard]] std::array<uint8_t, 32> datasetFingerprintForCache() const;

  [[nodiscard]] bool isNeuroglancerPrecomputed() const
  {
    return static_cast<bool>(m_ngVolume);
  }

  [[nodiscard]] std::shared_ptr<ZNeuroglancerPrecomputedVolume> neuroglancerVolumeShared() const
  {
    CHECK(m_ngVolume);
    return m_ngVolume;
  }

  [[nodiscard]] QString neuroglancerRootUrl() const;

  // Returns true when this ZImgPack is a Neuroglancer segmentation volume that has been adapted
  // to present an RGB (3x uint8) view for 3D rendering.
  [[nodiscard]] bool isNeuroglancerSegmentationRgbFor3D() const
  {
    return m_ngSegmentationRgbFor3D;
  }

  // Creates an RGB (3-channel uint8) view of a Neuroglancer segmentation volume for 3D visualization.
  // This does not change the underlying dataset; it only affects how reads are presented to callers
  // (e.g. Z3DImg expects scalar channels and will treat these as R/G/B).
  [[nodiscard]] std::unique_ptr<ZImgPack> makeNeuroglancerSegmentationRgbFor3D() const;

  // ---- Neuroglancer external sources (mesh/skeletons) ----
  //
  // Neuroglancer "mesh" and "skeletons" are key→geometry stores, not volumes.
  // Many datasets do not declare these sources in the volume's info. To support
  // explicit per-dataset configuration, Atlas allows users to register override
  // source URLs. These overrides are used by UI actions (mesh/skeleton loading)
  // and do not affect image chunk reads.
  [[nodiscard]] bool hasNeuroglancerMeshSourceOverride() const;
  [[nodiscard]] QString neuroglancerMeshSourceOverrideUrl() const;
  [[nodiscard]] bool hasNeuroglancerSkeletonSourceOverride() const;
  [[nodiscard]] QString neuroglancerSkeletonSourceOverrideUrl() const;
  [[nodiscard]] bool hasNeuroglancerAnnotationsSourceOverride() const;
  [[nodiscard]] QString neuroglancerAnnotationsSourceOverrideUrl() const;

  // Returns true if either the dataset declares a mesh/skeletons directory in its volume info
  // or the user has registered an override URL.
  [[nodiscard]] bool hasNeuroglancerMeshSourceConfigured() const;
  [[nodiscard]] bool hasNeuroglancerSkeletonSourceConfigured() const;
  [[nodiscard]] bool hasNeuroglancerAnnotationsSourceConfigured() const;

  // Sets an override URL (absolute URL or relative path) for the mesh/skeleton source used by this dataset.
  // If userText is relative, it is resolved against the dataset's root URL. The resulting URL is normalized
  // to a directory form with a trailing slash. Returns false and sets errorMsg on parse/validation failure.
  bool setNeuroglancerMeshSourceOverride(QString userText, QString* errorMsg);
  bool setNeuroglancerSkeletonSourceOverride(QString userText, QString* errorMsg);
  bool setNeuroglancerAnnotationsSourceOverride(QString userText, QString* errorMsg);

  void clearNeuroglancerMeshSourceOverride();
  void clearNeuroglancerSkeletonSourceOverride();
  void clearNeuroglancerAnnotationsSourceOverride();

  // Opens the configured mesh/skeleton source. If a user override is set, it is used; otherwise the dataset's
  // declared 'mesh'/'skeletons' directory is used. This may perform network I/O.
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> loadNeuroglancerMeshSourceBlocking() const;
  std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> loadNeuroglancerSkeletonSourceBlocking() const;
  std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> loadNeuroglancerAnnotationsSourceBlocking() const;

  void setChannelColor(size_t c, col4 col);

  ZImgInfo& imgInfoRef()
  {
    return m_imgInfo;
  }

  const ZImgSource& imgSource() const
  {
    return m_imgSource;
  }

  const QStringList& paths() const
  {
    return m_imgSource.filenames;
  }

  size_t sceneIdx() const
  {
    return m_imgSource.scene;
  }

  Dimension catDim() const
  {
    return m_imgSource.catDim;
  }

  bool hasUnsavedChange() const
  {
    return m_hasUnsavedChange;
  }

  void save(const QString& fileName, FileFormat format, const ZImgWriteParameters& paras);

  bool needUpdate(const QRectF& viewport,
                  double scale,
                  const QRectF& oldViewport,
                  double oldScale,
                  size_t t,
                  size_t z,
                  bool mip) const;

  void retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>>& imgs,
                           std::vector<QPoint>& locs,
                           std::vector<double>& scales,
                           size_t z,
                           size_t t,
                           const QRectF& viewport,
                           double scale) const;

  void retrieveCoveredMIPImgs(std::vector<std::shared_ptr<ZImg>>& imgs,
                              std::vector<QPoint>& locs,
                              std::vector<double>& scales,
                              size_t zStart,
                              size_t zEnd,
                              size_t t,
                              const QRectF& viewport,
                              double scale) const;

  double value(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  // same as value but can use low resolution image value
  double displayValue(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  // Neuroglancer: returns a cached segmentation ID (uint64) at the specified base voxel coordinate,
  // without triggering any network I/O. This is suitable for hot UI paths (e.g. context-menu actions)
  // that must not block or initiate downloads. Returns nullopt if the needed chunk is not in cache.
  [[nodiscard]] std::optional<uint64_t> tryGetCachedNeuroglancerSegmentationId(size_t x, size_t y, size_t z) const;

  ZImg crop(const ZImgRegion& region) const;

  ZImg resizedImg(size_t width, size_t height, size_t depth, size_t t) const;

#if 0
  folly::Future<std::shared_ptr<ZImg>> readRegionToImg(index_t xyRatio,
                                                       index_t zRatio,
                                                       index_t sx,
                                                       index_t sy,
                                                       index_t sz,
                                                       size_t sc,
                                                       size_t t,
                                                       const ZImgInfo& resInfo,
                                                       double displayRangeMin,
                                                       double displayRangeMax,
                                                       const folly::CancellationToken& cancellationToken) const;
#endif

  folly::coro::Task<void> readTileToImgAsync(size_t tileIndex,
                                             ZImg* img,
                                             index_t xyRatio,
                                             index_t zRatio,
                                             index_t sx,
                                             index_t sy,
                                             index_t sz,
                                             size_t sc,
                                             std::array<size_t, 3> readRatio) const;

  folly::coro::Task<std::shared_ptr<ZImg>> readRegionToImgAsync(index_t xyRatio,
                                                                index_t zRatio,
                                                                index_t sx,
                                                                index_t sy,
                                                                index_t sz,
                                                                size_t sc,
                                                                size_t t,
                                                                const ZImgInfo& resInfo,
                                                                double displayRangeMin,
                                                                double displayRangeMax) const;

  __forceinline bool isEmptyBlock(index_t xyRatio,
                                  index_t zRatio,
                                  index_t sx,
                                  index_t sy,
                                  index_t sz,
                                  size_t sc,
                                  size_t t,
                                  size_t width,
                                  size_t height,
                                  size_t depth,
                                  double displayRangeMin) const
  {
    double maxIntensity;
    if (m_blockInfo.cvisit(std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, width, height, depth),
                           [&](const auto& x) {
                             maxIntensity = x.second.second;
                           })) {
      return maxIntensity <= displayRangeMin;
    }

    return false;
  }

  // only for non-disk-cached image
  bool isDiskCached() const
  {
    return m_diskCached;
  }

  const ZImg& img() const
  {
    CHECK(!m_diskCached);
    return m_img;
  }

  const ZImg& maxZProjectedImg(size_t zStart, size_t zEnd) const;

  void show3DImgContextMenu(QPoint globalPos, float x, float y, float z, bool enter, bool exit);

  // ZImgSliceProvider interface

public:
  ZImgInfo imgInfo() const override
  {
    return m_imgInfo;
  }

  ZImg slice(size_t z, size_t t) const override;

  ZImg allSlices(size_t t) const override;

  ZImg wholeImg() const override;

Q_SIGNALS:
  void enterSubregionView(float x, float y, float z);

  void exitSubregionView();

protected:
  // will take ownership of img
  void createSliceTiles(ZImg* img, size_t z, size_t t);

  void buildPyramidal(ZImg& img);

  void buildPyramidal();
  // Build only the pyramidal tile index (metadata + R-tree) using ZImgTileSubBlock
  // without reading or generating downsampled images. This is fast and sufficient
  // for on-demand rendering paths.
  void buildPyramidalIndexOnly();

  // Compute a fast percentile-based display window for float data by sampling
  // a few base-ratio tiles. Sets m_minIntensity/m_maxIntensity and marks
  // m_minMaxState=Partial when successful.
  void computeQuickWindowIfNeeded();

  void buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks);

  void createTileIndexStructure();

  ZImg assembleImg(std::array<size_t, 3> ratio) const;

  ZImg assembleImg(std::array<size_t, 3> ratio, size_t t) const;

  ZImg assembleImg(std::array<size_t, 3> ratio, size_t t, size_t z) const;

  void updateDerivedData();

  void updateNameTootip();

private:
  std::array<size_t, 3> ratioForScale(double xScale, double yScale, double zScale) const;

  std::array<size_t, 3> readRatioOf(size_t xRatio, size_t yRatio, size_t zRatio) const;

protected:
  ZImgInfo m_imgInfo;
  ZImgMetadata m_imgMetaData;
  ZImgSource m_imgSource;
  bool m_hasUnsavedChange;

  // derived data

private:
  mutable QString m_sizeInfo;
  mutable QString m_detailedInfo;
  double m_rangeMin;
  double m_rangeMax;
  QString m_name;
  QString m_tooltip;

  size_t m_tileSize = 512;
  size_t m_fastReadSizeThreshold = 1_i64 * 1024 * 1024 * 1024; // 1000MB

  std::vector<std::shared_ptr<ZImgSubBlock>> m_allTiles;
  std::set<std::array<size_t, 3>> m_pyramidalRatios;
  using RTType = std::tuple<size_t, size_t, size_t, size_t>;
  std::map<RTType, std::vector<size_t>> m_rtToTileIndice;
  using TileCornerType = bg::model::point<index_t, 3, bg::cs::cartesian>;
  using TileBoxType = bg::model::box<TileCornerType>;
  using RTreeValueType = std::pair<TileBoxType, size_t>;
  using RTreeType = bgi::rtree<RTreeValueType, bgi::rstar<16>>;
  using RTToTileBoxRTreeType = std::map<RTType, std::unique_ptr<RTreeType>>;
  RTToTileBoxRTreeType m_rtToTileBoxRTree{};

  double m_minIntensity;
  double m_maxIntensity;
  MinMaxState m_minMaxState;

  std::shared_ptr<ZNeuroglancerPrecomputedVolume> m_ngVolume;
  bool m_ngSegmentationRgbFor3D = false;

  // Optional user-registered Neuroglancer external source URLs (normalized, with trailing slash).
  // These are stored on the dataset object (shared by aliases) so the configuration is explicit.
  mutable std::mutex m_ngExternalSourcesMutex;
  QString m_ngMeshSourceOverrideUrl;
  QString m_ngSkeletonSourceOverrideUrl;
  QString m_ngAnnotationsSourceOverrideUrl;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> m_ngMeshSourceOverride;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> m_ngSkeletonSourceOverride;
  mutable std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> m_ngAnnotationsSourceOverride;

  bool m_diskCached;
  ZImg m_img;
  mutable ZImg m_maximumProjectedAlongZImg;
  mutable size_t m_mipZStart;
  mutable size_t m_mipZEnd;

  mutable std::vector<std::shared_ptr<ZImg>> m_mipImgs;

  using BlockInfoKeyType =
    std::tuple<index_t, index_t, index_t, index_t, index_t, size_t, size_t, size_t, size_t, size_t>;
  mutable boost::concurrent_flat_map<BlockInfoKeyType, std::pair<double, double>> m_blockInfo;

  mutable std::mutex m_datasetFingerprintMutex;
  mutable std::atomic<bool> m_datasetFingerprintValid{false};
  mutable std::array<uint8_t, 32> m_datasetFingerprint{};
};

} // namespace nim
