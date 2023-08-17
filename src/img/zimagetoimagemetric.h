#pragma once

#include "zbenchtimer.h"
#include "zimage2dutils.h"
#include "zstatisticsutils.h"
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>
#include <cmath>
#include <type_traits>
#include <utility>

namespace nim {

class ZImageToImageMetric
{
public:
  enum class Type
  {
    MeanDifferences,
    MeanSquaredDifferences,
    LogAbsoluteDifferences,
    NormalizedCrossCorrelation,
    NormalizedMutualInformation
  };

  void setType(Type type)
  {
    m_type = type;
  }

  [[nodiscard]] Type type() const
  {
    return m_type;
  }

  void setUseMultithreading(bool v)
  {
    m_useMultithreading = v;
  }

  // for mutual information
  // number of histogram bins and value range to build histogram
  // default is 128
  void setNumHistogramBins(size_t nbins)
  {
    m_nbins = nbins;
  }

  // if not set, default for float type is 0.0 to 1.0
  // for other type is available range
  void setHistogramRange(double Imin, double Imax)
  {
    m_Imin = Imin;
    m_Imax = Imax;
  }

  // this method works for both 2d and 3d image, set depth to 1 for 2d image
  template<typename TPixel1, typename TPixel2>
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
  Type m_type = Type::LogAbsoluteDifferences;
  size_t m_nbins = 128;
  double m_Imin = 0;
  double m_Imax = -2;
  bool m_useMultithreading = true;
};

// template

template<typename TPixel1, typename TPixel2>
struct EvaluateMetricForOneBlock
{
  EvaluateMetricForOneBlock(const TPixel1* img1,
                            const TPixel2* img2,
                            size_t size,
                            ZImageToImageMetric::Type type,
                            double mean1 = 0,
                            double std1 = 0,
                            double mean2 = 0,
                            double std2 = 0)
    : m_img1(img1)
    , m_img2(img2)
    , m_size(size)
    , m_type(type)
    , m_mean1(mean1)
    , m_mean2(mean2)
    , m_std1(std1)
    , m_std2(std2)
    , m_metric(0)
  {}

  void operator()(const tbb::blocked_range<size_t>& range)
  {
    if (m_type == ZImageToImageMetric::Type::MeanDifferences) {
      for (size_t i = range.begin(); i < range.end(); ++i) {
        m_metric += (m_img1[i] * 1.0 - m_img2[i]) / m_size;
      }
    } else if (m_type == ZImageToImageMetric::Type::MeanSquaredDifferences) {
      for (size_t i = range.begin(); i < range.end(); ++i) {
        m_metric += (m_img1[i] * 1.0 - m_img2[i]) * (m_img1[i] * 1.0 - m_img2[i]) / m_size;
      }
    } else if (m_type == ZImageToImageMetric::Type::LogAbsoluteDifferences) {
      for (size_t i = range.begin(); i < range.end(); ++i) {
        m_metric += std::log(std::abs(m_img1[i] * 1.0 - m_img2[i]) + 1.0) / m_size;
      }
    } else if (m_type == ZImageToImageMetric::Type::NormalizedCrossCorrelation) {
      double scale = 1. / (m_size * m_std1 * m_std2);
      for (size_t i = range.begin(); i < range.end(); ++i) {
        m_metric += -(m_img1[i] - m_mean1) * (m_img2[i] - m_mean2) * scale;
      }
    }
  }

  EvaluateMetricForOneBlock(EvaluateMetricForOneBlock& x, tbb::split)
    : m_img1(x.m_img1)
    , m_img2(x.m_img2)
    , m_size(x.m_size)
    , m_type(x.m_type)
    , m_mean1(x.m_mean1)
    , m_mean2(x.m_mean2)
    , m_std1(x.m_std1)
    , m_std2(x.m_std2)
    , m_metric(0)
  {}

  void join(const EvaluateMetricForOneBlock& y)
  {
    m_metric += y.m_metric;
  }

  const TPixel1* m_img1;
  const TPixel2* m_img2;
  double m_size;
  ZImageToImageMetric::Type m_type;
  double m_mean1;
  double m_mean2;
  double m_std1;
  double m_std2;
  double m_metric;
};

// template<typename TPixel1, typename TPixel2>
// struct BuildImageHistogramForOneBlock {
//   BuildImageHistogramForOneBlock(const TPixel1 *img1,
//                                  const TPixel2 *img2,
//                                  size_t size,
//                                  double Imin,
//                                  double Imax,
//                                  size_t nbins)
//     : m_img1(img1), m_img2(img2), m_size(size)
//     , m_Imin(Imin), m_Imax(Imax), m_nbins(nbins)
//   {
//   }

//  using result_type = std::vector<std::vector<double>>;

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

template<typename TPixel1, typename TPixel2>
double ZImageToImageMetric::value(const TPixel1* img1, const TPixel2* img2, size_t width, size_t height, size_t depth)
{
  size_t size = width * height * depth;

  double mean1 = 0, mean2 = 0, std1 = 0, std2 = 0;
  if (m_type == Type::NormalizedCrossCorrelation) {
    //    meanAndStandardDeviation(img1, img1 + size, mean1, std1, true);
    //    meanAndStandardDeviation(img2, img2 + size, mean2, std2, true);
    std::tie(mean1, std1) = parallel_mean_and_standard_deviation(img1, img1 + size);
    std::tie(mean2, std2) = parallel_mean_and_standard_deviation(img2, img2 + size);
    if (std1 == 0.0 || std2 == 0.0) {
      return 0;
    }
  }
  EvaluateMetricForOneBlock<TPixel1, TPixel2> func(img1, img2, size, m_type, mean1, std1, mean2, std2);

  if (!m_useMultithreading) {
    func(tbb::blocked_range<size_t>(0, size));
    return func.m_metric;
  }
  tbb::parallel_reduce(tbb::blocked_range<size_t>(0, size), func);
  return func.m_metric;
}

} // namespace nim
