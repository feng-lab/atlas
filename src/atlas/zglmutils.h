#pragma once

// This file includes some commonly used headers from glm and defines some useful functions
// for glm

#define GLM_FORCE_SSE3
//#define GLM_FORCE_INLINE
#define GLM_FORCE_SIZE_T_LENGTH
#define GLM_FORCE_NO_CTOR_INIT
#define GLM_FORCE_EXPLICIT_CTOR
//#define GLM_MESSAGES
#define GLM_SWIZZLE

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtx/hash.hpp>

#include <iostream>
#include <sstream>
#include <tuple>

#include <QRegularExpression>
#include <QStringList>
#include <QColor>
#include <QLocale>

namespace glm {
typedef tvec3<unsigned char, highp> col3;
typedef tvec4<unsigned char, highp> col4;

// apply transform matrix
template<typename T, precision P>
tvec3<T, P> applyMatrix(const tmat4x4<T, P>& mat, const tvec3<T, P>& vec)
{
  tvec4<T, P> res = mat * tvec4<T, P>(vec, T(1));
  return tvec3<T, P>(res / res.w);
}

// given vec, get normalized vector e1 and e2 to make (e1,e2,vec) orthogonal to each other
// **crash** if vec is zero
template<typename T, precision P>
void getOrthogonalVectors(const tvec3<T, P>& vec, tvec3<T, P>& e1, tvec3<T, P>& e2)
{
  GLM_STATIC_ASSERT(std::numeric_limits<T>::is_iec559, "'getOrthogonalVectors' only accept floating-point inputs");
  T eps = std::numeric_limits<T>::epsilon() * 1e2;

  e1 = cross(vec, tvec3<T, P>(T(1), T(0), T(0)));
  if (dot(e1, e1) < eps)
    e1 = cross(vec, tvec3<T, P>(T(0), T(1), T(0)));
  e1 = normalize(e1);
  e2 = normalize(cross(e1, vec));
}

inline quat mix(const quat& q1, const quat& q2, double p)
{
  return mix(q1, q2, float(p));
}

}

namespace nim {

template<typename T, glm::precision P>
class Vec2Compare
{
  bool less;
public:
  Vec2Compare(bool less = true) : less(less)
  {}

  bool operator()(const glm::tvec2<T, P>& lhs, const glm::tvec2<T, P>& rhs) const
  {
    if (less) {
      return std::tie(lhs.y, lhs.x) < std::tie(rhs.y, rhs.x);
    } else {
      return std::tie(lhs.y, lhs.x) > std::tie(rhs.y, rhs.x);
    }
  }
};

template<typename T, glm::precision P>
class Vec3Compare
{
  bool less;
public:
  Vec3Compare(bool less = true) : less(less)
  {}

  bool operator()(const glm::tvec3<T, P>& lhs, const glm::tvec3<T, P>& rhs) const
  {
    if (less) {
      return std::tie(lhs.z, lhs.y, lhs.x) < std::tie(rhs.z, rhs.y, rhs.x);
    } else {
      return std::tie(lhs.z, lhs.y, lhs.x) > std::tie(rhs.z, rhs.y, rhs.x);
    }
  }
};

template<typename T, glm::precision P>
class Vec4Compare
{
  bool less;
public:
  Vec4Compare(bool less = true) : less(less)
  {}

  bool operator()(const glm::tvec4<T, P>& lhs, const glm::tvec4<T, P>& rhs) const
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

inline void toVal(const QString& str, bool& v)
{
  v = QString::compare(str, "false", Qt::CaseInsensitive) != 0;
}

inline void toVal(const QString& str, QString& v)
{
  v = str;
}

inline void toVal(const QString& str, float& v)
{
  v = str.toFloat();
}

inline void toVal(const QString& str, double& v)
{
  v = str.toDouble();
}

inline void toVal(const QString& str, int& v)
{
  v = str.toInt();
}

inline void toVal(const QString& str, unsigned char& v)
{
  v = str.toUShort();
}

inline void toVal(const QString& str, size_t& v)
{
  v = str.toULongLong();
}

template<typename T>
inline void toVal(const std::string& str, T& v)
{
  toVal(QString::fromStdString(str), v);
}

template<typename T>
inline QString toQString(T v)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return QString::number(v);
}

inline QString toQString(float v)
{
  return QString::number(v, 'g', QLocale::FloatingPointShortest);
}

inline QString toQString(double v)
{
  return QString::number(v, 'g', QLocale::FloatingPointShortest);
}

inline QString toQString(const QString& v)
{
  return v;
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tvec2<T, P>& v)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(v[0]) +
         ", " + QString::number(v[1]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec2<float, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec2<double, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tvec2<T, P>& v)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(2, numList.size()); ++i) {
    toVal(numList[i], v[i]);
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tvec3<T, P>& v)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(v[0]) +
         ", " + QString::number(v[1]) +
         ", " + QString::number(v[2]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec3<float, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec3<double, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tvec3<T, P>& v)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(3, numList.size()); ++i) {
    toVal(numList[i], v[i]);
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tvec4<T, P>& v)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(v[0]) +
         ", " + QString::number(v[1]) +
         ", " + QString::number(v[2]) +
         ", " + QString::number(v[3]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec4<float, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tvec4<double, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tvec4<T, P>& v)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(4, numList.size()); ++i) {
    toVal(numList[i], v[i]);
  }
}

inline QString toQString(const QColor& v)
{
  return "[" + QString::number(v.red()) +
         ", " + QString::number(v.green()) +
         ", " + QString::number(v.blue()) +
         ", " + QString::number(v.alpha()) +
         "]";
}

inline void toVal(const QString& str, QColor& v)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(4, numList.size()); ++i) {
    int c;
    toVal(numList[i], c);
    if (i == 0) {
      v.setRed(c);
    } else if (i == 1) {
      v.setGreen(c);
    } else if (i == 2) {
      v.setBlue(c);
    } else if (i == 3) {
      v.setAlpha(c);
    }
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tmat2x2<T, P>& m)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(m[0][0]) +
         ", " + QString::number(m[1][0]) +
         "; " + QString::number(m[0][1]) +
         ", " + QString::number(m[1][1]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat2x2<float, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat2x2<double, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tmat2x2<T, P>& m)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(4, numList.size()); ++i) {
    toVal(numList[i], m[i % 2][i / 2]);
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tmat3x3<T, P>& m)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(m[0][0]) +
         ", " + QString::number(m[1][0]) +
         ", " + QString::number(m[2][0]) +
         "; " + QString::number(m[0][1]) +
         ", " + QString::number(m[1][1]) +
         ", " + QString::number(m[2][1]) +
         "; " + QString::number(m[0][2]) +
         ", " + QString::number(m[1][2]) +
         ", " + QString::number(m[2][2]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat3x3<float, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][1], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][2], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat3x3<double, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][1], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][2], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tmat3x3<T, P>& m)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(9, numList.size()); ++i) {
    toVal(numList[i], m[i % 3][i / 3]);
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tmat4x4<T, P>& m)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(m[0][0]) +
         ", " + QString::number(m[1][0]) +
         ", " + QString::number(m[2][0]) +
         ", " + QString::number(m[3][0]) +
         "; " + QString::number(m[0][1]) +
         ", " + QString::number(m[1][1]) +
         ", " + QString::number(m[2][1]) +
         ", " + QString::number(m[3][1]) +
         "; " + QString::number(m[0][2]) +
         ", " + QString::number(m[1][2]) +
         ", " + QString::number(m[2][2]) +
         ", " + QString::number(m[3][2]) +
         "; " + QString::number(m[0][3]) +
         ", " + QString::number(m[1][3]) +
         ", " + QString::number(m[2][3]) +
         ", " + QString::number(m[3][3]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat4x4<float, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][1], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][2], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tmat4x4<double, P>& m)
{
  return "[" + QString::number(m[0][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][0], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][1], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][2], 'g', QLocale::FloatingPointShortest) +
         "; " + QString::number(m[0][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[1][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[2][3], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(m[3][3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tmat4x4<T, P>& m)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(16, numList.size()); ++i) {
    toVal(numList[i], m[i % 4][i / 4]);
  }
}

template<typename T, glm::precision P>
inline QString toQString(const glm::tquat<T, P>& v)
{
  static_assert(std::is_integral<T>::value, "Integer required.");
  return "[" + QString::number(v[0]) +
         ", " + QString::number(v[1]) +
         ", " + QString::number(v[2]) +
         ", " + QString::number(v[3]) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tquat<float, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<glm::precision P>
inline QString toQString(const glm::tquat<double, P>& v)
{
  return "[" + QString::number(v[0], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[1], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[2], 'g', QLocale::FloatingPointShortest) +
         ", " + QString::number(v[3], 'g', QLocale::FloatingPointShortest) +
         "]";
}

template<typename T, glm::precision P>
inline void toVal(const QString& str, glm::tquat<T, P>& q)
{
  QRegularExpression rx("(\\ |\\,|\\[|\\]|\\;)"); //RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, QString::SkipEmptyParts);
  for (int i = 0; i < std::min(4, numList.size()); ++i) {
    toVal(numList[i], q[i]);
  }
}

//-------------------------------------------------------------------------------------------------------------------------
// std iostream print

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tvec2<T, P>& v)
{
  return (s << qUtf8Printable(toQString(v)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tvec3<T, P>& v)
{
  return (s << qUtf8Printable(toQString(v)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tvec4<T, P>& v)
{
  return (s << qUtf8Printable(toQString(v)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tmat2x2<T, P>& m)
{
  return (s << qUtf8Printable(toQString(m)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tmat3x3<T, P>& m)
{
  return (s << qUtf8Printable(toQString(m)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tmat4x4<T, P>& m)
{
  return (s << qUtf8Printable(toQString(m)));
}

template<typename T, glm::precision P>
inline std::ostream& operator<<(std::ostream& s, const glm::tquat<T, P>& q)
{
  return (s << qUtf8Printable(toQString(q)));
}

} // namespace nim

//-------------------------------------------------------------------------------------------------------------------------

