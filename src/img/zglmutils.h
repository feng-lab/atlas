#pragma once

// This file includes some commonly used headers from glm and defines some useful functions
// for glm

#define GLM_FORCE_SSE3
//#define GLM_FORCE_INLINE
#define GLM_FORCE_SIZE_T_LENGTH
#define GLM_FORCE_EXPLICIT_CTOR
//#define GLM_FORCE_MESSAGES
#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL

#include "zglobal.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtx/hash.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtx/matrix_transform_2d.hpp>
#include <QRegularExpression>
#include <QStringList>
#include <QLocale>
#include <iostream>
#include <sstream>
#include <tuple>

namespace glm {

using col3 = vec<3, unsigned char, highp>;
using col4 = vec<4, unsigned char, highp>;

// apply transform matrix
template<typename T, qualifier Q>
vec<3, T, Q> applyMatrix(const mat<4, 4, T, Q>& m, const vec<3, T, Q>& v)
{
  vec<4, T, Q> res = m * vec<4, T, Q>(v, T(1));
  return vec<3, T, Q>(res / res.w);
}

// given vec, get normalized vector e1 and e2 to make (e1,e2,vec) orthogonal to each other
// **crash** if vec is zero
template<typename T, qualifier Q>
void getOrthogonalVectors(const vec<3, T, Q>& v, vec<3, T, Q>& e1, vec<3, T, Q>& e2)
{
  static_assert(std::numeric_limits<T>::is_iec559, "'getOrthogonalVectors' only accept floating-point inputs");
  T eps = std::numeric_limits<T>::epsilon() * 1e2;

  e1 = cross(v, vec<3, T, Q>(T(1), T(0), T(0)));
  if (dot(e1, e1) < eps)
    e1 = cross(v, vec<3, T, Q>(T(0), T(1), T(0)));
  e1 = normalize(e1);
  e2 = normalize(cross(e1, v));
}

inline quat mix(const quat& q1, const quat& q2, double p)
{
  return mix(q1, q2, float(p));
}

} // namespace glm

namespace nim {

template<typename T, glm::qualifier Q>
class Vec2Compare
{
  bool less;
public:
  explicit Vec2Compare(bool less_ = true)
    : less(less_)
  {}

  bool operator()(const glm::vec<2, T, Q>& lhs, const glm::vec<2, T, Q>& rhs) const
  {
    if (less) {
      return std::tie(lhs.y, lhs.x) < std::tie(rhs.y, rhs.x);
    } else {
      return std::tie(lhs.y, lhs.x) > std::tie(rhs.y, rhs.x);
    }
  }
};

template<typename T, glm::qualifier Q>
class Vec3Compare
{
  bool less;
public:
  explicit Vec3Compare(bool less_ = true)
    : less(less_)
  {}

  bool operator()(const glm::vec<3, T, Q>& lhs, const glm::vec<3, T, Q>& rhs) const
  {
    if (less) {
      return std::tie(lhs.z, lhs.y, lhs.x) < std::tie(rhs.z, rhs.y, rhs.x);
    } else {
      return std::tie(lhs.z, lhs.y, lhs.x) > std::tie(rhs.z, rhs.y, rhs.x);
    }
  }
};

template<typename T, glm::qualifier Q>
class Vec4Compare
{
  bool less;
public:
  explicit Vec4Compare(bool less_ = true)
    : less(less_)
  {}

  bool operator()(const glm::vec<4, T, Q>& lhs, const glm::vec<4, T, Q>& rhs) const
  {
    if (less) {
      return std::tie(lhs.w, lhs.z, lhs.y, lhs.x) < std::tie(rhs.w, rhs.z, rhs.y, rhs.x);
    } else {
      return std::tie(lhs.w, lhs.z, lhs.y, lhs.x) > std::tie(rhs.w, rhs.z, rhs.y, rhs.x);
    }
  }
};

using Col3Compare = Vec3Compare<unsigned char, glm::highp>;
using Col4Compare = Vec4Compare<unsigned char, glm::highp>;

// serialization support

__forceinline void toVal(const QString& str, bool& v)
{
  v = QString::compare(str, "false", Qt::CaseInsensitive) != 0;
}

__forceinline void toVal(const QString& str, QString& v)
{
  v = str;
}

__forceinline void toVal(const QString& str, float& v)
{
  v = str.toFloat();
}

__forceinline void toVal(const QString& str, double& v)
{
  v = str.toDouble();
}

__forceinline void toVal(const QString& str, int& v)
{
  v = str.toInt();
}

__forceinline void toVal(const QString& str, unsigned char& v)
{
  v = str.toUShort();
}

__forceinline void toVal(const QString& str, size_t& v)
{
  v = str.toULongLong();
}

template<typename T>
__forceinline void toVal(const std::string& str, T& v)
{
  toVal(QString::fromStdString(str), v);
}

template<typename T>
__forceinline QString toQString(T v)
{
  if constexpr (std::is_floating_point_v<std::remove_reference_t<T>>) {
    return QString::number(v, 'g', QLocale::FloatingPointShortest);
  } else if constexpr (std::is_integral_v<std::remove_reference_t<T>>) {
    return QString::number(v);
  } else {
    static_assert(dependent_false<T>::value, "Must be number");
  }
}

__forceinline QString toQString(const QString& v)
{
  return v;
}

template<size_t L, typename T, glm::qualifier Q>
inline QString toQString(const glm::vec<L, T, Q>& v)
{
  QString res = "[" + QString::number(v[0]);
  for (size_t i = 1; i < L; ++i) {
    res += ", ";
    res += toQString(v[i]);
  }
  res += "]";
  return res;
}

template<size_t L, typename T, glm::qualifier Q>
inline void toVal(const QString& str, glm::vec<L, T, Q>& v)
{
  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (size_t i = 0; i < std::min(L, size_t(numList.size())); ++i) {
    toVal(numList[i], v[i]);
  }
}

template<size_t C, size_t R, typename T, glm::qualifier Q>
inline QString toQString(const glm::mat<C, R, T, Q>& m)
{
  QString res = "[";
  for (size_t r = 0; r < R; ++r) {
    if (r > 0)
      res += "; ";
    for (size_t c = 0; c < C; ++c) {
      if (c > 0)
        res += ", ";
      res += toQString(m[c][r]);
    }
  }
  res += "]";
  return res;
}

template<size_t C, size_t R, typename T, glm::qualifier Q>
inline void toVal(const QString& str, glm::mat<C, R, T, Q>& m)
{
  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (size_t i = 0; i < std::min(C * R, size_t(numList.size())); ++i) {
    toVal(numList[i], m[i % C][i / R]);
  }
}

template<typename T, glm::qualifier Q>
inline QString toQString(const glm::tquat<T, Q>& v)
{
  return "[" + toQString(v[0]) +
         ", " + toQString(v[1]) +
         ", " + toQString(v[2]) +
         ", " + toQString(v[3]) +
         "]";
}

template<typename T, glm::qualifier Q>
inline void toVal(const QString& str, glm::tquat<T, Q>& q)
{
  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (size_t i = 0; i < std::min(q.length(), size_t(numList.size())); ++i) {
    toVal(numList[i], q[i]);
  }
}

//-------------------------------------------------------------------------------------------------------------------------
// std iostream print

template<size_t L, typename T, glm::qualifier Q>
inline std::ostream& operator<<(std::ostream& s, const glm::vec<L, T, Q>& v)
{
  return (s << qUtf8Printable(toQString(v)));
}

template<size_t C, size_t R, typename T, glm::qualifier Q>
inline std::ostream& operator<<(std::ostream& s, const glm::mat<C, R, T, Q>& m)
{
  return (s << qUtf8Printable(toQString(m)));
}

template<typename T, glm::qualifier Q>
inline std::ostream& operator<<(std::ostream& s, const glm::tquat<T, Q>& q)
{
  return (s << qUtf8Printable(toQString(q)));
}

} // namespace nim

//-------------------------------------------------------------------------------------------------------------------------

