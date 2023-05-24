#pragma once

#include "zexception.h"
#include "zglmutils.h"
#include <cstddef>
#include <list>
#include <boost/unordered/unordered_flat_map.hpp>

namespace nim {

template<typename KeyType>
class Z3DBlockCache
{
public:
  using KeyValuePairType = typename std::pair<KeyType, glm::uvec3>;
  using ListIteratorType = typename std::list<KeyValuePairType>::iterator;

  Z3DBlockCache(const glm::uvec3& blockSize, const glm::uvec3& numBlocks, const KeyType& invalidKey)
    : m_invalidKey(invalidKey)
  {
    CHECK(numBlocks.x > 0 && numBlocks.y > 0 && numBlocks.z > 0 && blockSize.x > 0 && blockSize.y > 0 &&
          blockSize.z > 0)
      << blockSize << numBlocks;

    m_size = numBlocks.x * numBlocks.y * numBlocks.z;
    for (uint32_t z = 0; z < numBlocks.z; ++z) {
      for (uint32_t y = 0; y < numBlocks.y; ++y) {
        for (uint32_t x = 0; x < numBlocks.x; ++x) {
          m_cacheItemsList.push_front(
            KeyValuePairType(invalidKey, glm::uvec3(x * blockSize.x, y * blockSize.y, z * blockSize.z)));
        }
      }
    }
  }

  inline glm::uvec3 insert(const KeyType& key, KeyType& erasedKey)
  {
    if (exists(key)) {
      erasedKey = m_invalidKey;
      auto it = m_cacheItemsMap.at(key);
      m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, it);
      return it->second;
    }
    auto last = std::prev(m_cacheItemsList.end());
    erasedKey = last->first;
    if (erasedKey != m_invalidKey) {
      // CHECK(m_cacheItemsMap.size() == m_size) << m_cacheItemsMap.size() << " " << m_size;
      m_cacheItemsMap.erase(erasedKey);
    }
    // else {
    //   CHECK(m_cacheItemsMap.size() < m_size) << m_cacheItemsMap.size() << " " << m_size;
    // }
    last->first = key;
    m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, last);
    m_cacheItemsMap[key] = last;
    return last->second;
  }

  //  inline void remove(const KeyType& key)
  //  {
  //    auto it = m_cacheItemsMap.find(key);
  //    m_cacheItemsList.splice(m_cacheItemsList.end(), m_cacheItemsList, it->second);
  //    it->second->first = m_invalidKey;
  //    m_cacheItemsMap.erase(it);
  //    if (m_numValidItems > 0) {
  //      --m_numValidItems;
  //    }
  //  }
  //
  //  inline void popFront()
  //  {
  //    auto first = m_cacheItemsList.begin();
  //    m_cacheItemsMap.erase(first->first);
  //    first->first = m_invalidKey;
  //    m_cacheItemsList.splice(m_cacheItemsList.end(), m_cacheItemsList, first);
  //    if (m_numValidItems > 0) {
  //      --m_numValidItems;
  //    }
  //  }

  inline const glm::uvec3& get(const KeyType& key) const
  {
    return m_cacheItemsMap.at(key)->second;
  }

  inline void touch(const KeyType& key)
  {
    m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, m_cacheItemsMap.at(key));
  }

  inline bool exists(const KeyType& key) const
  {
    return m_cacheItemsMap.find(key) != m_cacheItemsMap.end();
  }

  [[nodiscard]] inline size_t size() const
  {
    return m_size;
  }

private:
  std::list<KeyValuePairType> m_cacheItemsList;
  boost::unordered_flat_map<KeyType, ListIteratorType> m_cacheItemsMap;

  size_t m_size;
  KeyType m_invalidKey;
};

} // namespace nim
