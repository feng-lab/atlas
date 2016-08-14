#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <iterator>
#include <memory>
#include "zexception.h"

namespace nim {

#ifdef _MSC_VER
#define __warn_unused_result _Check_return_
#define __align(...)        __declspec(align(__VA_ARGS__))
#else
#define __warn_unused_result  __attribute__((warn_unused_result))
#define __forceinline       inline __attribute__((always_inline))
#define __align(...)        __attribute__((aligned(__VA_ARGS__)))
#endif

template<typename TEnum>
constexpr typename std::underlying_type<TEnum>::type enumToUnderlyingType(TEnum e) noexcept
{
  return static_cast<typename std::underlying_type<TEnum>::type>(e);
}

template<typename TResult, typename TEnum>
constexpr TResult enumToType(TEnum e) noexcept
{
  return static_cast<TResult>(static_cast<typename std::underlying_type<TEnum>::type>(e));
}

// define string of enum as:
// template<> const char* EnumString<TEnum>::data[] = {"a", "b", ...};
// to enable enumToString for TEnum
template<typename TEnum>
struct EnumStrings
{
  static const char* data[];
};

// crash if enum e is not valid
template<typename TEnum>
constexpr const char* enumToString(TEnum e) noexcept
{
  return EnumStrings<TEnum>::data[static_cast<typename std::underlying_type<TEnum>::type>(e)];
}

// https://chromium.googlesource.com/chromium/src/+/master/base/bit_cast.h
template<class Dest, class Source>
inline Dest bit_cast(const Source& source)
{
  static_assert(sizeof(Dest) == sizeof(Source),
                "bit_cast requires source and destination to be the same size");
  static_assert(std::is_trivially_copyable<Dest>::value,
                "non-trivially-copyable bit_cast is undefined");
  static_assert(std::is_trivially_copyable<Source>::value,
                "non-trivially-copyable bit_cast is undefined");
  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

// effective stl, item 24, Scott Meyers
template<typename MapType, // type of map
  typename KeyArgType, // see below for why
  typename ValueArgtype> // KeyArgType and ValueArgtype are type
typename MapType::iterator // parameters
efficientAddOrUpdate(MapType& m, const KeyArgType& k, const ValueArgtype& v)
{
  typename MapType::iterator lb = m.lower_bound(k); // find where k is or should be
  if (lb != m.end() && !(m.key_comp()(k, lb->first))) { // if Ib points to a pair whose key is equiv to k...
    lb->second = v; // update the pair's value
    return lb; // and return an iterator to that pair
  } else {
    return m.emplace_hint(lb, k, v); // add pair(k, v) to m and return an iterator to the new map element
  }
}

// literal
constexpr size_t operator "" _usize(unsigned long long int n) noexcept { return n; }
constexpr ptrdiff_t operator "" _isize(unsigned long long int n) noexcept { return n; }
constexpr uint8_t operator "" _u8(unsigned long long int n) noexcept { return n; }
constexpr int8_t operator "" _i8(unsigned long long int n) noexcept { return n; }
constexpr uint16_t operator "" _u16(unsigned long long int n) noexcept { return n; }
constexpr int16_t operator "" _i16(unsigned long long int n) noexcept { return n; }
constexpr uint32_t operator "" _u32(unsigned long long int n) noexcept { return n; }
constexpr int32_t operator "" _i32(unsigned long long int n) noexcept { return n; }
constexpr uint64_t operator "" _u64(unsigned long long int n) noexcept { return n; }
constexpr int64_t operator "" _i64(unsigned long long int n) noexcept { return n; }

//http://stackoverflow.com/questions/8542591/c11-reverse-range-based-for-loop
template<typename T>
struct reversion_wrapper
{
  T& iterable;
};

template<typename T>
inline auto begin(reversion_wrapper<T> w) { return std::rbegin(w.iterable); }

template<typename T>
inline auto end(reversion_wrapper<T> w) { return std::rend(w.iterable); }

template<typename T>
inline reversion_wrapper<T> make_reverse(T&& iterable) { return {iterable}; }


} // namespace nim
