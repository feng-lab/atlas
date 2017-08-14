#pragma once

#include "zimg.h"
#include <QSize>
#include <set>

namespace nim {

// interface for a img pack that can be accessed slice by slice
class ZImgSliceProvider
{
public:
  virtual const ZImgInfo& imgInfo() const = 0;

  // must have 1
  virtual std::map<size_t, QSize> ratioSizeMap() const
  { return std::map<size_t, QSize>{{1, QSize(imgInfo().width, imgInfo().height)}}; }

  virtual ZImg slice(size_t z, size_t t, size_t ratio) const = 0;

  virtual ZImg allSlices(size_t t, size_t ratio) const;
};

} // namespace nim


