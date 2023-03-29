#include "zimgregioncache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_region_cache_memory_proportion,
              0.2,
              "Proportion of RAM that will be used for image region cache, default is 0.2");

namespace nim {

ZImgRegionCache& ZImgRegionCache::instance()
{
  static ZImgRegionCache imgRegionCache(true);
  return imgRegionCache;
}

ZImgRegionCache::ZImgRegionCache(bool canSkipDestructor)
  : ZThreadSafeScalableImageRegionCache(ZCpuInfo::instance().nPhysicalRAM *
                                          FLAGS_atlas_image_region_cache_memory_proportion,
                                        ZCpuInfo::instance().nLogicalCores * 4,
                                        canSkipDestructor)
{}

} // namespace nim
