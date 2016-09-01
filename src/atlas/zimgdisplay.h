#pragma once

#include "zimg.h"
#include "zimgvoxelcolormap.h"
#include <QImage>
#include <tbb/blocked_range.h>
#include <map>

namespace nim {

class ZImgDisplay
{
public:
  explicit ZImgDisplay(const ZImg& img);

  // default, hide all channels
  // default go to first slice and set alpha to 1
  void reset();

  size_t slice() const
  { return m_z; }

  size_t time() const
  { return m_t; }

  double alpha() const
  { return m_alpha; }

  inline void setSlice(size_t z)
  { m_z = std::min(z, m_img.depth() - 1); }

  inline void setTime(size_t t)
  { m_t = std::min(t, m_img.numTimes() - 1); }

  inline void setAlpha(double a)
  { m_alpha = std::min(1.0, std::max(0.0, a)); }

  // show channel ch and map minData to 0, map maxData to 255
  void showChannel(size_t ch, double minData, double maxData);   // todo: long double??
  void hideChannel(size_t ch);

  // show all channels use min max value as range
  void showAllChannels(double minData, double maxData);

  //
  void hideAllChannels();

  static size_t toQImageSizeLimit();

  // note: will crash if image is bigger than toQImageSizeLimit x toQImageSizeLimit, QImage has size limit
  QImage toQImage() const;

  // QImage has size limit, so we split big image into tiles and return a list of image and their start
  // locations
  // might return empty vector
  ZQImagePack toQImagePack(size_t tileWidth = 4096, size_t tileHeight = 4096) const;

private:
  inline const ZImgInfo& imgInfo() const
  { return m_img.info(); }

  template<typename TVoxel>
  void setQImageDataBlockCM(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                            const std::vector<size_t>* channels,
                            const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;

  template<typename TVoxel>
  void setQImageDataBlockCMMultAlpha(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                                     const std::vector<size_t>* channels,
                                     const std::vector<ZImgVoxelColormap<TVoxel>>* colormaps) const;

  template<typename TVoxel>
  void setQImageDataCM(const ZImg& img, QImage& qim) const;

  template<typename TVoxel>
  void setQImageDataBlock(const ZImg* img, QImage* qim, const tbb::blocked_range<size_t>& rowRange,
                          const std::vector<size_t>* channels) const;

  template<typename TVoxel>
  void setQImageData(const ZImg& img, QImage& qim) const;

  void fillQImage(const ZImg& img, QImage& qim) const;

private:
  const ZImg& m_img;
  mutable size_t m_z;
  mutable size_t m_t;
  std::map<size_t, std::pair<double, double>> m_channels;
  mutable double m_alpha;
};

} // namespace nim

