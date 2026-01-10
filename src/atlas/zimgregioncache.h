#pragma once

#include "zimg.h"
// #include "zthreadsafescalablecache.h"
// #include "zhashutils.h"
#include "zconcurrentlrucache.h"

#include <array>
#include <cstdint>

namespace nim {

enum class ZImgRegionCacheSourceKind : uint8_t
{
  File = 0,
  Neuroglancer = 1,
};

// NOTE: We use a stable dataset fingerprint (not a ZImgPack pointer) so that
// in-memory and disk cache keys match across Atlas processes and sessions.
using ImageRegionCacheHashKeyType = std::tuple<std::array<uint8_t, 32>,
                                               ZImgRegionCacheSourceKind,
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
                                               size_t,
                                               size_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               double,
                                               double>;

// using ZParentImgRegionCache = ZThreadSafeScalableCache<ImageRegionCacheHashKeyType,
//                                                        std::shared_ptr<ZImg>,
//                                                        ZHashCompare<ImageRegionCacheHashKeyType>>;

using ZParentImgRegionCache = ZConcurrentLRUCache<ImageRegionCacheHashKeyType, std::shared_ptr<ZImg>>;

class ZImgRegionCache : public ZParentImgRegionCache
{
public:
  using FindStategy = ZParentImgRegionCache::FindStrategy;

  static ZImgRegionCache& instance();

  explicit ZImgRegionCache(bool canSkipDestructor = false);

  void insert(const ImageRegionCacheHashKeyType& key, const std::shared_ptr<ZImg>& object)
  {
    ZParentImgRegionCache::insert(key, object, object->byteNumber());
  }

  std::shared_ptr<ZImg> get(const ImageRegionCacheHashKeyType& key,
                            FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    return find(key, findStategy).value_or(std::shared_ptr<ZImg>());
  }

  bool contains(const ImageRegionCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    return find(key, findStategy).has_value();
  }
};

} // namespace nim
