#pragma once

#include "zexception.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>
#include <type_traits>
#include <vector>
#include <numeric>

#ifdef _MSC_VER
#else
#define __forceinline inline __attribute__((always_inline))
#endif

namespace std {

// c++23 feature
template<typename Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
{
  return static_cast<std::underlying_type_t<Enum>>(e);
}

// c++23
template<typename E>
  struct is_scoped_enum : std::bool_constant < requires
{
  requires std::is_enum_v<E>;
  requires !std::is_convertible_v<E, std::underlying_type_t<E>>;
} > {};
template<class T>
inline constexpr bool is_scoped_enum_v = is_scoped_enum<T>::value;

// c++23 utility
[[noreturn]] __forceinline void unreachable()
{
  // Uses compiler specific extensions if possible.
  // Even if no extension is used, undefined behavior is still raised by
  // an empty function body and the noreturn attribute.
#ifdef __GNUC__ // GCC, Clang, ICC
  __builtin_unreachable();
#elif defined(_MSC_VER) // MSVC
  __assume(false);
#endif
}

} // namespace std

namespace nim {

template<typename Type>
__forceinline bool isAligned(Type* ptr)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (alignof(Type) - 1)) == 0;
}

template<typename Type>
__forceinline bool isAligned(Type* ptr, size_t a)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (a - 1)) == 0;
}

// inline bool hostIsLittleEndian()
//{
//   int32_t num = 1;
//   return *reinterpret_cast<char*>(&num) == 1;
// }

template<typename Container>
__forceinline void clearAndDeallocate(Container& c)
{
  Container().swap(c);
}

template<typename Container>
__forceinline void shrinkToFit(Container& c)
{
  Container(c).swap(c);
}

using index_t = std::ptrdiff_t;

// literal
// from c++23 literal 'uz'
constexpr size_t operator""_uz(unsigned long long int n) noexcept
{
  return static_cast<size_t>(n);
}

// from c++23 literal 'z'
constexpr ptrdiff_t operator""_z(unsigned long long int n) noexcept
{
  return static_cast<ptrdiff_t>(n);
}

constexpr uint8_t operator""_u8(unsigned long long int n) noexcept
{
  return static_cast<uint8_t>(n);
}

constexpr int8_t operator""_i8(unsigned long long int n) noexcept
{
  return static_cast<int8_t>(n);
}

constexpr uint16_t operator""_u16(unsigned long long int n) noexcept
{
  return static_cast<uint16_t>(n);
}

constexpr int16_t operator""_i16(unsigned long long int n) noexcept
{
  return static_cast<int16_t>(n);
}

constexpr uint32_t operator""_u32(unsigned long long int n) noexcept
{
  return static_cast<uint32_t>(n);
}

constexpr int32_t operator""_i32(unsigned long long int n) noexcept
{
  return static_cast<int32_t>(n);
}

constexpr uint64_t operator""_u64(unsigned long long int n) noexcept
{
  return static_cast<uint64_t>(n);
}

constexpr int64_t operator""_i64(unsigned long long int n) noexcept
{
  return static_cast<int64_t>(n);
}

// http://stackoverflow.com/questions/8542591/c11-reverse-range-based-for-loop
template<typename T>
struct reversion_wrapper
{
  T& iterable;
};

template<typename T>
__forceinline auto begin(reversion_wrapper<T> w)
{
  return std::rbegin(w.iterable);
}

template<typename T>
__forceinline auto end(reversion_wrapper<T> w)
{
  return std::rend(w.iterable);
}

template<typename T>
__forceinline reversion_wrapper<T> makeReverse(T&& iterable)
{
  return {iterable};
}

template<class RAIter, class Compare>
__forceinline std::vector<size_t> argSort(RAIter first, RAIter last, Compare comp)
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
__forceinline std::vector<size_t> argSort(RAIter first, RAIter last)
{
  std::vector<size_t> idx(last - first);
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::stable_sort(idx.begin(), idx.end(), [&first](size_t i1, size_t i2) {
    return first[i1] < first[i2];
  });

  return idx;
}

template<typename C, typename V>
__forceinline bool contains(const C& iterable, const V& v)
{
  return std::find(std::begin(iterable), std::end(iterable), v) != std::end(iterable);
}

template<typename C, typename V>
__forceinline ptrdiff_t indexOf(const C& iterable, const V& v)
{
  if (auto it = std::find(std::begin(iterable), std::end(iterable), v); it != std::end(iterable)) {
    return std::distance(std::begin(iterable), it);
  }
  return -1;
}

template<typename C>
__forceinline void removeAt(C& iterable, size_t idx)
{
  // CHECK(idx < iterable.size());
  iterable.erase(iterable.begin() + idx);
}

template<class Container>
__forceinline Container& unique(Container& on)
{
  on.erase(std::unique(std::begin(on), std::end(on)), std::end(on));
  return on;
}

template<class Container, class Pred>
__forceinline Container& uniqueIf(Container& on, Pred pred)
{
  on.erase(std::unique(std::begin(on), std::end(on), pred), std::end(on));
  return on;
}

// to support std::get for local type
template<std::size_t Index, std::size_t N, typename T>
constexpr auto&& tupleLikeGetHelper(T&& t) noexcept
{
  static_assert(Index < N, "Index out of bounds for tuple_like");
  return std::forward<T>(t)[Index];
}

template<typename T>
struct IsStdArray : std::false_type
{};

template<typename T, std::size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type
{};

} // namespace nim
