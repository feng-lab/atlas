#pragma once

#include "zimg.h"
#include "zconcurrentlrucache.h"

namespace nim {

using ImageCacheHashKeyType = std::tuple<const void*, size_t>;
using ZParentImgCache = ZConcurrentLRUCache<ImageCacheHashKeyType, std::shared_ptr<ZImg>>;

class ZImgCache : public ZParentImgCache
{
public:
  using FindStategy = ZParentImgCache::FindStrategy;

  static ZImgCache& instance();

  explicit ZImgCache(bool canSkipDestructor = false);

  void insert(const ImageCacheHashKeyType& key, const std::shared_ptr<ZImg>& object)
  {
    ZParentImgCache::insert(key, object, object->byteNumber());
  }

  // never return nullptr, throw ZException on error
  std::shared_ptr<ZImg> getOrRead(const ImageCacheHashKeyType& key,
                                  const ZImgSubBlock& imgBlock,
                                  FindStategy findStategy = FindStategy::UpdateLRUList,
                                  /*optional*/ bool* didRead = nullptr)
  {
    if (auto resOpt = find(key, findStategy); resOpt) {
      if (didRead) {
        *didRead = false;
      }
      return resOpt.value();
    } else {
      auto res = imgBlock.read();
      insert(key, res);
      if (didRead) {
        *didRead = true;
      }
      return res;
    }
  }

  std::shared_ptr<ZImg> get(const ImageCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    return find(key, findStategy).value_or(std::shared_ptr<ZImg>());
  }

  bool contains(const ImageCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    return find(key, findStategy).has_value();
  }
};

} // namespace nim
