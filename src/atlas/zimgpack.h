#pragma once

#include "zimg.h"
#include "zimgsliceprovider.h"
#include "zlog.h"
#include <QRectF>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <tuple>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

namespace nim {

class ZImgPackSubBlock : public ZImgSubBlock
{
public:
  enum class Type
  {
    CacheFile, OrigSource, OrigSourceMIP
  };

  ZImgPackSubBlock(const QString& fn, size_t ratio, size_t t, size_t z,
                   int64_t x, int64_t y, size_t width, size_t height);

  ZImgPackSubBlock(const ZImgSource& imgSource, size_t ratio, size_t t, size_t z,
                   int64_t x, int64_t y, size_t width, size_t height);

  ZImgPackSubBlock(const ZImgSource& imgSource, size_t ratio, size_t t, size_t sliceStart, size_t sliceEnd,
                   int64_t x, int64_t y, size_t width, size_t height);

  ZImgPackSubBlock(std::shared_ptr<ZImg>& img, size_t ratio, size_t t, size_t z,
                   int64_t x, int64_t y, size_t width, size_t height);

  ZImgPackSubBlock(const std::shared_ptr<ZImg>& img, size_t ratio, size_t t, size_t sliceStart, size_t sliceEnd,
                   int64_t x, int64_t y, size_t width, size_t height);

  std::shared_ptr<ZImg> read() const override;
  ZImgInfo readInfo() const override;

protected:
  Type m_type;
  ZImgSource m_imgSource;
  size_t m_zStart;
  size_t m_zEnd;
  std::shared_ptr<ZImg> m_img;
};

// might throw Exception
class ZImgPack : public ZImgSliceProvider
{
public:
  using HashKeyType = std::tuple<const ZImgPack*, size_t>;

  enum class MinMaxState
  {
    Invalid, Partial, Complete
  };

  ZImgPack(ZImg& img, const QString& fileName);

  ZImgPack(const QString& fileName, size_t scene, FileFormat format, size_t numScene = 0,
           const ZImgInfo* info = nullptr,
           const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  ZImgPack(const QStringList& files, Dimension catDim, size_t scene, FileFormat format,
           size_t numScene = 0, const ZImgInfo* info = nullptr,
           const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  virtual ~ZImgPack();

  const QString& sizeInfo() const;

  const QString& detailedInfo() const;

  inline double rangeMin() const
  { return m_rangeMin; }

  inline double rangeMax() const
  { return m_rangeMax; }

  inline bool hasMinMax() const
  { return m_minMaxState != MinMaxState::Invalid; }

  inline double minIntensity() const
  {
    CHECK(hasMinMax());
    return m_minIntensity;
  }

  inline double maxIntensity() const
  {
    CHECK(hasMinMax());
    return m_maxIntensity;
  }

  inline bool isSequence() const
  { return m_imgSource.filenames.size() > 1; }

  inline bool pathHasMultipleTiles() const
  { return m_numScenes > 1; }

  inline const QString& name() const
  { return m_name; }

  inline const QString& tooltip() const
  { return m_tooltip; }

  void setChannelColor(size_t c, col4 col);

  ZImgInfo& imgInfoRef()
  { return m_imgInfo; }

  const ZImgSource& imgSource() const
  { return m_imgSource; }

  const QStringList& paths() const
  { return m_imgSource.filenames; }

  size_t sceneIdx() const
  { return m_imgSource.scene; }

  size_t numScenes() const
  { return m_numScenes; }

  Dimension catDim() const
  { return m_imgSource.catDim; }

  bool hasUnsavedChange() const
  { return m_hasUnsavedChange; }

  void save(const QString& fileName, FileFormat format, Compression comp);

  bool needUpdate(const QRectF& viewport, double scale,
                  const QRectF& oldViewport, double oldScale,
                  size_t t, size_t z, bool mip) const;

  void retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>>& imgs,
                           std::vector<QPoint>& locs,
                           std::vector<double>& scales,
                           size_t z,
                           size_t t,
                           const QRectF& viewport,
                           double scale,
                           bool mip) const;

  double value(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  // same as value but can use low resolution image value
  double displayValue(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  ZImg crop(const ZImgRegion& region) const;

  ZImg resizedImg(size_t width, size_t height, size_t depth, size_t t) const;

  void readRegionToImg(size_t xyRatio, size_t zRatio, int64_t sx, int64_t sy, int64_t sz, size_t sc, size_t t,
                       ZImg& res) const;

  // only for non-disk-cached image
  bool isDiskCached() const
  { return m_diskCached; }

  const ZImg& img() const
  {
    CHECK(!m_diskCached);
    return m_img;
  }

  const ZImg& maxZProjectedImg() const;

  ZImg& maxZProjectedImg();

  // ZImgSliceProvider interface
public:
  const ZImgInfo& imgInfo() const override
  { return m_imgInfo; }

  std::set<size_t> ratios() const override;

  ZImg slice(size_t z, size_t t, size_t ratio) const override;

  ZImg allSlices(size_t t, size_t ratio) const override;

  ZImg wholeImg(size_t ratio) const override;

protected:
  // will take ownership of img
  void createSliceTiles(ZImg* img, size_t z, size_t t, bool mip = false);

  void buildPyramidal(ZImg& img);

  void buildPyramidal();

  void buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks);

  void createTileIndexStructure();

  ZImg assembleImg(size_t ratio) const;

  ZImg assembleImg(size_t ratio, size_t t) const;

  ZImg assembleImg(size_t ratio, size_t t, size_t z) const;

  void updateDerivedData();

  void updateNameTootip();

private:
  size_t ratioForScale(double scale) const;

  size_t readRatioOf(size_t needRatio) const;

protected:
  ZImgInfo m_imgInfo;
  ZImgSource m_imgSource;
  size_t m_numScenes;
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
  int64_t m_fastReadSizeThreshold = 100 * 1024 * 1024;  // 100MB

  std::vector<std::shared_ptr<ZImgSubBlock>> m_allTiles;
  std::map<size_t, QSize> m_ratioToSize;
  using RTZType = std::tuple<size_t, size_t, int>;
  std::map<RTZType, std::vector<size_t>> m_rtzToTileIndice;
  using TileCornerType = bg::model::point<int64_t, 2, bg::cs::cartesian>;
  using TileBoxType = bg::model::box<TileCornerType>;
  using RTreeValueType = std::pair<TileBoxType, size_t>;
  using RTreeType = bgi::rtree<RTreeValueType, bgi::quadratic<16>>;
  using RTZToTileBoxRTreeType = std::map<RTZType, std::unique_ptr<RTreeType>>;
  RTZToTileBoxRTreeType m_rtzToTileBoxRTree;

  double m_minIntensity;
  double m_maxIntensity;
  MinMaxState m_minMaxState;

  bool m_diskCached;
  ZImg m_img;
  mutable ZImg m_maximumProjectedAlongZImg;
};

}  // namespace nim

