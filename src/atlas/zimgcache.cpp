#include "zimgcache.h"

#include "zcpuinfo.h"

namespace nim {

ZImgCache& ZImgCache::instance()
{
  static ZImgCache imgCache;
  return imgCache;
}

ZImgCache::ZImgCache()
  : QCache<size_t, std::shared_ptr<ZImg>>(ZCpuInfo::instance().nPhysicalRAM / 2 / 1024 / 1024)
{

}

void ZImgCache::insert(size_t key, std::shared_ptr<ZImg>* object)
{
  QWriteLocker lock(&m_lock);
  if (!QCache<size_t, std::shared_ptr<ZImg>>::insert(key, object,
                                                     std::max<int>(1, (*object)->byteNumber() / 1024 / 1024))) {
    throw ZImgException(QString("Can not insert img (%1) to cache").arg((*object)->info().toQString()));
  }
}

bool ZImgCache::remove(size_t key)
{
  QWriteLocker lock(&m_lock);
  return QCache<size_t, std::shared_ptr<ZImg>>::remove(key);
}

std::shared_ptr<ZImg>* ZImgCache::getOrRead(size_t key, const ZImgSubBlock& imgBlock)
{
  QReadLocker lock(&m_lock);
  std::shared_ptr<ZImg>* res = QCache<size_t, std::shared_ptr<ZImg>>::object(key);
  if (!res) {
    lock.unlock();
    res = new std::shared_ptr<ZImg>(imgBlock.read());
    insert(key, res);
  }
  return res;
}

} // namespace
