#pragma once

#include "zlog.h"
#include "zfolly.h"

#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace nim {

class ZSqliteLRUCache;

// ZSqliteAsyncWriteQueue serializes blocking SQLite operations onto a dedicated
// background thread and provides best-effort drop-on-overload semantics.
//
// This is intended for *cache* workloads:
// - Reads remain synchronous at call sites.
// - Writes (put/erase/touch/prune) are queued so callers don't block on disk I/O.
// - The queue is bounded by an approximate "pending bytes" budget; when exceeded,
//   new tasks are dropped (best-effort cache semantics).
class ZSqliteAsyncWriteQueue
{
public:
  using TaskFn = folly::Function<void(ZSqliteLRUCache&)>;

  struct Stats
  {
    uint64_t enqueuedOps = 0;
    uint64_t droppedOps = 0;
    uint64_t droppedBytes = 0;
  };

  ZSqliteAsyncWriteQueue(std::unique_ptr<ZSqliteLRUCache> cache,
                         uint64_t maxPendingBytes,
                         QString debugName);
  ~ZSqliteAsyncWriteQueue();

  ZSqliteAsyncWriteQueue(const ZSqliteAsyncWriteQueue&) = delete;
  ZSqliteAsyncWriteQueue& operator=(const ZSqliteAsyncWriteQueue&) = delete;

  [[nodiscard]] bool isEnabled() const;

  // Attempts to enqueue a task. Returns false if the queue is disabled or full.
  [[nodiscard]] bool tryEnqueue(TaskFn fn, uint64_t estimatedBytes);

  // Like tryEnqueue, but constructs the task only if it will be accepted.
  // This avoids copying large payloads when the queue is already full.
  template<typename TaskFactory>
  [[nodiscard]] bool tryEnqueueWithFactory(uint64_t estimatedBytes, TaskFactory&& factory)
  {
    if (!tryReserveBytes(estimatedBytes)) {
      return false;
    }

    TaskFn fn;
    try {
      fn = factory();
    } catch (...) {
      releaseBytes(estimatedBytes);
      return false;
    }
    if (!fn) {
      releaseBytes(estimatedBytes);
      return false;
    }

    if (!scheduleReserved(std::move(fn), estimatedBytes)) {
      releaseBytes(estimatedBytes);
      return false;
    }

    m_enqueuedOps.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Requests stop and drains remaining tasks best-effort.
  void stop();

  [[nodiscard]] Stats stats() const;

  // Blocks until all tasks enqueued before this call have completed, or until
  // the timeout expires. This is intended for tests and shutdown sequences.
  [[nodiscard]] bool drain(std::chrono::milliseconds timeout);

private:
  [[nodiscard]] bool tryReserveBytes(uint64_t bytes);
  void releaseBytes(uint64_t bytes);
  [[nodiscard]] bool scheduleReserved(TaskFn fn, uint64_t bytes);

  void recordDrop(uint64_t bytes, const char* reason);

private:
  QString m_debugName;
  uint64_t m_maxPendingBytes = 0;

  std::atomic<uint64_t> m_pendingBytes{0};
  std::atomic<bool> m_enabled{false};

  // Serializes stop() vs concurrent enqueue/scheduling.
  mutable std::mutex m_mu;

  std::unique_ptr<ZSqliteLRUCache> m_cache;
  std::unique_ptr<folly::CPUThreadPoolExecutor> m_executor;

  std::atomic<uint64_t> m_enqueuedOps{0};
  std::atomic<uint64_t> m_droppedOps{0};
  std::atomic<uint64_t> m_droppedBytes{0};
};

} // namespace nim
