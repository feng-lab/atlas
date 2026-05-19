#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nim {

template<class T>
struct ZQObjectThreadInvokeResult
{
  bool ok = false;
  T value{};
  std::string error;
};

template<>
struct ZQObjectThreadInvokeResult<void>
{
  bool ok = false;
  std::string error;
};

namespace detail {

template<class T>
struct ZQObjectThreadInvokeState
{
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> cancelled{false};
  bool done = false;
  std::string error;
  std::optional<T> value;
};

template<>
struct ZQObjectThreadInvokeState<void>
{
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> cancelled{false};
  bool done = false;
  std::string error;
};

} // namespace detail

// Run work on a QObject's thread using a queued invocation and wait for completion.
//
// Compared to Qt::BlockingQueuedConnection, this helper fails cleanly when:
// - the target object is destroyed, or
// - its QThread finishes (e.g., window closed / engine shutdown).
//
// Cancellation is best-effort:
// - The waiting thread can provide `shouldCancel`, which is polled while waiting.
// - The queued work checks an atomic flag before executing.
//
// IMPORTANT: This function is intended for short computations or state snapshots.
// Long-running work should be restructured to avoid blocking the calling thread.
template<class Func>
auto invokeOnObjectThreadWait(QObject* obj,
                              Func&& f,
                              std::string_view what,
                              std::function<bool()> shouldCancel = nullptr,
                              std::chrono::milliseconds cancelPoll = std::chrono::milliseconds(10))
  -> ZQObjectThreadInvokeResult<decltype(f())>
{
  using R = decltype(f());
  if (!obj) {
    ZQObjectThreadInvokeResult<R> out;
    out.ok = false;
    out.error = std::string(what) + ": target not ready";
    return out;
  }

  QThread* targetThread = obj->thread();
  if (!targetThread) {
    ZQObjectThreadInvokeResult<R> out;
    out.ok = false;
    out.error = std::string(what) + ": target has no thread";
    return out;
  }

  // Fast-path: already on the target thread.
  if (QThread::currentThread() == targetThread) {
    if constexpr (std::is_void_v<R>) {
      f();
      ZQObjectThreadInvokeResult<void> out;
      out.ok = true;
      return out;
    } else {
      ZQObjectThreadInvokeResult<R> out;
      out.ok = true;
      out.value = f();
      return out;
    }
  }

  // If the target thread is finished, a blocking call would deadlock.
  if (targetThread->isFinished()) {
    ZQObjectThreadInvokeResult<R> out;
    out.ok = false;
    out.error = std::string(what) + ": target thread finished";
    return out;
  }
  // If the target thread isn't running, the queued call will never execute.
  if (!targetThread->isRunning()) {
    ZQObjectThreadInvokeResult<R> out;
    out.ok = false;
    out.error = std::string(what) + ": target thread not running";
    return out;
  }

  auto state = std::make_shared<detail::ZQObjectThreadInvokeState<R>>();

  const QMetaObject::Connection destroyedConn = QObject::connect(obj, &QObject::destroyed, [state, what]() {
    state->cancelled.store(true);
    {
      std::scoped_lock lock(state->mu);
      state->done = true;
      state->error = std::string(what) + ": target destroyed";
    }
    state->cv.notify_all();
  });

  const QMetaObject::Connection threadFinishedConn =
    QObject::connect(targetThread, &QThread::finished, [state, what]() {
      state->cancelled.store(true);
      {
        std::scoped_lock lock(state->mu);
        state->done = true;
        state->error = std::string(what) + ": target thread finished";
      }
      state->cv.notify_all();
    });

  const bool invokeOk = QMetaObject::invokeMethod(
    obj,
    [state, fn = std::forward<Func>(f)]() mutable {
      if (state->cancelled.load()) {
        return;
      }
      if constexpr (std::is_void_v<R>) {
        fn();
        {
          std::scoped_lock lock(state->mu);
          state->done = true;
        }
        state->cv.notify_all();
      } else {
        R v = fn();
        {
          std::scoped_lock lock(state->mu);
          state->value = std::move(v);
          state->done = true;
        }
        state->cv.notify_all();
      }
    },
    Qt::QueuedConnection);

  if (!invokeOk) {
    QObject::disconnect(destroyedConn);
    QObject::disconnect(threadFinishedConn);
    ZQObjectThreadInvokeResult<R> out;
    out.ok = false;
    out.error = std::string(what) + ": failed to invoke on target thread";
    return out;
  }

  if (!shouldCancel) {
    std::unique_lock<std::mutex> lock(state->mu);
    state->cv.wait(lock, [&]() { return state->done; });
  } else {
    while (true) {
      if (shouldCancel()) {
        state->cancelled.store(true);
        break;
      }
      std::unique_lock<std::mutex> lock(state->mu);
      if (state->done) {
        break;
      }
      // Poll for cancellation at a low rate; completion is signaled.
      state->cv.wait_for(lock, cancelPoll, [&]() { return state->done; });
    }
  }

  QObject::disconnect(destroyedConn);
  QObject::disconnect(threadFinishedConn);

  {
    std::scoped_lock lock(state->mu);
    if (!state->done) {
      ZQObjectThreadInvokeResult<R> out;
      out.ok = false;
      out.error = std::string(what) + ": cancelled";
      return out;
    }
    if (!state->error.empty()) {
      ZQObjectThreadInvokeResult<R> out;
      out.ok = false;
      out.error = state->error;
      return out;
    }
    if constexpr (std::is_void_v<R>) {
      ZQObjectThreadInvokeResult<void> out;
      out.ok = true;
      return out;
    } else {
      if (!state->value) {
        ZQObjectThreadInvokeResult<R> out;
        out.ok = false;
        out.error = std::string(what) + ": no result";
        return out;
      }
      ZQObjectThreadInvokeResult<R> out;
      out.ok = true;
      out.value = std::move(*state->value);
      return out;
    }
  }
}

} // namespace nim
