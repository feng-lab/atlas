//
// Created by Linqing Feng on 8/19/16.
//

#pragma once

#include <type_traits>

namespace nim {

template<typename Enum, typename std::enable_if_t<std::is_enum<Enum>::value, int> = 0>
struct is_flags : public std::false_type
{
};

// usage:
#define DECLARE_OPERATORS_FOR_ENUM(name) template <> struct is_flags< name > : std::true_type {};

} // namespace nim

// in global namespace so it doesn't hide qt's operator
//http://stackoverflow.com/questions/10755058/qflags-enum-type-conversion-fails-all-of-a-sudden
// impl:
#define ___DECLARE_ENUM_UNARY_OPERATOR(OP) \
  template<typename Enum> \
  constexpr std::enable_if_t<nim::is_flags<Enum>::value, Enum> \
  operator OP(Enum value) noexcept \
  { \
    using underlyingT = typename std::underlying_type<Enum>::type; \
    return static_cast<Enum>(OP static_cast<underlyingT>(value)); \
  }

#define ___DECLARE_ENUM_BINARY_OPERATOR(OP) \
  template<typename Enum> \
  constexpr std::enable_if_t<nim::is_flags<Enum>::value, Enum> \
  operator OP(Enum l, Enum r) noexcept \
  { \
    using underlyingT = typename std::underlying_type<Enum>::type; \
    return static_cast<Enum>(static_cast<underlyingT>(l) OP static_cast<underlyingT>(r)); \
  } \
  template<typename Enum> \
  constexpr std::enable_if_t<nim::is_flags<Enum>::value, Enum&> \
  operator OP##=(Enum& l, Enum r) noexcept \
  { \
      return l = l OP r; \
  }

___DECLARE_ENUM_UNARY_OPERATOR(~)
___DECLARE_ENUM_BINARY_OPERATOR(|)
___DECLARE_ENUM_BINARY_OPERATOR(&)
___DECLARE_ENUM_BINARY_OPERATOR(^)

namespace nim {

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr bool has_flag(Enum value, Enum flag) noexcept
{
  using underlyingT = typename std::underlying_type<Enum>::type;
  return (static_cast<underlyingT>(value) & static_cast<underlyingT>(flag)) != 0;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void set_flag(Enum& value) noexcept
{
  value |= ~value;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void set_flag(Enum& value, Enum flag) noexcept
{
  value |= flag;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void reset_flag(Enum& value) noexcept
{
  value &= ~value;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void reset_flag(Enum& value, Enum flag) noexcept
{
  value &= ~flag;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void flip_flag(Enum& value) noexcept
{
  value = ~value;
}

template<typename Enum, std::enable_if_t<is_flags<Enum>::value, int> = 0>
constexpr void flip_flag(Enum& value, Enum flag) noexcept
{
  value ^= flag;
}

} // namespace nim

