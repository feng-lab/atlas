#pragma once

#include "zvoxelvolume.h"

#include "zimg.h"
#include "zlog.h"

#include <cstddef>

namespace nim {

class ZDenseVoxelVolume final : public ZVoxelVolume
{
public:
  explicit ZDenseVoxelVolume(const ZImg& img)
  {
    if (img.isEmpty()) {
      m_empty = true;
      return;
    }

    CHECK(img.numChannels() == 1) << "ZDenseVoxelVolume expects a single-channel view, got " << img.info();
    CHECK(img.numTimes() == 1) << "ZDenseVoxelVolume expects a single-time view, got " << img.info();

    m_width = img.width();
    m_height = img.height();
    m_depth = img.depth();
    m_area = m_width * m_height;

    const ZImgInfo info = img.info();
    m_voxelSizeX = info.voxelSizeX;
    m_voxelSizeY = info.voxelSizeY;
    m_voxelSizeZ = info.voxelSizeZ;

    if (img.isType<std::uint8_t>()) {
      m_valueType = ZVoxelValueType::Uint8;
      m_data = static_cast<const void*>(img.timeData<std::uint8_t>(0));
    } else if (img.isType<std::uint16_t>()) {
      m_valueType = ZVoxelValueType::Uint16;
      m_data = static_cast<const void*>(img.timeData<std::uint16_t>(0));
    } else if (img.isType<float>()) {
      m_valueType = ZVoxelValueType::Float32;
      m_data = static_cast<const void*>(img.timeData<float>(0));
    } else if (img.isType<double>()) {
      m_valueType = ZVoxelValueType::Float64;
      m_data = static_cast<const void*>(img.timeData<double>(0));
    } else {
      CHECK(false) << "ZDenseVoxelVolume: unsupported voxel type " << img.info();
    }
  }

  [[nodiscard]] bool isEmpty() const override
  {
    return m_empty || m_data == nullptr || m_width == 0 || m_height == 0 || m_depth == 0;
  }

  [[nodiscard]] size_t width() const override
  {
    return m_width;
  }

  [[nodiscard]] size_t height() const override
  {
    return m_height;
  }

  [[nodiscard]] size_t depth() const override
  {
    return m_depth;
  }

  [[nodiscard]] double voxelSizeX() const override
  {
    return m_voxelSizeX;
  }

  [[nodiscard]] double voxelSizeY() const override
  {
    return m_voxelSizeY;
  }

  [[nodiscard]] double voxelSizeZ() const override
  {
    return m_voxelSizeZ;
  }

  [[nodiscard]] ZVoxelValueType valueType() const override
  {
    return m_valueType;
  }

  [[nodiscard]] double valueAsDouble(int x, int y, int z) const override
  {
    if (isEmpty()) {
      return 0.0;
    }
    if (x < 0 || y < 0 || z < 0) {
      return 0.0;
    }
    if (static_cast<size_t>(x) >= m_width || static_cast<size_t>(y) >= m_height || static_cast<size_t>(z) >= m_depth) {
      return 0.0;
    }

    const size_t idx = static_cast<size_t>(x) + static_cast<size_t>(y) * m_width + static_cast<size_t>(z) * m_area;
    switch (m_valueType) {
      case ZVoxelValueType::Uint8:
        return static_cast<double>(static_cast<const std::uint8_t*>(m_data)[idx]);
      case ZVoxelValueType::Uint16:
        return static_cast<double>(static_cast<const std::uint16_t*>(m_data)[idx]);
      case ZVoxelValueType::Float32:
        return static_cast<double>(static_cast<const float*>(m_data)[idx]);
      case ZVoxelValueType::Float64:
        return static_cast<const double*>(m_data)[idx];
      default:
        break;
    }

    CHECK(false) << "ZDenseVoxelVolume: invalid voxel value type";
    return 0.0;
  }

private:
  bool m_empty = false;
  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;
  size_t m_area = 0;

  double m_voxelSizeX = 1.0;
  double m_voxelSizeY = 1.0;
  double m_voxelSizeZ = 1.0;

  ZVoxelValueType m_valueType = ZVoxelValueType::Uint8;
  const void* m_data = nullptr;
};

} // namespace nim
