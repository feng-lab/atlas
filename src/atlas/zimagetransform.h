#ifndef ZIMAGETRANSFORM_H
#define ZIMAGETRANSFORM_H

#include <vector>
#ifndef _USE_QTCONCURRENT_
#include <tbb/parallel_for.h>
#else
#include <QtConcurrent/QtConcurrentMap>
#endif
#include "zimageinterpolation.h"
#include "zsaturateoperation.h"
#include "zstatisticsutils.h"
#include "zlog.h"
#include <utility>

namespace nim {

class ZImageTransform
{
public:
  ZImageTransform();
  virtual ~ZImageTransform();

  virtual size_t numParameters() const = 0;
  void setParameters(const std::vector<double> &para);
  virtual void setParameters(double const *para) = 0;
  virtual bool is2DTransform() const = 0;

  const std::vector<double>& parameters() const { return m_parameters; }
  virtual void adaptParameters(size_t fromLevel, size_t toLevel) = 0;
  virtual std::vector<double> estimateParameterScales(const double* dims) const;

  void setUseMultithreading(bool v) { m_useMultithreading = v; }

  // inoutCoords can not be nullptr, 2d transform cahnge first 2 elements of outCoords, 3d transform change first 3 elements of outCoords
  virtual void transformPoint(double *inoutCoords) const = 0;

  //  void setPadOption(PadOption po, double fillValue = 0.0, bool boundInBorder = false)
  //  { m_imageInterpolation.setPadOption(po); m_imageInterpolation.setFillValue(fillValue); m_imageInterpolation.setBoundInBorder(boundInBorder); }
  //  void setInterpolant(Interpolant met) { m_imageInterpolation.setInterpolant(met); }
  void setImageInterpolation(const ZImageInterpolation& inter) { m_imageInterpolation = inter; }
  // output image size is [xend-xstart] x [yend-ystart] x [zend-zstart]
  // if end < start, then end = start + size(input, dim)
  // must be 3d transform
  template<typename TPixel, typename TPixelOut = TPixel>
  void transformImage(const TPixel *Iin, size_t width, size_t height, size_t depth,
                      TPixelOut *Iout, int xstart = 0, int xend = -1, int ystart = 0, int yend = -1,
                      int zstart = 0, int zend = -1) const;
  // output image size is [xend-xstart] x [yend-ystart] x [zend-zstart]
  // if end < start, then end = start + size(input, dim)
  // must be 2d transform
  template<typename TPixel, typename TPixelOut = TPixel>
  void transformImage(const TPixel *Iin, size_t width, size_t height,
                      TPixelOut *Iout, int xstart = 0, int xend = -1, int ystart = 0, int yend = -1) const;

  virtual QString toQString() const = 0;
  QString paraQString() const;

  virtual ZImageTransform* clone() const = 0;
  virtual ZImageTransform* makeInverseTransform() const = 0;

protected:
  ZImageInterpolation m_imageInterpolation;
  bool m_useMultithreading;

  std::vector<double> m_parameters;
};

std::ostream& operator << (std::ostream& s, const nim::ZImageTransform& tfm);
#ifdef _USE_QSLOG_
QDebug operator << (QDebug s, const nim::ZImageTransform& tfm);
#endif

template<typename TPixel, typename TPixelOut>
struct AffineTransform3DForOneBlock {
  AffineTransform3DForOneBlock(const TPixel *img, size_t width, size_t height, size_t depth,
                               const ZImageTransform &tfm,
                               const ZImageInterpolation &sampler,
                               TPixelOut *imgOut, int xstart, int xend, int ystart, int yend, int zstart)
    : m_img(img), m_width(width), m_height(height), m_depth(depth), m_tfm(tfm)
    , m_sampler(sampler)
    , m_imgOut(imgOut), m_xstart(xstart), m_xend(xend), m_ystart(ystart), m_yend(yend), m_zstart(zstart)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_
  void operator()(const tbb::blocked_range<int> &range) const
#else
  void operator()(const std::pair<int,int> &range) const
#endif
  {
    size_t outWidth = m_xend-m_xstart;
    size_t outHeight = m_yend-m_ystart;

    double outCoords[3];
#ifndef _USE_QTCONCURRENT_
    for (int z=range.begin(); z != range.end(); ++z) {
#else
    for (int z=range.first; z<range.second; ++z) {
#endif
      for (int y=m_ystart; y<m_yend; ++y) {
        for (int x=m_xstart; x<m_xend; ++x) {
          outCoords[0] = x;
          outCoords[1] = y;
          outCoords[2] = z;
          m_tfm.transformPoint(outCoords);

          // interpolate the intensities
          double value = m_sampler.sample(m_img, m_width, m_height, m_depth, outCoords[0], outCoords[1], outCoords[2]);

          m_imgOut[(z-m_zstart)*outWidth*outHeight+(y-m_ystart)*outWidth+(x-m_xstart)] = saturate_cast<TPixelOut>(value);
        }
      }
    }
  }

  const TPixel *m_img;
  size_t m_width;
  size_t m_height;
  size_t m_depth;
  const ZImageTransform &m_tfm;
  const ZImageInterpolation &m_sampler;
  TPixelOut *m_imgOut;
  int m_xstart;
  int m_xend;
  int m_ystart;
  int m_yend;
  int m_zstart;
};

template<typename TPixel, typename TPixelOut>
void ZImageTransform::transformImage(const TPixel *Iin, size_t width, size_t height, size_t depth,
                                     TPixelOut *Iout, int xstart, int xend, int ystart, int yend,
                                     int zstart, int zend) const
{
  if (xend < xstart)
    xend = xstart + width;
  if (yend < ystart)
    yend = ystart + height;
  if (zend < zstart)
    zend = zstart + depth;

  AffineTransform3DForOneBlock<TPixel,TPixelOut> func(Iin, width, height, depth,
                                                      *this,
                                                      this->m_imageInterpolation,
                                                      Iout, xstart, xend, ystart, yend, zstart);
  if (!this->m_useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    func(tbb::blocked_range<int>(zstart, zend));
#else
    func(std::make_pair(zstart, zend));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<int>(zstart, zend), func);
#else
    int outDepth = zend - zstart;
    int numThreads = QThread::idealThreadCount();
    int numBlock = std::min(outDepth, numThreads * 2);
    int zPerBlock = outDepth / numBlock;
    QList<std::pair<int,int>> allRange;
    for (int i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*zPerBlock + zstart,
                                        (i==numBlock-1) ? zend : ((i+1)*zPerBlock + zstart)));
    }

    QtConcurrent::blockingMap(allRange, func);
#endif
  }
}

template<typename TPixel, typename TPixelOut>
struct AffineTransform2DForOneBlock {
  AffineTransform2DForOneBlock(const TPixel *img, size_t width, size_t height,
                               const ZImageTransform &tfm,
                               const ZImageInterpolation &sampler,
                               TPixelOut *imgOut, int xstart, int xend, int ystart)
    : m_img(img), m_width(width), m_height(height), m_tfm(tfm)
    , m_sampler(sampler), m_imgOut(imgOut)
    , m_xstart(xstart), m_xend(xend), m_ystart(ystart)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_
  void operator()(const tbb::blocked_range<int> &range) const
#else
  void operator()(const std::pair<int,int> &range) const
#endif
  {
    size_t outWidth = m_xend-m_xstart;

    double outCoords[2];
#ifndef _USE_QTCONCURRENT_
    for (int y=range.begin(); y != range.end(); ++y) {
#else
    for (int y=range.first; y<range.second; ++y) {
#endif
      for (int x=m_xstart; x<m_xend; ++x) {
        outCoords[0] = x;
        outCoords[1] = y;
        m_tfm.transformPoint(outCoords);

        // interpolate the intensities
        double value = m_sampler.sample(m_img, m_width, m_height, outCoords[0], outCoords[1]);
        m_imgOut[(y-m_ystart)*outWidth+(x-m_xstart)] = saturate_cast<TPixelOut>(value);
      }
    }
  }

  const TPixel *m_img;
  size_t m_width;
  size_t m_height;
  const ZImageTransform &m_tfm;
  const ZImageInterpolation &m_sampler;
  TPixelOut *m_imgOut;
  int m_xstart;
  int m_xend;
  int m_ystart;
};

template<typename TPixel, typename TPixelOut>
void ZImageTransform::transformImage(const TPixel *Iin, size_t width, size_t height,
                                     TPixelOut *Iout, int xstart, int xend, int ystart, int yend) const
{
  assert(is2DTransform());
  if (xend < xstart)
    xend = xstart + width;
  if (yend < ystart)
    yend = ystart + height;

  AffineTransform2DForOneBlock<TPixel,TPixelOut> func(Iin, width, height,
                                                      *this,
                                                      this->m_imageInterpolation,
                                                      Iout, xstart, xend, ystart);
  if (!this->m_useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    func(tbb::blocked_range<int>(ystart, yend));
#else
    func(std::make_pair(ystart, yend));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<int>(ystart, yend), func);
#else
    int outHeight = yend - ystart;
    int numThreads = QThread::idealThreadCount();
    int numBlock = std::min(outHeight, numThreads * 2);
    int rowsPerBlock = outHeight / numBlock;
    QList<std::pair<int,int>> allRange;
    for (int i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*rowsPerBlock + ystart,
                                        (i==numBlock-1) ? yend : ((i+1)*rowsPerBlock) + ystart));
    }

    QtConcurrent::blockingMap(allRange, func);
#endif
  }
}

} // namespace nim

#endif // ZIMAGETRANSFORM_H
