#include "zsqliteasyncwritequeue.h"

#include "zsqlitelrucache.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#include <folly/synchronization/Baton.h>

namespace nim {

namespace {

constexpr std::chrono::seconds kDropLogMinInterval{10};

} // namespace

ZSqliteAsyncWriteQueue::ZSqliteAsyncWriteQueue(std::unique_ptr<ZSqliteLRUCache> cache,
                                               uint64_t maxPendingBytes,
                                               QString debugName)
  : m_debugName(std::move(debugName))
  , m_maxPendingBytes(maxPendingBytes)
  , m_cache(std::move(cache))
{
  m_debugName = m_debugName.trimmed();
  if (m_debugName.isEmpty()) {
    m_debugName = QStringLiteral("<unnamed>");
  }

  if (!m_cache || !m_cache->isOpen() || m_maxPendingBytes == 0) {
    m_cache.reset();
    return;
  }

  auto threadFactory =
    std::make_shared<folly::NamedThreadFactory>(QStringLiteral("SQLiteCacheWriter_%1_").arg(m_debugName).toStdString());
  m_executor = std::make_unique<folly::CPUThreadPoolExecutor>(/*numThreads=*/1, std::move(threadFactory));

  m_enabled.store(true, std::memory_order_release);
}

ZSqliteAsyncWriteQueue::~ZSqliteAsyncWriteQueue()
{
  stop();
}

bool ZSqliteAsyncWriteQueue::isEnabled() const
{
  return m_enabled.load(std::memory_order_acquire);
}

bool ZSqliteAsyncWriteQueue::tryEnqueue(TaskFn fn, uint64_t estimatedBytes)
{
  return tryEnqueueWithFactory(estimatedBytes, [fn = std::move(fn)]() mutable { return std::move(fn); });
}

void ZSqliteAsyncWriteQueue::stop()
{
  std::unique_ptr<folly::CPUThreadPoolExecutor> executor;
  {
    std::scoped_lock lock(m_mu);
    if (!m_enabled.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    executor = std::move(m_executor);
  }

  if (executor) {
    executor->stop();
    executor->join();
  }

  std::scoped_lock lock(m_mu);
  m_cache.reset();
  m_pendingBytes.store(0, std::memory_order_release);
}

ZSqliteAsyncWriteQueue::Stats ZSqliteAsyncWriteQueue::stats() const
{
  Stats out;
  out.enqueuedOps = m_enqueuedOps.load(std::memory_order_relaxed);
  out.droppedOps = m_droppedOps.load(std::memory_order_relaxed);
  out.droppedBytes = m_droppedBytes.load(std::memory_order_relaxed);
  return out;
}

bool ZSqliteAsyncWriteQueue::drain(std::chrono::milliseconds timeout)
{
  if (!isEnabled()) {
    return false;
  }

  folly::Baton<> baton;
  const bool queued = tryEnqueue([&baton](ZSqliteLRUCache&) { baton.post(); }, /*estimatedBytes=*/0);
  if (!queued) {
    return false;
  }
  return baton.try_wait_for(timeout);
}

bool ZSqliteAsyncWriteQueue::tryReserveBytes(uint64_t bytes)
{
  if (!isEnabled()) {
    return false;
  }

  if (bytes == 0) {
    return true;
  }

  if (bytes > m_maxPendingBytes) {
    recordDrop(bytes, "single_task_exceeds_budget");
    return false;
  }

  while (true) {
    uint64_t current = m_pendingBytes.load(std::memory_order_relaxed);
    if (current > m_maxPendingBytes - bytes) {
      recordDrop(bytes, "queue_budget_exceeded");
      return false;
    }
    if (m_pendingBytes.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
      return true;
    }
  }
}

void ZSqliteAsyncWriteQueue::releaseBytes(uint64_t bytes)
{
  if (bytes == 0) {
    return;
  }
  m_pendingBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

bool ZSqliteAsyncWriteQueue::scheduleReserved(TaskFn fn, uint64_t bytes)
{
  if (!fn) {
    return false;
  }

  std::scoped_lock lock(m_mu);
  if (!m_enabled.load(std::memory_order_acquire) || !m_executor || !m_cache) {
    return false;
  }

  ZSqliteLRUCache* cachePtr = m_cache.get();
  CHECK(cachePtr != nullptr);

  // Serialize all operations for a DB through a single executor thread.
  m_executor->add([this, cachePtr, bytes, fn = std::move(fn)]() mutable {
    auto release = [&]() { releaseBytes(bytes); };

    // If we're stopping, drop work without touching the DB.
    if (!isEnabled()) {
      release();
      return;
    }

    if (!cachePtr->isOpen()) {
      release();
      return;
    }

    try {
      fn(*cachePtr);
    } catch (...) {
      // Best-effort: treat as no-op.
      static std::mutex dropMu;
      static std::chrono::steady_clock::time_point lastDropLog{};
      const auto now = std::chrono::steady_clock::now();
      {
        std::scoped_lock lg(dropMu);
        if (lastDropLog.time_since_epoch() == std::chrono::steady_clock::duration::zero() ||
            (now - lastDropLog) >= kDropLogMinInterval) {
          lastDropLog = now;
          VLOG(1) << "SQLite async write task failed (ignored): cache=" << m_debugName;
        }
      }
    }

    release();
  });

  return true;
}

void ZSqliteAsyncWriteQueue::recordDrop(uint64_t bytes, const char* reason)
{
  m_droppedOps.fetch_add(1, std::memory_order_relaxed);
  m_droppedBytes.fetch_add(bytes, std::memory_order_relaxed);
  VLOG(2) << "SQLite async write dropped task: cache=" << m_debugName
          << " reason=" << (reason ? reason : "<unknown>")
          << " bytes=" << bytes
          << " pendingBytes=" << m_pendingBytes.load(std::memory_order_relaxed)
          << " maxPendingBytes=" << m_maxPendingBytes;
}

} // namespace nim
