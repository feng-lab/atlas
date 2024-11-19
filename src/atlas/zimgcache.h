#pragma once

#include "zimg.h"
// #include "zthreadsafescalablecache.h"
// #include "zhashutils.h"
#include "zconcurrentlrucache.h"

// #define USE_KeyWithMemoizedHash

namespace nim {

#ifdef USE_KeyWithMemoizedHash

struct ImageCacheHashKeyType
{
  ImageCacheHashKeyType(const void* p, size_t i)
    : m_storage(new Storage(p, i))
  {}

  ImageCacheHashKeyType() {}

  size_t index() const
  {
    return std::get<1>(m_storage->m_data);
  }

  size_t hash() const
  {
    return m_storage->hash();
  }

  bool operator==(const ImageCacheHashKeyType& other) const
  {
    return m_storage->m_data == other.m_storage->m_data;
  }

  bool operator<(const ImageCacheHashKeyType& other) const
  {
    return m_storage->m_data < other.m_storage->m_data;
  }

  struct HashCompare
  {
    bool equal(const ImageCacheHashKeyType& j, const ImageCacheHashKeyType& k) const
    {
      return j == k;
    }

    size_t hash(const ImageCacheHashKeyType& k) const
    {
      return k.hash();
    }
  };

private:
  struct Storage
  {
    Storage(const void* p, size_t i)
      : m_data(p, i)
    {}

    std::tuple<const void*, size_t> m_data;
    mutable std::atomic<size_t> m_hash;

    size_t hash() const
    {
      size_t h = m_hash.load(std::memory_order_relaxed);
      if (h == 0) {
        m_hash.store(boost::hash<std::tuple<const void*, size_t>>{}(m_data), std::memory_order_relaxed);
      }
      return h;
    }
  };

  std::shared_ptr<Storage> m_storage;
};

using ZThreadSafeScalableImageCache =
  ZThreadSafeScalableCache<ImageCacheHashKeyType, std::shared_ptr<ZImg>, ImageCacheHashKeyType::HashCompare>;

#else

using ImageCacheHashKeyType = std::tuple<const void*, size_t>;

// using ZParentImgCache =
//   ZThreadSafeScalableCache<ImageCacheHashKeyType, std::shared_ptr<ZImg>, ZHashCompare<ImageCacheHashKeyType>>;

using ZParentImgCache = ZConcurrentLRUCache<ImageCacheHashKeyType, std::shared_ptr<ZImg>>;

#endif

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
                                  FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    if (auto resOpt = find(key, findStategy); resOpt) {
      return resOpt.value();
    } else {
      auto res = imgBlock.read();
      insert(key, res);
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
