#pragma once

#include "zexception.h"
#include <folly/Executor.h>
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

namespace nim {

template<class T>
struct remove_cvref
{
  typedef std::remove_cv_t<std::remove_reference_t<T>> type;
};

template<class T>
using remove_cvref_t = typename remove_cvref<T>::type;

#ifdef _MSC_VER
#else
#define __forceinline inline __attribute__((always_inline))
#endif

template<typename TEnum>
constexpr typename std::underlying_type<TEnum>::type enumToUnderlyingType(TEnum e) noexcept
{
  return static_cast<typename std::underlying_type<TEnum>::type>(e);
}

template<typename TEnum>
std::string_view enumToString(TEnum e);

template<typename TEnum>
TEnum stringToEnum(std::string_view s);

template<typename TEnum>
inline QString enumToQString(TEnum e)
{
  auto str = enumToString(e);
  return QString::fromUtf8(str.data(), str.size());
}

#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
template<typename TEnum>
inline TEnum stringToEnum(QStringView s)
{
  auto str = s.toUtf8();
  return stringToEnum<TEnum>(std::string_view(str.data(), str.size()));
}
#else
template<typename TEnum>
inline TEnum stringToEnum(const QString& s)
{
  return stringToEnum<TEnum>(s.toStdString());
}
#endif

// https://chromium.googlesource.com/chromium/src/+/master/base/bit_cast.h
template<class Dest, class Source>
__forceinline Dest bit_cast(const Source& source)
{
  static_assert(sizeof(Dest) == sizeof(Source), "bit_cast requires source and destination to be the same size");
  static_assert(std::is_trivially_copyable_v<Dest>, "bit_cast requires the destination type to be copyable");
  static_assert(std::is_trivially_copyable_v<Source>, "bit_cast requires the source type to be copyable");
  Dest dest;
  std::memcpy(&dest, &source, sizeof(dest));
  return dest;
}

template<typename Type>
__forceinline bool is_aligned(Type* ptr)
{
  return (reinterpret_cast<uintptr_t>(ptr) & (alignof(Type) - 1)) == 0;
}

template<typename Type>
__forceinline bool is_aligned(Type* ptr, size_t a)
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

using index_t = std::ptrdiff_t;

// literal
// from c++23 literal 'uz'
constexpr size_t operator"" _uz(unsigned long long int n) noexcept
{
  return static_cast<size_t>(n);
}

// from c++23 literal 'z'
constexpr ptrdiff_t operator"" _z(unsigned long long int n) noexcept
{
  return static_cast<ptrdiff_t>(n);
}

constexpr uint8_t operator"" _u8(unsigned long long int n) noexcept
{
  return static_cast<uint8_t>(n);
}

constexpr int8_t operator"" _i8(unsigned long long int n) noexcept
{
  return static_cast<int8_t>(n);
}

constexpr uint16_t operator"" _u16(unsigned long long int n) noexcept
{
  return static_cast<uint16_t>(n);
}

constexpr int16_t operator"" _i16(unsigned long long int n) noexcept
{
  return static_cast<int16_t>(n);
}

constexpr uint32_t operator"" _u32(unsigned long long int n) noexcept
{
  return static_cast<uint32_t>(n);
}

constexpr int32_t operator"" _i32(unsigned long long int n) noexcept
{
  return static_cast<int32_t>(n);
}

constexpr uint64_t operator"" _u64(unsigned long long int n) noexcept
{
  return static_cast<uint64_t>(n);
}

constexpr int64_t operator"" _i64(unsigned long long int n) noexcept
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
__forceinline reversion_wrapper<T> make_reverse(T&& iterable)
{
  return {iterable};
}

template<class RAIter, class Compare>
__forceinline std::vector<size_t> argSort(RAIter first, RAIter last, Compare comp)
{
  std::vector<size_t> idx(last - first);
  std::iota(idx.begin(), idx.end(), 0);

  auto idxComp = [&first, comp](size_t i1, size_t i2) { return comp(first[i1], first[i2]); };

  std::stable_sort(idx.begin(), idx.end(), idxComp);

  return idx;
}

template<class RAIter>
__forceinline std::vector<size_t> argSort(RAIter first, RAIter last)
{
  std::vector<size_t> idx(last - first);
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::stable_sort(idx.begin(), idx.end(), [&first](size_t i1, size_t i2) { return first[i1] < first[i2]; });

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
  } else {
    return -1;
  }
}

template<typename C>
__forceinline void removeAt(C& iterable, size_t idx)
{
  // CHECK(idx < iterable.size());
  iterable.erase(iterable.begin() + idx);
}

class ZGlobal
{
public:
  inline static QString jdkDIR;
  inline static QString jarsDIR;
  inline static QString resourcesDIR;
};

// std::visit(overloaded {
//   [](auto arg) { std::cout << arg << ' '; },
//   [](double arg) { std::cout << std::fixed << arg << ' '; },
//   [](const std::string& arg) { std::cout << std::quoted(arg) << ' '; },
// }, v);
template<class... Ts>
struct overloaded : Ts...
{
  using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>; // not needed as of C++20

// some c++20 lib functions
template<class Container, class T>
__forceinline Container& erase(Container& on, const T& val)
{
  on.erase(std::remove(std::begin(on), std::end(on), val), std::end(on));
  return on;
}

template<class Container, class Pred>
__forceinline Container& erase_if(Container& on, Pred pred)
{
  on.erase(std::remove_if(std::begin(on), std::end(on), pred), std::end(on));
  return on;
}

template<class Container>
__forceinline Container& unique(Container& on)
{
  on.erase(std::unique(std::begin(on), std::end(on)), std::end(on));
  return on;
}

template<class Container, class Pred>
__forceinline Container& unique_if(Container& on, Pred pred)
{
  on.erase(std::unique(std::begin(on), std::end(on), pred), std::end(on));
  return on;
}

// to support std::get for local type
template<std::size_t Index, std::size_t N, typename T>
constexpr auto&& tuple_like_get_helper(T&& t) noexcept
{
  static_assert(Index < N, "Index out of bounds for tuple_like");
  return std::forward<T>(t)[Index];
}

folly::Executor::KeepAlive<> getGlobalCPUExecutor();

} // namespace nim
