#pragma once

#include <boost/unordered/concurrent_flat_map.hpp>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>

namespace nim {

/**
 * ZConcurrentLRUCache is a thread-safe hashtable with a limited size. When
 * it is full, insert() evicts the least recently used item from the cache.
 *
 * The cache is segmented to reduce lock contention. Each segment has its own
 * mutex and LRU list, allowing multiple threads to operate on different segments
 * concurrently.
 */
template<class TKey, class TValue>
class ZConcurrentLRUCache
{
  /**
   * The LRU list node.
   */
  struct ListNode
  {
    ListNode() = default;

    ListNode(const TKey& key, size_t size)
      : m_key(key)
      , m_size(size)
    {}

    TKey m_key;
    size_t m_size = 0;
    bool m_inList = false;
    ListNode* m_prev = nullptr;
    ListNode* m_next = nullptr;
  };

  /**
   * The value that we store in the hashtable.
   */
  struct HashMapValue
  {
    HashMapValue() = default;

    HashMapValue(const TValue& value, std::unique_ptr<ListNode>&& node)
      : m_value(value)
      , m_listNode(std::move(node))
    {}

    TValue m_value;
    std::unique_ptr<ListNode> m_listNode;
  };

  using HashMap = boost::concurrent_flat_map<TKey, HashMapValue>;

  /**
   * Each segment contains its own LRU list and mutex.
   */
  struct Segment
  {
    Segment()
      : m_head()
      , m_tail()
      , m_size(0)
    {
      m_head.m_next = &m_tail;
      m_tail.m_prev = &m_head;
    }

    ListNode m_head;
    ListNode m_tail;
    std::mutex m_mutex;
    size_t m_size;
  };

public:
  /**
   * Create a container with a given maximum size and number of segments.
   */
  explicit ZConcurrentLRUCache(size_t maxSize, size_t numSegments = 0, bool canSkipDestructor = false)
    : m_maxSize(maxSize)
    , m_numSegments(numSegments)
    , m_map(std::thread::hardware_concurrency() * 4)
    , m_canSkipDestructor(canSkipDestructor)
  {
    if (numSegments == 0) {
      m_numSegments = std::thread::hardware_concurrency() * 4;
      if (m_numSegments == 0) {
        m_numSegments = 8; // Fallback in case hardware_concurrency returns 0
      }
    }

    m_maxSizePerSegment = m_maxSize / m_numSegments;
    m_segments.resize(m_numSegments);

    for (size_t i = 0; i < m_numSegments; ++i) {
      m_segments[i] = std::make_unique<Segment>();
    }
  }

  ZConcurrentLRUCache(const ZConcurrentLRUCache& other) = delete;
  ZConcurrentLRUCache& operator=(const ZConcurrentLRUCache&) = delete;

  ~ZConcurrentLRUCache()
  {
    if (!m_canSkipDestructor) {
      clear();
    }
  }

  enum class FindStrategy
  {
    NoUpdateLRUList,
    UpdateLRUList,
    MaybeUpdateLRUList
  };

  /**
   * Find a value by key and return an optional containing the value if found.
   * Updates the LRU list, making the element the most-recently used, depending on the strategy.
   */
  std::optional<TValue> find(const TKey& key, FindStrategy findStrategy = FindStrategy::UpdateLRUList)
  {
    TValue value;
    if (m_map.cvisit(key, [&](const auto& accessor) {
          value = accessor.second.m_value;
          if (findStrategy != FindStrategy::NoUpdateLRUList) {
            auto& segment = *m_segments[getSegmentIndex(key)];
            auto node = accessor.second.m_listNode.get();
            if (findStrategy == FindStrategy::UpdateLRUList) {
              std::unique_lock lock(segment.m_mutex);
              if (node->m_inList) {
                delink(node);
                pushFront(segment, node);
              }
            } else if (findStrategy == FindStrategy::MaybeUpdateLRUList) {
              // Try to lock without blocking
              std::unique_lock lock(segment.m_mutex, std::try_to_lock);
              if (lock && node->m_inList) {
                delink(node);
                pushFront(segment, node);
              }
            }
          }
        })) {
      return value;
    }
    return {};
  }

  /**
   * Remove a value by key.
   */
  void remove(const TKey& key)
  {
    auto& segment = *m_segments[getSegmentIndex(key)];

    if (m_map.cvisit(key, [&](const auto& accessor) {
          std::unique_lock lock(segment.m_mutex);
          if (auto node = accessor.second.m_listNode.get(); node->m_inList) {
            delink(node);
            segment.m_size -= node->m_size;
            // Node will be deleted when unique_ptr goes out of scope
          }
        })) {
      m_map.erase(key);
    }
  }

  /**
   * Insert a value into the container. Both the key and value will be copied.
   * The new element will be placed into the LRU list as the most-recently used.
   *
   * If there was already an element in the container with the same key, it
   * will not be updated, and false will be returned. Otherwise, true will be
   * returned.
   */
  bool insert(const TKey& key, const TValue& value, size_t objSize)
  {
    auto& segment = *m_segments[getSegmentIndex(key)];

    // Create a new list node
    auto node = std::make_unique<ListNode>(key, objSize);

    // todo: use emplace_and_visit

    // Attempt to insert into the hash map
    if (!m_map.emplace(key, HashMapValue(value, std::move(node)))) {
      // Key already exists, do not insert
      // node will be deleted automatically when std::unique_ptr goes out of scope
      return false;
    }

    // Retrieve the node from the map
    m_map.cvisit(key, [&](const auto& accessor) {
      // Add the node to the LRU list
      std::unique_lock lock(segment.m_mutex);
      pushFront(segment, accessor.second.m_listNode.get());
      segment.m_size += objSize;
    });

    // Check if eviction is necessary
    if (segment.m_size > m_maxSizePerSegment) {
      evict(segment);
    }

    return true;
  }

  /**
   * Clear the container. NOT THREAD SAFE -- do not use while other threads
   * are accessing the container.
   */
  void clear()
  {
    // Clear the map first
    m_map.clear();

    // Then reset the segments
    for (auto& segmentPtr : m_segments) {
      auto& segment = *segmentPtr;
      // No need to lock since clear() is not thread-safe
      segment.m_head.m_next = &segment.m_tail;
      segment.m_tail.m_prev = &segment.m_head;
      segment.m_size = 0;
    }
  }

  /**
   * Get a snapshot of the keys in the container by copying them into the
   * supplied vector.
   */
  void snapshotKeys(std::vector<TKey>& keys)
  {
    for (const auto& segmentPtr : m_segments) {
      auto& segment = *segmentPtr;
      std::unique_lock lock(segment.m_mutex);
      for (ListNode* node = segment.m_head.m_next; node != &segment.m_tail; node = node->m_next) {
        keys.push_back(node->m_key);
      }
    }
  }

  /**
   * Get the approximate size of the container.
   */
  [[nodiscard]] size_t size() const
  {
    size_t totalSize = 0;
    for (const auto& segmentPtr : m_segments) {
      totalSize += segmentPtr->m_size;
    }
    return totalSize;
  }

private:
  /**
   * Get the segment index for a given key.
   */
  size_t getSegmentIndex(const TKey& key) const
  {
    if (m_numSegments == 1) {
      return 0;
    }

    boost::hash<TKey> hasher;
    // auto hashValue = hasher(key);
    // constexpr int shift = std::numeric_limits<size_t>::digits - 16;
    // return (hashValue >> shift + hashValue & 0xFFFF) % m_numSegments;
    return hasher(key) % m_numSegments;
  }

  /**
   * Unlink a node from the list. The caller must hold the segment's mutex.
   */
  void delink(ListNode* node)
  {
    if (node->m_inList) {
      node->m_prev->m_next = node->m_next;
      node->m_next->m_prev = node->m_prev;
      node->m_prev = nullptr;
      node->m_next = nullptr;
      node->m_inList = false;
    }
  }

  /**
   * Add a new node to the list in the most-recently used position. The caller
   * must hold the segment's mutex.
   */
  void pushFront(Segment& segment, ListNode* node)
  {
    if (node->m_inList) {
      // Node is already in the list, remove it first
      delink(node);
    }
    ListNode* oldHead = segment.m_head.m_next;
    node->m_prev = &segment.m_head;
    node->m_next = oldHead;
    oldHead->m_prev = node;
    segment.m_head.m_next = node;
    node->m_inList = true;
  }

  /**
   * Evict items from the segment until its size is within limits.
   */
  void evict(Segment& segment)
  {
    while (segment.m_size > m_maxSizePerSegment) {
      std::unique_lock lock(segment.m_mutex);
      ListNode* moribund = segment.m_tail.m_prev;
      if (moribund == &segment.m_head) {
        // List is empty, can't evict
        return;
      }
      delink(moribund);
      segment.m_size -= moribund->m_size;
      lock.unlock();
      // Remove from the hash map
      m_map.erase(moribund->m_key);
      // Node will be deleted when unique_ptr goes out of scope
    }
  }

  /**
   * The maximum allowed size of the cache.
   */
  size_t m_maxSize;

  /**
   * The number of segments.
   */
  size_t m_numSegments;

  /**
   * The maximum size per segment.
   */
  size_t m_maxSizePerSegment;

  /**
   * The segments of the cache.
   */
  std::vector<std::unique_ptr<Segment>> m_segments;

  /**
   * The underlying concurrent hash map.
   */
  HashMap m_map;

  bool m_canSkipDestructor;
};

} // namespace nim
