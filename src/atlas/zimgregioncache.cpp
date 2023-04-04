#include "zimgregioncache.h"

#include "zcpuinfo.h"

DEFINE_double(atlas_image_region_cache_memory_proportion,
              0.2,
              "Proportion of RAM that will be used for image region cache, default is 0.2");
DECLARE_bool(limit_memory_usage_to_64G);

namespace nim {

ZImgRegionCache& ZImgRegionCache::instance()
{
  static ZImgRegionCache imgRegionCache(true);
  return imgRegionCache;
}

ZImgRegionCache::ZImgRegionCache(bool canSkipDestructor)
  : ZThreadSafeScalableImageRegionCache(
      FLAGS_limit_memory_usage_to_64G
        ? std::min(13'700'000'000.,
                   ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_region_cache_memory_proportion)
        : ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_region_cache_memory_proportion,
      ZCpuInfo::instance().nLogicalCores * 2,
      canSkipDestructor)
{}

} // namespace nim
