#include "zconcurrentlrucache.h"
#include "zexception.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

using Cache = nim::ZConcurrentLRUCache<int, int>;

class IntDiskBackend final : public Cache::DiskBackend
{
public:
  struct Entry
  {
    int value = 0;
    size_t objSize = 0;
  };

  [[nodiscard]] std::optional<Cache::DiskLookupResult> tryGet(const int& key) override
  {
    ++getCount;
    const auto it = entries.find(key);
    if (it == entries.end()) {
      return {};
    }
    return Cache::DiskLookupResult{it->second.value, it->second.objSize};
  }

  void put(const int& key, const int& value, size_t objSize) override
  {
    ++putCount;
    entries[key] = Entry{value, objSize};
  }

  void erase(const int& key) override
  {
    ++eraseCount;
    entries.erase(key);
  }

  std::map<int, Entry> entries;
  int getCount = 0;
  int putCount = 0;
  int eraseCount = 0;
};

class ThrowingDiskBackend final : public Cache::DiskBackend
{
public:
  [[nodiscard]] std::optional<Cache::DiskLookupResult> tryGet(const int&) override
  {
    throw nim::ZException("disk get failed");
  }

  void put(const int&, const int&, size_t) override
  {
    throw nim::ZException("disk put failed");
  }

  void erase(const int&) override
  {
    throw nim::ZException("disk erase failed");
  }
};

} // namespace

TEST(ConcurrentLRUCache, InsertFindRemoveClearAndSnapshot)
{
  Cache cache(10, 1);

  EXPECT_TRUE(cache.insert(1, 10, 1));
  EXPECT_TRUE(cache.insert(2, 20, 1));
  EXPECT_TRUE(cache.insert(3, 30, 1));

  ASSERT_TRUE(cache.find(1).has_value());
  EXPECT_EQ(cache.find(1).value(), 10);
  ASSERT_TRUE(cache.find(2).has_value());
  EXPECT_EQ(cache.find(2).value(), 20);

  std::vector<int> keys;
  cache.snapshotKeys(keys);
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(keys, (std::vector<int>{1, 2, 3}));

  cache.remove(2);
  EXPECT_FALSE(cache.find(2).has_value());
  EXPECT_TRUE(cache.find(1).has_value());
  EXPECT_TRUE(cache.find(3).has_value());

  cache.clear();
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.find(1).has_value());
  EXPECT_FALSE(cache.find(3).has_value());
}

TEST(ConcurrentLRUCache, DuplicateInsertDoesNotOverwrite)
{
  Cache cache(4, 1);

  EXPECT_TRUE(cache.insert(1, 10, 1));
  EXPECT_FALSE(cache.insert(1, 20, 1));

  ASSERT_TRUE(cache.find(1).has_value());
  EXPECT_EQ(cache.find(1).value(), 10);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(ConcurrentLRUCache, EvictsLeastRecentlyUsedEntry)
{
  Cache cache(2, 1);

  EXPECT_TRUE(cache.insert(1, 10, 1));
  EXPECT_TRUE(cache.insert(2, 20, 1));
  EXPECT_TRUE(cache.insert(3, 30, 1));

  EXPECT_FALSE(cache.find(1).has_value());
  ASSERT_TRUE(cache.find(2).has_value());
  EXPECT_EQ(cache.find(2).value(), 20);
  ASSERT_TRUE(cache.find(3).has_value());
  EXPECT_EQ(cache.find(3).value(), 30);
  EXPECT_LE(cache.size(), 2u);
}

TEST(ConcurrentLRUCache, UpdateFindStrategyRefreshesLRUOrder)
{
  Cache cache(2, 1);

  EXPECT_TRUE(cache.insert(1, 10, 1));
  EXPECT_TRUE(cache.insert(2, 20, 1));
  ASSERT_TRUE(cache.find(1, Cache::FindStrategy::UpdateLRUList).has_value());
  EXPECT_TRUE(cache.insert(3, 30, 1));

  ASSERT_TRUE(cache.find(1).has_value());
  EXPECT_EQ(cache.find(1).value(), 10);
  EXPECT_FALSE(cache.find(2).has_value());
  ASSERT_TRUE(cache.find(3).has_value());
  EXPECT_EQ(cache.find(3).value(), 30);
}

TEST(ConcurrentLRUCache, NoUpdateFindStrategyDoesNotRefreshLRUOrder)
{
  Cache cache(2, 1);

  EXPECT_TRUE(cache.insert(1, 10, 1));
  EXPECT_TRUE(cache.insert(2, 20, 1));
  ASSERT_TRUE(cache.find(1, Cache::FindStrategy::NoUpdateLRUList).has_value());
  EXPECT_TRUE(cache.insert(3, 30, 1));

  EXPECT_FALSE(cache.find(1).has_value());
  ASSERT_TRUE(cache.find(2).has_value());
  EXPECT_EQ(cache.find(2).value(), 20);
  ASSERT_TRUE(cache.find(3).has_value());
  EXPECT_EQ(cache.find(3).value(), 30);
}

TEST(ConcurrentLRUCache, EvictionUsesObjectSize)
{
  Cache cache(3, 1);

  EXPECT_TRUE(cache.insert(1, 10, 2));
  EXPECT_TRUE(cache.insert(2, 20, 2));

  EXPECT_FALSE(cache.find(1).has_value());
  ASSERT_TRUE(cache.find(2).has_value());
  EXPECT_EQ(cache.find(2).value(), 20);
  EXPECT_LE(cache.size(), 3u);
}

TEST(ConcurrentLRUCache, DiskHitReportsSourceAndPromotesToMemory)
{
  Cache cache(2, 1);
  auto backend = std::make_unique<IntDiskBackend>();
  auto* backendPtr = backend.get();
  backendPtr->entries[7] = IntDiskBackend::Entry{70, 1};
  cache.setDiskBackend(std::move(backend));

  const auto diskHit = cache.findWithSource(7);
  ASSERT_TRUE(diskHit.has_value());
  EXPECT_EQ(diskHit->value, 70);
  EXPECT_EQ(diskHit->source, Cache::FindSource::Disk);
  EXPECT_EQ(backendPtr->getCount, 1);

  backendPtr->entries.clear();
  const auto memoryHit = cache.findWithSource(7);
  ASSERT_TRUE(memoryHit.has_value());
  EXPECT_EQ(memoryHit->value, 70);
  EXPECT_EQ(memoryHit->source, Cache::FindSource::Memory);
  EXPECT_EQ(backendPtr->getCount, 1);
}

TEST(ConcurrentLRUCache, InsertAndRemoveMirrorToDiskBackend)
{
  Cache cache(2, 1);
  auto backend = std::make_unique<IntDiskBackend>();
  auto* backendPtr = backend.get();
  cache.setDiskBackend(std::move(backend));

  EXPECT_TRUE(cache.insert(4, 40, 1));
  EXPECT_EQ(backendPtr->putCount, 1);
  ASSERT_NE(backendPtr->entries.find(4), backendPtr->entries.end());
  EXPECT_EQ(backendPtr->entries[4].value, 40);
  EXPECT_EQ(backendPtr->entries[4].objSize, 1u);

  cache.remove(4);
  EXPECT_EQ(backendPtr->eraseCount, 1);
  EXPECT_EQ(backendPtr->entries.find(4), backendPtr->entries.end());
}

TEST(ConcurrentLRUCache, DiskBackendExceptionsAreBestEffortFailures)
{
  Cache cache(2, 1);
  cache.setDiskBackend(std::make_unique<ThrowingDiskBackend>());

  EXPECT_FALSE(cache.find(1).has_value());
  EXPECT_NO_THROW({ EXPECT_TRUE(cache.insert(1, 10, 1)); });
  EXPECT_NO_THROW({ cache.remove(1); });
}

TEST(ConcurrentLRUCache, ConcurrentMixedOperationsKeepValidValuesAndCapacity)
{
  constexpr int kThreadCount = 4;
  constexpr int kOperationCount = 4000;
  constexpr int kKeyRange = 512;
  constexpr size_t kMaxCacheSize = 4096;

  Cache cache(kMaxCacheSize, 8);
  std::atomic<bool> sawBadValue{false};
  std::atomic<int> hits{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int threadId = 0; threadId < kThreadCount; ++threadId) {
    threads.emplace_back([&cache, &sawBadValue, &hits, threadId]() {
      std::mt19937 rng(threadId);
      std::uniform_int_distribution<int> opDist(0, 2);
      std::uniform_int_distribution<int> keyDist(0, kKeyRange - 1);

      for (int i = 0; i < kOperationCount; ++i) {
        const int key = keyDist(rng);
        const int value = key * 10;
        switch (opDist(rng)) {
          case 0:
            cache.insert(key, value, sizeof(value));
            break;
          case 1:
            if (const auto result = cache.find(key, Cache::FindStrategy::MaybeUpdateLRUList); result.has_value()) {
              ++hits;
              if (result.value() != value) {
                sawBadValue.store(true, std::memory_order_relaxed);
              }
            }
            break;
          case 2:
            cache.remove(key);
            break;
          default:
            sawBadValue.store(true, std::memory_order_relaxed);
            break;
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(sawBadValue.load(std::memory_order_relaxed));
  EXPECT_LE(cache.size(), kMaxCacheSize);
}
