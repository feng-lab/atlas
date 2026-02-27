#pragma once

#include <QWidget>

#include <cstdint>
#include <map>

class QVBoxLayout;
class QLabel;

namespace nim {

class ZBackgroundTask;
class ZBackgroundTaskManager;

class ZBackgroundTaskManagerWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit ZBackgroundTaskManagerWidget(ZBackgroundTaskManager& manager, QWidget* parent = nullptr);

private:
  void addTaskRow(ZBackgroundTask* task);
  void updateEmptyState();

  ZBackgroundTaskManager& m_manager;
  QVBoxLayout* m_layout = nullptr;
  QLabel* m_emptyLabel = nullptr;
  QVBoxLayout* m_tasksLayout = nullptr;
  QWidget* m_tasksContainer = nullptr;
  std::map<uint64_t, QWidget*> m_rows;
};

} // namespace nim
