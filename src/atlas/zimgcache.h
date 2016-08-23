#pragma once

#include <QMutexLocker>
#include <QCache>
#include "zimgpack.h"

namespace nim {

class ZImgCache : private QCache<size_t, std::shared_ptr<ZImg>>
{
public:
  static ZImgCache& instance();

  ZImgCache();

  ~ZImgCache();

  using QCache<size_t, std::shared_ptr<ZImg>>::object;

  // thread-safe functions:
  std::shared_ptr<ZImg>* get(size_t key);

  bool insert(size_t key, std::shared_ptr<ZImg>* object, int cost = 1);

  bool remove(size_t key);

  std::shared_ptr<ZImg>* getOrRead(size_t key, const ZImgSubBlock& imgBlock);

private:
  QMutex m_mutex;
};

} // namespace

