#include "zimgcache.h"

#include "zcpuinfo.h"

namespace nim {

ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : ZSharedCache<ZImgPack::HashKeyType, ZImg>(ZCpuInfo::instance().nPhysicalRAM / 2 / 1024 / 1024)
{
}

} // namespace
