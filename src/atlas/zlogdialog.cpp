#include "zlogdialog.h"

#include "ui_LogDialog.h"
#include "ztheme.h"

#include <QSortFilterProxyModel>
#include <QClipboard>
#include <QKeyEvent>
#include <QFileDialog>
#include <QtGlobal>
#include <QTextStream>

namespace nim {

class ZLogFilterProxyModel : public QSortFilterProxyModel
{
public:
  explicit ZLogFilterProxyModel(LogSeverity level, QObject* parent = nullptr)
    : QSortFilterProxyModel(parent)
    , mLevel(level)
    , mLastVisibleRow(0)
  {}

  [[nodiscard]] LogSeverity level() const
  {
    return mLevel;
  }

  void setLevel(const LogSeverity level)
  {
    mLevel = level;
    invalidateFilter();
  }

  void setPaused(bool paused)
  {
    mLastVisibleRow = paused ? rowCount() : 0;
    if (!paused) {
      invalidateFilter();
    }
  }

protected:
  [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& /*source_parent*/) const override
  {
    if (!mLastVisibleRow) {
      auto* model = dynamic_cast<ZLogModelSink*>(sourceModel());
      const LogData& d = model->at(source_row);
      return d.level >= mLevel;
    }

    return source_row <= mLastVisibleRow;
  }

private:
  LogSeverity mLevel;
  int mLastVisibleRow;
};

ZLogDialog::ZLogDialog(const LogSinkPtr& destination, QWidget* parent)
  : QDialog(parent)
  , mUi(nullptr)
  , mProxyModel(nullptr)
  , mIsPaused(false)
  , mHasAutoScroll(true)
{
  mModelDestination = dynamic_cast<ZLogModelSink*>(destination.get());
  CHECK(mModelDestination) << "log window needs a destination of type ModelDestination";

  mUi = new Ui::LogWindow();
  mUi->setupUi(this);

  mUi->toolButtonClear->setIcon(ZTheme::instance().icon(ZTheme::ClearIcon));
  mUi->toolButtonCopy->setIcon(ZTheme::instance().icon(ZTheme::CopyIcon));
  mUi->toolButtonPause->setIcon(ZTheme::instance().icon(ZTheme::PauseIcon));
  mUi->toolButtonSave->setIcon(ZTheme::instance().icon(ZTheme::SaveIcon));

  connect(mUi->toolButtonPause, SIGNAL(clicked()), SLOT(OnPauseClicked()));
  connect(mUi->toolButtonSave, SIGNAL(clicked()), SLOT(OnSaveClicked()));
  connect(mUi->toolButtonClear, SIGNAL(clicked()), SLOT(OnClearClicked()));
  connect(mUi->toolButtonCopy, SIGNAL(clicked()), SLOT(OnCopyClicked()));
  connect(mUi->comboBoxLevel, SIGNAL(currentIndexChanged(int)), SLOT(OnLevelChanged(int)));
  connect(mUi->checkBoxAutoScroll, SIGNAL(toggled(bool)), SLOT(OnAutoScrollChanged(bool)));
  connect(mModelDestination,
          SIGNAL(rowsInserted(const QModelIndex&, int, int)),
          SLOT(ModelRowsInserted(const QModelIndex&, int, int)));

  // Install the sort / filter model
  mProxyModel = new ZLogFilterProxyModel(InfoLevel, this);
  mProxyModel->setSourceModel(mModelDestination);
  mUi->tableViewMessages->setModel(mProxyModel);

  mUi->tableViewMessages->installEventFilter(this);

  mUi->tableViewMessages->setSelectionBehavior(QAbstractItemView::SelectRows);
#if QT_VERSION >= 0x050000
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::TimeColumn,
                                                                   QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::LevelNameColumn,
                                                                   QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::MessageColumn, QHeaderView::Stretch);
  mUi->tableViewMessages->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
#else
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::TimeColumn, QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::LevelNameColumn,
                                                            QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::MessageColumn, QHeaderView::Stretch);
  mUi->tableViewMessages->verticalHeader()->setResizeMode(QHeaderView::ResizeToContents);
#endif

  // Initialize log level selection
  for (auto l = InfoLevel; l < OffLevel; ++l) {
    const QString ln = levelToString(static_cast<LogSeverity>(l));
    mUi->comboBoxLevel->addItem(ln, l);
  }
  mUi->comboBoxLevel->setCurrentIndex(InfoLevel);
}

ZLogDialog::~ZLogDialog()
{
  delete mUi;
}

bool ZLogDialog::eventFilter(QObject* obj, QEvent* event)
{
  if (obj == mUi->tableViewMessages) {
    if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (keyEvent->key() == Qt::Key_C && (keyEvent->modifiers() & Qt::ControlModifier)) {
        copySelection();
        return true;
      }
    }

    return false;
  }
  return QDialog::eventFilter(obj, event);
}

void ZLogDialog::OnPauseClicked()
{
  mUi->toolButtonPause->setIcon(mIsPaused ? ZTheme::instance().icon(ZTheme::PauseIcon)
                                          : ZTheme::instance().icon(ZTheme::PlayIcon));
  mUi->toolButtonPause->setText(mIsPaused ? tr("&Pause") : tr("&Resume"));

  mIsPaused = !mIsPaused;

  mProxyModel->setPaused(mIsPaused);
}

void ZLogDialog::OnSaveClicked()
{
  saveSelection();
}

void ZLogDialog::OnClearClicked()
{
  mModelDestination->clear();
}

void ZLogDialog::OnCopyClicked()
{
  copySelection();
}

void ZLogDialog::OnLevelChanged(int value)
{
  mProxyModel->setLevel(static_cast<LogSeverity>(value));
}

void ZLogDialog::OnAutoScrollChanged(bool checked)
{
  mHasAutoScroll = checked;
}

void ZLogDialog::ModelRowsInserted(const QModelIndex& /*parent*/, int /*start*/, int /*last*/)
{
  if (mHasAutoScroll) {
    mUi->tableViewMessages->scrollToBottom();
  }
}

void ZLogDialog::copySelection() const
{
  const QString text = getSelectionText();
  if (text.isEmpty()) {
    return;
  }

  QApplication::clipboard()->setText(text);
}

void ZLogDialog::saveSelection()
{
  const QString text = getSelectionText();
  if (text.isEmpty()) {
    return;
  }

  QFileDialog dialog(this);
  dialog.setWindowTitle(tr("Save log"));
  dialog.setNameFilter(tr("Log file (*.log)"));
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setDefaultSuffix("log");
  dialog.exec();

  const QStringList sel = dialog.selectedFiles();
  if (sel.empty()) {
    return;
  }

  QFile file(sel.at(0));
  if (file.open(QIODevice::WriteOnly)) {
    QTextStream stream(&file);
    stream << text;
    file.close();
  }
}

QString ZLogDialog::getSelectionText() const
{
  QModelIndexList rows = mUi->tableViewMessages->selectionModel()->selectedRows();
  std::sort(rows.begin(), rows.end());

  QString text;

  if (rows.count() == 0) {
    for (int i = 0; i < mProxyModel->rowCount(); ++i) {
      const int srow = mProxyModel->mapToSource(mProxyModel->index(i, 0)).row();
      text += mModelDestination->at(srow).formatted + "\n";
    }
  } else {
    for (const auto& row : rows) {
      const int srow = mProxyModel->mapToSource(row).row();
      text += mModelDestination->at(srow).formatted + "\n";
    }
  }

  return text;
}

} // namespace nim
