#pragma once

#include "zexception.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>
#include <numeric>

namespace nim {

#ifdef _MSC_VER
#else
#define __forceinline       inline __attribute__((always_inline))
#endif

template<typename TEnum>
constexpr typename std::underlying_type<TEnum>::type enumToUnderlyingType(TEnum e) noexcept
{
  return static_cast<typename std::underlying_type<TEnum>::type>(e);
}

// https://chromium.googlesource.com/chromium/src/+/master/base/bit_cast.h
template<class Dest, class Source>
__forceinline Dest bit_cast(const Source& source)
{
  static_assert(sizeof(Dest) == sizeof(Source),
                "bit_cast requires source and destination to be the same size");
  static_assert(std::is_trivially_copyable_v<Dest>,
                "bit_cast requires the destination type to be copyable");
  static_assert(std::is_trivially_copyable_v<Source>,
                "bit_cast requires the source type to be copyable");
  Dest dest;
  std::memcpy(&dest, &source, sizeof(dest));
  return dest;
}

template<typename Type>
inline bool is_aligned(Type* ptr)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (alignof(Type) - 1)) == 0;
}

template<typename Type>
inline bool is_aligned(Type* ptr, size_t a)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (a - 1)) == 0;
}

inline bool hostIsLittleEndian()
{
  int32_t num = 1;
  return *reinterpret_cast<char*>(&num) == 1;
}

template<typename Container>
inline void clearAndDeallocate(Container& c)
{
  Container().swap(c);
}

// literal
constexpr size_t operator "" _usize(unsigned long long int n) noexcept
{ return static_cast<size_t>(n); }

constexpr ptrdiff_t operator "" _isize(unsigned long long int n) noexcept
{ return static_cast<ptrdiff_t>(n); }

constexpr uint8_t operator "" _u8(unsigned long long int n) noexcept
{ return static_cast<uint8_t>(n); }

constexpr int8_t operator "" _i8(unsigned long long int n) noexcept
{ return static_cast<int8_t>(n); }

constexpr uint16_t operator "" _u16(unsigned long long int n) noexcept
{ return static_cast<uint16_t>(n); }

constexpr int16_t operator "" _i16(unsigned long long int n) noexcept
{ return static_cast<int16_t>(n); }

constexpr uint32_t operator "" _u32(unsigned long long int n) noexcept
{ return static_cast<uint32_t>(n); }

constexpr int32_t operator "" _i32(unsigned long long int n) noexcept
{ return static_cast<int32_t>(n); }

constexpr uint64_t operator "" _u64(unsigned long long int n) noexcept
{ return static_cast<uint64_t>(n); }

constexpr int64_t operator "" _i64(unsigned long long int n) noexcept
{ return static_cast<int64_t>(n); }

//http://stackoverflow.com/questions/8542591/c11-reverse-range-based-for-loop
template<typename T>
struct reversion_wrapper
{
  T& iterable;
};

template<typename T>
inline auto begin(reversion_wrapper<T> w)
{ return std::rbegin(w.iterable); }

template<typename T>
inline auto end(reversion_wrapper<T> w)
{ return std::rend(w.iterable); }

template<typename T>
inline reversion_wrapper<T> make_reverse(T&& iterable)
{ return {iterable}; }

template<class RAIter, class Compare>
std::vector<size_t> argSort(RAIter first, RAIter last, Compare comp)
{
  std::vector<size_t> idx(last - first);
  std::iota(idx.begin(), idx.end(), 0);

  auto idxComp = [&first, comp](size_t i1, size_t i2) {
    return comp(first[i1], first[i2]);
  };

  std::stable_sort(idx.begin(), idx.end(), idxComp);

  return idx;
}

template<class RAIter>
std::vector<size_t> argSort(RAIter first, RAIter last)
{
  std::vector<size_t> idx(last - first);
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::stable_sort(idx.begin(), idx.end(), [&first](size_t i1, size_t i2) { return first[i1] < first[i2]; });

  return idx;
}

template<class T>
struct dependent_false : std::false_type
{
};

class ZGlobal
{
public:
  inline static QString jdkDIR = "";
  inline static QString jarsDIR = "";
};

} // namespace nim
