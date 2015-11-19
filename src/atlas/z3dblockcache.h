#ifndef Z3DBLOCKCACHE_H
#define Z3DBLOCKCACHE_H

#include <unordered_map>
#include <list>
#include <cstddef>
#include "zexception.h"
#include "zglmutils.h"

namespace nim {

template<typename keyType, typename ValueType>
class Z3DBlockCache {
public:
  typedef typename std::pair<keyType, ValueType> KeyValuePairType;
  typedef typename std::list<KeyValuePairType>::iterator ListIteratorType;

  Z3DBlockCache(const glm::ivec3& blockSize, const glm::ivec3& numBlocks)
    : m_blockSize(blockSize)
    , m_numBlocks(numBlocks)
  {
  }

  void put(const keyType& key, const ValueType& value)
  {
    auto it = m_cacheItemsMap.find(key);
    if (it != m_cacheItemsMap.end()) {
      m_cacheItemsList.erase(it->second);
      m_cacheItemsMap.erase(it);
    }

    m_cacheItemsList.push_front(KeyValuePairType(key, value));
    m_cacheItemsMap[key] = m_cacheItemsList.begin();

    if (m_cacheItemsMap.size() > m_maxSize) {
      auto last = m_cacheItemsList.end();
      last--;
      m_cacheItemsMap.erase(last->first);
      m_cacheItemsList.pop_back();
    }
  }

  const ValueType& get(const keyType& key)
  {
    auto it = m_cacheItemsMap.find(key);
    if (it == m_cacheItemsMap.end()) {
      throw ZGLException("There is no such key in 3d block cache");
    } else {
      m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, it->second);
      return it->second->second;
    }
  }

  bool exists(const keyType& key) const
  {
    return m_cacheItemsMap.find(key) != m_cacheItemsMap.end();
  }

  size_t size() const
  {
    return m_cacheItemsMap.size();
  }

private:
  std::list<KeyValuePairType> m_cacheItemsList;
  std::unordered_map<keyType, ListIteratorType> m_cacheItemsMap;
  size_t m_maxSize;
  glm::ivec3 m_blockSize;
  glm::ivec3 m_numBlocks;
};

} // namespace nim

#endif // Z3DBLOCKCACHE_H

