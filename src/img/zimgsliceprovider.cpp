#include "zimgsliceprovider.h"

namespace nim {

ZImg ZImgSliceProvider::allSlices(size_t t, size_t ratio) const
{
  ZImg res;
  if (imgInfo().depth == 1) {
    slice(0, t, ratio).swap(res);
  } else {
    ZImgRegion rgn(0, -1, 0, -1, 0, -1, 0, -1, t, t + 1);
    ZImgInfo info = rgn.clip(imgInfo());
    info.width = std::ceil(info.width * 1.0 / ratio);
    info.height = std::ceil(info.height * 1.0 / ratio);
    res = ZImg(info);

    for (size_t z = 0; z < res.depth(); ++z) {
      res.pasteImg(slice(z, t, ratio), ZVoxelCoordinate(0, 0, z, 0, 0));
    }
  }
  return res;
}


} // namespace nim
