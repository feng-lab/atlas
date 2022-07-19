#pragma once

#include "zimgpack.h"
#include "zthreadsafescalablecache.h"
#include <QReadWriteLock>
#include <boost/functional/hash.hpp>
#include <list>
#include <unordered_map>

namespace nim {

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

    if (size > m_maxSize)
      return;
    size_t keepSize = m_maxSize - size;

    while (m_totalSize > keepSize) {
      const auto& back = m_cacheItemsList.back();
      m_cacheItemsMap.erase(std::get<0>(back));
      m_totalSize -= std::get<2>(back);
      m_cacheItemsList.pop_back();
    }

    m_cacheItemsList.emplace_front(key, std::move(object), size);
    m_cacheItemsMap[key] = m_cacheItemsList.begin();
    m_totalSize += size;
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
      //m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, it->second);
      return std::get<1>(*(it->second));
    } else {
      return ValueType();
    }
  }

protected:
  ~ZSharedCache() = default;

private:
  // first item is latest item
  std::list<KeyValueType> m_cacheItemsList;
  std::unordered_map<KeyType, ListIteratorType, boost::hash<KeyType>> m_cacheItemsMap;
  size_t m_maxSize;
  size_t m_totalSize = 0;
  mutable QReadWriteLock m_lock;
};

template<typename K>
struct ZHashCompare
{
  static size_t hash( const K& key )
  {
    boost::hash<K> hasher;
    return hasher(key);
  }
  static bool equal( const K& key1, const K& key2 ) {return key1 == key2;}
};

#if 0
class ZImgCache : public ZSharedCache<ZImgPack::HashKeyType, ZImg>
{
public:
  static ZImgCache& instance();

  ZImgCache();

  inline void insert(const ZImgPack::HashKeyType& key, std::shared_ptr<ZImg>&& object)
  {
    ZSharedCache<ZImgPack::HashKeyType, ZImg>::insert(key, std::move(object), object->byteNumber());
  }

  // never return nullptr, throw ZException on error
  inline std::shared_ptr<ZImg> getOrRead(const ZImgPack::HashKeyType& key, const ZImgSubBlock& imgBlock)
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

using ZThreadSafeScalableImageCache = ZThreadSafeScalableCache<ZImgPack::HashKeyType, std::shared_ptr<ZImg>, ZHashCompare<ZImgPack::HashKeyType>>;

class ZImgCache : public ZThreadSafeScalableImageCache
{
public:
  static ZImgCache& instance();

  ZImgCache();

  inline void insert(const ZImgPack::HashKeyType& key, std::shared_ptr<ZImg>&& object)
  {
    ZThreadSafeScalableImageCache::insert(key, std::move(object), object->byteNumber());
  }

  // never return nullptr, throw ZException on error
  inline std::shared_ptr<ZImg> getOrRead(const ZImgPack::HashKeyType& key, const ZImgSubBlock& imgBlock)
  {
    ZThreadSafeScalableImageCache::ConstAccessor ca;
    if (find(ca, key)) {
      return *ca;
    } else {
      std::shared_ptr<ZImg> res = imgBlock.read();
      insert(key, std::shared_ptr<ZImg>(res));
      return res;
    }
  }

  inline std::shared_ptr<ZImg> get(const ZImgPack::HashKeyType& key)
  {
    ZThreadSafeScalableImageCache::ConstAccessor ca;
    if (find(ca, key)) {
      return *ca;
    } else {
      return std::shared_ptr<ZImg>();
    }
  }
};
#endif

}  // namespace nim

