#include "zimgcache.h"

#include <mutex>
#include "zcpuinfo.h"

namespace nim {

ZImgCache &ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : QCache<size_t, std::shared_ptr<ZImg>>(ZCpuInfoInstance.nPhysicalRAM / 2 / 1024 / 1024)
{

}

ZImgCache::~ZImgCache()
{

}

std::shared_ptr<ZImg> *ZImgCache::get(size_t key)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return QCache<size_t, std::shared_ptr<ZImg>>::object(key);
}

bool ZImgCache::remove(size_t key)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return QCache<size_t, std::shared_ptr<ZImg>>::remove(key);
}

std::shared_ptr<ZImg> *ZImgCache::getOrRead(size_t key, const ZImgSubBlock &imgBlock)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::shared_ptr<ZImg> *res = ZImgCacheInstance.object(key);
  if (!res) {
    res = new std::shared_ptr<ZImg>(new ZImg());
    imgBlock.read().swap(**res);
    insert(key, res, std::max(size_t(1), (*res)->byteNumber() / 1024 / 1024));
  }
  return res;
}

} // namespace
