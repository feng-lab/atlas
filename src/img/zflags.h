#pragma once

#include <type_traits>

namespace nim {

template<typename TEnum>
struct IsFlags : public std::false_type
{};

// usage:
#define DECLARE_OPERATORS_FOR_ENUM(TEnum)                                 \
  template<>                                                              \
  struct IsFlags<TEnum> : public std::true_type                           \
  {                                                                       \
    static_assert(std::is_enum<TEnum>::value, "TEnum must be enum type"); \
  };

} // namespace nim

// in global namespace so it doesn't hide qt's operator
// http://stackoverflow.com/questions/10755058/qflags-enum-type-conversion-fails-all-of-a-sudden
// impl:
#define INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_UNARY_OPERATOR(OP) \
  template<typename TEnum>                                                        \
    requires nim::IsFlags<TEnum>::value                                           \
  constexpr TEnum operator OP(TEnum value) noexcept                               \
  {                                                                               \
    using underlyingT = typename std::underlying_type<TEnum>::type;               \
    return static_cast<TEnum>(OP static_cast<underlyingT>(value));                \
  }

#define INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_BINARY_OPERATOR(OP)         \
  template<typename TEnum>                                                                 \
    requires nim::IsFlags<TEnum>::value                                                    \
  constexpr TEnum operator OP(TEnum l, TEnum r) noexcept                                   \
  {                                                                                        \
    using underlyingT = typename std::underlying_type<TEnum>::type;                        \
    return static_cast<TEnum>(static_cast<underlyingT>(l) OP static_cast<underlyingT>(r)); \
  }                                                                                        \
  template<typename TEnum>                                                                 \
    requires nim::IsFlags<TEnum>::value                                                    \
  constexpr TEnum& operator OP##=(TEnum & l, TEnum r) noexcept                             \
  {                                                                                        \
    return l = l OP r;                                                                     \
  }

INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_UNARY_OPERATOR(~)

INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_BINARY_OPERATOR(|)

INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_BINARY_OPERATOR(&)

INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_BINARY_OPERATOR(^)

#undef INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_UNARY_OPERATOR
#undef INTERNAL_IMPLEMENTATION_DETAIL_DO_NOT_USE_DECLARE_ENUM_BINARY_OPERATOR

namespace nim {

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr bool isFlagSet(TEnum value, TEnum flag) noexcept
{
  using underlyingT = std::underlying_type_t<TEnum>;
  return (static_cast<underlyingT>(value) & static_cast<underlyingT>(flag)) == static_cast<underlyingT>(flag);
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void setFlag(TEnum& value) noexcept
{
  value |= ~value;
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void setFlag(TEnum& value, TEnum flag) noexcept
{
  value |= flag;
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void resetFlag(TEnum& value) noexcept
{
  value &= ~value;
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void resetFlag(TEnum& value, TEnum flag) noexcept
{
  value &= ~flag;
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void flipFlag(TEnum& value) noexcept
{
  value = ~value;
}

template<typename TEnum>
  requires IsFlags<TEnum>::value
constexpr void flipFlag(TEnum& value, TEnum flag) noexcept
{
  value ^= flag;
}

} // namespace nim
