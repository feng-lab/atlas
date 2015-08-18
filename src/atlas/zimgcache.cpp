#include "zimgcache.h"

#include "zcpuinfo.h"

namespace nim {

ZImgCache &ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : QCache<size_t, std::shared_ptr<ZImg> >(ZCpuInfoInstance.nPhysicalRAM / 2 / 1024 / 1024)
{

}

ZImgCache::~ZImgCache()
{

}

} // namespace
