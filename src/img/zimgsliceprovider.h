#pragma once

#include "zimg.h"
#include <set>

namespace nim {

// interface for a img pack that can be accessed slice by slice
class ZImgSliceProvider
{
public:
  virtual const ZImgInfo& imgInfo() const = 0;

  // must have 1
  virtual std::set<size_t> ratios() const
  { return std::set<size_t>{1}; }

  virtual ZImg slice(size_t z, size_t t, size_t ratio) const = 0;

  virtual ZImg allSlices(size_t t, size_t ratio) const;

  virtual ZImg wholeImg(size_t ratio) const;
};

} // namespace nim


