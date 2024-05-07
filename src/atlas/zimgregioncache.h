#pragma once

#include "zimg.h"
#include "zthreadsafescalablecache.h"
#include "zhashutils.h"

namespace nim {

using ImageRegionCacheHashKeyType = std::tuple<const void*,
                                               index_t,
                                               index_t,
                                               index_t,
                                               index_t,
                                               index_t,
                                               size_t,
                                               size_t,
                                               size_t,
                                               size_t,
                                               size_t,
                                               double,
                                               double>;

using ZThreadSafeScalableImageRegionCache = ZThreadSafeScalableCache<ImageRegionCacheHashKeyType,
                                                                     std::shared_ptr<ZImg>,
                                                                     ZHashCompare<ImageRegionCacheHashKeyType>>;

class ZImgRegionCache : public ZThreadSafeScalableImageRegionCache
{
public:
  using FindStategy = ZThreadSafeScalableImageRegionCache::FindStrategy;

  static ZImgRegionCache& instance();

  explicit ZImgRegionCache(bool canSkipDestructor = false);

  void insert(const ImageRegionCacheHashKeyType& key, const std::shared_ptr<ZImg>& object)
  {
    ZThreadSafeScalableImageRegionCache::insert(key, object, object->byteNumber());
  }

  std::shared_ptr<ZImg> get(const ImageRegionCacheHashKeyType& key,
                            FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    if (ZThreadSafeScalableImageRegionCache::ConstAccessor ca; find(ca, key, findStategy)) {
      return *ca;
    } else {
      return {};
    }
  }

  bool contains(const ImageRegionCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    ZThreadSafeScalableImageRegionCache::ConstAccessor ca;
    return find(ca, key, findStategy);
  }
};

} // namespace nim
