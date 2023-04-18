#include "zimgcache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_cache_memory_proportion,
              0.2,
              "Proportion of RAM that will be used for image cache, default is 0.2");

namespace nim {

#ifdef USE_ZSharedCache
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZSharedCache<ZImgPack::HashKeyType, ZImg>(ZCpuInfo::instance().nPhysicalRAM *
                                              FLAGS_atlas_image_cache_memory_proportion)
{}
#else
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache(false);
  return imgCache;
}

ZImgCache::ZImgCache(bool canSkipDestructor)
  : ZThreadSafeScalableImageCache(ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_cache_memory_proportion,
                                  ZCpuInfo::instance().nLogicalCores * 2,
                                  canSkipDestructor)
{}
#endif

} // namespace nim
