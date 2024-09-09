#pragma once

#include "zglobal.h"
#include <boost/align/aligned_allocator.hpp>
#include <complex>
#include <string>
#include <vector>

namespace nim {

class ZComplexImg
{
public:
  // create a empty img
  ZComplexImg() = default;

  // create a img and allocate space
  ZComplexImg(size_t width, size_t height, size_t depth);

  ZComplexImg(ZComplexImg&&) = default;

  ZComplexImg& operator=(ZComplexImg&&) = default;

  ZComplexImg(const ZComplexImg&) = default;

  ZComplexImg& operator=(const ZComplexImg&) = default;

  void swap(ZComplexImg& other) noexcept;

  // make empty, release all data
  void clear();

  [[nodiscard]] bool isEmpty() const;

  [[nodiscard]] bool isSameSize(const ZComplexImg& rhs) const;

  [[nodiscard]] size_t width() const
  {
    return m_width;
  }

  [[nodiscard]] size_t height() const
  {
    return m_height;
  }

  [[nodiscard]] size_t depth() const
  {
    return m_depth;
  }

  std::complex<double>* rawData()
  {
    return m_data.data();
  }

  [[nodiscard]] static constexpr size_t voxelByteNumber()
  {
    return sizeof(std::complex<double>);
  }

  [[nodiscard]] static constexpr index_t sVoxelByteNumber()
  {
    return voxelByteNumber();
  }

  [[nodiscard]] size_t rowByteNumber() const
  {
    return voxelByteNumber() * m_width;
  }

  [[nodiscard]] index_t sRowByteNumber() const
  {
    return static_cast<index_t>(rowByteNumber());
  }

  [[nodiscard]] size_t planeByteNumber() const
  {
    return voxelByteNumber() * m_width * m_height;
  }

  [[nodiscard]] index_t sPlaneByteNumber() const
  {
    return static_cast<index_t>(planeByteNumber());
  }

  [[nodiscard]] std::string toString() const;

  ZComplexImg& conj();

  static ZComplexImg conj(const ZComplexImg& img);

  // for operator between img, input should have same size, otherwise ZException will be thrown
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
  std::vector<std::complex<double>, boost::alignment::aligned_allocator<std::complex<double>, 64>> m_data;
  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;
};

} // namespace nim
