#pragma once

#include "zglobal.h"
#include <boost/align/aligned_allocator.hpp>
#include <complex>
#include <vector>

namespace nim {

class ZComplexImg
{
public:
  // create a empty img
  ZComplexImg() = default;

  // create a img and allocate space
  ZComplexImg(size_t width, size_t height, size_t depth);

  ~ZComplexImg();

  ZComplexImg(ZComplexImg&&) = default;

  ZComplexImg& operator=(ZComplexImg&&) = default;

  ZComplexImg(const ZComplexImg&) = default;

  ZComplexImg& operator=(const ZComplexImg&) = default;

  void swap(ZComplexImg& other) noexcept;

  // make empty, release all data
  void clear();

  bool isEmpty() const;

  bool isSameSize(const ZComplexImg& rhs) const;

  inline size_t width() const
  { return m_width; }

  inline size_t height() const
  { return m_height; }

  inline size_t depth() const
  { return m_depth; }

  inline std::complex<double>* rawData()
  { return m_data.data(); }

  inline QString toQString() const
  {
    return QString("width:%1, height:%2, depth:%3").arg(m_width).arg(m_height).arg(m_depth);
  }

  ZComplexImg& conj();

  static ZComplexImg conj(const ZComplexImg& img);

  // for operator between img, input should have same size, otherwise ZImgException will be thrown
  ZComplexImg& operator+=(const std::complex<double>& rhs);

  ZComplexImg& operator+=(const ZComplexImg& rhs);

  ZComplexImg operator+(const std::complex<double>& rhs) const;

  ZComplexImg operator+(const ZComplexImg& rhs) const;

  ZComplexImg& operator-=(const std::complex<double>& rhs);

  ZComplexImg& operator-=(const ZComplexImg& rhs);

  ZComplexImg operator-(const std::complex<double>& rhs) const;

  ZComplexImg operator-(const ZComplexImg& rhs) const;

  ZComplexImg operator-() const;

  ZComplexImg& operator*=(const std::complex<double>& rhs);

  ZComplexImg& operator*=(const ZComplexImg& rhs);

  ZComplexImg operator*(const std::complex<double>& rhs) const;

  ZComplexImg operator*(const ZComplexImg& rhs) const;

  ZComplexImg& operator/=(const std::complex<double>& rhs);

  ZComplexImg& operator/=(const ZComplexImg& rhs);

  ZComplexImg operator/(const std::complex<double>& rhs) const;

  ZComplexImg operator/(const ZComplexImg& rhs) const;

private:
  std::vector<std::complex<double>, boost::alignment::aligned_allocator<std::complex<double>, 32>> m_data;
  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;
};

} // namespace nim

