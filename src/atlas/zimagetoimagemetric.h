#ifndef ZIMAGETOIMAGEMETRIC_H
#define ZIMAGETOIMAGEMETRIC_H

#include <cmath>
#include "zstatisticsutils.h"

#ifndef _USE_QTCONCURRENT_
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>
#else
#include <QtConcurrent/QtConcurrentMap>
#endif
#include <QList>

#include <utility>
#include "zbenchtimer.h"
#include "zimage2dutils.h"
#include <type_traits>

namespace nim {

class ZImageToImageMetric
{
public:
  enum class Type {
    MeanDifferences,
    MeanSquaredDifferences,
    LogAbsoluteDifferences,
    NormalizedCrossCorrelation,
    NormalizedMutualInformation
  };

  ZImageToImageMetric();

  void setType(Type type) { m_type = type; }
  Type type() const { return m_type; }

  void setUseMultithreading(bool v) { m_useMultithreading = v; }

  // for mutual information
  // number of histogram bins and value range to build histogram
  // default is 128
  void setNumHistogramBins(size_t nbins) { m_nbins = nbins; }
  // if not set, default for float type is 0.0 to 1.0
  // for other type is available range
  void setHistogramRange(double Imin, double Imax)
    { m_Imin = Imin; m_Imax = Imax; }

  // this method works for both 2d and 3d image, set depth to 1 for 2d image
  template <typename TPixel1, typename TPixel2>
  double value(const TPixel1* img1, const TPixel2* img2, size_t width, size_t height, size_t depth = 1);

protected:
//  template <typename TPixel>
//  void getHistogramRange(double &Imin, double &Imax)
//  {
//    if (m_Imax > m_Imin) {
//      Imin = m_Imin;
//      Imax = m_Imax;
//      return;
//    }
//    if (std::is_integral<TPixel>::value) {
//      Imin = std::numeric_limits<TPixel>::min();
//      Imax = std::numeric_limits<TPixel>::max();
//    } else {
//      Imin = TPixel(0.0);
//      Imax = TPixel(1.0);
//    }
//  }

private:
  Type m_type;
  size_t m_nbins;
  double m_Imin;
  double m_Imax;
  bool m_useMultithreading;
};

// template

template<typename TPixel1, typename TPixel2>
struct EvaluateMetricForOneBlock {
  EvaluateMetricForOneBlock(const TPixel1 *img1,
                            const TPixel2 *img2,
                            size_t size,
                            ZImageToImageMetric::Type type,
                            double mean1 = 0, double std1 = 0, double mean2 = 0, double std2 = 0)
    : m_img1(img1), m_img2(img2), m_size(size), m_type(type)
    , m_mean1(mean1), m_mean2(mean2), m_std1(std1), m_std2(std2)
  #ifndef _USE_QTCONCURRENT_
    , m_metric(0)
  #endif
  {
  }

#ifndef _USE_QTCONCURRENT_
  void operator()(const tbb::blocked_range<size_t>& range) {
    if (m_type == ZImageToImageMetric::Type::MeanDifferences) {
      for (size_t i=range.begin(); i<range.end(); ++i)
        m_metric += (m_img1[i] * 1.0 - m_img2[i]) / m_size;
    } else if (m_type == ZImageToImageMetric::Type::MeanSquaredDifferences) {
      for (size_t i=range.begin(); i<range.end(); ++i)
        m_metric += (m_img1[i] * 1.0 - m_img2[i]) * (m_img1[i] * 1.0 - m_img2[i]) / m_size;
    } else if (m_type == ZImageToImageMetric::Type::LogAbsoluteDifferences) {
      for (size_t i=range.begin(); i<range.end(); ++i)
        m_metric += std::log(std::abs(m_img1[i] * 1.0 - m_img2[i]) + 1.0) / m_size;
    } else if (m_type == ZImageToImageMetric::Type::NormalizedCrossCorrelation) {
      double scale = 1. / (m_size * m_std1 * m_std2);
      for (size_t i=range.begin(); i<range.end(); ++i)
        m_metric += -(m_img1[i]-m_mean1)*(m_img2[i]-m_mean2) * scale;
    }
  }

  EvaluateMetricForOneBlock(EvaluateMetricForOneBlock& x, tbb::split)
    : m_img1(x.m_img1), m_img2(x.m_img2), m_size(x.m_size), m_type(x.m_type)
    , m_mean1(x.m_mean1), m_mean2(x.m_mean2), m_std1(x.m_std1), m_std2(x.m_std2), m_metric(0)
  {}

  void join(const EvaluateMetricForOneBlock& y)
  {
    m_metric += y.m_metric;
  }
#else
  typedef double result_type;

  double operator()(const std::pair<size_t,size_t> &range) const {
    double value = 0;
    if (m_type == ZImageToImageMetric::MeanDifferences) {
      for (size_t i=range.first; i<range.second; ++i)
        value += (m_img1[i] * 1.0 - m_img2[i]) / m_size;
    } else if (m_type == ZImageToImageMetric::MeanSquaredDifferences) {
      for (size_t i=range.first; i<range.second; ++i)
        value += (m_img1[i] * 1.0 - m_img2[i]) * (m_img1[i] * 1.0 - m_img2[i]) / m_size;
    } else if (m_type == ZImageToImageMetric::LogAbsoluteDifferences) {
      for (size_t i=range.first; i<range.second; ++i)
        value += std::log(std::abs(m_img1[i] * 1.0 - m_img2[i]) + 1.0) / m_size;
    } else if (m_type == ZImageToImageMetric::NormalizedCrossCorrelation) {
      double scale = 1. / (m_size * m_std1 * m_std2);
      for (size_t i=range.first; i<range.second; ++i)
        value += -(m_img1[i]-m_mean1)*(m_img2[i]-m_mean2) * scale;
    }

    return value;
  }
#endif

  const TPixel1 *m_img1;
  const TPixel2 *m_img2;
  double m_size;
  ZImageToImageMetric::Type m_type;
  double m_mean1;
  double m_mean2;
  double m_std1;
  double m_std2;
#ifndef _USE_QTCONCURRENT_
  double m_metric;
#endif
};

//template<typename TPixel1, typename TPixel2>
//struct BuildImageHistogramForOneBlock {
//  BuildImageHistogramForOneBlock(const TPixel1 *img1,
//                                 const TPixel2 *img2,
//                                 size_t size,
//                                 double Imin,
//                                 double Imax,
//                                 size_t nbins)
//    : m_img1(img1), m_img2(img2), m_size(size)
//    , m_Imin(Imin), m_Imax(Imax), m_nbins(nbins)
//  {
//  }

//  typedef std::vector<std::vector<double>> result_type;

//  std::vector<std::vector<double>> operator()(const std::pair<size_t,size_t> &range) const {
//    std::vector<std::vector<double>> res(3);
//    res[0] = std::vector<double>(m_nbins, 0);
//    res[1] = std::vector<double>(m_nbins, 0);
//    res[2] = std::vector<double>(m_nbins*m_nbins, 0);
//    double scalev = m_nbins / (m_Imax - m_Imin);
//    double scaleh = 1.0 / m_size;
//    int sizexd = m_nbins - 1;
//    for (size_t i=range.first; i<range.second; ++i) {
//      double xd = scalev * (m_img1[i] - m_Imin);
//      int xm = static_cast<int>(std::floor(xd));
//      int xp = xm + 1;
//      double xmd = (xp - xd) * scaleh;
//      double xpd = (xd - xm) * scaleh;

//      double yd = scalev * (m_img2[i] - m_Imin);
//      int ym = static_cast<int>(std::floor(yd));
//      int yp = ym + 1;
//      double ymd = (yp - yd) * scaleh;
//      double ypd = (yd - ym) * scaleh;

//      if (xm<0) { xm=0; } else if (xm>sizexd) { xm=sizexd; }
//      if (xp<0) { xp=0; } else if (xp>sizexd) { xp=sizexd; }
//      if (ym<0) { ym=0; } else if (ym>sizexd) { ym=sizexd; }
//      if (yp<0) { yp=0; } else if (yp>sizexd) { yp=sizexd; }

//      res[2][xm+ym*m_nbins] += xmd*ymd;
//      res[2][xp+ym*m_nbins] += xpd*ymd;
//      res[2][xm+yp*m_nbins] += xmd*ypd;
//      res[2][xp+yp*m_nbins] += xpd*ypd;

//      res[0][xm] += xmd;
//      res[0][xp] += xpd;
//      res[1][ym] += ymd;
//      res[1][yp] += ypd;
//    }

//    return res;
//  }

//  const TPixel1 *m_img1;
//  const TPixel2 *m_img2;
//  double m_size;
//  double m_Imin;
//  double m_Imax;
//  size_t m_nbins;
//};

template <typename TPixel1, typename TPixel2>
double ZImageToImageMetric::value(const TPixel1 *img1, const TPixel2 *img2, size_t width, size_t height, size_t depth)
{
  size_t size = width * height * depth;

  //  if (m_type == NormalizedMutualInformation) {
  //    double Imin, Imax;
  //    getHistogramRange<TPixel>(Imin, Imax);
  //    BuildImageHistogramForOneBlock<TPixel> func(img1, img2, size, Imin, Imax, m_nbins);
  //    std::vector<std::vector<double>> hists;
  //    if (m_numThreads == 1) {
  //      hists = func(std::make_pair((size_t)0, size));
  //    } else {
  //      size_t numBlock = std::min(size, (size_t)m_numThreads*2);
  //      size_t pixelPerBlock = size / numBlock;
  //      QList<std::pair<size_t,size_t>> allRange;
  //      for (size_t i=0; i<numBlock; ++i) {
  //        allRange.push_back(std::make_pair(i*pixelPerBlock,
  //                                          (i==numBlock-1) ? size : (i+1)*pixelPerBlock));
  //      }
  //      if (m_numThreads != (size_t)QThread::idealThreadCount())
  //        QThreadPool::globalInstance()->setMaxThreadCount(m_numThreads);
  //      QList<std::vector<std::vector<double>>> values = QtConcurrent::blockingMapped(allRange, func);
  //      if (m_numThreads != (size_t)QThread::idealThreadCount())
  //        QThreadPool::globalInstance()->setMaxThreadCount(QThread::idealThreadCount());
  //      hists = values[0];
  //      for (int i=1; i<values.size(); ++i) {
  //        for (size_t j=0; j<m_nbins; ++j) {
  //          hists[0][j] += values[i][0][j];
  //          hists[1][j] += values[i][1][j];
  //        }
  //        for (size_t j=0; j<m_nbins*m_nbins; ++j) {
  //          hists[2][j] += values[i][2][j];
  //        }
  //      }
  //    }
  //    //logContainer(INFO, hists[0], m_nbins, "1");
  //    //logContainer(INFO, hists[1], m_nbins, "2");
  //    //logContainer(INFO, hists[2], m_nbins, "3");
  //    image2DGaussianFilter(&hists[0][0], m_nbins, 1, 1.0, 1.0, &hists[0][0], -1, 1, PadOption::Replicate);
  //    image2DGaussianFilter(&hists[1][0], m_nbins, 1, 1.0, 1.0, &hists[1][0], -1, 1, PadOption::Replicate);
  //    image2DGaussianFilter(&hists[2][0], m_nbins, m_nbins, 1.0, 1.0, &hists[2][0], -1, -1, PadOption::Replicate);
  //    //logContainer(INFO, hists[0], m_nbins, "1");
  //    //logContainer(INFO, hists[1], m_nbins, "2");
  //    //logContainer(INFO, hists[2], m_nbins, "3");
  //    double eps = std::numeric_limits<double>::epsilon() * 1e3;
  //    double HA = 0.0;
  //    double HB = 0.0;
  //    double HAB = 0.0;
  //    for (size_t i=0; i<m_nbins; ++i) {
  //      HA += -hists[0][i] * std::log(hists[0][i] + eps);
  //      HB += -hists[1][i] * std::log(hists[1][i] + eps);
  //    }
  //    for (size_t i=0; i<m_nbins*m_nbins; ++i) {
  //      HAB += -hists[2][i] * std::log(hists[2][i] + eps);
  //    }
  //    if( HAB == 0)
  //      HAB = eps;
  //    return -(HA + HB) / HAB;
  //  }

  double mean1 = 0, mean2 = 0, std1 = 0, std2 = 0;
  if (m_type == Type::NormalizedCrossCorrelation) {
    meanAndStandardDeviation(img1, img1+size, mean1, std1, true);
    meanAndStandardDeviation(img2, img2+size, mean2, std2, true);
    if (std1 == 0.0 || std2 == 0.0) {
      return 0;
    }
  }
  EvaluateMetricForOneBlock<TPixel1, TPixel2> func(img1, img2, size, m_type, mean1, std1, mean2, std2);

  if (!m_useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    func(tbb::blocked_range<size_t>(0, size));
    return func.m_metric;
#else
    return func(std::make_pair(size_t(0), size));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_reduce(tbb::blocked_range<size_t>(0, size), func);
    return func.m_metric;
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(size, numThreads*2);
    size_t pixelPerBlock = size / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*pixelPerBlock,
                                        (i==numBlock-1) ? size : (i+1)*pixelPerBlock));
    }
    QList<double> values = QtConcurrent::blockingMapped(allRange, func);
    return std::accumulate(values.begin(), values.end(), 0.0);
#endif
  }
}

} // namespace nim

#endif // ZIMAGETOIMAGEMETRIC_H
