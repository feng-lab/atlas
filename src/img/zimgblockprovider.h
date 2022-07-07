#pragma once

#include "zimg.h"
#include <set>

namespace nim {

// interface for a img pack that can be accessed block by block
class ZImgBlockProvider
{
public:
  virtual ~ZImgBlockProvider() = default;

  virtual ZImgInfo imgInfo() const = 0;

  // must have 1
  virtual size_t numBlocks() const = 0;

  virtual ZImg block(size_t blockIdx) const = 0;

  virtual ZVoxelCoordinate blockCoord(size_t blockIdx) const = 0;

  // default implementation assumes that blocks do not overlap
  virtual ZImg wholeImg() const;
};

} // namespace nim

