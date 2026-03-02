#pragma once

#include "zimgpack.h"

#include "zvoxelvolume.h"

#include "zlog.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace nim {

// Read-only voxel-volume adapter over ZImgPack, intended for tracing code paths that must operate
// on disk-cached datasets without materializing `wholeImg()`.
//
// This adapter provides random-access reads on a single channel/time (c,t). For disk-cached images,
// it caches the last accessed base tile to keep hot-loop sampling fast.
class ZImgPackVoxelVolume final : public ZVoxelVolume
{
public:
  ZImgPackVoxelVolume(std::shared_ptr<const ZImgPack> imgPack, size_t c, size_t t)
    : m_imgPack(std::move(imgPack))
    , m_c(c)
    , m_t(t)
  {
    CHECK(m_imgPack != nullptr);
    const ZImgInfo info = m_imgPack->imgInfo();
    CHECK(m_c < info.numChannels);
    CHECK(m_t < info.numTimes);

    m_width = info.width;
    m_height = info.height;
    m_depth = info.depth;

    m_voxelSizeX = info.voxelSizeX;
    m_voxelSizeY = info.voxelSizeY;
    m_voxelSizeZ = info.voxelSizeZ;

    if (info.voxelFormat == VoxelFormat::Float) {
      if (info.bytesPerVoxel == 4) {
        m_valueType = ZVoxelValueType::Float32;
      } else if (info.bytesPerVoxel == 8) {
        m_valueType = ZVoxelValueType::Float64;
      } else {
        CHECK(false) << "ZImgPackVoxelVolume: unsupported float voxel bytes " << info.bytesPerVoxel;
      }
    } else if (info.voxelFormat == VoxelFormat::Unsigned) {
      if (info.bytesPerVoxel == 1) {
        m_valueType = ZVoxelValueType::Uint8;
      } else if (info.bytesPerVoxel == 2) {
        m_valueType = ZVoxelValueType::Uint16;
      } else {
        CHECK(false) << "ZImgPackVoxelVolume: unsupported unsigned voxel bytes " << info.bytesPerVoxel;
      }
    } else {
      CHECK(false) << "ZImgPackVoxelVolume: unsupported voxel format " << static_cast<int>(info.voxelFormat);
    }
  }

  [[nodiscard]] bool isEmpty() const override
  {
    return m_imgPack == nullptr || m_width == 0 || m_height == 0 || m_depth == 0;
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

    const size_t ux = static_cast<size_t>(x);
    const size_t uy = static_cast<size_t>(y);
    const size_t uz = static_cast<size_t>(z);
    if (ux >= m_width || uy >= m_height || uz >= m_depth) {
      return 0.0;
    }

    if (!m_imgPack->isDiskCached()) {
      return m_imgPack->img().value<double>(ux, uy, uz, m_c, m_t);
    }

    if (m_imgPack->isNeuroglancerPrecomputed()) {
      return m_imgPack->value(ux, uy, uz, m_c, m_t, /*mip=*/false);
    }

    if (m_cachedTileImg && cachedTileContains(ux, uy, uz)) {
      return m_cachedTileImg->value<double>(ux - static_cast<size_t>(m_cachedTileX),
                                            uy - static_cast<size_t>(m_cachedTileY),
                                            uz - static_cast<size_t>(m_cachedTileZ),
                                            m_c,
                                            0);
    }

    const std::optional<size_t> tileIndexOpt = m_imgPack->tryFindBaseTileIndexForVoxel(ux, uy, uz, m_t);
    if (!tileIndexOpt.has_value()) {
      return 0.0;
    }

    const size_t tileIndex = *tileIndexOpt;
    const ZImgSubBlock& tile = m_imgPack->tileByIndex(tileIndex);
    std::shared_ptr<ZImg> tileImg = m_imgPack->readTileBlocking(tileIndex);

    m_cachedTileIndex = tileIndex;
    m_cachedTileImg = std::move(tileImg);
    m_cachedTileX = tile.x;
    m_cachedTileY = tile.y;
    m_cachedTileZ = tile.z;
    m_cachedTileWidth = tile.width;
    m_cachedTileHeight = tile.height;
    m_cachedTileDepth = tile.depth;

    CHECK(m_cachedTileImg != nullptr);

    if (!cachedTileContains(ux, uy, uz)) {
      return 0.0;
    }

    return m_cachedTileImg->value<double>(ux - static_cast<size_t>(m_cachedTileX),
                                          uy - static_cast<size_t>(m_cachedTileY),
                                          uz - static_cast<size_t>(m_cachedTileZ),
                                          m_c,
                                          0);
  }

private:
  [[nodiscard]] bool cachedTileContains(size_t x, size_t y, size_t z) const
  {
    if (!m_cachedTileImg) {
      return false;
    }
    CHECK(m_cachedTileX >= 0 && m_cachedTileY >= 0 && m_cachedTileZ >= 0);

    const size_t x0 = static_cast<size_t>(m_cachedTileX);
    const size_t y0 = static_cast<size_t>(m_cachedTileY);
    const size_t z0 = static_cast<size_t>(m_cachedTileZ);

    return x >= x0 && x < x0 + static_cast<size_t>(m_cachedTileWidth) && y >= y0 &&
           y < y0 + static_cast<size_t>(m_cachedTileHeight) && z >= z0 &&
           z < z0 + static_cast<size_t>(m_cachedTileDepth);
  }

private:
  std::shared_ptr<const ZImgPack> m_imgPack;
  size_t m_c = 0;
  size_t m_t = 0;

  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;

  double m_voxelSizeX = 1.0;
  double m_voxelSizeY = 1.0;
  double m_voxelSizeZ = 1.0;

  ZVoxelValueType m_valueType = ZVoxelValueType::Uint8;

  // One-tile cache for disk-cached datasets.
  mutable std::optional<size_t> m_cachedTileIndex;
  mutable std::shared_ptr<ZImg> m_cachedTileImg;
  mutable index_t m_cachedTileX = 0;
  mutable index_t m_cachedTileY = 0;
  mutable index_t m_cachedTileZ = 0;
  mutable index_t m_cachedTileWidth = 0;
  mutable index_t m_cachedTileHeight = 0;
  mutable index_t m_cachedTileDepth = 0;
};

} // namespace nim
