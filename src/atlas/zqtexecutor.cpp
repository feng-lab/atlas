#include "zqtexecutor.h"

#include "zlog.h"

#include <QMetaObject>
#include <QThread>

#include <memory>
#include <utility>

namespace nim {

ZQtExecutor::ZQtExecutor(QObject* target, std::string name)
  : m_target(target)
  , m_name(std::move(name))
{
  CHECK(m_target != nullptr) << "ZQtExecutor requires a non-null target QObject";
}

ZQtExecutor::~ZQtExecutor()
{
  // Folly keep-alive contract: if keepAliveAcquire() returns true, the caller
  // expects the executor to remain valid until all KeepAlive tokens are released.
  const uint32_t alive = m_keepAliveCount.load(std::memory_order_acquire);
  CHECK(alive == 0u) << "ZQtExecutor '" << m_name << "' destroyed with outstanding KeepAlive tokens";
}

void ZQtExecutor::add(folly::Func fn)
{
  if (!fn) {
    return;
  }

  QObject* obj = m_target.data();
  CHECK(obj != nullptr) << "ZQtExecutor '" << m_name << "' target QObject is gone";

  // If we're already on the target thread, run inline. This avoids deadlocks
  // during teardown (the render thread can block while waiting for Vulkan
  // fences, so work posted to its Qt event loop may never run).
  if (QThread::currentThread() == obj->thread()) {
    fn();
    return;
  }

  // QMetaObject::invokeMethod stores a copy of the functor, so ensure the
  // capture is copyable (folly::Func is move-only).
  auto fnPtr = std::make_shared<folly::Func>(std::move(fn));
  const bool queued = QMetaObject::invokeMethod(
    obj,
    [fnPtr]() mutable {
      if (*fnPtr) {
        (*fnPtr)();
      }
    },
    Qt::QueuedConnection);
  CHECK(queued) << "ZQtExecutor '" << m_name << "' failed to queue task via QMetaObject::invokeMethod";
}

bool ZQtExecutor::keepAliveAcquire() noexcept
{
  m_keepAliveCount.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

void ZQtExecutor::keepAliveRelease() noexcept
{
  const uint32_t prev = m_keepAliveCount.fetch_sub(1u, std::memory_order_acq_rel);
  CHECK(prev > 0u) << "ZQtExecutor '" << m_name << "' keep-alive underflow";
}

} // namespace nim
