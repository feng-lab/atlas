#include "zcompleximg.h"

#include "zlog.h"
#include "zmkl.h"
#include <utility>

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
#if ZIMG_MKL_ENABLED
  vzConj(m_data.size(), m_data.data(), m_data.data());
#else
  for (auto& v : m_data) {
    v = std::conj(v);
  }
#endif
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
    throw ZException(
      fmt::format("complex img addition requires same size img as input: this <{}>, other <{}>", *this, rhs));
  }
#if ZIMG_MKL_ENABLED
  vzAdd(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
#else
  for (size_t i = 0; i < m_data.size(); ++i) {
    m_data[i] += rhs.m_data[i];
  }
#endif
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
    throw ZException(
      fmt::format("complex img subtraction requires same size img as input: this <{}>, other <{}>", *this, rhs));
  }
#if ZIMG_MKL_ENABLED
  vzSub(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
#else
  for (size_t i = 0; i < m_data.size(); ++i) {
    m_data[i] -= rhs.m_data[i];
  }
#endif
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
  for (auto& v : res.m_data) {
    v = -v;
  }
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
    throw ZException(
      fmt::format("complex img multiplies requires same size img as input: this <{}>, other <{}>", *this, rhs));
  }
#if ZIMG_MKL_ENABLED
  vzMul(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
#else
  for (size_t i = 0; i < m_data.size(); ++i) {
    m_data[i] *= rhs.m_data[i];
  }
#endif
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
    throw ZException(
      fmt::format("complex img divides requires same size img as input: this <{}>, other <{}>", *this, rhs));
  }
#if ZIMG_MKL_ENABLED
  vzDiv(m_data.size(), m_data.data(), rhs.m_data.data(), m_data.data());
#else
  for (size_t i = 0; i < m_data.size(); ++i) {
    m_data[i] /= rhs.m_data[i];
  }
#endif
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
