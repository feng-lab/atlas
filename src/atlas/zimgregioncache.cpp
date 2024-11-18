#include "zimgregioncache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_region_cache_memory_proportion,
              0.3,
              "Proportion of RAM that will be used for image region cache, default is 0.3");

namespace nim {

ZImgRegionCache& ZImgRegionCache::instance()
{
  static ZImgRegionCache imgRegionCache(false);
  return imgRegionCache;
}

ZImgRegionCache::ZImgRegionCache(bool canSkipDestructor)
  : ZParentImgRegionCache(ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_region_cache_memory_proportion,
                          ZCpuInfo::instance().nLogicalCores * 2,
                          canSkipDestructor)
{}

} // namespace nim
