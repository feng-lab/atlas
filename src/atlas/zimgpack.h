#pragma once

#include "zimg.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include <QRectF>
#include <folly/futures/Future.h>
#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <tbb/concurrent_unordered_map.h>
#include <tuple>
#include <array>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

namespace nim {

class ZImgPackSubBlock : public ZImgSubBlock
{
public:
  ZImgPackSubBlock(std::shared_ptr<ZImg>& img,
                   size_t ratio,
                   size_t t,
                   size_t z,
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

  ZImg crop(const ZImgRegion& region) const;

  ZImg resizedImg(size_t width, size_t height, size_t depth, size_t t) const;

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
    if (auto it = m_blockInfo.find(std::make_tuple(xyRatio, zRatio, sx, sy, sz, sc, t, width, height, depth));
        it != m_blockInfo.end()) {
      if (const auto [minv, maxv] = it->second; maxv <= displayRangeMin) {
        return true;
      }
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

  bool m_diskCached;
  ZImg m_img;
  mutable ZImg m_maximumProjectedAlongZImg;
  mutable size_t m_mipZStart;
  mutable size_t m_mipZEnd;

  mutable std::vector<std::shared_ptr<ZImg>> m_mipImgs;

  using BlockInfoKeyType =
    std::tuple<index_t, index_t, index_t, index_t, index_t, size_t, size_t, size_t, size_t, size_t>;
  mutable tbb::concurrent_unordered_map<BlockInfoKeyType, std::pair<double, double>> m_blockInfo;
};

} // namespace nim
