#pragma once

#include "zimg.h"

namespace nim {

// interface for a img pack that can be accessed slice by slice
class ZImgSliceProvider
{
public:
  virtual ~ZImgSliceProvider() = default;

  [[nodiscard]] virtual ZImgInfo imgInfo() const = 0;

  [[nodiscard]] virtual ZImg slice(size_t z, size_t t) const = 0;

  [[nodiscard]] virtual ZImg allSlices(size_t t) const;

  [[nodiscard]] virtual ZImg wholeImg() const;
};

} // namespace nim
