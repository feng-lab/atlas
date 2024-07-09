#pragma once

#include <QImage>

namespace nim {

// only works for integer pixel
template<typename TVoxel>
class ZImgVoxelColormap
{
public:
  ZImgVoxelColormap()
    : m_min(0)
  {}

  void setRange(TVoxel minData, TVoxel maxData)
  {
    m_min = minData;
    m_colormap.resize(maxData - minData + 1);
  }

  col4& color(TVoxel data)
  {
    return m_colormap[data - m_min];
  }

  const col4& color(TVoxel data) const
  {
    return m_colormap[data - m_min];
  }

private:
  std::vector<col4> m_colormap;
  TVoxel m_min;
};

template<typename Real>
col4 scaleDownColorRGB(const col4& c, Real scale)
{
  col4 res;
  if (scale <= Real(0)) {
    res = col4{0, 0, 0, c.a};
  } else if (scale >= Real(1)) {
    res = c;
  } else {
    res = col4{static_cast<uint8_t>(scale * c.r + Real(0.5)),
               static_cast<uint8_t>(scale * c.g + Real(0.5)),
               static_cast<uint8_t>(scale * c.b + Real(0.5)),
               c.a};
  }
  return res;
}

// several tiles of qimage represent one big 2d image
class ZQImagePack
{
  std::vector<QImage> m_qimages;
  std::vector<QPoint> m_locations;
  std::vector<double> m_scales;

public:
  size_t numImages() const
  {
    return m_qimages.size();
  }

  QImage& image(size_t n)
  {
    return m_qimages[n];
  }

  QPoint& location(size_t n)
  {
    return m_locations[n];
  }

  const QImage& image(size_t n) const
  {
    return m_qimages[n];
  }

  const QPoint& location(size_t n) const
  {
    return m_locations[n];
  }

  double scale(size_t n) const
  {
    return m_scales[n];
  }

  void addImage(const QImage& image, const QPoint& loc, double scale = 1.0)
  {
    m_qimages.push_back(image);
    m_locations.push_back(loc);
    m_scales.push_back(scale);
  }
};

} // namespace nim
