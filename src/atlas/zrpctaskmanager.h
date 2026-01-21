#pragma once

#include <QStringList>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nim {

class ZRpcUiDispatcher;

struct ZRpcListedObject
{
  uint64_t id = 0;
  std::string type;
  std::string name;
  std::string path;
  bool visible = false;
};

enum class ZRpcTaskState
{
  Queued,
  Running,
  Succeeded,
  Failed,
  Cancelled,
};

struct ZRpcLoadTaskResult
{
  std::vector<uint64_t> loadedIds;
  std::vector<ZRpcListedObject> objects;
  std::vector<std::string> warnings;
};

struct ZRpcTaskSnapshot
{
  uint64_t id = 0;
  std::string kind;
  ZRpcTaskState state = ZRpcTaskState::Queued;
  std::optional<double> progress;
  std::string message;
  std::string error;
  std::optional<ZRpcLoadTaskResult> loadResult;
};

class ZRpcTaskManager
{
public:
  explicit ZRpcTaskManager(ZRpcUiDispatcher* uiDispatcher);

  struct StartLoadParams
  {
    QStringList sources;
    std::chrono::milliseconds networkTimeout{0};
    bool setVisible = true;
  };

  [[nodiscard]] uint64_t startLoadTask(StartLoadParams params);

  [[nodiscard]] std::optional<ZRpcTaskSnapshot> taskStatus(uint64_t taskId) const;

  // Wait until the task reaches a terminal state, or until timeout.
  // Returns a snapshot on success, or nullopt if taskId is unknown.
  [[nodiscard]] std::optional<ZRpcTaskSnapshot> waitForCompletion(uint64_t taskId,
                                                                  std::chrono::milliseconds timeout,
                                                                  std::chrono::milliseconds pollInterval,
                                                                  std::function<bool()> shouldCancel) const;

  [[nodiscard]] bool cancelTask(uint64_t taskId);

  [[nodiscard]] bool deleteTask(uint64_t taskId);

private:
  struct TaskRecord;

  ZRpcUiDispatcher* m_uiDispatcher = nullptr; // non-owning
  mutable std::mutex m_mu;
  uint64_t m_nextId = 1;
  std::map<uint64_t, std::shared_ptr<TaskRecord>> m_tasks;
};

} // namespace nim
