#pragma once

// adapt from Tim Starling's scalable-cache.h

#include "zthreadsafelrucache.h"

namespace nim {

/**
 * ZThreadSafeScalableCache is a thread-safe sharded hashtable with limited
 * size. When it is full, it evicts a rough approximation to the least recently
 * used item.
 *
 * The find() operation fills a ConstAccessor object, which is a smart pointer
 * similar to TBB's const_accessor. After eviction, destruction of the value is
 * deferred until all ConstAccessor objects are destroyed.
 *
 * Since the hash value of each key is requested multiple times, you should use
 * a key with a memoized hash function. ThreadSafeStringKey is provided for
 * this purpose.
 */
template<class TKey, class TValue, class THash = tbb::tbb_hash_compare<TKey>>
struct ZThreadSafeScalableCache
{
  using Shard = ZThreadSafeLRUCache<TKey, TValue, THash>;

  /**
   * Constructor
   *   - maxSize: the maximum number of items in the container
   *   - numShards: the number of child containers. If this is zero, the
   *     "hardware concurrency" will be used (typically the logical processor
   *     count).
   */
  explicit ZThreadSafeScalableCache(size_t maxSize, size_t numShards = 0, bool canSkipDestructor = false)
    : m_maxSize(maxSize)
    , m_numShards(numShards)
  {
    if (m_numShards == 0) {
      m_numShards = std::thread::hardware_concurrency() * 4;
      if (m_numShards == 0) {
        m_numShards = 8; // Fallback in case hardware_concurrency returns 0
      }
    }
    for (size_t i = 0; i < m_numShards; i++) {
      size_t s = maxSize / m_numShards;
      if (i == 0) {
        s += maxSize % m_numShards;
      }
      m_shards.emplace_back(std::make_shared<Shard>(s, canSkipDestructor));
    }
  }

  ZThreadSafeScalableCache(const ZThreadSafeScalableCache&) = delete;

  ZThreadSafeScalableCache& operator=(const ZThreadSafeScalableCache&) = delete;

  using FindStrategy = typename Shard::FindStrategy;

  /**
   * Find a value by key, and returns optional contained the value if the element was found, empty optional
   * otherwise. Updates the eviction list, making the element the most-recently used.
   */
  std::optional<TValue> find(const TKey& key, FindStrategy findStategy = FindStrategy::UpdateLRUList)
  {
    return getShard(key).find(key, findStategy);
  }

  void remove(const TKey& key)
  {
    return getShard(key).remove(key);
  }

  /**
   * Insert a value into the container. Both the key and value will be copied.
   * The new element will put into the eviction list as the most-recently
   * used.
   *
   * If there was already an element in the container with the same key, it
   * will not be updated, and false will be returned. Otherwise, true will be
   * returned.
   */
  bool insert(const TKey& key, const TValue& value, size_t objSize)
  {
    return getShard(key).insert(key, value, objSize);
  }

  /**
   * Clear the container. NOT THREAD SAFE -- do not use while other threads
   * are accessing the container.
   */
  void clear()
  {
    for (size_t i = 0; i < m_numShards; i++) {
      m_shards[i]->clear();
    }
  }

  /**
   * Get a snapshot of the keys in the container by copying them into the
   * supplied vector. This will block inserts and prevent LRU updates while it
   * completes. The keys will be inserted in a random order.
   */
  void snapshotKeys(std::vector<TKey>& keys)
  {
    for (size_t i = 0; i < m_numShards; i++) {
      m_shards[i]->snapshotKeys(keys);
    }
  }

  /**
   * Get the approximate size of the container. May be slightly too low when
   * insertion is in progress.
   */
  [[nodiscard]] size_t size() const
  {
    size_t size = 0;
    for (size_t i = 0; i < m_numShards; i++) {
      size += m_shards[i]->size();
    }
    return size;
  }

private:
  /**
   * Get the child container for a given key
   */
  Shard& getShard(const TKey& key)
  {
    THash hashObj;
    // constexpr int shift = std::numeric_limits<size_t>::digits - 16;
    // size_t h = (hashObj.hash(key) >> shift) % m_numShards;
    size_t h = hashObj.hash(key) % m_numShards;
    return *m_shards.at(h);
  }

  /**
   * The maximum number of elements in the container.
   */
  size_t m_maxSize;

  /**
   * The child containers
   */
  size_t m_numShards;
  typedef std::shared_ptr<Shard> ShardPtr;
  std::vector<ShardPtr> m_shards;
};

} // namespace nim
