#include "zimgcache.h"

#include "zcpuinfo.h"

namespace nim {

#ifdef USE_ZSharedCache
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZSharedCache<ZImgPack::HashKeyType, ZImg>(ZCpuInfo::instance().nPhysicalRAM / 2)
{
}
#else
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZThreadSafeScalableImageCache(ZCpuInfo::instance().nPhysicalRAM / 2, ZCpuInfo::instance().nLogicalCores * 4)
{
}
#endif

} // namespace nim
