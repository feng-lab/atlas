#include "zlogdialog.h"

#include "ui_LogDialog.h"

#include <QIcon>
#include <QSortFilterProxyModel>
#include <QClipboard>
#include <QKeyEvent>
#include <QFileDialog>
#include <QtGlobal>
#include <cstddef>

namespace nim {

static const QIcon& pauseIcon()
{
  static QIcon icon(QString::fromLatin1(":/LogDialog/images/icon-pause-16.png"));
  return icon;
}

static const QIcon& playIcon()
{
  static QIcon icon(QString::fromLatin1(":/LogDialog/images/icon-resume-16.png"));
  return icon;
}


class ZLogFilterProxyModel : public QSortFilterProxyModel
{
public:
  ZLogFilterProxyModel(LogSeverity level, QObject* parent = 0)
    : QSortFilterProxyModel(parent)
    , mLevel(level)
    , mLastVisibleRow(0)
  {
  }

  LogSeverity level() const
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
  virtual bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
  {
    Q_UNUSED(source_parent);
    if (!mLastVisibleRow) {
      ZLogModelSink* model = dynamic_cast<ZLogModelSink*>(sourceModel());
      const LogMessage& d = model->at(source_row);
      return d.level >= mLevel;
    }

    return source_row <= mLastVisibleRow;
  }

private:
  LogSeverity mLevel;
  int mLastVisibleRow;
};

ZLogDialog::ZLogDialog(LogSinkPtr destination, QWidget* parent)
  : QDialog(parent)
  , mUi(NULL)
  , mProxyModel(NULL)
  , mIsPaused(false)
  , mHasAutoScroll(true)
{
  mModelDestination = destination.dynamicCast<ZLogModelSink>();
  Q_ASSERT_X(destination.data(), "Window", "log window needs a destination of type ModelDestination");

  mUi = new Ui::LogWindow();
  mUi->setupUi(this);

  connect(mUi->toolButtonPause, SIGNAL(clicked()), SLOT(OnPauseClicked()));
  connect(mUi->toolButtonSave, SIGNAL(clicked()), SLOT(OnSaveClicked()));
  connect(mUi->toolButtonClear, SIGNAL(clicked()), SLOT(OnClearClicked()));
  connect(mUi->toolButtonCopy, SIGNAL(clicked()), SLOT(OnCopyClicked()));
  connect(mUi->comboBoxLevel, SIGNAL(currentIndexChanged(int)), SLOT(OnLevelChanged(int)));
  connect(mUi->checkBoxAutoScroll, SIGNAL(toggled(bool)), SLOT(OnAutoScrollChanged(bool)));
  connect(mModelDestination.data(), SIGNAL(rowsInserted(const QModelIndex&, int, int)), SLOT(ModelRowsInserted(const QModelIndex&, int, int)));

  // Install the sort / filter model
  mProxyModel = new ZLogFilterProxyModel(InfoLevel, this);
  mProxyModel->setSourceModel(mModelDestination.data());
  mUi->tableViewMessages->setModel(mProxyModel);

  mUi->tableViewMessages->installEventFilter(this);

  mUi->tableViewMessages->setSelectionBehavior(QAbstractItemView::SelectRows);
#if QT_VERSION >= 0x050000
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::TimeColumn, QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::LevelNameColumn, QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setSectionResizeMode(ZLogModelSink::MessageColumn, QHeaderView::Stretch);
  mUi->tableViewMessages->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
#else
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::TimeColumn, QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::LevelNameColumn, QHeaderView::ResizeToContents);
  mUi->tableViewMessages->horizontalHeader()->setResizeMode(ZLogModelSink::MessageColumn, QHeaderView::Stretch);
  mUi->tableViewMessages->verticalHeader()->setResizeMode(QHeaderView::ResizeToContents);
#endif

  // Initialize log level selection
  for (int l = TraceLevel; l < OffLevel; l++) {
    const QString ln = LocalizedLevelName(static_cast<LogSeverity>(l));
    mUi->comboBoxLevel->addItem(ln, l);
  }
  mUi->comboBoxLevel->setCurrentIndex(InfoLevel);
}

ZLogDialog::~ZLogDialog()
{
  delete mUi;
}

bool ZLogDialog::eventFilter(QObject *obj, QEvent *event)
{
  if (obj == mUi->tableViewMessages) {
    if (event->type() == QEvent::KeyPress) {
      QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
      if (keyEvent->key() == Qt::Key_C && (keyEvent->modifiers() & Qt::ControlModifier)) {
        copySelection();
        return true;
      }
    }

    return false;
  } else {
    return QDialog::eventFilter(obj, event);
  }
}

void ZLogDialog::OnPauseClicked()
{
  mUi->toolButtonPause->setIcon(mIsPaused ? pauseIcon() : playIcon());
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

void ZLogDialog::ModelRowsInserted(const QModelIndex& parent, int start, int last)
{
  Q_UNUSED(parent);
  Q_UNUSED(start);
  Q_UNUSED(last);
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
  if (sel.size() < 1) {
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
    for (int i = 0; i < mProxyModel->rowCount(); i++) {
      const int srow = mProxyModel->mapToSource(mProxyModel->index(i, 0)).row();
      text += mModelDestination->at(srow).formatted + "\n";
    }
  } else {
    for (QModelIndexList::const_iterator i = rows.begin();i != rows.end();++i) {
      const int srow = mProxyModel->mapToSource(*i).row();
      text += mModelDestination->at(srow).formatted + "\n";
    }
  }

  return text;
}

} // namespace nim
