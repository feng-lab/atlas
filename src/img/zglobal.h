#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>
#include <type_traits>
#include <vector>
#include <algorithm>
#include <numeric>
#include <concepts>
#include <bit>

#ifdef _MSC_VER
#else
#define __forceinline inline __attribute__((always_inline))
#endif

#if __cplusplus < 202302L

#include <boost/endian/conversion.hpp>

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
#if defined(_MSC_VER) && !defined(__clang__) // MSVC
  __assume(false);
#else // GCC, Clang
  __builtin_unreachable();
#endif
}

// std::forward_like
template<class A, class B>
using _CopyConst = std::conditional_t<std::is_const_v<A>, const B, B>;

template<class A, class B>
using _OverrideRef = std::conditional_t<std::is_rvalue_reference_v<A>, std::remove_reference_t<B>&&, B&>;

template<class A, class B>
using _ForwardLike = _OverrideRef<A&&, _CopyConst<std::remove_reference_t<A>, std::remove_reference_t<B>>>;

template<class T, class U>
[[nodiscard]] constexpr auto forward_like(U&& x) noexcept -> _ForwardLike<T, U>
{
  return static_cast<_ForwardLike<T, U>>(x);
}

template<class T>
constexpr T byteswap(T n) noexcept
{
  return boost::endian::endian_reverse(n);
}

} // namespace std

#endif

namespace nim {

template<typename Type>
__forceinline bool isAligned(Type* ptr, size_t a)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (a - 1)) == 0;
}

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

template<typename Iterator, typename Compare = std::ranges::less>
__forceinline std::vector<size_t> argSort(Iterator first, Iterator last, Compare comp = {})
{
  std::vector<size_t> idx(std::distance(first, last));
  std::iota(idx.begin(), idx.end(), 0);

  std::ranges::stable_sort(idx, [&](size_t i, size_t j) {
    return comp(first[i], first[j]);
  });

  return idx;
}

template<std::ranges::random_access_range R, typename Compare = std::ranges::less>
__forceinline std::vector<size_t> argSort(const R& range, Compare comp = {})
{
  std::vector<size_t> idx(std::ranges::size(range));
  std::iota(idx.begin(), idx.end(), 0);

  std::ranges::stable_sort(idx, [&](size_t i, size_t j) {
    return comp(range[i], range[j]);
  });

  return idx;
}

// Check if a range contains a specific value
template<std::ranges::input_range R, typename V>
__forceinline constexpr bool contains(const R& range, const V& v)
{
  return std::ranges::find(range, v) != std::ranges::end(range);
}

// Get the index of a value in a range (or -1 if not found)
template<std::ranges::input_range R, typename V>
__forceinline constexpr ptrdiff_t indexOf(const R& range, const V& v)
{
  if (auto it = std::ranges::find(range, v); it != std::ranges::end(range)) {
    return std::ranges::distance(std::ranges::begin(range), it);
  }
  return -1;
}

// Remove duplicate elements from a container
template<std::ranges::forward_range R,
         std::indirect_equivalence_relation<std::ranges::iterator_t<R>> C = std::ranges::equal_to>
__forceinline R& unique(R& range, bool doSorting = false, C comp = {})
{
  if (doSorting) {
    std::ranges::sort(range);
  }
  const auto [first, last] = std::ranges::unique(range, std::move(comp));
  range.erase(first, last);
  return range;
}

template<typename T>
struct IsStdArray : std::false_type
{};

template<typename T, auto N>
struct IsStdArray<std::array<T, N>> : std::true_type
{};

template<class T, class... U>
concept IsAnyOf = std::disjunction_v<std::is_same<T, U>...>;

template<typename T>
__forceinline void byteswap_inplace(T& value)
{
  static_assert(std::is_integral_v<T>, "T must be an integral type.");
  static_assert(sizeof(T) > 1, "Byteswap is unnecessary for types with size <= 1.");

  value = std::byteswap(value);
}

} // namespace nim
