#ifndef ZIMGPACKDISPLAY_H
#define ZIMGPACKDISPLAY_H

#include "zimgpack.h"
#include "zimgvoxelcolormap.h"
#include <map>
#include <QImage>

#ifndef _USE_QTCONCURRENT_

#include <tbb/blocked_range.h>

#endif


namespace nim {

class ZImgPackDisplay
{
public:
  ZImgPackDisplay(const ZImgPack& imgPack, bool mip);

  ~ZImgPackDisplay();

  // default, hide all channels
  // default go to first slice and set alpha and scale to 1
  void reset();

  size_t slice() const
  { return m_z; }

  size_t time() const
  { return m_t; }

  double scale() const
  { return m_scale; }

  QRectF viewport() const
  { return m_viewport; }

  double alpha() const
  { return m_alpha; }

  inline void setSlice(size_t z)
  { m_z = std::min(z, m_imgPack.imgInfo().depth - 1); }

  inline void setTime(size_t t)
  { m_t = std::min(t, m_imgPack.imgInfo().numTimes - 1); }

  inline void setScale(double s)
  { m_scale = s; }

  inline void setViewport(const QRectF& rect)
  { m_viewport = rect; }

  inline void setAlpha(double a)
  { m_alpha = std::min(1.0, std::max(0.0, a)); }

  // show channel ch and map minData to 0, map maxData to 255
  void showChannel(size_t ch, double minData, double maxData);   // todo: long double??
  void hideChannel(size_t ch);

  // show all channels use min max value as range
  void showAllChannels(double minData, double maxData);

  // not very useful
  void hideAllChannels();

  // QImage has size limit, so we split big image into tiles and return a list of image and their start
  // locations
  // might return empty vector
  ZQImagePack toQImagePack(size_t tileWidth = 4096, size_t tileHeight = 4096) const;

private:
  //  template<typename TVoxel>
  //  void setQImageDataBlockCM(QImage &qim, size_t startLine, size_t endLine,
  //                            const std::vector<size_t>& channels,
  //                            const std::vector<ZImgVoxelColormap<TVoxel>>& colormaps) const;

#ifndef _USE_QTCONCURRENT_

  template<typename TVoxel>
  void setQImageDataBlockCM(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                            const std::vector<size_t>* channels,
                            const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;

  template<typename TVoxel>
  void setQImageDataBlockCMMultAlpha(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                                     const std::vector<size_t>* channels,
                                     const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;

#else
  template<typename TVoxel>
  void setQImageDataBlockCM(const ZImg *img, QImage *qim, std::pair<size_t,size_t> rowRange,
                            const std::vector<size_t>* channels,
                            const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;

  template<typename TVoxel>
  void setQImageDataBlockCMMultAlpha(const ZImg *img, QImage *qim, std::pair<size_t,size_t> rowRange,
                                     const std::vector<size_t>* channels,
                                     const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;
#endif

  template<typename TVoxel>
  void setQImageDataCM(const ZImg& img, QImage& qim) const;

  //  template<typename TVoxel>
  //  void setQImageDataBlock(QImage &qim, size_t startLine, size_t endLine,
  //                            const std::vector<size_t>& channels) const;

#ifndef _USE_QTCONCURRENT_

  template<typename TVoxel>
  void setQImageDataBlock(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                          const std::vector<size_t>* channels) const;

#else
  template<typename TVoxel>
  void setQImageDataBlock(const ZImg *img, QImage *qim, std::pair<size_t, size_t> rowRange,
                            const std::vector<size_t>* channels) const;
#endif

  template<typename TVoxel>
  void setQImageData(const ZImg& img, QImage& qim) const;

  void fillQImage(const ZImg& img, QImage& qim) const;

private:
  const ZImgPack& m_imgPack;
  size_t m_z;
  size_t m_t;
  double m_scale;
  QRectF m_viewport;
  std::map<size_t, std::pair<double, double>> m_channels;
  mutable double m_alpha;

  bool m_mip;
};

} // namespace nim

#endif // ZIMGPACKDISPLAY_H
