#ifndef ZIMGCACHE_H
#define ZIMGCACHE_H

#include <QCache>
#include "zimgpack.h"

namespace nim {

#define ZImgCacheInstance nim::ZImgCache::instance()

class ZImgCache : public QCache<size_t, std::shared_ptr<ZImg> >
{
public:
  static ZImgCache& instance();

  ZImgCache();
  ~ZImgCache();
};

} // namespace

#endif // ZIMGCACHE_H
