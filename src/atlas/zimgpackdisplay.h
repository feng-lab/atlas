#pragma once

#include "zimgpack.h"
#include "zimgvoxelcolormap.h"
#include <QImage>
#include <tbb/blocked_range.h>
#include <map>

namespace nim {

class ZImgPackDisplay
{
public:
  explicit ZImgPackDisplay(const ZImgPack& imgPack);

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

  bool mip() const
  { return m_mip; }

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

  inline void setMIP(bool v)
  {
    if (m_imgPack.imgInfo().depth > 1)
      m_mip = v;
  }

  inline void setMIPZRange(size_t s, size_t e)
  {
    if (m_imgPack.imgInfo().depth > 1) {
      CHECK(s <= e && e < m_imgPack.imgInfo().depth);
      m_mipZStart = s;
      m_mipZEnd = e;
    }
  }

  void setChannelColor(size_t ch, col4 col)
  {
    m_channelColors[ch] = col;
  }

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
  inline const ZImgInfo& imgInfo() const
  { return m_imgPack.imgInfo(); }

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
  const ZImgPack& m_imgPack;
  size_t m_z;
  size_t m_t;
  double m_scale;
  QRectF m_viewport;
  std::map<size_t, std::pair<double, double>> m_channels;
  std::map<size_t, col4> m_channelColors;
  mutable double m_alpha;

  bool m_mip = false;
  size_t m_mipZStart = 0;
  size_t m_mipZEnd = 0;
};

} // namespace nim

