#pragma once

#include "zimgpack.h"
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

  explicit ZSharedCache(size_t maxCost)
    : m_maxCost(maxCost)
  {}

  ZSharedCache(ZSharedCache&&) = delete;

  ZSharedCache& operator=(ZSharedCache&&) = delete;

  ZSharedCache(const ZSharedCache&) = delete;

  ZSharedCache& operator=(const ZSharedCache&) = delete;

  // thread-safe functions:

  // do nothing if object is too big
  void insert(const KeyType& key, ValueType&& object, size_t cost = 1)
  {
    QWriteLocker lock(&m_lock);
    // same as remove(key)
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      auto listIt = it->second;
      m_totalCost -= std::get<2>(*listIt);
      m_cacheItemsList.erase(listIt);
      m_cacheItemsMap.erase(it);
    }

    if (cost > m_maxCost)
      return;
    size_t keepCost = m_maxCost - cost;

    while (m_totalCost > keepCost) {
      const auto& back = m_cacheItemsList.back();
      m_cacheItemsMap.erase(std::get<0>(back));
      m_totalCost -= std::get<2>(back);
      m_cacheItemsList.pop_back();
    }

    m_cacheItemsList.emplace_front(key, std::move(object), cost);
    m_cacheItemsMap[key] = m_cacheItemsList.begin();
    m_totalCost += cost;
  }

  void remove(const KeyType& key)
  {
    QWriteLocker lock(&m_lock);
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      auto listIt = it->second;
      m_totalCost -= std::get<2>(*listIt);
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
  size_t m_maxCost;
  size_t m_totalCost = 0;
  mutable QReadWriteLock m_lock;
};

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

}  // namespace nim

