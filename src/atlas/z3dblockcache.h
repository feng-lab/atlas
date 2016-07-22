#ifndef Z3DBLOCKCACHE_H
#define Z3DBLOCKCACHE_H

#include <unordered_map>
#include <list>
#include <cstddef>
#include "zexception.h"
#include "zglmutils.h"
//#include <QsLog.h>

namespace nim {

template<typename KeyType>
class Z3DBlockCache {
public:
  typedef typename std::pair<KeyType, glm::ivec3> KeyValuePairType;
  typedef typename std::list<KeyValuePairType>::iterator ListIteratorType;

  Z3DBlockCache(const glm::uvec3& blockSize, const glm::uvec3& numBlocks, const KeyType& invalidKey)
    : m_numValidItems(0)
    , m_blockSize(blockSize)
    , m_numBlocks(numBlocks)
    , m_invalidKey(invalidKey)
  {
    assert(m_numBlocks.x > 0 && m_numBlocks.y > 0 && m_numBlocks.z > 0 &&
           m_blockSize.x > 0 && m_blockSize.y > 0 && m_blockSize.z > 0);

    m_size = m_numBlocks.x * m_numBlocks.y * m_numBlocks.z;
    for (uint32_t z=0; z<m_numBlocks.z; ++z) {
      for (uint32_t y=0; y<m_numBlocks.y; ++y) {
        for (uint32_t x=0; x<m_numBlocks.x; ++x) {
          m_cacheItemsList.push_front(KeyValuePairType(m_invalidKey, glm::ivec3(x*m_blockSize.x, y*m_blockSize.y, z*m_blockSize.z)));
          //LINFO() << glm::ivec3(x*m_blockSize.x, y*m_blockSize.y, z*m_blockSize.z);
        }
      }
    }
  }

  glm::ivec3 insert(const KeyType& key, KeyType& erasedKey)
  {
    auto last = m_cacheItemsList.end();
    last--;
    if (m_numValidItems == m_size) {
      m_cacheItemsMap.erase(last->first);
    } else {
      ++m_numValidItems;
    }
    erasedKey = last->first;
    last->first = key;
    m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, last);
    m_cacheItemsMap[key] = m_cacheItemsList.begin();
    return last->second;
  }

  void remove(const KeyType& key)
  {
    auto it = m_cacheItemsMap.find(key);
    m_cacheItemsList.splice(m_cacheItemsList.end(), m_cacheItemsList, it->second);
    it->second->first = m_invalidKey;
    m_cacheItemsMap.erase(it);
    if (m_numValidItems > 0) {
      --m_numValidItems;
    }
  }

  void popFront()
  {
    auto first = m_cacheItemsList.begin();
    m_cacheItemsMap.erase(first->first);
    first->first = m_invalidKey;
    m_cacheItemsList.splice(m_cacheItemsList.end(), m_cacheItemsList, first);
    if (m_numValidItems > 0) {
      --m_numValidItems;
    }
  }

  const glm::ivec3& get(const KeyType& key)
  {
    auto it = m_cacheItemsMap.find(key);
    return it->second->second;
  }

  void touch(const KeyType& key)
  {
    m_cacheItemsList.splice(m_cacheItemsList.begin(), m_cacheItemsList, m_cacheItemsMap.find(key)->second);
  }

  bool exists(const KeyType& key) const
  {
    return m_cacheItemsMap.find(key) != m_cacheItemsMap.end();
  }

  size_t size() const
  {
    return m_size;
  }

private:
  std::list<KeyValuePairType> m_cacheItemsList;
  std::unordered_map<KeyType, ListIteratorType> m_cacheItemsMap;
  size_t m_numValidItems;

  size_t m_size;
  glm::uvec3 m_blockSize;
  glm::uvec3 m_numBlocks;
  KeyType m_invalidKey;
};

} // namespace nim

#endif // Z3DBLOCKCACHE_H

