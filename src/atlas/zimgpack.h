#ifndef ZIMGPACK_H
#define ZIMGPACK_H

#include "zimg.h"
#include <tuple>
#include <QRectF>
#include "zimgsliceprovider.h"

namespace nim {

class ZImgPack;
// 0    1     2  3  4  5  6       7       8
// pt, ratio, t, z, x, y, width, height, mip
using ZImgTileKey = std::tuple<ZImgPack*, size_t, size_t, size_t, int64_t, int64_t, size_t, size_t, bool>;

class ZImgPackSubBlock : public ZImgSubBlock
{
public:
  enum class Type {
    CacheFile, OrigSource, OrigSourceMIP
  };

  ZImgPackSubBlock(const QString &fn, const ZImgTileKey &key);
  ZImgPackSubBlock(const ZImgSource &imgSource, size_t t, size_t slice, const ZImgTileKey &key);
  ZImgPackSubBlock(const ZImgSource &imgSource, size_t t, size_t sliceStart, size_t sliceEnd, const ZImgTileKey &key);
  virtual ~ZImgPackSubBlock() {}

  virtual ZImg read() const override;

protected:
  Type m_type;
  ZImgSource m_imgSource;
  size_t m_zStart;
  size_t m_zEnd;
};

class ZImgPack : public ZImgSliceProvider
{
public:
  enum class MinMaxState {
    Invalid, Partial, Complete
  };

  ZImgPack(ZImg& img, const QString &fileName);
  ZImgPack(const QString &fileName, size_t scene, FileFormat format, size_t numScene = 0, const ZImgInfo *info = nullptr,
           const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock = nullptr);
  ZImgPack(const QStringList &files, Dimension catDim, size_t scene, FileFormat format,
           size_t numScene = 0, const ZImgInfo *info = nullptr,
           const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock = nullptr);
  virtual ~ZImgPack();

  const QString& sizeInfo() const;
  inline double rangeMin() const { return m_rangeMin; }
  inline double rangeMax() const { return m_rangeMax; }
  inline bool hasMinMax() const { return m_minMaxState != MinMaxState::Invalid; }
  inline double minIntensity() const { assert(hasMinMax()); return m_minIntensity; }
  inline double maxIntensity() const { assert(hasMinMax()); return m_maxIntensity; }
  inline bool isSequence() const { return m_imgSource.filenames.size() > 1; }
  inline bool pathHasMultipleTiles() const { return m_numScenes > 1; }

  inline const QString& name() const { return m_name; }
  inline const QString& tooltip() const { return m_tooltip; }

  ZImgInfo& imgInfoRef() { return m_imgInfo; }
  const ZImgSource& imgSource() const { return m_imgSource; }
  const QStringList& paths() const { return m_imgSource.filenames; }
  size_t sceneIdx() const { return m_imgSource.scene; }
  size_t numScenes() const { return m_numScenes; }
  Dimension catDim() const { return m_imgSource.catDim; }
  bool hasUnsavedChange() const { return m_hasUnsavedChange; }

  int offsetX() const { return m_offsetX; }
  int offsetY() const { return m_offsetY; }
  int offsetZ() const { return m_offsetZ; }
  int offsetT() const { return m_offsetT; }
  void setOffsetX(int off) { m_offsetX = off; }
  void setOffsetY(int off) { m_offsetY = off; }
  void setOffsetZ(int off) { m_offsetZ = off; }
  void setOffsetT(int off) { m_offsetT = off; }

  void save(QString fileName, FileFormat format, Compression comp);

  bool needUpdate(const QRectF &viewport, double scale,
                  const QRectF &oldViewport, double oldScale) const;

  void retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>> &imgs,
                           std::vector<QPoint> &locs,
                           std::vector<double> &scales,
                           size_t z,
                           size_t t,
                           const QRectF &viewport,
                           double scale,
                           bool mip) const;

  double value(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  // same as value but can use low resolution image value
  double displayValue(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0, bool mip = false) const;

  ZImg crop(const ZImgRegion &region) const;

  ZImg resizedImg(size_t width, size_t height, size_t depth, size_t t) const;

  // only for non-disk-cached image
  bool isDiskCached() const { return m_diskCached; }
  const ZImg& img() const { assert(!m_diskCached); return m_img; }
  const ZImg& maxZProjectedImg() const;
  ZImg& maxZProjectedImg();

  // ZImgSliceProvider interface
public:
  virtual const ZImgInfo& imgInfo() const override { return m_imgInfo; }
  virtual ZImg slice(size_t z, size_t t) const override;
  virtual ZImg allSlices(size_t t) const override;

protected:
  void createPyramidalFolder(const QString &fileName);
  // will take ownership of img
  void createSliceTiles(ZImg *img, size_t z, size_t t, bool mip = false);
  void buildPyramidal(ZImg& img);
  void buildPyramidal();
  void buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>> &subBlocks);
  ZImg assembleImg(size_t ratio) const;
  ZImg assembleImg(size_t ratio, size_t t) const;
  ZImg assembleImg(size_t ratio, size_t t, size_t z) const;

  void updateDerivedData();
  void updateNameTootip();

private:
  size_t ratioForScale(double scale) const;

protected:
  ZImgInfo m_imgInfo;
  ZImgSource m_imgSource;
  size_t m_numScenes;
  bool m_hasUnsavedChange;

  int m_offsetX;
  int m_offsetY;
  int m_offsetZ;
  int m_offsetT;

  // derived data
private:
  mutable QString m_sizeInfo;
  double m_rangeMin;
  double m_rangeMax;
  QString m_name;
  QString m_tooltip;

  size_t m_tileSize = 4096;
  int64_t m_fastReadSizeThreshold = 100 * 1024 * 1024;  // 100MB
  QString m_pyramidalFolder;
  std::vector<std::map<ZImgTileKey, std::shared_ptr<ZImgSubBlock>>> m_ratioTileMaps;
  std::vector<size_t> m_ratioWidths;
  std::vector<size_t> m_ratioHeights;

  double m_minIntensity;
  double m_maxIntensity;
  MinMaxState m_minMaxState;

  bool m_diskCached;
  ZImg m_img;
  mutable ZImg m_maximumProjectedAlongZImg;
};

} // namespace

#endif // ZIMGPACK_H
