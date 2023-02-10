#pragma once

#include "zimg.h"
#include "zimgcache.h"
#include <QReadWriteLock>
#include <folly/futures/Future.h>
#include <boost/functional/hash.hpp>
#include <list>
#include <unordered_map>
#include <atomic>

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

  inline void insert(const ImageRegionCacheHashKeyType& key, const std::shared_ptr<ZImg>& object)
  {
    ZThreadSafeScalableImageRegionCache::insert(key, object, object->byteNumber());
  }

  inline std::shared_ptr<ZImg> get(const ImageRegionCacheHashKeyType& key,
                                   FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    if (ZThreadSafeScalableImageRegionCache::ConstAccessor ca; find(ca, key, findStategy)) {
      return *ca;
    } else {
      return {};
    }
  }

  inline bool contains(const ImageRegionCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    ZThreadSafeScalableImageRegionCache::ConstAccessor ca;
    return find(ca, key, findStategy);
  }
};

} // namespace nim

namespace std {

// custom specialization of std::hash can be injected in namespace std
template<>
struct hash<nim::ImageRegionCacheHashKeyType>
{
  inline std::size_t operator()(const nim::ImageRegionCacheHashKeyType& s) const noexcept
  {
    return boost::hash<nim::ImageRegionCacheHashKeyType>{}(s);
  }
};

} // namespace std
