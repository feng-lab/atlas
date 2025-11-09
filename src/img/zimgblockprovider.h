#pragma once

#include "zimg.h"

namespace nim {

// interface for a img pack that can be accessed block by block
class ZImgBlockProvider
{
public:
  ZImgBlockProvider() = default;
  ZImgBlockProvider(const ZImgBlockProvider&) = delete;
  ZImgBlockProvider& operator=(const ZImgBlockProvider&) = delete;
  ZImgBlockProvider(ZImgBlockProvider&&) = default;
  ZImgBlockProvider& operator=(ZImgBlockProvider&&) = default;
  virtual ~ZImgBlockProvider() = default;

  [[nodiscard]] virtual ZImgInfo imgInfo() const = 0;

  // must > 0
  [[nodiscard]] virtual size_t numBlocks() const = 0;

  [[nodiscard]] virtual ZImg block(size_t blockIdx) const = 0;

  [[nodiscard]] virtual ZVoxelCoordinate blockCoord(size_t blockIdx) const = 0;

  // default implementation assumes that blocks do not overlap
  [[nodiscard]] virtual ZImg wholeImg() const;
};

} // namespace nim
