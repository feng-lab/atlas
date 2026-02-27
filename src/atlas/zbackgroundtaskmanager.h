#pragma once

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace nim {

class ZBackgroundTask final : public QObject
{
  Q_OBJECT

public:
  enum class State
  {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
  };
  Q_ENUM(State)

  [[nodiscard]] uint64_t id() const
  {
    return m_id;
  }

  [[nodiscard]] const QString& title() const
  {
    return m_title;
  }

  [[nodiscard]] const QString& message() const
  {
    return m_message;
  }

  [[nodiscard]] const QString& error() const
  {
    return m_error;
  }

  [[nodiscard]] State state() const
  {
    return m_state;
  }

  // Progress is always in [0.0, 1.0]. For tasks without progress support, the manager can
  // enable "fake" progress that asymptotically approaches 1.0 until the task finishes.
  [[nodiscard]] double progress() const
  {
    return m_progress;
  }

  [[nodiscard]] bool cancelRequested() const
  {
    return m_cancelRequested;
  }

  [[nodiscard]] bool isTerminal() const
  {
    return m_state == State::Succeeded || m_state == State::Failed || m_state == State::Cancelled;
  }

  // Optional wait hook used during application shutdown to ensure background work is complete before
  // core objects are torn down. Callers running tasks on thread pools should set this to a blocking
  // wait (e.g. QFuture::waitForFinished()).
  void setWaitForFinishedCallback(std::function<void()> cb);
  void waitForFinished() const;

  void requestCancel();

Q_SIGNALS:
  void changed();

private:
  friend class ZBackgroundTaskManager;

  explicit ZBackgroundTask(uint64_t id, QString title, QObject* parent = nullptr);

  void setState(State s);
  void setMessage(QString msg);
  void setError(QString err);
  void setProgress(double p);

  void setCancelCallback(std::function<void()> cb);

  void setUseFakeProgress(bool enabled);
  void startFakeProgressTimer();
  void stopFakeProgressTimer();

  uint64_t m_id = 0;
  QString m_title;
  QString m_message;
  QString m_error;
  State m_state = State::Queued;
  double m_progress = 0.0;

  bool m_cancelRequested = false;
  bool m_useFakeProgress = false;

  std::function<void()> m_cancelCallback;
  std::function<void()> m_waitForFinishedCallback;
  QTimer* m_fakeProgressTimer = nullptr;
};

class ZBackgroundTaskManager final : public QObject
{
  Q_OBJECT

public:
  struct TaskOptions
  {
    // When enabled, progress will move toward 1.0 (but not reach it) until the task finishes.
    bool useFakeProgress = true;
    std::function<void()> cancelCallback;
  };

  explicit ZBackgroundTaskManager(QObject* parent = nullptr);

  [[nodiscard]] const std::vector<ZBackgroundTask*>& tasks() const
  {
    return m_tasks;
  }

  [[nodiscard]] ZBackgroundTask* createTask(QString title);
  [[nodiscard]] ZBackgroundTask* createTask(QString title, TaskOptions options);
  void startTask(ZBackgroundTask* task, QString message = {});
  void setTaskProgress(ZBackgroundTask* task, double progress01);
  void setTaskMessage(ZBackgroundTask* task, QString message);
  void succeedTask(ZBackgroundTask* task, QString message = {});
  void failTask(ZBackgroundTask* task, QString error);
  void cancelTask(ZBackgroundTask* task, QString message = {});
  void cancelAllTasksAndWait();

Q_SIGNALS:
  void taskAdded(ZBackgroundTask* task);

private:
  uint64_t m_nextId = 1;
  std::vector<ZBackgroundTask*> m_tasks;
};

} // namespace nim
