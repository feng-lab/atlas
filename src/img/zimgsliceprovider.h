#ifndef ZIMGSLICEPROVIDER
#define ZIMGSLICEPROVIDER

#include "zimg.h"

namespace nim {

// interface for a img pack that can be accessed slice by slice
class ZImgSliceProvider
{
public:
  virtual const ZImgInfo& imgInfo() const = 0;
  virtual ZImg slice(size_t z, size_t t) const = 0;
  virtual ZImg allSlices(size_t t) const;
};

} // namespace nim

#endif // ZIMGSLICEPROVIDER

