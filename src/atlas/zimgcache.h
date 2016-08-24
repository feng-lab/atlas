#pragma once

#include <QReadWriteLock>
#include <QCache>
#include "zimgpack.h"

namespace nim {

class ZImgCache : private QCache<size_t, std::shared_ptr<ZImg>>
{
public:
  static ZImgCache& instance();

  ZImgCache();

  using QCache<size_t, std::shared_ptr<ZImg>>::object;

  // thread-safe functions:

  // throw ZImgException on error
  void insert(size_t key, std::shared_ptr<ZImg>* object);

  bool remove(size_t key);

  // never return nullptr, throw ZException on error
  std::shared_ptr<ZImg>* getOrRead(size_t key, const ZImgSubBlock& imgBlock);

private:
  QReadWriteLock m_lock;
};

} // namespace

