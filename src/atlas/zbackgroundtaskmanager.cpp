#include "zbackgroundtaskmanager.h"

#include "zlog.h"

#include <algorithm>
#include <utility>

namespace nim {

namespace {

constexpr int kFakeProgressTickMs = 150;
constexpr double kFakeProgressCeiling = 0.97;
constexpr double kFakeProgressAlpha = 0.05;

[[nodiscard]] double clamped01(double v)
{
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 1.0) {
    return 1.0;
  }
  return v;
}

} // namespace

ZBackgroundTask::ZBackgroundTask(uint64_t id, QString title, QObject* parent)
  : QObject(parent)
  , m_id(id)
  , m_title(std::move(title))
{
  m_state = State::Queued;
  m_progress = 0.0;
  m_message = QStringLiteral("queued");
}

void ZBackgroundTask::requestCancel()
{
  if (isTerminal()) {
    return;
  }
  if (m_cancelRequested) {
    return;
  }

  m_cancelRequested = true;
  if (m_message.isEmpty()) {
    m_message = QStringLiteral("cancel requested");
  } else if (!m_message.contains(QStringLiteral("cancel"), Qt::CaseInsensitive)) {
    m_message = QStringLiteral("%1 (cancel requested)").arg(m_message);
  }

  Q_EMIT changed();

  if (m_cancelCallback) {
    m_cancelCallback();
  }
}

void ZBackgroundTask::setState(State s)
{
  if (m_state == s) {
    return;
  }
  m_state = s;
  Q_EMIT changed();
}

void ZBackgroundTask::setMessage(QString msg)
{
  if (m_message == msg) {
    return;
  }
  m_message = std::move(msg);
  Q_EMIT changed();
}

void ZBackgroundTask::setError(QString err)
{
  if (m_error == err) {
    return;
  }
  m_error = std::move(err);
  Q_EMIT changed();
}

void ZBackgroundTask::setProgress(double p)
{
  const double clamped = clamped01(p);
  if (m_progress == clamped) {
    return;
  }
  m_progress = clamped;
  Q_EMIT changed();
}

void ZBackgroundTask::setCancelCallback(std::function<void()> cb)
{
  m_cancelCallback = std::move(cb);
}

void ZBackgroundTask::setWaitForFinishedCallback(std::function<void()> cb)
{
  m_waitForFinishedCallback = std::move(cb);
}

void ZBackgroundTask::waitForFinished() const
{
  if (!m_waitForFinishedCallback) {
    LOG(WARNING) << "Background task has no wait callback; shutdown may race with task teardown: "
                 << m_title.toStdString();
    return;
  }
  m_waitForFinishedCallback();
}

void ZBackgroundTask::setUseFakeProgress(bool enabled)
{
  if (m_useFakeProgress == enabled) {
    return;
  }
  m_useFakeProgress = enabled;
  if (!m_useFakeProgress) {
    stopFakeProgressTimer();
  }
}

void ZBackgroundTask::startFakeProgressTimer()
{
  if (!m_useFakeProgress) {
    return;
  }
  if (m_fakeProgressTimer != nullptr) {
    return;
  }

  m_fakeProgressTimer = new QTimer(this);
  m_fakeProgressTimer->setInterval(kFakeProgressTickMs);
  connect(m_fakeProgressTimer, &QTimer::timeout, this, [this]() {
    if (isTerminal()) {
      stopFakeProgressTimer();
      return;
    }
    if (m_state != State::Running) {
      return;
    }

    const double current = progress();
    const double target = std::min(kFakeProgressCeiling, 0.999);
    const double next = current + (target - current) * kFakeProgressAlpha;
    setProgress(std::min(next, target));
  });
  m_fakeProgressTimer->start();
}

void ZBackgroundTask::stopFakeProgressTimer()
{
  if (m_fakeProgressTimer == nullptr) {
    return;
  }
  m_fakeProgressTimer->stop();
  m_fakeProgressTimer->deleteLater();
  m_fakeProgressTimer = nullptr;
}

ZBackgroundTaskManager::ZBackgroundTaskManager(QObject* parent)
  : QObject(parent)
{}

ZBackgroundTask* ZBackgroundTaskManager::createTask(QString title)
{
  return createTask(std::move(title), TaskOptions{});
}

ZBackgroundTask* ZBackgroundTaskManager::createTask(QString title, TaskOptions options)
{
  auto* task = new ZBackgroundTask(m_nextId++, std::move(title), this);
  task->setCancelCallback(std::move(options.cancelCallback));
  task->setUseFakeProgress(options.useFakeProgress);
  m_tasks.push_back(task);
  Q_EMIT taskAdded(task);
  return task;
}

void ZBackgroundTaskManager::startTask(ZBackgroundTask* task, QString message)
{
  CHECK(task != nullptr);

  task->setState(ZBackgroundTask::State::Running);
  if (!message.isEmpty()) {
    task->setMessage(std::move(message));
  } else {
    task->setMessage(QStringLiteral("running"));
  }

  task->setError(QString{});
  task->setProgress(0.0);
  task->startFakeProgressTimer();
}

void ZBackgroundTaskManager::setTaskProgress(ZBackgroundTask* task, double progress01)
{
  CHECK(task != nullptr);
  task->stopFakeProgressTimer();
  task->setProgress(progress01);
}

void ZBackgroundTaskManager::setTaskMessage(ZBackgroundTask* task, QString message)
{
  CHECK(task != nullptr);
  if (message.isEmpty()) {
    return;
  }
  task->setMessage(std::move(message));
}

void ZBackgroundTaskManager::succeedTask(ZBackgroundTask* task, QString message)
{
  CHECK(task != nullptr);
  task->stopFakeProgressTimer();
  task->setProgress(1.0);
  task->setState(ZBackgroundTask::State::Succeeded);
  task->setError(QString{});
  task->setMessage(message.isEmpty() ? QStringLiteral("succeeded") : std::move(message));
}

void ZBackgroundTaskManager::failTask(ZBackgroundTask* task, QString error)
{
  CHECK(task != nullptr);
  task->stopFakeProgressTimer();
  task->setProgress(1.0);
  task->setState(ZBackgroundTask::State::Failed);
  task->setError(std::move(error));
  task->setMessage(QStringLiteral("failed"));
}

void ZBackgroundTaskManager::cancelTask(ZBackgroundTask* task, QString message)
{
  CHECK(task != nullptr);
  task->stopFakeProgressTimer();
  task->setProgress(1.0);
  task->setState(ZBackgroundTask::State::Cancelled);
  task->setError(QString{});
  task->setMessage(message.isEmpty() ? QStringLiteral("cancelled") : std::move(message));
}

void ZBackgroundTaskManager::cancelAllTasksAndWait()
{
  for (ZBackgroundTask* task : m_tasks) {
    if (task == nullptr) {
      continue;
    }
    if (!task->isTerminal()) {
      task->requestCancel();
    }
  }

  for (ZBackgroundTask* task : m_tasks) {
    if (task == nullptr) {
      continue;
    }
    if (!task->isTerminal()) {
      task->waitForFinished();
    }
  }
}

} // namespace nim
