#include "zrpctaskmanager.h"

#include "zimgdoc.h"
#include "zlog.h"
#include "zneuroglancerprecomputed.h"
#include "zrpcuidispatcher.h"

#include <QFuture>
#include <QMetaObject>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace nim {

namespace {

constexpr std::chrono::milliseconds kDefaultNeuroglancerOpenTimeout{30000};

[[nodiscard]] bool isTerminalState(ZRpcTaskState s)
{
  switch (s) {
    case ZRpcTaskState::Queued:
    case ZRpcTaskState::Running:
      return false;
    case ZRpcTaskState::Succeeded:
    case ZRpcTaskState::Failed:
    case ZRpcTaskState::Cancelled:
      return true;
  }
  return true;
}

[[nodiscard]] std::string joinLines(const std::vector<std::string>& lines)
{
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out += "\n";
    }
    out += lines[i];
  }
  return out;
}

[[nodiscard]] std::vector<ZRpcListedObject> toListedObjects(const ZRpcUiDispatcher::ListObjectsResult& r)
{
  std::vector<ZRpcListedObject> out;
  out.reserve(r.objects.size());
  for (const auto& o : r.objects) {
    ZRpcListedObject lo;
    lo.id = o.id;
    lo.type = o.type;
    lo.name = o.name;
    lo.path = o.path;
    lo.visible = o.visible;
    out.push_back(std::move(lo));
  }
  return out;
}

} // namespace

struct ZRpcTaskManager::TaskRecord
{
  uint64_t id = 0;
  std::string kind;
  ZRpcTaskState state = ZRpcTaskState::Queued;
  std::optional<double> progress;
  std::string message;
  std::string error;
  std::optional<ZRpcLoadTaskResult> loadResult;

  bool cancelRequested = false;
  bool deleted = false;

  mutable std::mutex mu;
  std::condition_variable cv;
};

ZRpcTaskManager::ZRpcTaskManager(ZRpcUiDispatcher* uiDispatcher)
  : m_uiDispatcher(uiDispatcher)
{
  CHECK(m_uiDispatcher);
}

uint64_t ZRpcTaskManager::startLoadTask(StartLoadParams params)
{
  CHECK(m_uiDispatcher);

  auto task = std::make_shared<TaskRecord>();
  {
    std::lock_guard<std::mutex> lock(m_mu);
    task->id = m_nextId++;
    task->kind = "load";
    task->state = ZRpcTaskState::Queued;
    task->message = "queued";
    m_tasks.emplace(task->id, task);
  }

  const uint64_t taskId = task->id;
  const QStringList sources = std::move(params.sources);
  const bool setVisible = params.setVisible;
  const std::chrono::milliseconds timeout =
    (params.networkTimeout.count() > 0) ? params.networkTimeout : kDefaultNeuroglancerOpenTimeout;

  // Background work: open Neuroglancer volumes off the UI thread, then enqueue
  // a UI-thread registration step. This avoids blocking UI and avoids shutdown
  // deadlocks (we never wait on UI from a threadpool worker).
  [[maybe_unused]] const QFuture<void> bg =
    QtConcurrent::run([task, sources, timeout, setVisible, uiDispatcher = m_uiDispatcher]() {
      auto setSnapshot = [&](ZRpcTaskState state,
                             std::optional<double> progress,
                             std::string message,
                             std::string error,
                             std::optional<ZRpcLoadTaskResult> loadResult) {
        std::lock_guard<std::mutex> lock(task->mu);
        if (task->deleted) {
          return;
        }
        if (isTerminalState(task->state) && state != task->state) {
          return;
        }
        task->state = state;
        task->progress = progress;
        if (!message.empty()) {
          task->message = std::move(message);
        }
        if (!error.empty()) {
          task->error = std::move(error);
        }
        if (loadResult.has_value()) {
          task->loadResult = std::move(loadResult);
        }
        task->cv.notify_all();
      };

      auto cancelled = [&]() -> bool {
        std::lock_guard<std::mutex> lock(task->mu);
        return task->deleted || task->cancelRequested || task->state == ZRpcTaskState::Cancelled;
      };

      setSnapshot(ZRpcTaskState::Running, 0.0, "starting", "", std::nullopt);

      QStringList localSources;
      std::vector<std::shared_ptr<ZNeuroglancerPrecomputedVolume>> openedVolumes;
      std::vector<std::string> errors;

      const int totalSources = sources.size();
      for (int i = 0; i < totalSources; ++i) {
        if (cancelled()) {
          setSnapshot(ZRpcTaskState::Cancelled, std::nullopt, "cancelled", "", std::nullopt);
          return;
        }

        const QString src = sources.at(i).trimmed();
        if (src.isEmpty()) {
          errors.push_back("sources[" + std::to_string(i) + "]: empty");
          continue;
        }

        const double p = (totalSources > 0) ? (static_cast<double>(i) / static_cast<double>(totalSources)) : 0.0;
        if (ZImgDoc::looksLikeNeuroglancerPrecomputedUrl(src)) {
          setSnapshot(ZRpcTaskState::Running,
                      std::min(0.9, p),
                      "opening neuroglancer_precomputed: " + src.toStdString(),
                      "",
                      std::nullopt);
          try {
            auto vol = ZNeuroglancerPrecomputedVolume::open(src, timeout);
            CHECK(vol);
            openedVolumes.push_back(std::move(vol));
          }
          catch (const std::exception& e) {
            errors.push_back("open failed for " + src.toStdString() + ": " + e.what());
          }
          catch (...) {
            errors.push_back("open failed for " + src.toStdString() + ": unknown error");
          }
        } else {
          localSources.push_back(src);
        }
      }

      if (cancelled()) {
        setSnapshot(ZRpcTaskState::Cancelled, std::nullopt, "cancelled", "", std::nullopt);
        return;
      }

      setSnapshot(ZRpcTaskState::Running, 0.9, "registering loaded datasets", "", std::nullopt);

      // Register all opened volumes (and any local file loads) on the UI thread.
      // We do not wait on the UI thread from this worker thread to avoid deadlocks during shutdown.
      QMetaObject::invokeMethod(
        uiDispatcher,
        [task, uiDispatcher, localSources, openedVolumes, setVisible, errors]() mutable {
          auto setSnapshotUi = [&](ZRpcTaskState state,
                                   std::optional<double> progress,
                                   std::string message,
                                   std::string error,
                                   std::optional<ZRpcLoadTaskResult> loadResult) {
            std::lock_guard<std::mutex> lock(task->mu);
            if (task->deleted) {
              return;
            }
            if (isTerminalState(task->state) && state != task->state) {
              return;
            }
            task->state = state;
            task->progress = progress;
            if (!message.empty()) {
              task->message = std::move(message);
            }
            if (!error.empty()) {
              task->error = std::move(error);
            }
            if (loadResult.has_value()) {
              task->loadResult = std::move(loadResult);
            }
            task->cv.notify_all();
          };

          auto cancelledUi = [&]() -> bool {
            std::lock_guard<std::mutex> lock(task->mu);
            return task->deleted || task->cancelRequested || task->state == ZRpcTaskState::Cancelled;
          };

          if (cancelledUi()) {
            setSnapshotUi(ZRpcTaskState::Cancelled, std::nullopt, "cancelled", "", std::nullopt);
            return;
          }

          std::vector<std::string> allErrors = std::move(errors);
          std::vector<std::string> warnings;
          std::unordered_set<uint64_t> loadedIds;

          // Optional local loads. These still run on the UI thread (same as the GUI file-open path),
          // but are wrapped in the task API so clients don't block a gRPC handler thread.
          if (!localSources.isEmpty()) {
            const auto before = uiDispatcher->listObjects();
            std::unordered_set<uint64_t> beforeIds;
            const bool beforeOk = before.ok;
            if (before.ok) {
              beforeIds.reserve(before.objects.size());
              for (const auto& o : before.objects) {
                beforeIds.insert(o.id);
              }
            } else {
              warnings.push_back("list_objects(before) failed: " + before.error);
            }

            const auto afterLoad = uiDispatcher->loadFilesAndListObjects(localSources);
            if (!afterLoad.ok) {
              allErrors.push_back(afterLoad.error.empty() ? "local load failed" : afterLoad.error);
            } else if (beforeOk) {
              for (const auto& o : afterLoad.objects) {
                if (!beforeIds.contains(o.id)) {
                  loadedIds.insert(o.id);
                }
              }
            }
          }

          for (auto& vol : openedVolumes) {
            if (cancelledUi()) {
              setSnapshotUi(ZRpcTaskState::Cancelled, std::nullopt, "cancelled", "", std::nullopt);
              return;
            }

            const QString url = vol ? vol->rootUrl() : QString();
            const auto add = uiDispatcher->addNeuroglancerPrecomputedVolume(std::move(vol), setVisible);
            if (!add.ok) {
              allErrors.push_back(add.error.empty() ? ("failed to register dataset: " + url.toStdString()) : add.error);
              continue;
            }
            loadedIds.insert(add.id);
          }

          const auto listAfter = uiDispatcher->listObjects();
          if (!listAfter.ok) {
            allErrors.push_back(listAfter.error.empty() ? "list_objects(after) failed" : listAfter.error);
          }

          ZRpcLoadTaskResult res;
          res.loadedIds.reserve(loadedIds.size());
          for (const uint64_t id : loadedIds) {
            res.loadedIds.push_back(id);
          }
          std::sort(res.loadedIds.begin(), res.loadedIds.end());

          res.objects = listAfter.ok ? toListedObjects(listAfter) : std::vector<ZRpcListedObject>{};
          res.warnings = std::move(warnings);

          if (!allErrors.empty()) {
            setSnapshotUi(ZRpcTaskState::Failed,
                          1.0,
                          "load task completed with errors",
                          joinLines(allErrors),
                          std::move(res));
            return;
          }

          setSnapshotUi(ZRpcTaskState::Succeeded, 1.0, "load task succeeded", "", std::move(res));
        },
        Qt::QueuedConnection);
    });

  return taskId;
}

std::optional<ZRpcTaskSnapshot> ZRpcTaskManager::taskStatus(uint64_t taskId) const
{
  std::shared_ptr<TaskRecord> task;
  {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
      return std::nullopt;
    }
    task = it->second;
  }

  std::lock_guard<std::mutex> lock(task->mu);
  ZRpcTaskSnapshot snap;
  snap.id = task->id;
  snap.kind = task->kind;
  snap.state = task->state;
  snap.progress = task->progress;
  snap.message = task->message;
  snap.error = task->error;
  snap.loadResult = task->loadResult;
  return snap;
}

std::optional<ZRpcTaskSnapshot> ZRpcTaskManager::waitForCompletion(uint64_t taskId,
                                                                   std::chrono::milliseconds timeout,
                                                                   std::chrono::milliseconds pollInterval,
                                                                   std::function<bool()> shouldCancel) const
{
  std::shared_ptr<TaskRecord> task;
  {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
      return std::nullopt;
    }
    task = it->second;
  }

  const auto interval = (pollInterval.count() > 0) ? pollInterval : std::chrono::milliseconds(50);
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + timeout;

  std::unique_lock<std::mutex> lock(task->mu);
  if (timeout.count() == 0) {
    ZRpcTaskSnapshot snap;
    snap.id = task->id;
    snap.kind = task->kind;
    snap.state = task->state;
    snap.progress = task->progress;
    snap.message = task->message;
    snap.error = task->error;
    snap.loadResult = task->loadResult;
    return snap;
  }

  while (!isTerminalState(task->state)) {
    if (shouldCancel && shouldCancel()) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto waitFor = std::min(interval, remaining);
    task->cv.wait_for(lock, waitFor);
  }

  ZRpcTaskSnapshot snap;
  snap.id = task->id;
  snap.kind = task->kind;
  snap.state = task->state;
  snap.progress = task->progress;
  snap.message = task->message;
  snap.error = task->error;
  snap.loadResult = task->loadResult;
  return snap;
}

bool ZRpcTaskManager::cancelTask(uint64_t taskId)
{
  std::shared_ptr<TaskRecord> task;
  {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
      return false;
    }
    task = it->second;
  }

  std::lock_guard<std::mutex> lock(task->mu);
  task->cancelRequested = true;
  if (!isTerminalState(task->state)) {
    task->state = ZRpcTaskState::Cancelled;
    task->progress.reset();
    task->message = "cancelled";
    task->cv.notify_all();
  }
  return true;
}

bool ZRpcTaskManager::deleteTask(uint64_t taskId)
{
  std::shared_ptr<TaskRecord> task;
  {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
      return false;
    }
    task = it->second;
    m_tasks.erase(it);
  }

  std::lock_guard<std::mutex> lock(task->mu);
  task->deleted = true;
  task->cancelRequested = true;
  if (!isTerminalState(task->state)) {
    task->state = ZRpcTaskState::Cancelled;
    task->progress.reset();
    task->message = "deleted";
    task->cv.notify_all();
  }
  return true;
}

} // namespace nim
