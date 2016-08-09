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
  {
  }

  inline void setRange(TVoxel minData, TVoxel maxData)
  {
    m_min = minData;
    m_colormap.resize(maxData - minData + 1);
  }

  inline col4& color(TVoxel data)
  {
    return m_colormap[data - m_min];
  }

  inline const col4& color(TVoxel data) const
  {
    return m_colormap[data - m_min];
  }

private:
  std::vector<col4> m_colormap;
  TVoxel m_min;
};

template<typename FloatType>
col4 scaleDownColorRGB(const col4& c, FloatType scale)
{
  scale *= static_cast<FloatType>(c.a) / 255.;
  if (scale <= FloatType(0))
    return col4(0, 0, 0, c.a);
  else if (scale >= FloatType(1))
    return c;

  uint8_t r = static_cast<uint8_t>(scale * c.r + FloatType(0.5));
  uint8_t g = static_cast<uint8_t>(scale * c.g + FloatType(0.5));
  uint8_t b = static_cast<uint8_t>(scale * c.b + FloatType(0.5));
  return col4(r, g, b, c.a);
}

template<typename FloatType>
col4 scaleDownColorRGBA(const col4& c, FloatType scale)
{
  if (scale <= FloatType(0))
    return col4(0, 0, 0, 0);
  else if (scale >= FloatType(1))
    return c;

  uint8_t r = static_cast<uint8_t>(scale * c.r + FloatType(0.5));
  uint8_t g = static_cast<uint8_t>(scale * c.g + FloatType(0.5));
  uint8_t b = static_cast<uint8_t>(scale * c.b + FloatType(0.5));
  uint8_t a = static_cast<uint8_t>(scale * c.a + FloatType(0.5));
  return col4(r, g, b, a);
}

// several tiles of qimage represent one big 2d image
class ZQImagePack
{
  std::vector<QImage> m_qimages;
  std::vector<QPoint> m_locations;
  std::vector<double> m_scales;
public:
  inline size_t numImages() const
  { return m_qimages.size(); }

  inline QImage& image(size_t n)
  { return m_qimages[n]; }

  inline QPoint& location(size_t n)
  { return m_locations[n]; }

  inline const QImage& image(size_t n) const
  { return m_qimages[n]; }

  inline const QPoint& location(size_t n) const
  { return m_locations[n]; }

  inline double scale(size_t n) const
  { return m_scales[n]; }

  inline void addImage(const QImage& image, const QPoint& loc, double scale = 1.0)
  {
    m_qimages.push_back(image);
    m_locations.push_back(loc);
    m_scales.push_back(scale);
  }
};

} // namespace nim


