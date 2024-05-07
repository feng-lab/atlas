#pragma once

#include "zimg.h"
#include "zthreadsafescalablecache.h"
#include "zhashutils.h"

// #define USE_ZSharedCache

#ifdef USE_ZSharedCache
#include <QReadWriteLock>
#include <list>
#include <unordered_map>
#include <atomic>
#endif

// #define USE_KeyWithMemoizedHash

namespace nim {

#ifdef USE_ZSharedCache
template<typename KeyType, typename SharedValueType>
class ZSharedCache
{
public:
  using ValueType = std::shared_ptr<SharedValueType>;
  using KeyValueType = typename std::tuple<KeyType, ValueType, size_t>;
  using ListIteratorType = typename std::list<KeyValueType>::iterator;

  explicit ZSharedCache(size_t maxSize)
    : m_maxSize(maxSize)
  {}

  ZSharedCache(ZSharedCache&&) = delete;

  ZSharedCache& operator=(ZSharedCache&&) = delete;

  ZSharedCache(const ZSharedCache&) = delete;

  ZSharedCache& operator=(const ZSharedCache&) = delete;

  // thread-safe functions:

  // do nothing if object is too big
  void insert(const KeyType& key, ValueType&& object, size_t size = 1)
  {
    QWriteLocker lock(&m_lock);
    // same as remove(key)
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      auto listIt = it->second;
      m_totalSize -= std::get<2>(*listIt);
      m_cacheItemsList.erase(listIt);
      m_cacheItemsMap.erase(it);
    }

    if (size > m_maxSize) {
      return;
    }

    m_cacheItemsList.emplace_front(key, std::move(object), size);
    m_cacheItemsMap[key] = m_cacheItemsList.begin();
    m_totalSize += size;

    evict();
  }

  void remove(const KeyType& key)
  {
    QWriteLocker lock(&m_lock);
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      auto listIt = it->second;
      m_totalSize -= std::get<2>(*listIt);
      m_cacheItemsList.erase(listIt);
      m_cacheItemsMap.erase(it);
    }
  }

  // might return empty ptr
  ValueType get(const KeyType& key) const
  {
    QReadLocker lock(&m_lock);
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      // m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, it->second);
      return std::get<1>(*(it->second));
    } else {
      return ValueType();
    }
  }

  //  void stopCacheEviction()
  //  {
  //    ++m_cacheEvictionLockCounter;
  //  }
  //
  //  void resumeCacheEviction()
  //  {
  //    // make sure only 1 thread do the eviction
  //    size_t expected = 1;
  //    if (m_cacheEvictionLockCounter.compare_exchange_strong(expected, 0)) {
  //      evict();
  //    } else {
  //      CHECK(m_cacheEvictionLockCounter.load() > 0);
  //      --m_cacheEvictionLockCounter;
  //    }
  //  }

protected:
  ~ZSharedCache() = default;

  void evict()
  {
    // if (m_cacheEvictionLockCounter.load() == 0) {
    while (m_totalSize > m_maxSize) {
      const auto& back = m_cacheItemsList.back();
      m_cacheItemsMap.erase(std::get<0>(back));
      m_totalSize -= std::get<2>(back);
      m_cacheItemsList.pop_back();
    }
    //}
  }

private:
  // first item is latest item
  std::list<KeyValueType> m_cacheItemsList;
  std::unordered_map<KeyType, ListIteratorType, boost::hash<KeyType>> m_cacheItemsMap;
  size_t m_maxSize;
  mutable size_t m_totalSize = 0;
  mutable QReadWriteLock m_lock;
  // std::atomic<size_t> m_cacheEvictionLockCounter = 0;
};

class ZImgCache : public ZSharedCache<ZImgPack::HashKeyType, ZImg>
{
public:
  static ZImgCache& instance();

  ZImgCache();

  void insert(const ZImgPack::HashKeyType& key, std::shared_ptr<ZImg>&& object)
  {
    ZSharedCache<ZImgPack::HashKeyType, ZImg>::insert(key, std::move(object), object->byteNumber());
  }

  // never return nullptr, throw ZException on error
  std::shared_ptr<ZImg> getOrRead(const ZImgPack::HashKeyType& key, const ZImgSubBlock& imgBlock)
  {
    std::shared_ptr<ZImg> res = get(key);
    if (!res) {
      res = imgBlock.read();
      insert(key, std::shared_ptr<ZImg>(res));
    }
    return res;
  }
};
#else

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

using ZThreadSafeScalableImageCache =
  ZThreadSafeScalableCache<ImageCacheHashKeyType, std::shared_ptr<ZImg>, ZHashCompare<ImageCacheHashKeyType>>;

#endif

class ZImgCache : public ZThreadSafeScalableImageCache
{
public:
  using FindStategy = ZThreadSafeScalableImageCache::FindStrategy;

  static ZImgCache& instance();

  explicit ZImgCache(bool canSkipDestructor = false);

  void insert(const ImageCacheHashKeyType& key, const std::shared_ptr<ZImg>& object)
  {
    ZThreadSafeScalableImageCache::insert(key, object, object->byteNumber());
  }

  // never return nullptr, throw ZException on error
  std::shared_ptr<ZImg> getOrRead(const ImageCacheHashKeyType& key,
                                  const ZImgSubBlock& imgBlock,
                                  FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    if (ZThreadSafeScalableImageCache::ConstAccessor ca; find(ca, key, findStategy)) {
      return *ca;
    } else {
      auto res = imgBlock.read();
      insert(key, res);
      return res;
    }
  }

  std::shared_ptr<ZImg> get(const ImageCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    if (ZThreadSafeScalableImageCache::ConstAccessor ca; find(ca, key, findStategy)) {
      return *ca;
    } else {
      return {};
    }
  }

  bool contains(const ImageCacheHashKeyType& key, FindStategy findStategy = FindStategy::UpdateLRUList)
  {
    ZThreadSafeScalableImageCache::ConstAccessor ca;
    return find(ca, key, findStategy);
  }
};

#endif

} // namespace nim
