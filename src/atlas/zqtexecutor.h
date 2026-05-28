#pragma once

#include "zfolly.h"

#include <QPointer>

#include <atomic>
#include <cstdint>
#include <string>

class QObject;

namespace nim {

// A folly::Executor that schedules work onto a Qt thread via QMetaObject::invokeMethod().
//
// This is the foundational primitive for expressing render-thread continuations
// (e.g. "after fence") as folly::coro flows without hand-rolled callback chains.
//
// Threading:
// - add() is thread-safe; the scheduled work runs on the target QObject's thread.
// - This does NOT make the target object itself thread-safe; it only provides a
//   safe way to hop work onto the object's owning thread.
class ZQtExecutor final : public folly::Executor
{
public:
  explicit ZQtExecutor(QObject* target, std::string name = {});
  ~ZQtExecutor() override;

  void add(folly::Func fn) override;

  [[nodiscard]] QObject* target() const noexcept
  {
    return m_target.data();
  }
  [[nodiscard]] const std::string& name() const noexcept
  {
    return m_name;
  }

protected:
  bool keepAliveAcquire() noexcept override;
  void keepAliveRelease() noexcept override;

private:
  QPointer<QObject> m_target;
  std::string m_name;
  std::atomic<uint32_t> m_keepAliveCount{0};
};

} // namespace nim
