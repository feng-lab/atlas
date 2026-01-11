#pragma once

#include "zsqliteasyncwritequeue.h"
#include "zsqlitelrucache.h"
#include "zsqlitereadonlypool.h"

#include <QString>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace nim {

// Shared SQLite-backed disk-cache bucket infrastructure used by multiple Atlas
// caches (HTTP byte cache, image-region cache, etc.).
//
// Responsibilities:
// - Open/initialize the bucket directory and SQLite DB (writer connection).
// - Provide per-thread read-only connections for synchronous lookups.
// - Provide an async writer queue for best-effort cache mutations (put/erase/touch).
//
// Best-effort boundary:
// - Any filesystem or SQLite error should degrade to cache-miss semantics. Call
//   sites should remain functional with memory-only caches.
class ZSqliteDiskCacheBucket
{
public:
  using KeyHash32 = std::array<std::uint8_t, 32>;
  using Blob = ZSqliteLRUCache::Blob;
  using TaskFn = ZSqliteAsyncWriteQueue::TaskFn;

  ZSqliteDiskCacheBucket(QString rootDir,
                         QString dbFileName,
                         uint64_t maxBytes,
                         uint64_t asyncMaxPendingBytes,
                         QString debugName);
  ~ZSqliteDiskCacheBucket();

  ZSqliteDiskCacheBucket(const ZSqliteDiskCacheBucket&) = delete;
  ZSqliteDiskCacheBucket& operator=(const ZSqliteDiskCacheBucket&) = delete;

  [[nodiscard]] bool isEnabled() const;

  [[nodiscard]] QString dbPath() const;

  [[nodiscard]] uint64_t maxBytes() const;

  [[nodiscard]] std::optional<ZSqliteLRUCache::GetNoTouchResult> tryGetNoTouch(std::span<const std::uint8_t> keyHash) const;

  void tryEnqueueErase(std::span<const std::uint8_t> keyHash) const;

  void tryEnqueueTouchIfStale(std::span<const std::uint8_t> keyHash,
                              int64_t lastAccessNs,
                              int64_t nowNs,
                              std::chrono::nanoseconds minInterval) const;

  // Enqueues an arbitrary SQLite task after copying the 32-byte key hash.
  //
  // The provided factory is invoked only if the async queue accepts the task
  // (i.e., after reserving pending-bytes). This enables call sites to avoid
  // expensive copies when the queue is full by deferring those copies into the
  // factory body.
  //
  // The factory signature should be:
  //   TaskFn factory(const KeyHash32& keyHashArr)
  template<typename TaskFactory>
  void tryEnqueueTaskWithFactory(std::span<const std::uint8_t> keyHash,
                                 uint64_t estimatedBytes,
                                 TaskFactory&& taskFactory) const
  {
    const auto writeQueue = m_writeQueue;
    if (!isEnabled() || !writeQueue) {
      return;
    }
    const auto keyArrOpt = copyKeyHash32(keyHash);
    if (!keyArrOpt.has_value()) {
      return;
    }

    (void)writeQueue->tryEnqueueWithFactory(
      estimatedBytes,
      [keyArr = *keyArrOpt, taskFactory = std::forward<TaskFactory>(taskFactory)]() mutable -> TaskFn {
        return taskFactory(keyArr);
      });
  }

  // Enqueue a cache put. The serialization work is deferred to the writer thread
  // by providing a factory that returns the value bytes.
  template<typename ValueFactory>
  void tryEnqueuePutValueWithFactory(std::span<const std::uint8_t> keyHash,
                                     uint64_t estimatedBytes,
                                     ValueFactory&& valueFactory) const
  {
    const auto writeQueue = m_writeQueue;
    if (!isEnabled() || !writeQueue) {
      return;
    }
    const auto keyArrOpt = copyKeyHash32(keyHash);
    if (!keyArrOpt.has_value()) {
      return;
    }

    (void)writeQueue->tryEnqueueWithFactory(
      estimatedBytes,
      [keyArr = *keyArrOpt, valueFactory = std::forward<ValueFactory>(valueFactory)]() mutable -> ZSqliteAsyncWriteQueue::TaskFn {
        return [keyArr, valueFactory = std::move(valueFactory)](ZSqliteLRUCache& cache) mutable {
          const Blob value = valueFactory();
          if (value.empty()) {
            return;
          }
          cache.put(std::span<const std::uint8_t>(keyArr.data(), keyArr.size()),
                    std::span<const std::uint8_t>(value.data(), value.size()));
        };
      });
  }

  [[nodiscard]] bool drainWrites(std::chrono::milliseconds timeout) const;

private:
  [[nodiscard]] static std::optional<KeyHash32> copyKeyHash32(std::span<const std::uint8_t> keyHash);

private:
  QString m_rootDir;
  QString m_cacheDir;
  QString m_dbPath;
  uint64_t m_maxBytes = 0;

  std::atomic<bool> m_enabled{false};
  std::shared_ptr<ZSqliteReadOnlyPool> m_readPool;
  std::shared_ptr<ZSqliteAsyncWriteQueue> m_writeQueue;
};

} // namespace nim
