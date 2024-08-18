#pragma once

#include "zglobal.h"
#include "zrandom.h"
#include "zjson.h"
#include "zlog.h"
#include <utility>
#include <type_traits>

namespace nim {

// multidimensional coordinate of a voxel, its value_type is a signed integer type

struct ZVoxelCoordinate
{
  using value_type = index_t;

  enum class Init
  {
    Minimum,
    Maximum
  };

  value_type x, y, z, c, t;

  constexpr ZVoxelCoordinate()
    : x(0)
    , y(0)
    , z(0)
    , c(0)
    , t(0)
  {}

  explicit ZVoxelCoordinate(Init init)
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

  constexpr ZVoxelCoordinate(value_type x_,
                             value_type y_,
                             value_type z_ = value_type(0),
                             value_type c_ = value_type(0),
                             value_type t_ = value_type(0))
    : x(x_)
    , y(y_)
    , z(z_)
    , c(c_)
    , t(t_)
  {}

  void set(value_type x_,
           value_type y_,
           value_type z_ = value_type(0),
           value_type c_ = value_type(0),
           value_type t_ = value_type(0))
  {
    x = x_;
    y = y_;
    z = z_;
    c = c_;
    t = t_;
  }

  [[nodiscard]] static constexpr size_t size()
  {
    return 5;
  }

  [[nodiscard]] static constexpr size_t length()
  {
    return 5;
  }

  // access
  value_type& operator[](size_t i)
  {
    switch (i) {
      case 0:
        return x;
      case 1:
        return y;
      case 2:
        return z;
      case 3:
        return c;
      case 4:
        return t;
      default:
        CHECK(false);
    }
  }

  const value_type& operator[](size_t i) const
  {
    switch (i) {
      case 0:
        return x;
      case 1:
        return y;
      case 2:
        return z;
      case 3:
        return c;
      case 4:
        return t;
      default:
        CHECK(false);
    }
  }

  [[nodiscard]] bool allGreaterThan(const ZVoxelCoordinate& other) const
  {
    return x > other.x && y > other.y && z > other.z && c > other.c && t > other.t;
  }

  [[nodiscard]] bool allGreaterEqual(const ZVoxelCoordinate& other) const
  {
    return x >= other.x && y >= other.y && z >= other.z && c >= other.c && t >= other.t;
  }

  [[nodiscard]] bool allLessThan(const ZVoxelCoordinate& other) const
  {
    return other.allGreaterThan(*this);
  }

  [[nodiscard]] bool allLessEqual(const ZVoxelCoordinate& other) const
  {
    return other.allGreaterEqual(*this);
  }

  [[nodiscard]] bool allGreaterThan(value_type other) const
  {
    return x > other && y > other && z > other && c > other && t > other;
  }

  [[nodiscard]] bool allGreaterEqual(value_type other) const
  {
    return x >= other && y >= other && z >= other && c >= other && t >= other;
  }

  [[nodiscard]] bool allLessThan(value_type other) const
  {
    return x < other && y < other && z < other && c < other && t < other;
  }

  [[nodiscard]] bool allLessEqual(value_type other) const
  {
    return x <= other && y <= other && z <= other && c <= other && t <= other;
  }

  [[nodiscard]] bool anyGreaterThan(const ZVoxelCoordinate& other) const
  {
    return !allLessEqual(other);
  }

  [[nodiscard]] bool anyGreaterEqual(const ZVoxelCoordinate& other) const
  {
    return !allLessThan(other);
  }

  [[nodiscard]] bool anyLessThan(const ZVoxelCoordinate& other) const
  {
    return !allGreaterEqual(other);
  }

  [[nodiscard]] bool anyLessEqual(const ZVoxelCoordinate& other) const
  {
    return !allGreaterThan(other);
  }

  [[nodiscard]] bool anyGreaterThan(value_type other) const
  {
    return !allLessEqual(other);
  }

  [[nodiscard]] bool anyGreaterEqual(value_type other) const
  {
    return !allLessThan(other);
  }

  [[nodiscard]] bool anyLessThan(value_type other) const
  {
    return !allGreaterEqual(other);
  }

  [[nodiscard]] bool anyLessEqual(value_type other) const
  {
    return !allGreaterThan(other);
  }

  [[nodiscard]] bool anyEqual(const ZVoxelCoordinate& other) const
  {
    return x == other.x || y == other.y || z == other.z || c == other.c || t == other.t;
  }

  [[nodiscard]] bool anyEqual(value_type other) const
  {
    return x == other || y == other || z == other || c == other || t == other;
  }

  // operators
  auto operator<=>(const ZVoxelCoordinate& other) const = default;

  ZVoxelCoordinate& operator+=(value_type rhs)
  {
    x += rhs;
    y += rhs;
    z += rhs;
    c += rhs;
    t += rhs;
    return *this;
  }

  ZVoxelCoordinate& operator+=(const ZVoxelCoordinate& rhs)
  {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    c += rhs.c;
    t += rhs.t;
    return *this;
  }

  ZVoxelCoordinate& operator-=(value_type rhs)
  {
    x -= rhs;
    y -= rhs;
    z -= rhs;
    c -= rhs;
    t -= rhs;
    return *this;
  }

  ZVoxelCoordinate& operator-=(const ZVoxelCoordinate& rhs)
  {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    c -= rhs.c;
    t -= rhs.t;
    return *this;
  }

  ZVoxelCoordinate operator-() const
  {
    return {-x, -y, -z, -c, -t};
  }

  ZVoxelCoordinate& operator*=(value_type rhs)
  {
    x *= rhs;
    y *= rhs;
    z *= rhs;
    c *= rhs;
    t *= rhs;
    return *this;
  }

  ZVoxelCoordinate& operator*=(const ZVoxelCoordinate& rhs)
  {
    x *= rhs.x;
    y *= rhs.y;
    z *= rhs.z;
    c *= rhs.c;
    t *= rhs.t;
    return *this;
  }

  ZVoxelCoordinate& operator/=(value_type rhs)
  {
    x /= rhs;
    y /= rhs;
    z /= rhs;
    c /= rhs;
    t /= rhs;
    return *this;
  }

  ZVoxelCoordinate& operator/=(const ZVoxelCoordinate& rhs)
  {
    x /= rhs.x;
    y /= rhs.y;
    z /= rhs.z;
    c /= rhs.c;
    t /= rhs.t;
    return *this;
  }

  [[nodiscard]] std::string toString() const
  {
    return jsonToString(*this);
  }

  // ttsize[0] to ttsize[4] are the sizes of x to t, advance current coord to next valid coord,
  // return false if reach the end
  template<typename T>
  bool advance(const T& ttsize)
  {
    ++(*this)[0];
    for (size_t i = 0; (i < 4) && ((*this)[i] >= ttsize[i]); ++i) {
      (*this)[i] = 0;
      ++(*this)[i + 1];
    }
    if ((*this)[4] >= ttsize[4]) {
      *this = lastCoordinate(ttsize);
      return false;
    } else {
      return true;
    }
  }

  // return a valid coord within size, ttsize[0] to ttsize[4] are the sizes of x to t
  template<typename T>
  static ZVoxelCoordinate random(const T& ttsize)
  {
    CHECK(ttsize[0] > 0 && ttsize[1] > 0 && ttsize[2] > 0 && ttsize[3] > 0 && ttsize[4] > 0);
    value_type resx = ttsize[0] == 1 ? 0 : ZRandom::instance().randInt<value_type>(ttsize[0] - 1);
    value_type resy = ttsize[1] == 1 ? 0 : ZRandom::instance().randInt<value_type>(ttsize[1] - 1);
    value_type resz = ttsize[2] == 1 ? 0 : ZRandom::instance().randInt<value_type>(ttsize[2] - 1);
    value_type resc = ttsize[3] == 1 ? 0 : ZRandom::instance().randInt<value_type>(ttsize[3] - 1);
    value_type rest = ttsize[4] == 1 ? 0 : ZRandom::instance().randInt<value_type>(ttsize[4] - 1);
    return {resx, resy, resz, resc, rest};
  }

  // return the last valid coord within size, ttsize[0] to ttsize[4] are the sizes of x to t
  template<typename T>
  static ZVoxelCoordinate lastCoordinate(const T& ttsize)
  {
    CHECK(ttsize[0] > 0 && ttsize[1] > 0 && ttsize[2] > 0 && ttsize[3] > 0 && ttsize[4] > 0);
    return ZVoxelCoordinate(ttsize[0] - 1, ttsize[1] - 1, ttsize[2] - 1, ttsize[3] - 1, ttsize[4] - 1);
  }
};

// Binary arithmetic operators
inline ZVoxelCoordinate operator+(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {a.x + b, a.y + b, a.z + b, a.c + b, a.t + b};
}

inline ZVoxelCoordinate operator+(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {a.x + b, a.y + b, a.z + b, a.c + b, a.t + b};
}

inline ZVoxelCoordinate operator+(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z, a.c + b.c, a.t + b.t};
}

inline ZVoxelCoordinate operator-(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {a.x - b, a.y - b, a.z - b, a.c - b, a.t - b};
}

inline ZVoxelCoordinate operator-(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {b - a.x, b - a.y, b - a.z, b - a.c, b - a.t};
}

inline ZVoxelCoordinate operator-(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z, a.c - b.c, a.t - b.t};
}

inline ZVoxelCoordinate operator*(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {a.x * b, a.y * b, a.z * b, a.c * b, a.t * b};
}

inline ZVoxelCoordinate operator*(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {a.x * b, a.y * b, a.z * b, a.c * b, a.t * b};
}

inline ZVoxelCoordinate operator*(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {a.x * b.x, a.y * b.y, a.z * b.z, a.c * b.c, a.t * b.t};
}

inline ZVoxelCoordinate operator/(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {a.x / b, a.y / b, a.z / b, a.c / b, a.t / b};
}

inline ZVoxelCoordinate operator/(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {b / a.x, b / a.y, b / a.z, b / a.c, b / a.t};
}

inline ZVoxelCoordinate operator/(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {a.x / b.x, a.y / b.y, a.z / b.z, a.c / b.c, a.t / b.t};
}

inline ZVoxelCoordinate max(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), std::max(a.c, b.c), std::max(a.t, b.t)};
}

inline ZVoxelCoordinate max(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {std::max(a.x, b), std::max(a.y, b), std::max(a.z, b), std::max(a.c, b), std::max(a.t, b)};
}

inline ZVoxelCoordinate max(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {std::max(a.x, b), std::max(a.y, b), std::max(a.z, b), std::max(a.c, b), std::max(a.t, b)};
}

inline ZVoxelCoordinate min(const ZVoxelCoordinate& a, const ZVoxelCoordinate& b)
{
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::min(a.c, b.c), std::min(a.t, b.t)};
}

inline ZVoxelCoordinate min(const ZVoxelCoordinate& a, ZVoxelCoordinate::value_type b)
{
  return {std::min(a.x, b), std::min(a.y, b), std::min(a.z, b), std::min(a.c, b), std::min(a.t, b)};
}

inline ZVoxelCoordinate min(ZVoxelCoordinate::value_type b, const ZVoxelCoordinate& a)
{
  return {std::min(a.x, b), std::min(a.y, b), std::min(a.z, b), std::min(a.c, b), std::min(a.t, b)};
}

template<std::size_t Index>
constexpr auto&& get(ZVoxelCoordinate& v) noexcept
{
  return tupleLikeGetHelper<Index, 5>(v);
}

template<std::size_t Index>
constexpr auto&& get(const ZVoxelCoordinate& v) noexcept
{
  return tupleLikeGetHelper<Index, 5>(v);
}

template<std::size_t Index>
constexpr auto&& get(ZVoxelCoordinate&& v) noexcept
{
  return tupleLikeGetHelper<Index, 5>(std::move(v));
}

template<std::size_t Index>
constexpr auto&& get(const ZVoxelCoordinate&& v) noexcept
{
  return tupleLikeGetHelper<Index, 5>(std::move(v));
}

} // namespace nim

namespace std {

template<>
struct tuple_size<nim::ZVoxelCoordinate> : integral_constant<size_t, 5>
{};

template<std::size_t Index>
struct tuple_element<Index, nim::ZVoxelCoordinate>
{
  static_assert(Index < 5, "Index out of bounds for ZVoxelCoordinate");
  using type = nim::ZVoxelCoordinate::value_type;
};

} // namespace std
