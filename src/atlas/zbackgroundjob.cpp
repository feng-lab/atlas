#include "zbackgroundjob.h"

#include "zbackgroundtaskmanager.h"
#include "zdoc.h"
#include "zexception.h"
#include "zfolly.h"
#include "zlog.h"

#include <QMetaObject>

#include <folly/OperationCancelled.h>
#include <folly/coro/Invoke.h>

#include <algorithm>
#include <utility>

namespace nim {

ZBackgroundJobContext::ZBackgroundJobContext(QPointer<ZDoc> doc,
                                             QPointer<ZBackgroundTask> task,
                                             folly::CancellationToken token)
  : m_doc(std::move(doc))
  , m_task(std::move(task))
  , m_token(token)
{}

folly::CancellationToken ZBackgroundJobContext::cancellationToken() const
{
  return m_token;
}

void ZBackgroundJobContext::maybeCancel() const
{
  if (m_token.isCancellationRequested()) {
    throw ZCancellationException();
  }
}

void ZBackgroundJobContext::setProgress01(double p01) const
{
  if (m_doc == nullptr || m_task == nullptr) {
    return;
  }

  const double clamped = std::clamp(p01, 0.0, 1.0);
  QPointer<ZDoc> docPtr = m_doc;
  QPointer<ZBackgroundTask> taskPtr = m_task;
  QMetaObject::invokeMethod(
    m_doc,
    [docPtr, taskPtr, clamped]() {
      if (docPtr == nullptr || taskPtr == nullptr) {
        return;
      }
      docPtr->backgroundTaskManager().setTaskProgress(taskPtr, clamped);
    },
    Qt::QueuedConnection);
}

void ZBackgroundJobContext::setMessage(QString message) const
{
  if (m_doc == nullptr || m_task == nullptr) {
    return;
  }
  if (message.isEmpty()) {
    return;
  }

  QPointer<ZDoc> docPtr = m_doc;
  QPointer<ZBackgroundTask> taskPtr = m_task;
  QMetaObject::invokeMethod(
    m_doc,
    [docPtr, taskPtr, message = std::move(message)]() mutable {
      if (docPtr == nullptr || taskPtr == nullptr) {
        return;
      }
      docPtr->backgroundTaskManager().setTaskMessage(taskPtr, std::move(message));
    },
    Qt::QueuedConnection);
}

namespace {

[[nodiscard]] QString defaultRunningMessage(const QString& msg)
{
  return msg.isEmpty() ? QStringLiteral("running") : msg;
}

[[nodiscard]] QString defaultCancelMessage(const QString& msg)
{
  return msg.isEmpty() ? QStringLiteral("cancelled") : msg;
}

[[nodiscard]] QString defaultSuccessMessage(const QString& msg)
{
  return msg.isEmpty() ? QStringLiteral("succeeded") : msg;
}

} // namespace

ZBackgroundTask* startBackgroundJob(ZDoc& doc, ZBackgroundJobSpec spec)
{
  CHECK(!spec.title.isEmpty());
  CHECK(spec.work);

  folly::Executor::KeepAlive<> executor = spec.executor;
  if (!executor) {
    executor = getAtlasBackgroundExecutor();
  }
  CHECK(executor);

  ZBackgroundTaskManager& tm = doc.backgroundTaskManager();

  auto cancellationSource = std::make_shared<folly::CancellationSource>();
  const folly::CancellationToken token = cancellationSource->getToken();

  ZBackgroundTaskManager::TaskOptions taskOptions;
  taskOptions.useFakeProgress = spec.useFakeProgress;
  taskOptions.cancelCallback = [cancellationSource]() {
    cancellationSource->requestCancellation();
  };

  auto* task = tm.createTask(spec.title, std::move(taskOptions));
  tm.startTask(task, defaultRunningMessage(spec.runningMessage));
  doc.showBackgroundTasksPanel();

  QPointer<ZDoc> docPtr(&doc);
  QPointer<ZBackgroundTask> taskPtr(task);

  std::string debugLabel = std::move(spec.debugLabel);
  if (debugLabel.empty()) {
    debugLabel = spec.title.toStdString();
  }

  auto work = std::move(spec.work);

  tm.spawnDetachedTask(
    std::move(executor),
    folly::coro::co_invoke(
      [docPtr, taskPtr, work = std::move(work), token, debugLabel]() mutable -> folly::coro::Task<void> {
        ZBackgroundJobOutcome outcome;
        try {
          ZBackgroundJobContext ctx(docPtr, taskPtr, token);
          outcome = co_await work(std::move(ctx));
        }
        catch (const ZCancellationException&) {
          outcome.state = ZBackgroundJobOutcome::State::Cancelled;
          outcome.message = QStringLiteral("cancelled");
        }
        catch (const folly::OperationCancelled&) {
          outcome.state = ZBackgroundJobOutcome::State::Cancelled;
          outcome.message = QStringLiteral("cancelled");
        }
        catch (const std::exception& e) {
          outcome.state = ZBackgroundJobOutcome::State::Failed;
          outcome.message = QString::fromUtf8(e.what());
        }
        catch (...) {
          outcome.state = ZBackgroundJobOutcome::State::Failed;
          outcome.message = QStringLiteral("unknown error");
        }

        const bool cancelled = token.isCancellationRequested();
        if (cancelled && outcome.state == ZBackgroundJobOutcome::State::Succeeded) {
          outcome.state = ZBackgroundJobOutcome::State::Cancelled;
          outcome.message = QStringLiteral("cancelled");
        }

        if (docPtr != nullptr && taskPtr != nullptr) {
          QMetaObject::invokeMethod(
            docPtr,
            [docPtr, taskPtr, outcome = std::move(outcome)]() mutable {
              if (docPtr == nullptr || taskPtr == nullptr) {
                return;
              }

              ZBackgroundTaskManager& tm = docPtr->backgroundTaskManager();
              switch (outcome.state) {
                case ZBackgroundJobOutcome::State::Cancelled:
                  tm.cancelTask(taskPtr, defaultCancelMessage(outcome.message));
                  break;
                case ZBackgroundJobOutcome::State::Failed: {
                  const QString err = outcome.message.isEmpty() ? QStringLiteral("failed") : outcome.message;
                  tm.failTask(taskPtr, err);
                  break;
                }
                case ZBackgroundJobOutcome::State::Succeeded:
                  tm.succeedTask(taskPtr, defaultSuccessMessage(outcome.message));
                  break;
              }

              if (outcome.uiCallback) {
                try {
                  outcome.uiCallback(*docPtr, *taskPtr);
                }
                catch (...) {
                  // Best-effort: UI callbacks must not throw.
                }
              }
            },
            Qt::QueuedConnection);
        }

        co_return;
      }),
    debugLabel);

  return task;
}

} // namespace nim
