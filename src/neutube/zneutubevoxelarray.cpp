#include "zneutubevoxelarray.h"

#include "zimg.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim::neutube {

namespace {

[[nodiscard]] int iround(double v)
{
  return static_cast<int>(std::lround(v));
}

[[nodiscard]] double imgValueAt(const ZImg& img, size_t idx)
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  if (img.isType<uint8_t>()) {
    return static_cast<double>(img.timeData<uint8_t>(0)[idx]);
  }
  if (img.isType<uint16_t>()) {
    return static_cast<double>(img.timeData<uint16_t>(0)[idx]);
  }

  throw ZException(fmt::format("ZNeutubeVoxelArray::sample: unsupported voxel type {}", img.info()));
}

} // namespace

void ZNeutubeVoxelArray::addValue(double delta)
{
  for (auto& v : m_voxels) {
    v.value += delta;
  }
}

void ZNeutubeVoxelArray::multiplyValue(double a)
{
  for (auto& v : m_voxels) {
    v.value *= a;
  }
}

void ZNeutubeVoxelArray::minimizeValue(double v)
{
  for (auto& voxel : m_voxels) {
    voxel.value = std::min(voxel.value, v);
  }
}

void ZNeutubeVoxelArray::sample(const ZImg& img)
{
  sample(img, nullptr);
}

void ZNeutubeVoxelArray::sample(const ZImg& img, double (*f)(double))
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const int width = static_cast<int>(img.width());
  const int height = static_cast<int>(img.height());
  const int depth = static_cast<int>(img.depth());

  const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);

  for (auto& voxel : m_voxels) {
    if (!voxel.isInBound(width, height, depth)) {
      continue;
    }
    const size_t idx = static_cast<size_t>(voxel.x) + static_cast<size_t>(voxel.y) * static_cast<size_t>(width) +
                       static_cast<size_t>(voxel.z) * area;
    double value = imgValueAt(img, idx);
    if (f != nullptr) {
      value = f(value);
    }
    voxel.value = value;
  }
}

void ZNeutubeVoxelArray::labelImgWithBall(ZImg& img, uint8_t label) const
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);
  CHECK(img.isType<uint8_t>()) << img.info();

  const int width = static_cast<int>(img.width());
  const int height = static_cast<int>(img.height());
  const int depth = static_cast<int>(img.depth());

  uint8_t* out = img.timeData<uint8_t>(0);
  const int area = width * height;

  for (const auto& center : m_voxels) {
    CHECK(center.isInBound(width, height, depth));

    const int r = iround(center.value);
    const int zRange = r;
    const int zStart = std::max(0, center.z - zRange);
    const int zEnd = std::min(center.z + zRange, depth - 1);

    uint8_t* zArrayStart = out + static_cast<int64_t>(center.x) + static_cast<int64_t>(center.y) * width +
                           static_cast<int64_t>(zStart) * area;

    for (int z = zStart; z <= zEnd; ++z) {
      const int dz = std::abs(z - center.z);
      const double y_r = std::sqrt(center.value * center.value - static_cast<double>(dz * dz));
      const int yRange = iround(y_r);
      const int yStart = std::max(0, center.y - yRange);
      const int yEnd = std::min(center.y + yRange, height - 1);

      uint8_t* yArrayStart = zArrayStart - width * std::min(center.y, yRange);
      for (int y = yStart; y <= yEnd; ++y) {
        const int dy = std::abs(y - center.y);
        const int xRange = iround(std::sqrt(y_r * y_r - static_cast<double>(dy * dy)));
        const int xStart = std::max(0, center.x - xRange);
        const int xEnd = std::min(center.x + xRange, width - 1);

        uint8_t* xArrayStart = yArrayStart - std::min(center.x, xRange);
        for (int x = xStart; x <= xEnd; ++x) {
          *xArrayStart++ = label;
        }
        yArrayStart += width;
      }

      zArrayStart += area;
    }
  }
}

} // namespace nim::neutube
