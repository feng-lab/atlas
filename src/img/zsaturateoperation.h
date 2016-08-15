#pragma once

// note: these operations only works for sized type define like int32_t uint64_t
// that means one of "long" and "long long" type is not overloaded which will cause ambiguous overloading
// if it happens you have to convert the ambiguous type (e.g. size_t can be long or long long) into one of
// the sized type to make the overload work

#include <cstdint>
#include <numeric>
#include <cmath>
#include <limits>
#include <immintrin.h>
#if 0
#include <boost/multiprecision/cpp_int.hpp>
#endif

namespace nim {

// T must be integer type and Float must be float type
template<typename T, typename Float>
inline T roundTo(Float x)
{
  if (std::isnan(x))
    return 0;
  else if (x <= std::numeric_limits<T>::min())
    return std::numeric_limits<T>::min();
  else if (x >= std::numeric_limits<T>::max())
    return std::numeric_limits<T>::max();
  else
    return static_cast<T>(std::round(x));
}

/////////////// saturate_cast (modified from opencv, add (u)int32_t and (u)int64_t suppport) ///////////////////
///     for float type, do round cast
///

template<typename _Tp> static inline _Tp saturate_cast(uint8_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(int8_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(uint16_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(int16_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(uint32_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(int32_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(uint64_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(int64_t v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(float v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(double v) { return static_cast<_Tp>(v); }
template<typename _Tp> static inline _Tp saturate_cast(long double v) { return static_cast<_Tp>(v); }

template<> inline uint8_t saturate_cast<uint8_t>(int8_t v) { return static_cast<uint8_t>(std::max<int32_t>(v, 0)); }
template<> inline uint8_t saturate_cast<uint8_t>(uint16_t v) { return static_cast<uint8_t>(std::min<int32_t>(v, UINT8_MAX)); }
template<> inline uint8_t saturate_cast<uint8_t>(int32_t v) { return static_cast<uint8_t>(static_cast<uint32_t>(v) <= static_cast<uint32_t>(UINT8_MAX) ? v : v > 0 ? UINT8_MAX : 0); }
template<> inline uint8_t saturate_cast<uint8_t>(int16_t v) { return saturate_cast<uint8_t>(static_cast<int32_t>(v)); }
template<> inline uint8_t saturate_cast<uint8_t>(uint32_t v) { return static_cast<uint8_t>(std::min<uint32_t>(v, UINT8_MAX)); }
template<> inline uint8_t saturate_cast<uint8_t>(int64_t v) { return static_cast<uint8_t>(static_cast<uint64_t>(v) <= static_cast<uint64_t>(UINT8_MAX) ? v : v > 0 ? UINT8_MAX : 0); }
template<> inline uint8_t saturate_cast<uint8_t>(uint64_t v) { return static_cast<uint8_t>(std::min<uint64_t>(v, UINT8_MAX)); }
template<> inline uint8_t saturate_cast<uint8_t>(float v) { return roundTo<uint8_t>(v); }
template<> inline uint8_t saturate_cast<uint8_t>(double v) { return roundTo<uint8_t>(v);  }
template<> inline uint8_t saturate_cast<uint8_t>(long double v) { return roundTo<uint8_t>(v);  }

template<> inline int8_t saturate_cast<int8_t>(uint8_t v) { return static_cast<int8_t>(std::min<int32_t>(v, INT8_MAX)); }
template<> inline int8_t saturate_cast<int8_t>(uint16_t v) { return static_cast<int8_t>(std::min<int32_t>(v, INT8_MAX)); }
template<> inline int8_t saturate_cast<int8_t>(int32_t v) { return static_cast<int8_t>(static_cast<uint32_t>(v-INT8_MIN) <= static_cast<uint32_t>(UINT8_MAX) ? v : v > 0 ? INT8_MAX : INT8_MIN); }
template<> inline int8_t saturate_cast<int8_t>(int16_t v) { return saturate_cast<int8_t>(static_cast<int32_t>(v)); }
template<> inline int8_t saturate_cast<int8_t>(uint32_t v) { return static_cast<int8_t>(std::min<uint32_t>(v, INT8_MAX)); }
template<> inline int8_t saturate_cast<int8_t>(int64_t v) { return static_cast<int8_t>(static_cast<uint64_t>(v-INT8_MIN) <= static_cast<uint64_t>(UINT8_MAX) ? v : v > 0 ? INT8_MAX : INT8_MIN); }
template<> inline int8_t saturate_cast<int8_t>(uint64_t v) { return static_cast<int8_t>(std::min<uint64_t>(v, INT8_MAX)); }
template<> inline int8_t saturate_cast<int8_t>(float v) { return roundTo<int8_t>(v);  }
template<> inline int8_t saturate_cast<int8_t>(double v) { return roundTo<int8_t>(v);  }
template<> inline int8_t saturate_cast<int8_t>(long double v) { return roundTo<int8_t>(v);  }

template<> inline uint16_t saturate_cast<uint16_t>(int8_t v) { return static_cast<uint16_t>(std::max<int32_t>(v, 0)); }
template<> inline uint16_t saturate_cast<uint16_t>(int16_t v) { return static_cast<uint16_t>(std::max<int32_t>(v, 0)); }
template<> inline uint16_t saturate_cast<uint16_t>(int32_t v) { return static_cast<uint16_t>(static_cast<uint32_t>(v) <= static_cast<uint32_t>(UINT16_MAX) ? v : v > 0 ? UINT16_MAX : 0); }
template<> inline uint16_t saturate_cast<uint16_t>(uint32_t v) { return static_cast<uint16_t>(std::min<uint32_t>(v, UINT16_MAX)); }
template<> inline uint16_t saturate_cast<uint16_t>(int64_t v) { return static_cast<uint16_t>(static_cast<uint64_t>(v) <= static_cast<uint64_t>(UINT16_MAX) ? v : v > 0 ? UINT16_MAX : 0); }
template<> inline uint16_t saturate_cast<uint16_t>(uint64_t v) { return static_cast<uint16_t>(std::min<uint64_t>(v, UINT16_MAX)); }
template<> inline uint16_t saturate_cast<uint16_t>(float v) { return roundTo<uint16_t>(v);  }
template<> inline uint16_t saturate_cast<uint16_t>(double v) { return roundTo<uint16_t>(v); }
template<> inline uint16_t saturate_cast<uint16_t>(long double v) { return roundTo<uint16_t>(v); }

template<> inline int16_t saturate_cast<int16_t>(uint16_t v) { return static_cast<int16_t>(std::min<int32_t>(v, INT16_MAX)); }
template<> inline int16_t saturate_cast<int16_t>(int32_t v) { return static_cast<int16_t>(static_cast<uint32_t>(v-INT16_MIN) <= static_cast<uint32_t>(UINT16_MAX) ? v : v > 0 ? INT16_MAX : INT16_MIN); }
template<> inline int16_t saturate_cast<int16_t>(uint32_t v) { return static_cast<int16_t>(std::min<uint32_t>(v, INT16_MAX)); }
template<> inline int16_t saturate_cast<int16_t>(int64_t v) { return static_cast<int16_t>(static_cast<uint64_t>(v-INT16_MIN) <= static_cast<uint64_t>(UINT16_MAX) ? v : v > 0 ? INT16_MAX : INT16_MIN); }
template<> inline int16_t saturate_cast<int16_t>(uint64_t v) { return static_cast<int16_t>(std::min<uint64_t>(v, INT16_MAX)); }
template<> inline int16_t saturate_cast<int16_t>(float v) { return roundTo<int16_t>(v); }
template<> inline int16_t saturate_cast<int16_t>(double v) { return roundTo<int16_t>(v); }
template<> inline int16_t saturate_cast<int16_t>(long double v) { return roundTo<int16_t>(v); }

template<> inline int32_t saturate_cast<int32_t>(uint32_t v) { return static_cast<int32_t>(std::min<uint32_t>(v, INT32_MAX)); }
template<> inline int32_t saturate_cast<int32_t>(int64_t v) { return static_cast<int32_t>(static_cast<uint64_t>(v-INT32_MIN) <= static_cast<uint64_t>(UINT32_MAX) ? v : v > 0 ? INT32_MAX : INT32_MIN); }
template<> inline int32_t saturate_cast<int32_t>(uint64_t v) { return static_cast<int32_t>(std::min<uint64_t>(v, INT32_MAX)); }
template<> inline int32_t saturate_cast<int32_t>(float v) { return roundTo<int32_t>(v); }
template<> inline int32_t saturate_cast<int32_t>(double v) { return roundTo<int32_t>(v); }
template<> inline int32_t saturate_cast<int32_t>(long double v) { return roundTo<int32_t>(v); }

template<> inline uint32_t saturate_cast<uint32_t>(int8_t v) { return static_cast<uint32_t>(std::max<int32_t>(v, 0)); }
template<> inline uint32_t saturate_cast<uint32_t>(int16_t v) { return static_cast<uint32_t>(std::max<int32_t>(v, 0)); }
template<> inline uint32_t saturate_cast<uint32_t>(int32_t v) { return static_cast<uint32_t>(std::max(v, 0)); }
template<> inline uint32_t saturate_cast<uint32_t>(int64_t v) { return static_cast<uint32_t>(static_cast<uint64_t>(v) <= static_cast<uint64_t>(UINT32_MAX) ? v : v > 0 ? UINT32_MAX : 0); }
template<> inline uint32_t saturate_cast<uint32_t>(uint64_t v) { return static_cast<uint32_t>(std::min<uint64_t>(v, UINT32_MAX)); }
template<> inline uint32_t saturate_cast<uint32_t>(float v) { return roundTo<uint32_t>(v); }
template<> inline uint32_t saturate_cast<uint32_t>(double v) { return roundTo<uint32_t>(v); }
template<> inline uint32_t saturate_cast<uint32_t>(long double v) { return roundTo<uint32_t>(v); }

template<> inline int64_t saturate_cast<int64_t>(uint64_t v) { return static_cast<int64_t>(std::min<uint64_t>(v, INT64_MAX)); }
template<> inline int64_t saturate_cast<int64_t>(float v) { return roundTo<int64_t>(v); }
template<> inline int64_t saturate_cast<int64_t>(double v) { return roundTo<int64_t>(v); }
template<> inline int64_t saturate_cast<int64_t>(long double v) { return roundTo<int64_t>(v); }

template<> inline uint64_t saturate_cast<uint64_t>(int8_t v) { return static_cast<uint64_t>(std::max<int32_t>(v, 0)); }
template<> inline uint64_t saturate_cast<uint64_t>(int16_t v) { return static_cast<uint64_t>(std::max<int32_t>(v, 0)); }
template<> inline uint64_t saturate_cast<uint64_t>(int32_t v) { return static_cast<uint64_t>(std::max(v, 0)); }
template<> inline uint64_t saturate_cast<uint64_t>(int64_t v) { return static_cast<uint64_t>(std::max<int64_t>(v, 0)); }
template<> inline uint64_t saturate_cast<uint64_t>(float v) { return roundTo<uint64_t>(v); }
template<> inline uint64_t saturate_cast<uint64_t>(double v) { return roundTo<uint64_t>(v); }
template<> inline uint64_t saturate_cast<uint64_t>(long double v) { return roundTo<uint64_t>(v); }

template<> inline float saturate_cast<float>(double v)
{
  if (v >= std::numeric_limits<float>::max())
    return std::numeric_limits<float>::max();
  else if (v <= std::numeric_limits<float>::lowest())
    return std::numeric_limits<float>::lowest();
  else
    return v;
}
template<> inline float saturate_cast<float>(long double v)
{
  if (v >= std::numeric_limits<float>::max())
    return std::numeric_limits<float>::max();
  else if (v <= std::numeric_limits<float>::lowest())
    return std::numeric_limits<float>::lowest();
  else
    return v;
}

template<> inline double saturate_cast<double>(long double v)
{
  if (v >= std::numeric_limits<double>::max())
    return std::numeric_limits<double>::max();
  else if (v <= std::numeric_limits<double>::lowest())
    return std::numeric_limits<double>::lowest();
  else
    return v;
}

// saturate arithmetics

inline uint8_t saturate_add(uint8_t x, uint8_t y)
{
  uint8_t res = x + y;
  return -(res < x) | res;
}

inline uint16_t saturate_add(uint16_t x, uint16_t y)
{
  uint16_t res = x + y;
  return -(res < x) | res;
}

inline uint32_t saturate_add(uint32_t x, uint32_t y)
{
  uint32_t res = x + y;
  return -(res < x) | res;
}

inline uint64_t saturate_add(uint64_t x, uint64_t y)
{
  uint64_t res = x + y;
  return -(res < x) | res;
}

inline int8_t saturate_add(int8_t x, int8_t y)
{
  int8_t res =  (x < 0) ? INT8_MIN : INT8_MAX;
  int8_t comp = res - x;
  if ((x < 0) == (y > comp))
    res = x + y;
  return res;
}

inline int16_t saturate_add(int16_t x, int16_t y)
{
  int16_t res =  (x < 0) ? INT16_MIN : INT16_MAX;
  int16_t comp = res - x;
  if ((x < 0) == (y > comp))
    res = x + y;
  return res;
}

inline int32_t saturate_add(int32_t x, int32_t y)
{
  int32_t res =  (x < 0) ? INT32_MIN : INT32_MAX;
  int32_t comp = res - x;
  if ((x < 0) == (y > comp))
    res = x + y;
  return res;
}

inline int64_t saturate_add(int64_t x, int64_t y)
{
  int64_t res =  (x < 0) ? INT64_MIN : INT64_MAX;
  int64_t comp = res - x;
  if ((x < 0) == (y > comp))
    res = x + y;
  return res;
}

inline uint64_t saturate_add(uint64_t x, int64_t y)
{
  if (y <= 0)
    return x > static_cast<uint64_t>(-y) ? x - static_cast<uint64_t>(-y) : 0;
  else
    return saturate_add(x, static_cast<uint64_t>(y));
}

inline int64_t saturate_add(int64_t x, uint64_t y)
{
  if (static_cast<uint64_t>(INT64_MAX - x) <= y) {
    return INT64_MAX;
  } else {
    if (y > INT64_MAX) { // x must less than zero
      return y - static_cast<uint64_t>(-x);
    } else {
      return x + static_cast<int64_t>(y);
    }
  }
}


inline uint8_t saturate_sub(uint8_t x, uint8_t y)
{
  uint8_t res = x - y;
  return -(y < x) & res;  // if y<x, return FF & res, else return 0 & res
}

inline uint16_t saturate_sub(uint16_t x, uint16_t y)
{
  uint16_t res = x - y;
  return -(y < x) & res;
}

inline uint32_t saturate_sub(uint32_t x, uint32_t y)
{
  uint32_t res = x - y;
  return -(y < x) & res;
}

inline uint64_t saturate_sub(uint64_t x, uint64_t y)
{
  uint64_t res = x - y;
  return -(y < x) & res;
}

inline int8_t saturate_sub(int8_t x, int8_t y)
{
  int8_t res =  (x < 0) ? INT8_MIN : INT8_MAX;
  int8_t comp = res - x; // comp can not be INT8_MIN so it is safe to negate it
  if ((x < 0) == (y < -comp))
    res = x - y;
  return res;
}

inline int16_t saturate_sub(int16_t x, int16_t y)
{
  int16_t res =  (x < 0) ? INT16_MIN : INT16_MAX;
  int16_t comp = res - x;
  if ((x < 0) == (y < -comp))
    res = x - y;
  return res;
}

inline int32_t saturate_sub(int32_t x, int32_t y)
{
  int32_t res =  (x < 0) ? INT32_MIN : INT32_MAX;
  int32_t comp = res - x;
  if ((x < 0) == (y < -comp))
    res = x - y;
  return res;
}

inline int64_t saturate_sub(int64_t x, int64_t y)
{
  int64_t res =  (x < 0) ? INT64_MIN : INT64_MAX;
  int64_t comp = res - x;
  if ((x < 0) == (y < -comp))
    res = x - y;
  return res;
}

inline uint64_t saturate_sub(uint64_t x, int64_t y)
{
  if (y < 0) {
    return saturate_add(x, static_cast<uint64_t>(-y));
  } else {
    return x > static_cast<uint64_t>(y) ? (x -  static_cast<uint64_t>(y)) : 0;
  }
}

inline int64_t saturate_sub(int64_t x, uint64_t y)
{
  if (static_cast<uint64_t>(x - INT64_MIN) <= y) {
    return INT64_MIN;
  } else {
    if (y > INT64_MAX) { // x must large than zero
      return static_cast<uint64_t>(x) - y;
    } else {
      return x - static_cast<int64_t>(y);
    }
  }
}


inline uint8_t saturate_mul(uint8_t x, uint8_t y)
{
  uint32_t res = static_cast<uint32_t>(x) * static_cast<uint32_t>(y);
  return (-!!(res >> 8)) | static_cast<uint8_t>(res);
}

inline uint16_t saturate_mul(uint16_t x, uint16_t y)
{
  uint32_t res = static_cast<uint32_t>(x) * static_cast<uint32_t>(y);
  return (-!!(res >> 16)) | static_cast<uint16_t>(res);
}

inline uint32_t saturate_mul(uint32_t x, uint32_t y)
{
  uint64_t res = static_cast<uint64_t>(x) * static_cast<uint64_t>(y);
  return (-!!(res >> 32)) | static_cast<uint32_t>(res);
}

inline uint64_t saturate_mul(uint64_t x, uint64_t y)
{
  if (x == 0 || y == 0)
    return 0;
  else if (UINT64_MAX / x < y)
    return UINT64_MAX;
  else
    return x * y;
}

inline int8_t saturate_mul(int8_t x, int8_t y)
{
  // for this to work, compiler need to do arithmetic right shift
  static_assert((static_cast<int16_t>(-1) >> 1) == static_cast<int16_t>(-1), "Arithmetic shift unsupported.");
  static_assert((static_cast<int8_t>(-1) >> 1) == static_cast<int8_t>(-1), "Arithmetic shift unsupported.");

  int16_t res = static_cast<int16_t>(x) * static_cast<int16_t>(y);
  if (static_cast<int8_t>(res >> 8) != (static_cast<int8_t>(res) >> 7))
    res = (static_cast<uint8_t>(x ^ y) >> 7) + INT8_MAX;
  return res;
}

inline int16_t saturate_mul(int16_t x, int16_t y)
{
  static_assert((static_cast<int16_t>(-1) >> 1) == static_cast<int16_t>(-1), "arithmetic shift unsupported.");
  static_assert((static_cast<int32_t>(-1) >> 1) == static_cast<int32_t>(-1), "arithmetic shift unsupported.");

  int32_t res = static_cast<int32_t>(x) * static_cast<int32_t>(y);
  if (static_cast<int16_t>(res >> 16) != (static_cast<int16_t>(res) >> 15))
    res = (static_cast<uint16_t>(x ^ y) >> 15) + INT16_MAX;
  return res;
}

inline int32_t saturate_mul(int32_t x, int32_t y)
{
  static_assert((static_cast<int64_t>(-1) >> 1) == static_cast<int64_t>(-1), "arithmetic shift unsupported.");
  static_assert((static_cast<int32_t>(-1) >> 1) == static_cast<int32_t>(-1), "arithmetic shift unsupported.");

  int64_t res = static_cast<int64_t>(x) * static_cast<int64_t>(y);
  if (static_cast<int32_t>(res >> 32) != (static_cast<int32_t>(res) >> 31))
    res = (static_cast<uint32_t>(x ^ y) >> 31) + INT32_MAX;
  return res;
}

inline int64_t saturate_mul(int64_t x, int64_t y)
{
#if 0
  using namespace boost::multiprecision;

  int128_t res = static_cast<int128_t>(x) * static_cast<int128_t>(y);
  return res < static_cast<int128_t>(INT64_MIN) ? INT64_MIN :
                                                  res > static_cast<int128_t>(INT64_MAX) ? INT64_MAX :
                                                                                           static_cast<int64_t>(res);
#else
  static_assert(-std::numeric_limits<int64_t>::max() > std::numeric_limits<int64_t>::min(),
                "integer representation is not two's complement");
  if (x == 0 || y == 0) {
    return 0;
  } else if (x > 0 && y < 0) {
    return -static_cast<int64_t>(std::min(static_cast<uint64_t>(INT64_MAX)+1, saturate_mul(static_cast<uint64_t>(x), static_cast<uint64_t>(-y))));
  } else if (x < 0 && y > 0) {
    return -static_cast<int64_t>(std::min(static_cast<uint64_t>(INT64_MAX)+1, saturate_mul(static_cast<uint64_t>(-x), static_cast<uint64_t>(y))));
  } else if (x > 0) {
    return std::min<uint64_t>(INT64_MAX, saturate_mul(static_cast<uint64_t>(x), static_cast<uint64_t>(y)));
  } else {
    return std::min<uint64_t>(INT64_MAX, saturate_mul(static_cast<uint64_t>(-x), static_cast<uint64_t>(-y)));
  }
#endif
}

inline uint64_t saturate_mul(uint64_t x, int64_t y)
{
  return y <= 0 ? 0 : saturate_mul(x, static_cast<uint64_t>(y));
}

inline int64_t saturate_mul(int64_t x, uint64_t y)
{
  if (x >= 0) {
    return std::min<uint64_t>(INT64_MAX, saturate_mul(static_cast<uint64_t>(x), y));
  } else {
    return -static_cast<int64_t>(std::min(static_cast<uint64_t>(INT64_MAX)+1, saturate_mul(static_cast<uint64_t>(-x), y)));
  }
}

inline uint8_t saturate_div(uint8_t x, uint8_t y)
{
  if (y == 0)
    return x > 0 ? UINT8_MAX : 0;
  return x / y;
}

inline uint16_t saturate_div(uint16_t x, uint16_t y)
{
  if (y == 0)
    return x > 0 ? UINT16_MAX : 0;
  return x / y;
}

inline uint32_t saturate_div(uint32_t x, uint32_t y)
{
  if (y == 0)
    return x > 0 ? UINT32_MAX : 0;
  return x / y;
}

inline uint64_t saturate_div(uint64_t x, uint64_t y)
{
  if (y == 0)
    return x > 0 ? UINT64_MAX : 0;
  return x / y;
}

inline int8_t saturate_div(int8_t x, int8_t y)
{
  if (y == 0)
    return x > 0 ? INT8_MAX : x < 0 ? INT8_MIN : 0;
  // if y is -1 then x can not be INT_MIN, x need to be INT_MIN+1 so -x is INT_MAX
  x += (y == -1 && x == INT8_MIN);
  return x / y;
}

inline int16_t saturate_div(int16_t x, int16_t y)
{
  if (y == 0)
    return x > 0 ? INT16_MAX : x < 0 ? INT16_MIN : 0;
  x += (y == -1 && x == INT16_MIN);
  return x / y;
}

inline int32_t saturate_div(int32_t x, int32_t y)
{
  if (y == 0)
    return x > 0 ? INT32_MAX : x < 0 ? INT32_MIN : 0;
  x += (y == -1 && x == INT32_MIN);
  return x / y;
}

inline int64_t saturate_div(int64_t x, int64_t y)
{
  if (y == 0)
    return x > 0 ? INT64_MAX : x < 0 ? INT64_MIN : 0;
  x += (y == -1 && x == INT64_MIN);
  return x / y;
}

inline uint64_t saturate_div(uint64_t x, int64_t y)
{
  if (y == 0)
    return x > 0 ? UINT64_MAX : 0;
  return y < 0 ? 0 : x / static_cast<uint64_t>(y);
}

inline int64_t saturate_div(int64_t x, uint64_t y)
{
  if (y == 0)
    return x > 0 ? INT64_MAX : x < 0 ? INT64_MIN : 0;
  if (x >= 0) {
    return static_cast<uint64_t>(x) / y;
  } else {
    return -static_cast<int64_t>(static_cast<uint64_t>(-x) / y);
  }
}


inline uint64_t saturate_add(uint64_t x, int8_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, int16_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, int32_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, uint8_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, uint16_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, uint32_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_add(uint64_t x, float y) { return saturate_cast<uint64_t>(x+y); }
inline uint64_t saturate_add(uint64_t x, double y) { return saturate_cast<uint64_t>(x+y); }

inline int64_t saturate_add(int64_t x, int8_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline int64_t saturate_add(int64_t x, int16_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline int64_t saturate_add(int64_t x, int32_t y) { return saturate_add(x, static_cast<int64_t>(y)); }
inline int64_t saturate_add(int64_t x, uint8_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_add(int64_t x, uint16_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_add(int64_t x, uint32_t y) { return saturate_add(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_add(int64_t x, float y) { return saturate_cast<int64_t>(x+y); }
inline int64_t saturate_add(int64_t x, double y) { return saturate_cast<int64_t>(x+y); }

inline uint32_t saturate_add(uint32_t x, int8_t y) { return saturate_cast<uint32_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_add(uint32_t x, int16_t y) { return saturate_cast<uint32_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_add(uint32_t x, int32_t y) { return saturate_cast<uint32_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_add(uint32_t x, int64_t y) { return saturate_cast<uint32_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline uint32_t saturate_add(uint32_t x, uint8_t y) { return saturate_add(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_add(uint32_t x, uint16_t y) { return saturate_add(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_add(uint32_t x, uint64_t y) { return saturate_cast<uint32_t>(saturate_add(static_cast<uint64_t>(x), y)); }
inline uint32_t saturate_add(uint32_t x, float y) { return saturate_cast<uint32_t>(x+y); }
inline uint32_t saturate_add(uint32_t x, double y) { return saturate_cast<uint32_t>(x+y); }

inline int32_t saturate_add(int32_t x, int8_t y) { return saturate_add(x, static_cast<int32_t>(y)); }
inline int32_t saturate_add(int32_t x, int16_t y) { return saturate_add(x, static_cast<int32_t>(y)); }
inline int32_t saturate_add(int32_t x, int64_t y) { return saturate_cast<int32_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int32_t saturate_add(int32_t x, uint8_t y) { return saturate_add(x, static_cast<int32_t>(y)); }
inline int32_t saturate_add(int32_t x, uint16_t y) { return saturate_add(x, static_cast<int32_t>(y)); }
inline int32_t saturate_add(int32_t x, uint32_t y) { return saturate_cast<int32_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int32_t saturate_add(int32_t x, uint64_t y) { return saturate_cast<int32_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int32_t saturate_add(int32_t x, float y) { return saturate_cast<int32_t>(x+y); }
inline int32_t saturate_add(int32_t x, double y) { return saturate_cast<int32_t>(x+y); }

inline uint16_t saturate_add(uint16_t x, int8_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_add(uint16_t x, int16_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_add(uint16_t x, int32_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<int32_t>(x), y)); }
inline uint16_t saturate_add(uint16_t x, int64_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline uint16_t saturate_add(uint16_t x, uint8_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint16_t saturate_add(uint16_t x, uint32_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<uint32_t>(x), y)); }
inline uint16_t saturate_add(uint16_t x, uint64_t y) { return saturate_cast<uint16_t>(saturate_add(static_cast<uint64_t>(x), y)); }
inline uint16_t saturate_add(uint16_t x, float y) { return saturate_cast<uint16_t>(x+y); }
inline uint16_t saturate_add(uint16_t x, double y) { return saturate_cast<uint16_t>(x+y); }

inline int16_t saturate_add(int16_t x, int8_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_add(int16_t x, int32_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int32_t>(x), y)); }
inline int16_t saturate_add(int16_t x, int64_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int16_t saturate_add(int16_t x, uint8_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_add(int16_t x, uint16_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_add(int16_t x, uint32_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int16_t saturate_add(int16_t x, uint64_t y) { return saturate_cast<int16_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int16_t saturate_add(int16_t x, float y) { return saturate_cast<int16_t>(x+y); }
inline int16_t saturate_add(int16_t x, double y) { return saturate_cast<int16_t>(x+y); }

inline uint8_t saturate_add(uint8_t x, int8_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_add(uint8_t x, int16_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_add(uint8_t x, int32_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<int32_t>(x), y)); }
inline uint8_t saturate_add(uint8_t x, int64_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline uint8_t saturate_add(uint8_t x, uint16_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint8_t saturate_add(uint8_t x, uint32_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<uint32_t>(x), y)); }
inline uint8_t saturate_add(uint8_t x, uint64_t y) { return saturate_cast<uint8_t>(saturate_add(static_cast<uint64_t>(x), y)); }
inline uint8_t saturate_add(uint8_t x, float y) { return saturate_cast<uint8_t>(x+y); }
inline uint8_t saturate_add(uint8_t x, double y) { return saturate_cast<uint8_t>(x+y); }

inline int8_t saturate_add(int8_t x, int16_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_add(int8_t x, int32_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int32_t>(x), y)); }
inline int8_t saturate_add(int8_t x, int64_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int8_t saturate_add(int8_t x, uint8_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_add(int8_t x, uint16_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_add(int8_t x, uint32_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int8_t saturate_add(int8_t x, uint64_t y) { return saturate_cast<int8_t>(saturate_add(static_cast<int64_t>(x), y)); }
inline int8_t saturate_add(int8_t x, float y) { return saturate_cast<int8_t>(x+y); }
inline int8_t saturate_add(int8_t x, double y) { return saturate_cast<int8_t>(x+y); }

inline float saturate_add(float x, int8_t y) { return x+y; }
inline float saturate_add(float x, int16_t y) { return x+y; }
inline float saturate_add(float x, int32_t y) { return x+y; }
inline float saturate_add(float x, int64_t y) { return x+y; }
inline float saturate_add(float x, uint8_t y) { return x+y; }
inline float saturate_add(float x, uint16_t y) { return x+y; }
inline float saturate_add(float x, uint32_t y) { return x+y; }
inline float saturate_add(float x, uint64_t y) { return x+y; }
inline float saturate_add(float x, float y) { return x+y; }
inline float saturate_add(float x, double y) { return x+y; }

inline double saturate_add(double x, int8_t y) { return x+y; }
inline double saturate_add(double x, int16_t y) { return x+y; }
inline double saturate_add(double x, int32_t y) { return x+y; }
inline double saturate_add(double x, int64_t y) { return x+y; }
inline double saturate_add(double x, uint8_t y) { return x+y; }
inline double saturate_add(double x, uint16_t y) { return x+y; }
inline double saturate_add(double x, uint32_t y) { return x+y; }
inline double saturate_add(double x, uint64_t y) { return x+y; }
inline double saturate_add(double x, float y) { return x+y; }
inline double saturate_add(double x, double y) { return x+y; }

// sub
inline uint64_t saturate_sub(uint64_t x, int8_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, int16_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, int32_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, uint8_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, uint16_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, uint32_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_sub(uint64_t x, float y) { return saturate_cast<uint64_t>(x-y); }
inline uint64_t saturate_sub(uint64_t x, double y) { return saturate_cast<uint64_t>(x-y); }

inline int64_t saturate_sub(int64_t x, int8_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline int64_t saturate_sub(int64_t x, int16_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline int64_t saturate_sub(int64_t x, int32_t y) { return saturate_sub(x, static_cast<int64_t>(y)); }
inline int64_t saturate_sub(int64_t x, uint8_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_sub(int64_t x, uint16_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_sub(int64_t x, uint32_t y) { return saturate_sub(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_sub(int64_t x, float y) { return saturate_cast<int64_t>(x-y); }
inline int64_t saturate_sub(int64_t x, double y) { return saturate_cast<int64_t>(x-y); }

inline uint32_t saturate_sub(uint32_t x, int8_t y) { return saturate_cast<uint32_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_sub(uint32_t x, int16_t y) { return saturate_cast<uint32_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_sub(uint32_t x, int32_t y) { return saturate_cast<uint32_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_sub(uint32_t x, int64_t y) { return saturate_cast<uint32_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline uint32_t saturate_sub(uint32_t x, uint8_t y) { return saturate_sub(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_sub(uint32_t x, uint16_t y) { return saturate_sub(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_sub(uint32_t x, uint64_t y) { return saturate_cast<uint32_t>(saturate_sub(static_cast<uint64_t>(x), y)); }
inline uint32_t saturate_sub(uint32_t x, float y) { return saturate_cast<uint32_t>(x-y); }
inline uint32_t saturate_sub(uint32_t x, double y) { return saturate_cast<uint32_t>(x-y); }

inline int32_t saturate_sub(int32_t x, int8_t y) { return saturate_sub(x, static_cast<int32_t>(y)); }
inline int32_t saturate_sub(int32_t x, int16_t y) { return saturate_sub(x, static_cast<int32_t>(y)); }
inline int32_t saturate_sub(int32_t x, int64_t y) { return saturate_cast<int32_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int32_t saturate_sub(int32_t x, uint8_t y) { return saturate_sub(x, static_cast<int32_t>(y)); }
inline int32_t saturate_sub(int32_t x, uint16_t y) { return saturate_sub(x, static_cast<int32_t>(y)); }
inline int32_t saturate_sub(int32_t x, uint32_t y) { return saturate_cast<int32_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int32_t saturate_sub(int32_t x, uint64_t y) { return saturate_cast<int32_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int32_t saturate_sub(int32_t x, float y) { return saturate_cast<int32_t>(x-y); }
inline int32_t saturate_sub(int32_t x, double y) { return saturate_cast<int32_t>(x-y); }

inline uint16_t saturate_sub(uint16_t x, int8_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_sub(uint16_t x, int16_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_sub(uint16_t x, int32_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<int32_t>(x), y)); }
inline uint16_t saturate_sub(uint16_t x, int64_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline uint16_t saturate_sub(uint16_t x, uint8_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint16_t saturate_sub(uint16_t x, uint32_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<uint32_t>(x), y)); }
inline uint16_t saturate_sub(uint16_t x, uint64_t y) { return saturate_cast<uint16_t>(saturate_sub(static_cast<uint64_t>(x), y)); }
inline uint16_t saturate_sub(uint16_t x, float y) { return saturate_cast<uint16_t>(x-y); }
inline uint16_t saturate_sub(uint16_t x, double y) { return saturate_cast<uint16_t>(x-y); }

inline int16_t saturate_sub(int16_t x, int8_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_sub(int16_t x, int32_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int32_t>(x), y)); }
inline int16_t saturate_sub(int16_t x, int64_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int16_t saturate_sub(int16_t x, uint8_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_sub(int16_t x, uint16_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_sub(int16_t x, uint32_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int16_t saturate_sub(int16_t x, uint64_t y) { return saturate_cast<int16_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int16_t saturate_sub(int16_t x, float y) { return saturate_cast<int16_t>(x-y); }
inline int16_t saturate_sub(int16_t x, double y) { return saturate_cast<int16_t>(x-y); }

inline uint8_t saturate_sub(uint8_t x, int8_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_sub(uint8_t x, int16_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_sub(uint8_t x, int32_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<int32_t>(x), y)); }
inline uint8_t saturate_sub(uint8_t x, int64_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline uint8_t saturate_sub(uint8_t x, uint16_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint8_t saturate_sub(uint8_t x, uint32_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<uint32_t>(x), y)); }
inline uint8_t saturate_sub(uint8_t x, uint64_t y) { return saturate_cast<uint8_t>(saturate_sub(static_cast<uint64_t>(x), y)); }
inline uint8_t saturate_sub(uint8_t x, float y) { return saturate_cast<uint8_t>(x-y); }
inline uint8_t saturate_sub(uint8_t x, double y) { return saturate_cast<uint8_t>(x-y); }

inline int8_t saturate_sub(int8_t x, int16_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_sub(int8_t x, int32_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int32_t>(x), y)); }
inline int8_t saturate_sub(int8_t x, int64_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int8_t saturate_sub(int8_t x, uint8_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_sub(int8_t x, uint16_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_sub(int8_t x, uint32_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int8_t saturate_sub(int8_t x, uint64_t y) { return saturate_cast<int8_t>(saturate_sub(static_cast<int64_t>(x), y)); }
inline int8_t saturate_sub(int8_t x, float y) { return saturate_cast<int8_t>(x-y); }
inline int8_t saturate_sub(int8_t x, double y) { return saturate_cast<int8_t>(x-y); }

inline float saturate_sub(float x, int8_t y) { return x-y; }
inline float saturate_sub(float x, int16_t y) { return x-y; }
inline float saturate_sub(float x, int32_t y) { return x-y; }
inline float saturate_sub(float x, int64_t y) { return x-y; }
inline float saturate_sub(float x, uint8_t y) { return x-y; }
inline float saturate_sub(float x, uint16_t y) { return x-y; }
inline float saturate_sub(float x, uint32_t y) { return x-y; }
inline float saturate_sub(float x, uint64_t y) { return x-y; }
inline float saturate_sub(float x, float y) { return x-y; }
inline float saturate_sub(float x, double y) { return x-y; }

inline double saturate_sub(double x, int8_t y) { return x-y; }
inline double saturate_sub(double x, int16_t y) { return x-y; }
inline double saturate_sub(double x, int32_t y) { return x-y; }
inline double saturate_sub(double x, int64_t y) { return x-y; }
inline double saturate_sub(double x, uint8_t y) { return x-y; }
inline double saturate_sub(double x, uint16_t y) { return x-y; }
inline double saturate_sub(double x, uint32_t y) { return x-y; }
inline double saturate_sub(double x, uint64_t y) { return x-y; }
inline double saturate_sub(double x, float y) { return x-y; }
inline double saturate_sub(double x, double y) { return x-y; }

// mul
inline uint64_t saturate_mul(uint64_t x, int8_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, int16_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, int32_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, uint8_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, uint16_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, uint32_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_mul(uint64_t x, float y) { return saturate_cast<uint64_t>(x*y); }
inline uint64_t saturate_mul(uint64_t x, double y) { return saturate_cast<uint64_t>(x*y); }

inline int64_t saturate_mul(int64_t x, int8_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline int64_t saturate_mul(int64_t x, int16_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline int64_t saturate_mul(int64_t x, int32_t y) { return saturate_mul(x, static_cast<int64_t>(y)); }
inline int64_t saturate_mul(int64_t x, uint8_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_mul(int64_t x, uint16_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_mul(int64_t x, uint32_t y) { return saturate_mul(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_mul(int64_t x, float y) { return saturate_cast<int64_t>(x*y); }
inline int64_t saturate_mul(int64_t x, double y) { return saturate_cast<int64_t>(x*y); }

inline uint32_t saturate_mul(uint32_t x, int8_t y) { return saturate_cast<uint32_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_mul(uint32_t x, int16_t y) { return saturate_cast<uint32_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_mul(uint32_t x, int32_t y) { return saturate_cast<uint32_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_mul(uint32_t x, int64_t y) { return saturate_cast<uint32_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline uint32_t saturate_mul(uint32_t x, uint8_t y) { return saturate_mul(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_mul(uint32_t x, uint16_t y) { return saturate_mul(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_mul(uint32_t x, uint64_t y) { return saturate_cast<uint32_t>(saturate_mul(static_cast<uint64_t>(x), y)); }
inline uint32_t saturate_mul(uint32_t x, float y) { return saturate_cast<uint32_t>(x*y); }
inline uint32_t saturate_mul(uint32_t x, double y) { return saturate_cast<uint32_t>(x*y); }

inline int32_t saturate_mul(int32_t x, int8_t y) { return saturate_mul(x, static_cast<int32_t>(y)); }
inline int32_t saturate_mul(int32_t x, int16_t y) { return saturate_mul(x, static_cast<int32_t>(y)); }
inline int32_t saturate_mul(int32_t x, int64_t y) { return saturate_cast<int32_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int32_t saturate_mul(int32_t x, uint8_t y) { return saturate_mul(x, static_cast<int32_t>(y)); }
inline int32_t saturate_mul(int32_t x, uint16_t y) { return saturate_mul(x, static_cast<int32_t>(y)); }
inline int32_t saturate_mul(int32_t x, uint32_t y) { return saturate_cast<int32_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int32_t saturate_mul(int32_t x, uint64_t y) { return saturate_cast<int32_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int32_t saturate_mul(int32_t x, float y) { return saturate_cast<int32_t>(x*y); }
inline int32_t saturate_mul(int32_t x, double y) { return saturate_cast<int32_t>(x*y); }

inline uint16_t saturate_mul(uint16_t x, int8_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_mul(uint16_t x, int16_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_mul(uint16_t x, int32_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<int32_t>(x), y)); }
inline uint16_t saturate_mul(uint16_t x, int64_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline uint16_t saturate_mul(uint16_t x, uint8_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint16_t saturate_mul(uint16_t x, uint32_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<uint32_t>(x), y)); }
inline uint16_t saturate_mul(uint16_t x, uint64_t y) { return saturate_cast<uint16_t>(saturate_mul(static_cast<uint64_t>(x), y)); }
inline uint16_t saturate_mul(uint16_t x, float y) { return saturate_cast<uint16_t>(x*y); }
inline uint16_t saturate_mul(uint16_t x, double y) { return saturate_cast<uint16_t>(x*y); }

inline int16_t saturate_mul(int16_t x, int8_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_mul(int16_t x, int32_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int32_t>(x), y)); }
inline int16_t saturate_mul(int16_t x, int64_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int16_t saturate_mul(int16_t x, uint8_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_mul(int16_t x, uint16_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_mul(int16_t x, uint32_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int16_t saturate_mul(int16_t x, uint64_t y) { return saturate_cast<int16_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int16_t saturate_mul(int16_t x, float y) { return saturate_cast<int16_t>(x*y); }
inline int16_t saturate_mul(int16_t x, double y) { return saturate_cast<int16_t>(x*y); }

inline uint8_t saturate_mul(uint8_t x, int8_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_mul(uint8_t x, int16_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_mul(uint8_t x, int32_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<int32_t>(x), y)); }
inline uint8_t saturate_mul(uint8_t x, int64_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline uint8_t saturate_mul(uint8_t x, uint16_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint8_t saturate_mul(uint8_t x, uint32_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<uint32_t>(x), y)); }
inline uint8_t saturate_mul(uint8_t x, uint64_t y) { return saturate_cast<uint8_t>(saturate_mul(static_cast<uint64_t>(x), y)); }
inline uint8_t saturate_mul(uint8_t x, float y) { return saturate_cast<uint8_t>(x*y); }
inline uint8_t saturate_mul(uint8_t x, double y) { return saturate_cast<uint8_t>(x*y); }

inline int8_t saturate_mul(int8_t x, int16_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_mul(int8_t x, int32_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int32_t>(x), y)); }
inline int8_t saturate_mul(int8_t x, int64_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int8_t saturate_mul(int8_t x, uint8_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_mul(int8_t x, uint16_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_mul(int8_t x, uint32_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int8_t saturate_mul(int8_t x, uint64_t y) { return saturate_cast<int8_t>(saturate_mul(static_cast<int64_t>(x), y)); }
inline int8_t saturate_mul(int8_t x, float y) { return saturate_cast<int8_t>(x*y); }
inline int8_t saturate_mul(int8_t x, double y) { return saturate_cast<int8_t>(x*y); }

inline float saturate_mul(float x, int8_t y) { return x*y; }
inline float saturate_mul(float x, int16_t y) { return x*y; }
inline float saturate_mul(float x, int32_t y) { return x*y; }
inline float saturate_mul(float x, int64_t y) { return x*y; }
inline float saturate_mul(float x, uint8_t y) { return x*y; }
inline float saturate_mul(float x, uint16_t y) { return x*y; }
inline float saturate_mul(float x, uint32_t y) { return x*y; }
inline float saturate_mul(float x, uint64_t y) { return x*y; }
inline float saturate_mul(float x, float y) { return x*y; }
inline float saturate_mul(float x, double y) { return x*y; }

inline double saturate_mul(double x, int8_t y) { return x*y; }
inline double saturate_mul(double x, int16_t y) { return x*y; }
inline double saturate_mul(double x, int32_t y) { return x*y; }
inline double saturate_mul(double x, int64_t y) { return x*y; }
inline double saturate_mul(double x, uint8_t y) { return x*y; }
inline double saturate_mul(double x, uint16_t y) { return x*y; }
inline double saturate_mul(double x, uint32_t y) { return x*y; }
inline double saturate_mul(double x, uint64_t y) { return x*y; }
inline double saturate_mul(double x, float y) { return x*y; }
inline double saturate_mul(double x, double y) { return x*y; }

// div
inline uint64_t saturate_div(uint64_t x, int8_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, int16_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, int32_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, uint8_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, uint16_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, uint32_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline uint64_t saturate_div(uint64_t x, float y) { return saturate_cast<uint64_t>(x/y); }
inline uint64_t saturate_div(uint64_t x, double y) { return saturate_cast<uint64_t>(x/y); }

inline int64_t saturate_div(int64_t x, int8_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline int64_t saturate_div(int64_t x, int16_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline int64_t saturate_div(int64_t x, int32_t y) { return saturate_div(x, static_cast<int64_t>(y)); }
inline int64_t saturate_div(int64_t x, uint8_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_div(int64_t x, uint16_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_div(int64_t x, uint32_t y) { return saturate_div(x, static_cast<uint64_t>(y)); }
inline int64_t saturate_div(int64_t x, float y) { return saturate_cast<int64_t>(x/y); }
inline int64_t saturate_div(int64_t x, double y) { return saturate_cast<int64_t>(x/y); }

inline uint32_t saturate_div(uint32_t x, int8_t y) { return saturate_cast<uint32_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_div(uint32_t x, int16_t y) { return saturate_cast<uint32_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_div(uint32_t x, int32_t y) { return saturate_cast<uint32_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline uint32_t saturate_div(uint32_t x, int64_t y) { return saturate_cast<uint32_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline uint32_t saturate_div(uint32_t x, uint8_t y) { return saturate_div(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_div(uint32_t x, uint16_t y) { return saturate_div(x, static_cast<uint32_t>(y)); }
inline uint32_t saturate_div(uint32_t x, uint64_t y) { return saturate_cast<uint32_t>(saturate_div(static_cast<uint64_t>(x), y)); }
inline uint32_t saturate_div(uint32_t x, float y) { return saturate_cast<uint32_t>(x/y); }
inline uint32_t saturate_div(uint32_t x, double y) { return saturate_cast<uint32_t>(x/y); }

inline int32_t saturate_div(int32_t x, int8_t y) { return saturate_div(x, static_cast<int32_t>(y)); }
inline int32_t saturate_div(int32_t x, int16_t y) { return saturate_div(x, static_cast<int32_t>(y)); }
inline int32_t saturate_div(int32_t x, int64_t y) { return saturate_cast<int32_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int32_t saturate_div(int32_t x, uint8_t y) { return saturate_div(x, static_cast<int32_t>(y)); }
inline int32_t saturate_div(int32_t x, uint16_t y) { return saturate_div(x, static_cast<int32_t>(y)); }
inline int32_t saturate_div(int32_t x, uint32_t y) { return saturate_cast<int32_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int32_t saturate_div(int32_t x, uint64_t y) { return saturate_cast<int32_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int32_t saturate_div(int32_t x, float y) { return saturate_cast<int32_t>(x/y); }
inline int32_t saturate_div(int32_t x, double y) { return saturate_cast<int32_t>(x/y); }

inline uint16_t saturate_div(uint16_t x, int8_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_div(uint16_t x, int16_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint16_t saturate_div(uint16_t x, int32_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<int32_t>(x), y)); }
inline uint16_t saturate_div(uint16_t x, int64_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline uint16_t saturate_div(uint16_t x, uint8_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint16_t saturate_div(uint16_t x, uint32_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<uint32_t>(x), y)); }
inline uint16_t saturate_div(uint16_t x, uint64_t y) { return saturate_cast<uint16_t>(saturate_div(static_cast<uint64_t>(x), y)); }
inline uint16_t saturate_div(uint16_t x, float y) { return saturate_cast<uint16_t>(x/y); }
inline uint16_t saturate_div(uint16_t x, double y) { return saturate_cast<uint16_t>(x/y); }

inline int16_t saturate_div(int16_t x, int8_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_div(int16_t x, int32_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int32_t>(x), y)); }
inline int16_t saturate_div(int16_t x, int64_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int16_t saturate_div(int16_t x, uint8_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_div(int16_t x, uint16_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int16_t saturate_div(int16_t x, uint32_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int16_t saturate_div(int16_t x, uint64_t y) { return saturate_cast<int16_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int16_t saturate_div(int16_t x, float y) { return saturate_cast<int16_t>(x/y); }
inline int16_t saturate_div(int16_t x, double y) { return saturate_cast<int16_t>(x/y); }

inline uint8_t saturate_div(uint8_t x, int8_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_div(uint8_t x, int16_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline uint8_t saturate_div(uint8_t x, int32_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<int32_t>(x), y)); }
inline uint8_t saturate_div(uint8_t x, int64_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline uint8_t saturate_div(uint8_t x, uint16_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<uint32_t>(x), static_cast<uint32_t>(y))); }
inline uint8_t saturate_div(uint8_t x, uint32_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<uint32_t>(x), y)); }
inline uint8_t saturate_div(uint8_t x, uint64_t y) { return saturate_cast<uint8_t>(saturate_div(static_cast<uint64_t>(x), y)); }
inline uint8_t saturate_div(uint8_t x, float y) { return saturate_cast<uint8_t>(x/y); }
inline uint8_t saturate_div(uint8_t x, double y) { return saturate_cast<uint8_t>(x/y); }

inline int8_t saturate_div(int8_t x, int16_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_div(int8_t x, int32_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int32_t>(x), y)); }
inline int8_t saturate_div(int8_t x, int64_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int8_t saturate_div(int8_t x, uint8_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_div(int8_t x, uint16_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int32_t>(x), static_cast<int32_t>(y))); }
inline int8_t saturate_div(int8_t x, uint32_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int64_t>(x), static_cast<int64_t>(y))); }
inline int8_t saturate_div(int8_t x, uint64_t y) { return saturate_cast<int8_t>(saturate_div(static_cast<int64_t>(x), y)); }
inline int8_t saturate_div(int8_t x, float y) { return saturate_cast<int8_t>(x/y); }
inline int8_t saturate_div(int8_t x, double y) { return saturate_cast<int8_t>(x/y); }

inline float saturate_div(float x, int8_t y) { return x/y; }
inline float saturate_div(float x, int16_t y) { return x/y; }
inline float saturate_div(float x, int32_t y) { return x/y; }
inline float saturate_div(float x, int64_t y) { return x/y; }
inline float saturate_div(float x, uint8_t y) { return x/y; }
inline float saturate_div(float x, uint16_t y) { return x/y; }
inline float saturate_div(float x, uint32_t y) { return x/y; }
inline float saturate_div(float x, uint64_t y) { return x/y; }
inline float saturate_div(float x, float y) { return x/y; }
inline float saturate_div(float x, double y) { return x/y; }

inline double saturate_div(double x, int8_t y) { return x/y; }
inline double saturate_div(double x, int16_t y) { return x/y; }
inline double saturate_div(double x, int32_t y) { return x/y; }
inline double saturate_div(double x, int64_t y) { return x/y; }
inline double saturate_div(double x, uint8_t y) { return x/y; }
inline double saturate_div(double x, uint16_t y) { return x/y; }
inline double saturate_div(double x, uint32_t y) { return x/y; }
inline double saturate_div(double x, uint64_t y) { return x/y; }
inline double saturate_div(double x, float y) { return x/y; }
inline double saturate_div(double x, double y) { return x/y; }

// array version add
template<typename TVoxel1, typename TVoxel2>
inline void saturate_add(const TVoxel1* x, TVoxel2* y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

template<typename TVoxel1, typename TVoxel2>
inline void saturate_add(const TVoxel1* x, TVoxel2 y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

template<>
inline void saturate_add<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

template<>
inline void saturate_add<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  __m128i r = _mm_set1_epi8(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

template<>
inline void saturate_add<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

template<>
inline void saturate_add<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  __m128i r = _mm_set1_epi8(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

template<>
inline void saturate_add<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  size_t i;
  // process 8 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

template<>
inline void saturate_add<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  __m128i r = _mm_set1_epi16(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epu16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

template<>
inline void saturate_add<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  size_t i;
  // process 8 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y[i]);
  }
}

template<>
inline void saturate_add<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  __m128i r = _mm_set1_epi16(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_adds_epi16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_add(x[i], y);
  }
}

// array version sub

template<typename TVoxel1, typename TVoxel2>
inline void saturate_sub(const TVoxel1* x, TVoxel2* y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

template<typename TVoxel1, typename TVoxel2>
inline void saturate_sub(const TVoxel1* x, TVoxel2 y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

template<>
inline void saturate_sub<uint8_t, const uint8_t>(const uint8_t* x, const uint8_t* y, size_t count, uint8_t* res)
{
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

template<>
inline void saturate_sub<uint8_t, uint8_t>(const uint8_t* x, uint8_t y, size_t count, uint8_t* res)
{
  __m128i r = _mm_set1_epi8(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

template<>
inline void saturate_sub<int8_t, const int8_t>(const int8_t* x, const int8_t* y, size_t count, int8_t* res)
{
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

template<>
inline void saturate_sub<int8_t, int8_t>(const int8_t* x, int8_t y, size_t count, int8_t* res)
{
  __m128i r = _mm_set1_epi8(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 15; i += 16) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi8(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

template<>
inline void saturate_sub<uint16_t, const uint16_t>(const uint16_t* x, const uint16_t* y, size_t count, uint16_t* res)
{
  size_t i;
  // process 8 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

template<>
inline void saturate_sub<uint16_t, uint16_t>(const uint16_t* x, uint16_t y, size_t count, uint16_t* res)
{
  __m128i r = _mm_set1_epi16(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epu16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

template<>
inline void saturate_sub<int16_t, const int16_t>(const int16_t* x, const int16_t* y, size_t count, int16_t* res)
{
  size_t i;
  // process 8 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y[i]);
  }
}

template<>
inline void saturate_sub<int16_t, int16_t>(const int16_t* x, int16_t y, size_t count, int16_t* res)
{
  __m128i r = _mm_set1_epi16(y);
  size_t i;
  // process 16 elements per iteration
  for (i = 0; i < count - 7; i += 8) {
    __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + i), _mm_subs_epi16(l, r));
  }

  // clean up any remaining elements
  for (; i < count; ++i) {
    res[i] = saturate_sub(x[i], y);
  }
}

// array version mul

template<typename TVoxel1, typename TVoxel2>
inline void saturate_mul(const TVoxel1* x, TVoxel2* y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_mul(x[i], y[i]);
  }
}

template<typename TVoxel1, typename TVoxel2>
inline void saturate_mul(const TVoxel1* x, TVoxel2 y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_mul(x[i], y);
  }
}

// array version div

template<typename TVoxel1, typename TVoxel2>
inline void saturate_div(const TVoxel1* x, TVoxel2* y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_div(x[i], y[i]);
  }
}

template<typename TVoxel1, typename TVoxel2>
inline void saturate_div_secure(const TVoxel1* x, TVoxel2* y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = y[i] == TVoxel2(0) ? TVoxel1(0) : saturate_div(x[i], y[i]);
  }
}

template<typename TVoxel1, typename TVoxel2>
inline void saturate_div(const TVoxel1* x, TVoxel2 y, size_t count, TVoxel1* res)
{
  for (size_t i = 0; i < count; ++i) {
    res[i] = saturate_div(x[i], y);
  }
}

} // namespace nim

