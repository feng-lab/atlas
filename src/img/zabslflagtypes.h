#pragma once

#include <absl/log/check.h>
#include <absl/strings/match.h>
#include <absl/strings/string_view.h>

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>

namespace nim {

template<typename T>
struct AbslEnumFlagValue
{
  absl::string_view text;
  T value;
};

template<typename T, std::size_t N>
[[nodiscard]] bool parseAbslEnumFlag(absl::string_view text,
                                     T* value,
                                     /*nullable*/ std::string* error,
                                     absl::string_view typeName,
                                     const std::array<AbslEnumFlagValue<T>, N>& values)
{
  static_assert(std::is_enum_v<T>);
  CHECK(value != nullptr);

  for (const auto& candidate : values) {
    if (absl::EqualsIgnoreCase(text, candidate.text)) {
      *value = candidate.value;
      return true;
    }
  }

  if (error != nullptr) {
    std::string expected;
    for (const auto& candidate : values) {
      if (!expected.empty()) {
        expected += ", ";
      }
      expected += std::string(candidate.text);
    }
    *error = std::string(typeName) + " must be one of: " + expected;
  }
  return false;
}

template<typename T, std::size_t N>
[[nodiscard]] std::string unparseAbslEnumFlag(T value, const std::array<AbslEnumFlagValue<T>, N>& values)
{
  static_assert(std::is_enum_v<T>);
  for (const auto& candidate : values) {
    if (value == candidate.value) {
      return std::string(candidate.text);
    }
  }

  using Underlying = std::underlying_type_t<T>;
  return std::to_string(static_cast<Underlying>(value));
}

} // namespace nim
