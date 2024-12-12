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

    ListNode(const TKey& k, size_t s)
      : key(k)
      , size(s)
    {}

    TKey key;
    size_t size = 0;
    bool inList = false;
    ListNode* prev = nullptr;
    ListNode* next = nullptr;
  };

  /**
   * The value that we store in the hashtable.
   */
  struct HashMapValue
  {
    HashMapValue() = default;

    HashMapValue(const TValue& value, std::unique_ptr<ListNode>&& node)
      : value(value)
      , listNode(std::move(node))
    {}

    TValue value;
    std::unique_ptr<ListNode> listNode;
  };

  using HashMap = boost::concurrent_flat_map<TKey, HashMapValue>;

  /**
   * Each segment contains its own LRU list and mutex.
   */
  struct Segment
  {
    Segment()
    {
      head.next = &tail;
      tail.prev = &head;
    }

    ListNode head;
    ListNode tail;
    std::mutex mutex;
    size_t size = 0;
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
    if (m_map.cvisit(key, [&](const auto& mapValue) {
          value = mapValue.second.value;
          if (findStrategy != FindStrategy::NoUpdateLRUList) {
            auto& segment = *m_segments[getSegmentIndex(key)];
            auto node = mapValue.second.listNode.get();
            if (findStrategy == FindStrategy::UpdateLRUList) {
              std::unique_lock lock(segment.mutex);
              if (node->inList) {
                delink(node);
                pushFront(segment, node);
              }
            } else if (findStrategy == FindStrategy::MaybeUpdateLRUList) {
              // Try to lock without blocking
              std::unique_lock lock(segment.mutex, std::try_to_lock);
              if (lock && node->inList) {
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
    m_map.erase_if(key, [&](const auto& mapValue) {
      auto& segment = *m_segments[getSegmentIndex(key)];
      std::unique_lock lock(segment.mutex);
      if (auto node = mapValue.second.listNode.get(); node->inList) {
        delink(node);
        segment.size -= node->size;
        // Node will be deleted when unique_ptr goes out of scope
      }
      lock.unlock();
      return true;
    });
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

    // Attempt to insert into the hash map
    if (!m_map.try_emplace_and_cvisit(
          key,
          value,
          std::make_unique<ListNode>(key, objSize),
          [&](const auto& mapValue) {
            // Add the new node to the LRU list
            std::unique_lock lock(segment.mutex);
            pushFront(segment, mapValue.second.listNode.get());
            segment.size += objSize;
          },
          [&](const auto&) {})) {
      // Key already exists, do not insert
      // node will be deleted automatically when std::unique_ptr goes out of scope
      return false;
    }

    // Check if eviction is necessary
    if (segment.size > m_maxSizePerSegment) {
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
      segment.head.next = &segment.tail;
      segment.tail.prev = &segment.head;
      segment.size = 0;
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
      std::unique_lock lock(segment.mutex);
      for (auto node = segment.head.next; node != &segment.tail; node = node->next) {
        keys.push_back(node->key);
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
      totalSize += segmentPtr->size;
    }
    return totalSize;
  }

private:
  /**
   * Get the segment index for a given key.
   */
  size_t getSegmentIndex(const TKey& key) const
  {
    boost::hash<TKey> hasher;
    // auto hashValue = hasher(key);
    // constexpr int shift = std::numeric_limits<size_t>::digits - 16;
    // return (hashValue >> shift + hashValue & 0xFFFF) % m_numSegments;
    return hasher(key) % m_numSegments;
  }

  /**
   * Unlink a node from the list. The caller must hold the segment's mutex.
   */
  static void delink(ListNode* node)
  {
    if (node->inList) {
      node->prev->next = node->next;
      node->next->prev = node->prev;
      node->prev = nullptr;
      node->next = nullptr;
      node->inList = false;
    }
  }

  /**
   * Add a new node to the list in the most-recently used position. The caller
   * must hold the segment's mutex.
   */
  void pushFront(Segment& segment, ListNode* node)
  {
    if (node->inList) {
      // Node is already in the list, remove it first
      delink(node);
    }
    auto oldHead = segment.head.next;
    node->prev = &segment.head;
    node->next = oldHead;
    oldHead->prev = node;
    segment.head.next = node;
    node->inList = true;
  }

  /**
   * Evict items from the segment until its size is within limits.
   */
  void evict(Segment& segment)
  {
    while (segment.size > m_maxSizePerSegment) {
      std::unique_lock lock(segment.mutex);
      auto moribund = segment.tail.prev;
      if (moribund == &segment.head) {
        // List is empty, can't evict
        return;
      }
      delink(moribund);
      segment.size -= moribund->size;
      lock.unlock();
      // Remove from the hash map
      m_map.erase(moribund->key);
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
