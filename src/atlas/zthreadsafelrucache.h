#pragma once

// adapt from Tim Starling's lru-cache.h

#include <tbb/concurrent_hash_map.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace nim {

/**
 * ZThreadSafeLRUCache is a thread-safe hashtable with a limited size. When
 * it is full, insert() evicts the least recently used item from the cache.
 *
 * The find() operation fills a ConstAccessor object, which is a smart pointer
 * similar to TBB's const_accessor. After eviction, destruction of the value is
 * deferred until all ConstAccessor objects are destroyed.
 *
 * The implementation is generally conservative, relying on the documented
 * behaviour of tbb::concurrent_hash_map. LRU list transactions are protected
 * with a single mutex. Having our own doubly-linked list implementation helps
 * to ensure that list transactions are sufficiently brief, consisting of only
 * a few loads and stores. User code is not executed while the lock is held.
 *
 * The acquisition of the list mutex during find() is non-blocking (try_lock),
 * so under heavy lookup load, the container will not stall, instead some LRU
 * update operations will be omitted.
 *
 * Insert performance was observed to degrade rapidly when there is a heavy
 * concurrent insert/evict load, mostly due to locks in the underlying
 * TBB::CHM. So if that is a possibility for your workload,
 * ZThreadSafeScalableCache is recommended instead.
 */
template<class TKey, class TValue, class THash = tbb::tbb_hash_compare<TKey>>
class ZThreadSafeLRUCache
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

  using HashMap = tbb::concurrent_hash_map<TKey, HashMapValue, THash>;
  using HashMapConstAccessor = typename HashMap::const_accessor;
  using HashMapAccessor = typename HashMap::accessor;

public:
  /**
   * Create a container with a given maximum size
   */
  explicit ZThreadSafeLRUCache(size_t maxSize, bool canSkipDestructor = false)
    : m_maxSize(maxSize)
    , m_size(0)
    , m_map(std::thread::hardware_concurrency() * 4) // it will automatically grow
    , m_canSkipDestructor(canSkipDestructor)
  {
    m_head.m_next = &m_tail;
    m_tail.m_prev = &m_head;
  }

  ZThreadSafeLRUCache(const ZThreadSafeLRUCache& other) = delete;

  ZThreadSafeLRUCache& operator=(const ZThreadSafeLRUCache&) = delete;

  ~ZThreadSafeLRUCache()
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
   * Find a value by key, and returns optional contained the value if the element was found, empty optional
   * otherwise. Updates the eviction list, making the element the most-recently used.
   */
  std::optional<TValue> find(const TKey& key, FindStrategy findStrategy = FindStrategy::UpdateLRUList)
  {
    HashMapConstAccessor hashAccessor;
    if (!m_map.find(hashAccessor, key)) {
      return {};
    }

    if (findStrategy == FindStrategy::UpdateLRUList) {
      std::unique_lock lock(m_listMutex);
      // The list node may be out of the list if it is in the process of being
      // inserted or evicted. Doing this check allows us to lock the list for
      // shorter periods of time.
      if (ListNode* node = hashAccessor->second.m_listNode.get(); node->m_inList) {
        delink(node);
        pushFront(node);
      }
    } else if (findStrategy == FindStrategy::MaybeUpdateLRUList) {
      // Acquire the lock, but don't block if it is already held
      std::unique_lock lock(m_listMutex, std::try_to_lock);
      if (lock) {
        // The list node may be out of the list if it is in the process of being
        // inserted or evicted. Doing this check allows us to lock the list for
        // shorter periods of time.
        if (ListNode* node = hashAccessor->second.m_listNode.get(); node->m_inList) {
          delink(node);
          pushFront(node);
        }
      }
    }
    return hashAccessor->second.m_value;
  }

  void remove(const TKey& key)
  {
    HashMapAccessor hashAccessor;
    if (!m_map.find(hashAccessor, key)) {
      return;
    }

    std::unique_lock lock(m_listMutex);
    // The list node may be out of the list if it is in the process of being
    // inserted or evicted. Doing this check allows us to lock the list for
    // shorter periods of time.
    if (ListNode* node = hashAccessor->second.m_listNode.get(); node->m_inList) {
      delink(node);
      m_size.fetch_sub(node->m_size);
    }
    lock.unlock();

    m_map.erase(hashAccessor);
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
    // Create a new list node
    auto node = std::make_unique<ListNode>(key, objSize);
    auto nodep = node.get();
    HashMapAccessor hashAccessor;
    // Attempt to insert into the hash map
    if (!m_map.emplace(hashAccessor, key, HashMapValue(value, std::move(node)))) {
      // Key already exists, do not insert
      // node will be deleted automatically when std::unique_ptr goes out of scope
      return false;
    }

    // Evict if necessary, now that we know the hashmap insertion was successful.
    // size_t size = m_size.load();
    // bool evictionDone = false;
    // if (size >= m_maxSize) {
    // The container is at (or over) capacity, so eviction needs to be done.
    // Do not decrement m_size, since that would cause other threads to
    // inappropriately omit eviction during their own inserts.
    // evict();
    // evictionDone = true;
    //}

    // Note that we have to update the LRU list before we increment m_size, so
    // that other threads don't attempt to evict list items before they even
    // exist.

    std::unique_lock lock(m_listMutex);
    pushFront(nodep);
    lock.unlock();
    hashAccessor.release();
    //  if (!evictionDone) {
    //    size = m_size++;
    //  }
    if (auto size = m_size.fetch_add(objSize, std::memory_order_release); size > m_maxSize) {
      // It is possible for the size to temporarily exceed the maximum if there is
      // a heavy insert() load, once only as the cache fills. In this situation,
      // we have to be careful not to have every thread simultaneously attempt to
      // evict the extra entries, since we could end up underfilled. Instead we do
      // a compare-and-exchange to acquire an exclusive right to reduce the size
      // to a particular value.
      //
      // We could continue to evict in a loop, but if there are a lot of threads
      // here at the same time, that could lead to spinning. So we will just evict
      // one extra element per insert() until the overfill is rectified.
      // if (m_size.compare_exchange_strong(size, size)) {
      evict();
      // }
    }
    return true;
  }

  /**
   * Clear the container. NOT THREAD SAFE -- do not use while other threads
   * are accessing the container.
   */
  void clear()
  {
    m_map.clear();
    m_head.m_next = &m_tail;
    m_tail.m_prev = &m_head;
    m_size = 0;
  }

  /**
   * Get a snapshot of the keys in the container by copying them into the
   * supplied vector. This will block inserts and prevent LRU updates while it
   * completes. The keys will be inserted in order from most-recently used to
   * least-recently used.
   */
  void snapshotKeys(std::vector<TKey>& keys)
  {
    keys.reserve(keys.size() + m_size.load());
    std::scoped_lock lock(m_listMutex);
    for (ListNode* node = m_head.m_next; node != &m_tail; node = node->m_next) {
      keys.push_back(node->m_key);
    }
  }

  /**
   * Get the approximate size of the container. May be slightly too low when
   * insertion is in progress.
   */
  size_t size() const
  {
    return m_size.load();
  }

private:
  /**
   * Unlink a node from the list. The caller must lock the list mutex while
   * this is called.
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
   * must lock the list mutex while this is called.
   */
  void pushFront(ListNode* node)
  {
    if (node->m_inList) {
      // Node is already in the list, remove it first
      delink(node);
    }
    ListNode* oldHead = m_head.m_next;
    node->m_prev = &m_head;
    node->m_next = oldHead;
    oldHead->m_prev = node;
    m_head.m_next = node;
    node->m_inList = true;
  }

  /**
   * Evict the least-recently used item from the container. This function does
   * its own locking.
   */
  void evict()
  {
    while (m_size.load(std::memory_order_acquire) > m_maxSize) {
      std::unique_lock lock(m_listMutex);
      ListNode* moribund = m_tail.m_prev;
      if (moribund == &m_head) {
        // List is empty, can't evict
        return;
      }
      delink(moribund);
      m_size.fetch_sub(moribund->m_size, std::memory_order_relaxed);
      lock.unlock();

      //    HashMapAccessor hashAccessor;
      //    if (!m_map.find(hashAccessor, moribund->m_key)) {
      //      // Presumably unreachable
      //      continue;
      //    }
      //    m_map.erase(hashAccessor);
      m_map.erase(moribund->m_key);
      // m_size.fetch_sub(moribund->m_size, std::memory_order_relaxed);
      // delete moribund;
    }
  }

  /**
   * The maximum number of elements in the container.
   */
  size_t m_maxSize;

  /**
   * This atomic variable is used to signal to all threads whether or not
   * eviction should be done on insert. It is approximately equal to the
   * number of elements in the container.
   */
  std::atomic<size_t> m_size;

  /**
   * The underlying TBB hash map.
   */
  HashMap m_map;

  /**
   * The linked list. The "head" is the most-recently used node, and the
   * "tail" is the least-recently used node. The list mutex must be held
   * during both read and write.
   */
  ListNode m_head;
  ListNode m_tail;
  std::mutex m_listMutex;

  bool m_canSkipDestructor;
};

} // namespace nim
