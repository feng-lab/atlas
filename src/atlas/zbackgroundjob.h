#pragma once

#include <QPointer>
#include <QString>

#include <folly/CancellationToken.h>
#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/coro/Task.h>

#include <functional>
#include <optional>
#include <string>

namespace nim {

class ZBackgroundTask;
class ZDoc;

class ZBackgroundJobContext final
{
public:
  ZBackgroundJobContext(QPointer<ZDoc> doc, QPointer<ZBackgroundTask> task, folly::CancellationToken token);

  [[nodiscard]] folly::CancellationToken cancellationToken() const;

  void maybeCancel() const;

  void setProgress01(double p01) const;

  void setMessage(QString message) const;

private:
  QPointer<ZDoc> m_doc;
  QPointer<ZBackgroundTask> m_task;
  folly::CancellationToken m_token;
};

struct ZBackgroundJobOutcome
{
  enum class State
  {
    Succeeded,
    Failed,
    Cancelled,
  };

  State state = State::Succeeded;

  // For Succeeded/Cancelled: task message. For Failed: error string.
  QString message;

  // Optional UI callback invoked after the task state is updated.
  // Must not throw.
  std::function<void(ZDoc& doc, ZBackgroundTask& task)> uiCallback;
};

struct ZBackgroundJobSpec
{
  // Task title shown in the Tasks panel.
  QString title;

  // Optional initial task message (default: "running").
  QString runningMessage;

  // When enabled, progress will move toward 1.0 (but not reach it) until the task finishes.
  bool useFakeProgress = true;

  // Executor used to run the job body. If empty, the Atlas background executor is used.
  folly::Executor::KeepAlive<> executor;

  // Optional debug label for logging.
  std::string debugLabel;

  // Job body. Runs on `executor`. Must not touch Qt UI objects directly.
  // Use `ctx.setProgress01()` / `ctx.setMessage()` for UI-safe updates.
  folly::Function<folly::coro::Task<ZBackgroundJobOutcome>(ZBackgroundJobContext ctx)> work;
};

template<class Result>
[[nodiscard]] std::optional<QString> backgroundJobFailureMessageFromResult(const Result& result)
{
  // Background-job wrappers intentionally only infer failure from typed result
  // payloads that expose an `.error` member. If a workflow wants the Tasks
  // panel to reflect domain failures without throwing, prefer a small result
  // struct with `QString error` over ad hoc string-return conventions.
  if constexpr (requires {
                  result.error.isEmpty();
                  result.error;
                }) {
    if (!result.error.isEmpty()) {
      return QString(result.error);
    }
  }
  return std::nullopt;
}

[[nodiscard]] ZBackgroundTask* startBackgroundJob(ZDoc& doc, ZBackgroundJobSpec spec);

} // namespace nim
