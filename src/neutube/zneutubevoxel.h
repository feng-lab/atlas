#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace nim {

struct ZNeutubeVoxel
{
  int x = 0;
  int y = 0;
  int z = 0;
  double value = 0.0;

  ZNeutubeVoxel() = default;

  ZNeutubeVoxel(int x_, int y_, int z_, double value_ = 0.0)
    : x(x_)
    , y(y_)
    , z(z_)
    , value(value_)
  {}

  [[nodiscard]] double distanceTo(const ZNeutubeVoxel& other) const
  {
    const double dx = static_cast<double>(x - other.x);
    const double dy = static_cast<double>(y - other.y);
    const double dz = static_cast<double>(z - other.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  void setFromIndex(size_t index, int width, int height)
  {
    const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
    z = static_cast<int>(index / area);
    const size_t rem = index - static_cast<size_t>(z) * area;
    y = static_cast<int>(rem / static_cast<size_t>(width));
    x = static_cast<int>(rem - static_cast<size_t>(y) * static_cast<size_t>(width));
  }

  [[nodiscard]] int64_t toIndex(int width, int height, int depth) const
  {
    if (!isInBound(width, height, depth)) {
      return -1;
    }

    const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
    return static_cast<int64_t>(x) + static_cast<int64_t>(y) * width + static_cast<int64_t>(z) * area;
  }

  [[nodiscard]] bool isInBound(int width, int height, int depth) const
  {
    return x >= 0 && y >= 0 && z >= 0 && x < width && y < height && z < depth;
  }

  void translate(int dx, int dy, int dz)
  {
    x += dx;
    y += dy;
    z += dz;
  }
};

} // namespace nim
