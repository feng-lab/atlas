#pragma once

#include <vector>
#include <tbb/parallel_for.h>
#include "zimageinterpolation.h"
#include "zsaturateoperation.h"
#include "zstatisticsutils.h"
#include <utility>

namespace nim {

class ZImageTransform
{
public:
  ZImageTransform();

  virtual ~ZImageTransform();

  virtual size_t numParameters() const = 0;

  void setParameters(const std::vector<double>& para);

  virtual void setParameters(double const* para) = 0;

  virtual bool is2DTransform() const = 0;

  const std::vector<double>& parameters() const
  { return m_parameters; }

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) = 0;

  virtual std::vector<double> estimateParameterScales(const double* dims) const;

  void setUseMultithreading(bool v)
  { m_useMultithreading = v; }

  // inoutCoords can not be nullptr, 2d transform cahnge first 2 elements of outCoords, 3d transform change first 3 elements of outCoords
  virtual void transformPoint(double* inoutCoords) const = 0;

  //  void setPadOption(PadOption po, double fillValue = 0.0, bool boundInBorder = false)
  //  { m_imageInterpolation.setPadOption(po); m_imageInterpolation.setFillValue(fillValue); m_imageInterpolation.setBoundInBorder(boundInBorder); }
  //  void setInterpolant(Interpolant met) { m_imageInterpolation.setInterpolant(met); }
  void setImageInterpolation(const ZImageInterpolation& inter)
  { m_imageInterpolation = inter; }

  // output image size is [xend-xstart] x [yend-ystart] x [zend-zstart]
  // if end < start, then end = start + size(input, dim)
  // must be 3d transform
  template<typename TPixel, typename TPixelOut = TPixel>
  void transformImage(const TPixel* Iin, size_t width, size_t height, size_t depth,
                      TPixelOut* Iout, int xstart = 0, int xend = -1, int ystart = 0, int yend = -1,
                      int zstart = 0, int zend = -1) const;

  // output image size is [xend-xstart] x [yend-ystart] x [zend-zstart]
  // if end < start, then end = start + size(input, dim)
  // must be 2d transform
  template<typename TPixel, typename TPixelOut = TPixel>
  void transformImage(const TPixel* Iin, size_t width, size_t height,
                      TPixelOut* Iout, int xstart = 0, int xend = -1, int ystart = 0, int yend = -1) const;

  virtual QString toQString() const = 0;

  QString paraQString() const;

  virtual ZImageTransform* clone() const = 0;

  virtual ZImageTransform* makeInverseTransform() const = 0;

protected:
  ZImageInterpolation m_imageInterpolation;
  bool m_useMultithreading;

  std::vector<double> m_parameters;
};

std::ostream& operator<<(std::ostream& s, const ZImageTransform& tfm);

template<typename TPixel, typename TPixelOut>
struct AffineTransform3DForOneBlock
{
  AffineTransform3DForOneBlock(const TPixel* img, size_t width, size_t height, size_t depth,
                               const ZImageTransform& tfm,
                               const ZImageInterpolation& sampler,
                               TPixelOut* imgOut, int xstart, int xend, int ystart, int yend, int zstart)
    : m_img(img), m_width(width), m_height(height), m_depth(depth), m_tfm(tfm)
    , m_sampler(sampler)
    , m_imgOut(imgOut), m_xstart(xstart), m_xend(xend), m_ystart(ystart), m_yend(yend), m_zstart(zstart)
  {
  }

  void operator()(const tbb::blocked_range<int>& range) const
  {
    size_t outWidth = m_xend - m_xstart;
    size_t outHeight = m_yend - m_ystart;

    double outCoords[3];
    for (int z = range.begin(); z != range.end(); ++z) {
      for (int y = m_ystart; y < m_yend; ++y) {
        for (int x = m_xstart; x < m_xend; ++x) {
          outCoords[0] = x;
          outCoords[1] = y;
          outCoords[2] = z;
          m_tfm.transformPoint(outCoords);

          // interpolate the intensities
          double value = m_sampler.sample(m_img, m_width, m_height, m_depth, outCoords[0], outCoords[1], outCoords[2]);

          m_imgOut[(z - m_zstart) * outWidth * outHeight + (y - m_ystart) * outWidth +
                   (x - m_xstart)] = saturate_cast<TPixelOut>(value);
        }
      }
    }
  }

  const TPixel* m_img;
  size_t m_width;
  size_t m_height;
  size_t m_depth;
  const ZImageTransform& m_tfm;
  const ZImageInterpolation& m_sampler;
  TPixelOut* m_imgOut;
  int m_xstart;
  int m_xend;
  int m_ystart;
  int m_yend;
  int m_zstart;
};

template<typename TPixel, typename TPixelOut>
void ZImageTransform::transformImage(const TPixel* Iin, size_t width, size_t height, size_t depth,
                                     TPixelOut* Iout, int xstart, int xend, int ystart, int yend,
                                     int zstart, int zend) const
{
  if (xend < xstart)
    xend = xstart + width;
  if (yend < ystart)
    yend = ystart + height;
  if (zend < zstart)
    zend = zstart + depth;

  AffineTransform3DForOneBlock<TPixel, TPixelOut> func(Iin, width, height, depth,
                                                       *this,
                                                       this->m_imageInterpolation,
                                                       Iout, xstart, xend, ystart, yend, zstart);
  if (!this->m_useMultithreading) {
    func(tbb::blocked_range<int>(zstart, zend));
  } else {
    tbb::parallel_for(tbb::blocked_range<int>(zstart, zend), func);
  }
}

template<typename TPixel, typename TPixelOut>
struct AffineTransform2DForOneBlock
{
  AffineTransform2DForOneBlock(const TPixel* img, size_t width, size_t height,
                               const ZImageTransform& tfm,
                               const ZImageInterpolation& sampler,
                               TPixelOut* imgOut, int xstart, int xend, int ystart)
    : m_img(img), m_width(width), m_height(height), m_tfm(tfm)
    , m_sampler(sampler), m_imgOut(imgOut)
    , m_xstart(xstart), m_xend(xend), m_ystart(ystart)
  {
  }

  void operator()(const tbb::blocked_range<int>& range) const
  {
    size_t outWidth = m_xend - m_xstart;

    double outCoords[2];
    for (int y = range.begin(); y != range.end(); ++y) {
      for (int x = m_xstart; x < m_xend; ++x) {
        outCoords[0] = x;
        outCoords[1] = y;
        m_tfm.transformPoint(outCoords);

        // interpolate the intensities
        double value = m_sampler.sample(m_img, m_width, m_height, outCoords[0], outCoords[1]);
        m_imgOut[(y - m_ystart) * outWidth + (x - m_xstart)] = saturate_cast<TPixelOut>(value);
      }
    }
  }

  const TPixel* m_img;
  size_t m_width;
  size_t m_height;
  const ZImageTransform& m_tfm;
  const ZImageInterpolation& m_sampler;
  TPixelOut* m_imgOut;
  int m_xstart;
  int m_xend;
  int m_ystart;
};

template<typename TPixel, typename TPixelOut>
void ZImageTransform::transformImage(const TPixel* Iin, size_t width, size_t height,
                                     TPixelOut* Iout, int xstart, int xend, int ystart, int yend) const
{
  CHECK(is2DTransform());
  if (xend < xstart)
    xend = xstart + width;
  if (yend < ystart)
    yend = ystart + height;

  AffineTransform2DForOneBlock<TPixel, TPixelOut> func(Iin, width, height,
                                                       *this,
                                                       this->m_imageInterpolation,
                                                       Iout, xstart, xend, ystart);
  if (!this->m_useMultithreading) {
    func(tbb::blocked_range<int>(ystart, yend));
  } else {
    tbb::parallel_for(tbb::blocked_range<int>(ystart, yend), func);
  }
}

} // namespace nim

