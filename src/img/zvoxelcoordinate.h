#pragma once

#include <sstream>
#include <tuple>
#include <QString>
#include "zglobal.h"

namespace nim {

// multi-dimensional coordinate of a voxel, its value_type is a signed integer type
#pragma pack(push, 1)

struct ZVoxelCoordinate
{
  using value_type = int;

  enum class Init
  {
    Minimum, Maximum
  };

  value_type x, y, z, c, t;

  inline constexpr ZVoxelCoordinate()
    : x(0), y(0), z(0), c(0), t(0)
  {}

  inline ZVoxelCoordinate(Init init)
  {
    switch (init) {
      case Init::Minimum:
        x = std::numeric_limits<value_type>::min();
        y = std::numeric_limits<value_type>::min();
        z = std::numeric_limits<value_type>::min();
        c = std::numeric_limits<value_type>::min();
        t = std::numeric_limits<value_type>::min();
        break;
      case Init::Maximum:
        x = std::numeric_limits<value_type>::max();
        y = std::numeric_limits<value_type>::max();
        z = std::numeric_limits<value_type>::max();
        c = std::numeric_limits<value_type>::max();
        t = std::numeric_limits<value_type>::max();
        break;
      default:
        x = 0;
        y = 0;
        z = 0;
        c = 0;
        t = 0;
        break;
    }
  }

  inline constexpr ZVoxelCoordinate(value_type xin, value_type yin,
                                    value_type zin = value_type(0),
                                    value_type cin = value_type(0),
                                    value_type tin = value_type(0))
    : x(xin), y(yin), z(zin), c(cin), t(tin)
  {}

  inline void set(value_type xin, value_type yin,
                  value_type zin = value_type(0),
                  value_type cin = value_type(0),
                  value_type tin = value_type(0))
  {
    x = xin;
    y = yin;
    z = zin;
    c = cin;
    t = tin;
  }

  inline constexpr size_t size() const
  { return 5; }

  inline constexpr size_t length() const
  { return 5; }

  // access
  inline value_type& operator[](size_t i)
  { return (&x)[i]; }

  inline const value_type& operator[](size_t i) const
  { return (&x)[i]; }

  inline bool allGreaterThan(const ZVoxelCoordinate& other) const
  {
    return x > other.x && y > other.y && z > other.z && c > other.c && t > other.t;
  }

  inline bool allGreaterEqual(const ZVoxelCoordinate& other) const
  {
    return x >= other.x && y >= other.y && z >= other.z && c >= other.c && t >= other.t;
  }

  inline bool allLessThan(const ZVoxelCoordinate& other) const
  {
    return other.allGreaterThan(*this);
  }

  inline bool allLessEqual(const ZVoxelCoordinate& other) const
  {
    return other.allGreaterEqual(*this);
  }

  inline bool allGreaterThan(value_type other) const
  {
    return x > other && y > other && z > other && c > other && t > other;
  }

  inline bool allGreaterEqual(value_type other) const
  {
    return x >= other && y >= other && z >= other && c >= other && t >= other;
  }

  inline bool allLessThan(value_type other) const
  {
    return x < other && y < other && z < other && c < other && t < other;
  }

  inline bool allLessEqual(value_type other) const
  {
    return x <= other && y <= other && z <= other && c <= other && t <= other;
  }

  inline bool anyGreaterThan(const ZVoxelCoordinate& other) const
  {
    return !allLessEqual(other);
  }

  inline bool anyGreaterEqual(const ZVoxelCoordinate& other) const
  {
    return !allLessThan(other);
  }

  inline bool anyLessThan(const ZVoxelCoordinate& other) const
  {
    return !allGreaterEqual(other);
  }

  inline bool anyLessEqual(const ZVoxelCoordinate& other) const
  {
    return !allGreaterThan(other);
  }

  inline bool anyGreaterThan(value_type other) const
  {
    return !allLessEqual(other);
  }

  inline bool anyGreaterEqual(value_type other) const
  {
    return !allLessThan(other);
  }

  inline bool anyLessThan(value_type other) const
  {
    return !allGreaterEqual(other);
  }

  inline bool anyLessEqual(value_type other) const
  {
    return !allGreaterThan(other);
  }

  inline bool anyEqual(const ZVoxelCoordinate& other) const
  {
    return x == other.x || y == other.y || z == other.z || c == other.c || t == other.t;
  }

  inline bool anyEqual(value_type other) const
  {
    return x == other || y == other || z == other || c == other || t == other;
  }

  // operators
  inline bool operator<(const ZVoxelCoordinate& other) const
  {
    return std::tie(x, y, z, c, t) < std::tie(other.x, other.y, other.z, other.c, other.t);
  }

  inline bool operator==(const ZVoxelCoordinate& other) const
  {
    return x == other.x && y == other.y && z == other.z && c == other.c && t == other.t;
  }

  inline bool operator!=(const ZVoxelCoordinate& other) const
  {
    return x != other.x || y != other.y || z != other.z || c != other.c || t != other.t;
  }

  inline ZVoxelCoordinate& operator+=(value_type rhs)
  {
    x += rhs;
    y += rhs;
    z += rhs;
    c += rhs;
    t += rhs;
    return *this;
  }

  inline ZVoxelCoordinate& operator+=(const ZVoxelCoordinate& rhs)
  {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    c += rhs.c;
    t += rhs.t;
    return *this;
  }

  inline ZVoxelCoordinate& operator-=(value_type rhs)
  {
    x -= rhs;
    y -= rhs;
    z -= rhs;
    c -= rhs;
    t -= rhs;
    return *this;
  }

  inline ZVoxelCoordinate& operator-=(const ZVoxelCoordinate& rhs)
  {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    c -= rhs.c;
    t -= rhs.t;
    return *this;
  }

  inline ZVoxelCoordinate operator-() const
  {
    return ZVoxelCoordinate(-x, -y, -z, -c, -t);
  }

  inline ZVoxelCoordinate& operator*=(value_type rhs)
  {
    x *= rhs;
    y *= rhs;
    z *= rhs;
    c *= rhs;
    t *= rhs;
    return *this;
  }

  inline ZVoxelCoordinate& operator*=(const ZVoxelCoordinate& rhs)
  {
    x *= rhs.x;
    y *= rhs.y;
    z *= rhs.z;
    c *= rhs.c;
    t *= rhs.t;
    return *this;
  }

  inline ZVoxelCoordinate& operator/=(value_type rhs)
  {
    x /= rhs;
    y /= rhs;
    z /= rhs;
    c /= rhs;
    t /= rhs;
    return *this;
  }

  inline ZVoxelCoordinate& operator/=(const ZVoxelCoordinate& rhs)
  {
    x /= rhs.x;
    y /= rhs.y;
    z /= rhs.z;
    c /= rhs.c;
    t /= rhs.t;
    return *this;
  }

  inline QString toQString() const
  {
    return QString("(%1,%2,%3,%4,%5)").arg(x).arg(y).arg(z).arg(c).arg(t);
  }

};

#pragma pack(pop)

// Binary arithmetic operators
inline ZVoxelCoordinate operator+(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(a.x + b, a.y + b, a.z + b, a.c + b, a.t + b);
}

inline ZVoxelCoordinate operator+(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(a.x + b, a.y + b, a.z + b, a.c + b, a.t + b);
}

inline ZVoxelCoordinate operator+(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(a.x + b.x, a.y + b.y, a.z + b.z, a.c + b.c, a.t + b.t);
}

inline ZVoxelCoordinate operator-(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(a.x - b, a.y - b, a.z - b, a.c - b, a.t - b);
}

inline ZVoxelCoordinate operator-(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(b - a.x, b - a.y, b - a.z, b - a.c, b - a.t);
}

inline ZVoxelCoordinate operator-(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(a.x - b.x, a.y - b.y, a.z - b.z, a.c - b.c, a.t - b.t);
}

inline ZVoxelCoordinate operator*(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(a.x * b, a.y * b, a.z * b, a.c * b, a.t * b);
}

inline ZVoxelCoordinate operator*(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(a.x * b, a.y * b, a.z * b, a.c * b, a.t * b);
}

inline ZVoxelCoordinate operator*(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(a.x * b.x, a.y * b.y, a.z * b.z, a.c * b.c, a.t * b.t);
}

inline ZVoxelCoordinate operator/(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(a.x / b, a.y / b, a.z / b, a.c / b, a.t / b);
}

inline ZVoxelCoordinate operator/(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(b / a.x, b / a.y, b / a.z, b / a.c, b / a.t);
}

inline ZVoxelCoordinate operator/(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(a.x / b.x, a.y / b.y, a.z / b.z, a.c / b.c, a.t / b.t);
}

inline ZVoxelCoordinate max(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z),
                          std::max(a.c, b.c), std::max(a.t, b.t));
}

inline ZVoxelCoordinate max(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(std::max(a.x, b), std::max(a.y, b), std::max(a.z, b),
                          std::max(a.c, b), std::max(a.t, b));
}

inline ZVoxelCoordinate max(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(std::max(a.x, b), std::max(a.y, b), std::max(a.z, b),
                          std::max(a.c, b), std::max(a.t, b));
}

inline ZVoxelCoordinate min(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return ZVoxelCoordinate(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z),
                          std::min(a.c, b.c), std::min(a.t, b.t));
}

inline ZVoxelCoordinate min(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return ZVoxelCoordinate(std::min(a.x, b), std::min(a.y, b), std::min(a.z, b),
                          std::min(a.c, b), std::min(a.t, b));
}

inline ZVoxelCoordinate min(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return ZVoxelCoordinate(std::min(a.x, b), std::min(a.y, b), std::min(a.z, b),
                          std::min(a.c, b), std::min(a.t, b));
}

//
inline std::ostream& operator<<(std::ostream& cout, const ZVoxelCoordinate& c)
{
  return cout << qUtf8Printable(c.toQString());
}

} // namespace nim

