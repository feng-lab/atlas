#include "zimgcache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_cache_memory_proportion,
              0.2,
              "Proportion of RAM that will be used for image cache, default is 0.2");
DECLARE_bool(limit_memory_usage_to_64G);

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
  static ZImgCache imgCache(true);
  return imgCache;
}

ZImgCache::ZImgCache(bool canSkipDestructor)
  : ZThreadSafeScalableImageCache(
      FLAGS_limit_memory_usage_to_64G
        ? std::min(13'700'000'000., ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_cache_memory_proportion)
        : ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_cache_memory_proportion,
      ZCpuInfo::instance().nLogicalCores * 2,
      canSkipDestructor)
{}
#endif

} // namespace nim
