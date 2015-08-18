#include "zcompleximg.h"
#include "zimginterface.h"
#include <algorithm>
#include <functional>
#include "QsLog.h"

namespace nim {

ZComplexImg::ZComplexImg()
  : m_width(0)
  , m_height(0)
  , m_depth(0)
{
}

ZComplexImg::ZComplexImg(size_t width, size_t height, size_t depth)
  : m_data(width * height * depth)
  , m_width(width)
  , m_height(height)
  , m_depth(depth)
{
}

ZComplexImg::~ZComplexImg()
{
  clear();
}

void ZComplexImg::swap(ZComplexImg &other) noexcept
{
  m_data.swap(other.m_data);
  std::swap(m_width, other.m_width);
  std::swap(m_height, other.m_height);
  std::swap(m_depth, other.m_depth);
}

void ZComplexImg::clear()
{
  std::vector<std::complex<double>, boost::alignment::aligned_allocator<std::complex<double>, 32> >().swap(m_data);
  m_width = 0;
  m_height = 0;
  m_depth = 0;
}

bool ZComplexImg::isEmpty() const
{
  return m_data.empty() || m_width == 0 || m_height == 0 || m_depth == 0;
}

bool ZComplexImg::isSameSize(const ZComplexImg &rhs) const
{
  return m_width == rhs.m_width && m_height == rhs.m_height && m_depth == rhs.m_depth;
}

ZComplexImg &ZComplexImg::conj()
{
  for (std::complex<double> &v : m_data)
    v = std::conj(v);
  return *this;
}

ZComplexImg ZComplexImg::conj(const ZComplexImg &img)
{
  ZComplexImg res = img;
  return res.conj();
}

ZComplexImg& ZComplexImg::operator+=(const std::complex<double>& rhs)
{
  //std::transform(m_data.begin(), m_data.end(), m_data.begin(), std::bind2nd(std::plus<std::complex<double> >(), rhs));
  for (std::complex<double> &v : m_data)
    v += rhs;
  return *this;
}

ZComplexImg& ZComplexImg::operator+=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("complex img addition requires same size img as input: this <1>, other <%2>")
                        .arg(toQString()).arg(rhs.toQString()));
  }
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::plus<std::complex<double> >());
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
  //std::transform(m_data.begin(), m_data.end(), m_data.begin(), std::bind2nd(std::minus<std::complex<double> >(), rhs));
  for (std::complex<double> &v : m_data)
    v -= rhs;
  return *this;
}

ZComplexImg& ZComplexImg::operator-=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("complex img subtraction requires same size img as input: this <1>, other <%2>")
                        .arg(toQString()).arg(rhs.toQString()));
  }
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::minus<std::complex<double> >());
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
  std::transform(res.m_data.begin(), res.m_data.end(), res.m_data.begin(), std::negate<std::complex<double> >());
  return res;
}

ZComplexImg& ZComplexImg::operator*=(const std::complex<double>& rhs)
{
  //std::transform(m_data.begin(), m_data.end(), m_data.begin(), std::bind2nd(std::multiplies<std::complex<double> >(), rhs));
  for (std::complex<double> &v : m_data)
    v *= rhs;
  return *this;
}

ZComplexImg& ZComplexImg::operator*=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("complex img multiplies requires same size img as input: this <1>, other <%2>")
                        .arg(toQString()).arg(rhs.toQString()));
  }
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::multiplies<std::complex<double> >());
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
  //std::transform(m_data.begin(), m_data.end(), m_data.begin(), std::bind2nd(std::divides<std::complex<double> >(), rhs));
  for (std::complex<double> &v : m_data)
    v /= rhs;
  return *this;
}

ZComplexImg& ZComplexImg::operator/=(const ZComplexImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("complex img divides requires same size img as input: this <1>, other <%2>")
                        .arg(toQString()).arg(rhs.toQString()));
  }
  std::transform(m_data.begin(), m_data.end(), rhs.m_data.begin(), m_data.begin(), std::divides<std::complex<double> >());
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
