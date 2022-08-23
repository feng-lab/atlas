#pragma once

#include "zimg.h"
#include <set>

namespace nim {

// interface for a img pack that can be accessed slice by slice
class ZImgSliceProvider
{
public:
  virtual ~ZImgSliceProvider() = default;

  virtual ZImgInfo imgInfo() const = 0;

  virtual ZImg slice(size_t z, size_t t) const = 0;

  virtual ZImg allSlices(size_t t) const;

  virtual ZImg wholeImg() const;
};

} // namespace nim
