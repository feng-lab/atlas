#include "zimgcache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_cache_memory_proportion, 0.5,
              "Proportion of RAM that will be used for image cache, default is 0.5");

namespace nim {

#ifdef USE_ZSharedCache
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZSharedCache<ZImgPack::HashKeyType, ZImg>(ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_cache_memory_proportion)
{
}
#else
ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZThreadSafeScalableImageCache(ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_cache_memory_proportion,
                                  ZCpuInfo::instance().nLogicalCores * 4)
{
  LOG(INFO) << "FLAGS_atlas_image_cache_memory_proportion: " << FLAGS_atlas_image_cache_memory_proportion;
}
#endif

} // namespace nim
