#ifndef ZGLOBAL
#define ZGLOBAL

#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace nim {

#ifdef _MSC_VER
#define __warn_unused_result _Check_return_
#define __align(...)        __declspec(align(__VA_ARGS__))
#define noexcept
#define constexpr
#else
#define __warn_unused_result  __attribute__((warn_unused_result))
#define __forceinline       inline __attribute__((always_inline))
#define __align(...)        __attribute__((aligned(__VA_ARGS__)))
#endif

#define DECLARE_SWAP(TYPE) \
  inline void swap(TYPE &value1, TYPE &value2) \
  { value1.swap(value2); }

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
constexpr const char* enumToString(TEnum e)
{
  return EnumStrings<TEnum>::data[static_cast<typename std::underlying_type<TEnum>::type>(e)];
}

// https://chromium.googlesource.com/chromium/src/+/master/base/macros.h
template <class Dest, class Source>
inline Dest bit_cast(const Source& source)
{
  static_assert(sizeof(Dest) == sizeof(Source), "VerifySizesAreEqual");
  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

} // namespace nim

#endif // ZGLOBAL

