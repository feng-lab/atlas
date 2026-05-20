#include "zbackgroundtaskmanagerwidget.h"

#include "zbackgroundtaskmanager.h"
#include "ztheme.h"
#include "zlog.h"

#include <QHBoxLayout>
#include <QColor>
#include <QLabel>
#include <QProgressBar>
#include <QScrollBar>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

[[nodiscard]] QString stateLabel(ZBackgroundTask::State s)
{
  switch (s) {
    case ZBackgroundTask::State::Queued:
      return QStringLiteral("queued");
    case ZBackgroundTask::State::Running:
      return QStringLiteral("running");
    case ZBackgroundTask::State::Succeeded:
      return QStringLiteral("succeeded");
    case ZBackgroundTask::State::Failed:
      return QStringLiteral("failed");
    case ZBackgroundTask::State::Cancelled:
      return QStringLiteral("cancelled");
  }
  return QStringLiteral("unknown");
}

[[nodiscard]] QColor errorTextColor()
{
  return ZTheme::instance().isDarkTheme() ? QColor(242, 182, 179) : QColor(176, 0, 32);
}

class ZBackgroundTaskRowWidget final : public QWidget
{
public:
  explicit ZBackgroundTaskRowWidget(ZBackgroundTask* task, QWidget* parent = nullptr)
    : QWidget(parent)
    , m_task(task)
  {
    CHECK(m_task != nullptr);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    setMinimumWidth(0);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);

    m_titleLabel = new QLabel(this);
    // QLabel's minimumSizeHint() can be as wide as the whole title string, which can cause the right dock area
    // to expand and/or require horizontal scrolling. Allow the label to shrink and wrap instead.
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setMinimumWidth(0);
    headerRow->addWidget(m_titleLabel, 1);

    m_cancelButton = new QToolButton(this);
    m_cancelButton->setToolTip(tr("Cancel task"));
    ZTheme::instance().bindIcon(m_cancelButton, ZTheme::CancelIcon);
    headerRow->addWidget(m_cancelButton, 0);

    layout->addLayout(headerRow);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_progress->setMinimumWidth(0);
    layout->addWidget(m_progress);

    m_messageLabel = new QLabel(this);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_messageLabel->setMinimumWidth(0);
    layout->addWidget(m_messageLabel);

    connect(m_cancelButton, &QToolButton::clicked, this, [this]() {
      CHECK(m_task != nullptr);
      m_task->requestCancel();
    });

    connect(m_task, &ZBackgroundTask::changed, this, [this]() {
      refresh();
    });
    connect(&ZTheme::instance(), &ZTheme::themeChanged, this, [this]() {
      refresh();
    });
    refresh();
  }

  void refresh()
  {
    CHECK(m_task != nullptr);

    const QString title = m_task->title().isEmpty() ? QStringLiteral("Task") : m_task->title();
    const QString state = stateLabel(m_task->state());

    if (m_task->cancelRequested() && !m_task->isTerminal()) {
      m_titleLabel->setText(QStringLiteral("%1 (%2, cancel requested)").arg(title, state));
    } else {
      m_titleLabel->setText(QStringLiteral("%1 (%2)").arg(title, state));
    }

    const int v = static_cast<int>(std::lround(m_task->progress() * 100.0));
    m_progress->setValue(std::clamp(v, 0, 100));

    const bool terminal = m_task->isTerminal();
    m_cancelButton->setEnabled(!terminal);

    if (!m_task->error().isEmpty()) {
      m_messageLabel->setText(m_task->error());
      QPalette pal = m_messageLabel->palette();
      pal.setColor(QPalette::WindowText, errorTextColor());
      m_messageLabel->setPalette(pal);
    } else {
      m_messageLabel->setText(m_task->message());
      m_messageLabel->setPalette(QPalette{});
    }
  }

private:
  ZBackgroundTask* m_task = nullptr;
  QLabel* m_titleLabel = nullptr;
  QLabel* m_messageLabel = nullptr;
  QProgressBar* m_progress = nullptr;
  QToolButton* m_cancelButton = nullptr;
};

} // namespace

ZBackgroundTaskManagerWidget::ZBackgroundTaskManagerWidget(ZBackgroundTaskManager& manager, QWidget* parent)
  : QScrollArea(parent)
  , m_manager(manager)
{
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  setMinimumWidth(0);

  setWidgetResizable(true);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* content = new QWidget(this);
  content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  content->setMinimumWidth(0);
  setWidget(content);

  m_layout = new QVBoxLayout(content);
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(6);

  m_emptyLabel = new QLabel(tr("No background tasks."), content);
  m_emptyLabel->setAlignment(Qt::AlignHCenter);
  m_emptyLabel->setWordWrap(true);
  m_emptyLabel->setMinimumWidth(0);
  m_layout->addWidget(m_emptyLabel);

  m_tasksContainer = new QWidget(content);
  m_tasksContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_tasksContainer->setMinimumWidth(0);
  m_tasksLayout = new QVBoxLayout(m_tasksContainer);
  m_tasksLayout->setContentsMargins(0, 0, 0, 0);
  m_tasksLayout->setSpacing(8);
  m_layout->addWidget(m_tasksContainer);
  m_layout->addStretch(1);

  for (ZBackgroundTask* task : m_manager.tasks()) {
    addTaskRow(task);
  }
  updateEmptyState();

  connect(&m_manager, &ZBackgroundTaskManager::taskAdded, this, [this](ZBackgroundTask* task) {
    addTaskRow(task);
  });
}

void ZBackgroundTaskManagerWidget::addTaskRow(ZBackgroundTask* task)
{
  CHECK(task != nullptr);

  const uint64_t id = task->id();
  if (m_rows.contains(id)) {
    return;
  }

  auto* row = new ZBackgroundTaskRowWidget(task, m_tasksContainer);
  m_tasksLayout->addWidget(row);
  m_rows.emplace(id, row);
  updateEmptyState();

  QTimer::singleShot(0, this, [this]() {
    QScrollBar* sb = verticalScrollBar();
    if (sb == nullptr) {
      return;
    }
    sb->setValue(sb->maximum());
  });
}

void ZBackgroundTaskManagerWidget::updateEmptyState()
{
  const bool empty = m_rows.empty();
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setVisible(empty);
  }
  if (m_tasksContainer != nullptr) {
    m_tasksContainer->setVisible(!empty);
  }
}

} // namespace nim
