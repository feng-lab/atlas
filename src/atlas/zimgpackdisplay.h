#pragma once

#include "zimgpack.h"
#include "zimgvoxelcolormap.h"
#include <QImage>
#include <tbb/blocked_range.h>
#include <map>
#include <memory>
#include <vector>

namespace nim {

enum class ZImgColorizationMode
{
  Intensity,
  SegmentationLabels,
};

class ZImgPackDisplay
{
public:
  explicit ZImgPackDisplay(const ZImgPack& imgPack);

  // default, hide all channels
  // default go to first slice and set alpha and scale to 1
  void reset();

  size_t slice() const
  {
    return m_z;
  }

  size_t time() const
  {
    return m_t;
  }

  double scale() const
  {
    return m_scale;
  }

  QRectF viewport() const
  {
    return m_viewport;
  }

  double alpha() const
  {
    return m_alpha;
  }

  bool mip() const
  {
    return m_mip;
  }

  void setSlice(size_t z)
  {
    m_z = std::min(z, m_imgPack.imgInfo().depth - 1);
  }

  void setTime(size_t t)
  {
    m_t = std::min(t, m_imgPack.imgInfo().numTimes - 1);
  }

  void setScale(double s)
  {
    m_scale = s;
  }

  void setViewport(const QRectF& rect)
  {
    m_viewport = rect;
  }

  void setAlpha(double a)
  {
    m_alpha = std::min(1.0, std::max(0.0, a));
  }

  void setColorizationMode(ZImgColorizationMode mode)
  {
    m_colorizationMode = mode;
  }

  [[nodiscard]] ZImgColorizationMode colorizationMode() const
  {
    return m_colorizationMode;
  }

  void setMIP(bool v)
  {
    if (m_imgPack.imgInfo().depth > 1) {
      m_mip = v;
    }
  }

  void setMIPZRange(size_t s, size_t e)
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
  void showChannel(size_t ch, double minData, double maxData); // todo: long double??
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
  ZImgColorizationMode m_colorizationMode = ZImgColorizationMode::Intensity;

  bool m_mip = false;
  size_t m_mipZStart = 0;
  size_t m_mipZEnd = 0;
};

// Helper for converting raw ZImg tiles into a ZQImagePack using the same colormap logic as ZImgPackDisplay,
// without requiring access to a live ZImgPack object. This is safe to use from background threads as long as
// callers pass fully-owned ZImg instances and value-type parameters.
[[nodiscard]] ZQImagePack qImagePackFromZImgs(const std::vector<std::shared_ptr<ZImg>>& imgs,
                                              const std::vector<QPoint>& locs,
                                              const std::vector<double>& scales,
                                              const ZImgInfo& imgInfo,
                                              const std::map<size_t, std::pair<double, double>>& channels,
                                              const std::map<size_t, col4>& channelColors,
                                              double alpha,
                                              size_t tileWidth = 4096,
                                              size_t tileHeight = 4096,
                                              ZImgColorizationMode colorizationMode = ZImgColorizationMode::Intensity);

} // namespace nim
