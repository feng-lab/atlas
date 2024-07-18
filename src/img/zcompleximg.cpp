#include "zcompleximg.h"

#include "zlog.h"
#include "zmkl.h"
#include <algorithm>
#include <functional>

DECLARE_bool(zimg_use_mkl_for_fft_if_available);

namespace nim {

ZComplexImg::ZComplexImg(size_t width, size_t height, size_t depth)
  : m_data(width * height * depth)
  , m_width(width)
  , m_height(height)
  , m_depth(depth)
{}

void ZComplexImg::swap(ZComplexImg& other) noexcept
{
  m_data.swap(other.m_data);
  std::swap(m_width, other.m_width);
  std::swap(m_height, other.m_height);
  std::swap(m_depth, other.m_depth);
}

void ZComplexImg::clear()
{
  clearAndDeallocate(m_data);
  m_width = 0;
  m_height = 0;
  m_depth = 0;
}

bool ZComplexImg::isEmpty() const
{
  return m_data.empty() || m_width == 0 || m_height == 0 || m_depth == 0;
}

bool ZComplexImg::isSameSize(const ZComplexImg& rhs) const
{
  return m_width == rhs.m_width && m_height == rhs.m_height && m_depth == rhs.m_depth;
}

std::string ZComplexImg::toString() const
{
  return fmt::format("width:{}, height:{}, depth:{}", m_width, m_height, m_depth);
}

ZComplexImg& ZComplexImg::conj()
{
#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    vzConj(m_data.size(), m_data.data(), m_data.data());
    return *this;
  }
#endif
  for (auto& v : m_data) {
    v = std::conj(v);
  }
  return *this;
}

ZComplexImg ZComplexImg::conj(const ZComplexImg& img)
{
  ZComplexImg res = img;
  return res.conj();
}

ZComplexImg& ZComplexImg::operator+=(const std::complex<double>& rhs)
{
  for (auto& v : m_data) {
    v += rhs;
  }
  return *this;
}

ZComplexImg& ZComplexImg::operator+=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZException(fmt::format("complex img addition requires same size img as input: this <{}>, other <{}>",
                                 toString(),
                                 rhs.toString()));
  }
#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    vzAdd(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
    return *this;
  }
#endif
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::plus<>());
  return *this;
}

ZComplexImg ZComplexImg::operator+(const std::complex<double>& rhs) const
{
  ZComplexImg res(*this);
  res += rhs;
  return res;
}

ZComplexImg ZComplexImg::operator+(const ZComplexImg& rhs) const
{
  ZComplexImg res(*this);
  res += rhs;
  return res;
}

ZComplexImg& ZComplexImg::operator-=(const std::complex<double>& rhs)
{
  for (auto& v : m_data) {
    v -= rhs;
  }
  return *this;
}

ZComplexImg& ZComplexImg::operator-=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZException(fmt::format("complex img subtraction requires same size img as input: this <{}>, other <{}>",
                                 toString(),
                                 rhs.toString()));
  }
#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    vzSub(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
    return *this;
  }
#endif
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::minus<>());
  return *this;
}

ZComplexImg ZComplexImg::operator-(const std::complex<double>& rhs) const
{
  ZComplexImg res(*this);
  res -= rhs;
  return res;
}

ZComplexImg ZComplexImg::operator-(const ZComplexImg& rhs) const
{
  ZComplexImg res(*this);
  res -= rhs;
  return res;
}

ZComplexImg ZComplexImg::operator-() const
{
  ZComplexImg res(*this);
  std::transform(res.m_data.begin(), res.m_data.end(), res.m_data.begin(), std::negate<>());
  return res;
}

ZComplexImg& ZComplexImg::operator*=(const std::complex<double>& rhs)
{
  for (auto& v : m_data) {
    v *= rhs;
  }
  return *this;
}

ZComplexImg& ZComplexImg::operator*=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZException(fmt::format("complex img multiplies requires same size img as input: this <{}>, other <{}>",
                                 toString(),
                                 rhs.toString()));
  }
#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    vzMul(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
    return *this;
  }
#endif
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::multiplies<>());
  return *this;
}

ZComplexImg ZComplexImg::operator*(const std::complex<double>& rhs) const
{
  ZComplexImg res(*this);
  res *= rhs;
  return res;
}

ZComplexImg ZComplexImg::operator*(const ZComplexImg& rhs) const
{
  ZComplexImg res(*this);
  res *= rhs;
  return res;
}

ZComplexImg& ZComplexImg::operator/=(const std::complex<double>& rhs)
{
  for (auto& v : m_data) {
    v /= rhs;
  }
  return *this;
}

ZComplexImg& ZComplexImg::operator/=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZException(fmt::format("complex img divides requires same size img as input: this <{}>, other <{}>",
                                 toString(),
                                 rhs.toString()));
  }
#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    vzDiv(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
    return *this;
  }
#endif
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::divides<>());
  return *this;
}

ZComplexImg ZComplexImg::operator/(const std::complex<double>& rhs) const
{
  ZComplexImg res(*this);
  res /= rhs;
  return res;
}

ZComplexImg ZComplexImg::operator/(const ZComplexImg& rhs) const
{
  ZComplexImg res(*this);
  res /= rhs;
  return res;
}

} // namespace nim
