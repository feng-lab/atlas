#include "zlogmodelsink.h"

#include <QColor>
#include <cassert>

namespace nim {

LogSinkPtr logModelSinkInstance()
{
  static LogSinkPtr modelDestination(new ZLogModelSink());
  return modelDestination;
}

const std::deque<LogMessage>& logMessages()
{
  ZLogModelSink *md = dynamic_cast<ZLogModelSink*>(logModelSinkInstance().data());
  assert(md);
  return md->logMessages();
}

const char* const ZLogModelSink::Type = "window";

ZLogModelSink::ZLogModelSink(size_t max_items) :
  mMaxItems(max_items)
{
}

ZLogModelSink::~ZLogModelSink()
{
}

void ZLogModelSink::write(const LogMessage& message)
{
  addEntry(message);
}

bool ZLogModelSink::isValid()
{
  return true;
}

QString ZLogModelSink::type() const
{
  return QString::fromLatin1(Type);
}

void ZLogModelSink::addEntry(const LogMessage& message)
{
  const int next_idx = static_cast<int>(mLogMessages.size());
  beginInsertRows(QModelIndex(), next_idx, next_idx);
  {
    QWriteLocker lock(&mMessagesLock);
    mLogMessages.push_back(message);
  }
  endInsertRows();

  if (mMaxItems < std::numeric_limits<size_t>::max() && mLogMessages.size() > mMaxItems) {
    {
      QWriteLocker lock(&mMessagesLock);
      mLogMessages.pop_front();
    }
    // Every item changed
    const QModelIndex idx1 = index(0, 0);
    const QModelIndex idx2 = index(static_cast<int>(mLogMessages.size()), rowCount());
    emit dataChanged(idx1, idx2);
  }
}

void ZLogModelSink::clear()
{
  beginResetModel();
  {
    QWriteLocker lock(&mMessagesLock);
    mLogMessages.clear();
  }
  endResetModel();
}

LogMessage ZLogModelSink::at(size_t index)
{
  return mLogMessages[index];
}

int ZLogModelSink::columnCount(const QModelIndex& parent) const
{
  Q_UNUSED(parent);
  return 3;
}

int ZLogModelSink::rowCount(const QModelIndex& parent) const
{
  Q_UNUSED(parent);
  QReadLocker lock(&mMessagesLock);

  return static_cast<int>(mLogMessages.size());
}

QVariant ZLogModelSink::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  if (role == Qt::DisplayRole) {
    QReadLocker lock(&mMessagesLock);

    const LogMessage& item = mLogMessages.at(index.row());

    switch (index.column()) {
    case TimeColumn:
      return item.time.toLocalTime().toString();
    case LevelNameColumn:
      return LocalizedLevelName(item.level);
    case MessageColumn:
      return item.message;
    case FormattedMessageColumn:
      return item.formatted;
    default:
      return QVariant();
    }

    return QString();
  }

  if (role == Qt::BackgroundColorRole) {
    QReadLocker lock(&mMessagesLock);

    const LogMessage& item = mLogMessages.at(index.row());

    switch (item.level)
    {
    case WarnLevel:
      return QVariant(QColor(255, 255, 128));
    case ErrorLevel:
      return QVariant(QColor(255, 128, 128));
    case FatalLevel:
      return QVariant(QColor(255, 0, 0));
    default:
      break;
    }
  }

  return QVariant();
}

QVariant ZLogModelSink::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
    switch (section) {
    case TimeColumn:
      return tr("Time");
    case LevelNameColumn:
      return tr("Level");
    case MessageColumn:
      return tr("Message");
    default:
      break;
    }
  }

  return QVariant();
}

} // namespace nim
