#pragma once

#include <array>

namespace nim {

struct FieldRangeLegacyLike
{
  std::array<int, 3> firstCorner = {0, 0, 0};
  std::array<int, 3> size = {0, 0, 0};
};

} // namespace nim
