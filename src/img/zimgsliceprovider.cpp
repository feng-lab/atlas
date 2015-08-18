#include "zimgsliceprovider.h"

namespace nim {

ZImg ZImgSliceProvider::allSlices(size_t t) const
{
  ZImg res;
  if (imgInfo().depth == 1) {
    slice(0, t).swap(res);
  } else {
    ZImgRegion rgn(0,-1,0,-1,0,-1,0,-1,t,t+1);
    ZImgInfo info = rgn.clip(imgInfo());
    res = ZImg(info);

    for (size_t z=0; z<res.depth(); ++z) {
      res.pasteImg(slice(z,t), ZVoxelCoordinate(0,0,z,0,0));
    }
  }
  return res;
}


} // namespace
