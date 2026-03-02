#pragma once

#include "zvoxelvolume.h"

#include "zlog.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace nim {

namespace detail {

template<typename StorageT>
struct SparseVoxelMaskTraits;

template<>
struct SparseVoxelMaskTraits<std::uint8_t>
{
  static constexpr ZVoxelValueType kValueType = ZVoxelValueType::Uint8;
};

template<>
struct SparseVoxelMaskTraits<std::uint16_t>
{
  static constexpr ZVoxelValueType kValueType = ZVoxelValueType::Uint16;
};

} // namespace detail

// Sparse uint8/uint16 3D mask used for tracing workspaces on large datasets.
//
// Storage is block-backed and allocated on demand. Unallocated voxels read as `m_defaultValue`.
template<typename StorageT>
class ZSparseVoxelMask final : public ZVoxelVolumeMutable
{
public:
  static_assert(std::is_same_v<StorageT, std::uint8_t> || std::is_same_v<StorageT, std::uint16_t>,
                "ZSparseVoxelMask only supports uint8/uint16 storage");

  explicit ZSparseVoxelMask(size_t width, size_t height, size_t depth, size_t blockEdge = 64)
    : m_width(width)
    , m_height(height)
    , m_depth(depth)
    , m_blockEdge(blockEdge)
  {
    CHECK(m_blockEdge > 0);
    m_blockArea = m_blockEdge * m_blockEdge;
    m_blockVolume = m_blockArea * m_blockEdge;
  }

  [[nodiscard]] bool isEmpty() const override
  {
    return m_width == 0 || m_height == 0 || m_depth == 0;
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
    return 1.0;
  }

  [[nodiscard]] double voxelSizeY() const override
  {
    return 1.0;
  }

  [[nodiscard]] double voxelSizeZ() const override
  {
    return 1.0;
  }

  [[nodiscard]] ZVoxelValueType valueType() const override
  {
    return detail::SparseVoxelMaskTraits<StorageT>::kValueType;
  }

  [[nodiscard]] double valueAsDouble(int x, int y, int z) const override
  {
    return static_cast<double>(valueAt(x, y, z));
  }

  void setValueU16(int x, int y, int z, std::uint16_t value) override
  {
    if (!inBounds(x, y, z)) {
      return;
    }

    CHECK(value <= std::numeric_limits<StorageT>::max())
      << "ZSparseVoxelMask::setValueU16 value " << value << " does not fit mask storage type.";

    const BlockKey key = blockKey(x, y, z);
    auto it = m_blocks.find(key);
    if (it == m_blocks.end()) {
      auto blk = std::make_unique<Block>();
      blk->voxels.assign(m_blockVolume, m_defaultValue);
      it = m_blocks.emplace(key, std::move(blk)).first;
    }

    const size_t localIndex = blockLocalIndex(x, y, z);
    CHECK(localIndex < it->second->voxels.size());
    it->second->voxels[localIndex] = static_cast<StorageT>(value);
  }

  void clearU16(std::uint16_t value) override
  {
    CHECK(value <= std::numeric_limits<StorageT>::max())
      << "ZSparseVoxelMask::clearU16 value " << value << " does not fit mask storage type.";

    m_defaultValue = static_cast<StorageT>(value);
    m_blocks.clear();
  }

  [[nodiscard]] std::uint16_t valueU16(int x, int y, int z) const
  {
    return static_cast<std::uint16_t>(valueAt(x, y, z));
  }

private:
  struct Block
  {
    std::vector<StorageT> voxels;
  };

  struct BlockKey
  {
    size_t bx = 0;
    size_t by = 0;
    size_t bz = 0;

    bool operator==(const BlockKey& other) const
    {
      return bx == other.bx && by == other.by && bz == other.bz;
    }
  };

  struct BlockKeyHash
  {
    size_t operator()(const BlockKey& k) const noexcept
    {
      // A simple hash combiner; block indices are expected to be fairly small.
      size_t h = std::hash<size_t>{}(k.bx);
      h ^= std::hash<size_t>{}(k.by) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      h ^= std::hash<size_t>{}(k.bz) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  [[nodiscard]] bool inBounds(int x, int y, int z) const
  {
    if (x < 0 || y < 0 || z < 0) {
      return false;
    }
    if (static_cast<size_t>(x) >= m_width || static_cast<size_t>(y) >= m_height || static_cast<size_t>(z) >= m_depth) {
      return false;
    }
    return true;
  }

  [[nodiscard]] BlockKey blockKey(int x, int y, int z) const
  {
    CHECK(x >= 0 && y >= 0 && z >= 0);
    return BlockKey{static_cast<size_t>(x) / m_blockEdge,
                    static_cast<size_t>(y) / m_blockEdge,
                    static_cast<size_t>(z) / m_blockEdge};
  }

  [[nodiscard]] size_t blockLocalIndex(int x, int y, int z) const
  {
    CHECK(x >= 0 && y >= 0 && z >= 0);
    const size_t lx = static_cast<size_t>(x) % m_blockEdge;
    const size_t ly = static_cast<size_t>(y) % m_blockEdge;
    const size_t lz = static_cast<size_t>(z) % m_blockEdge;
    return lx + ly * m_blockEdge + lz * m_blockArea;
  }

  [[nodiscard]] StorageT valueAt(int x, int y, int z) const
  {
    if (!inBounds(x, y, z)) {
      return static_cast<StorageT>(0);
    }

    const BlockKey key = blockKey(x, y, z);
    auto it = m_blocks.find(key);
    if (it == m_blocks.end()) {
      return m_defaultValue;
    }

    const size_t localIndex = blockLocalIndex(x, y, z);
    CHECK(localIndex < it->second->voxels.size());
    return it->second->voxels[localIndex];
  }

  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;

  size_t m_blockEdge = 64;
  size_t m_blockArea = 0;
  size_t m_blockVolume = 0;

  StorageT m_defaultValue = 0;

  std::unordered_map<BlockKey, std::unique_ptr<Block>, BlockKeyHash> m_blocks;
};

using ZSparseVoxelMaskU8 = ZSparseVoxelMask<std::uint8_t>;

} // namespace nim
