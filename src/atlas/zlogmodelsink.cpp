#include "zlogmodelsink.h"

#include <QColor>
#include <cassert>

namespace nim {

LogSinkPtr logModelSinkInstance()
{
  static LogSinkPtr modelDestination(new ZLogModelSink());
  return modelDestination;
}

#ifdef _USE_QSLOG_
const std::deque<LogData>& logMessages()
{
  ZLogModelSink *md = dynamic_cast<ZLogModelSink*>(logModelSinkInstance().data());
  assert(md);
  return md->logMessages();
}
#else
const std::deque<LogData>& logMessages()
{
  ZLogModelSink *md = dynamic_cast<ZLogModelSink*>(logModelSinkInstance().get());
  assert(md);
  return md->logMessages();
}
#endif

const char* const ZLogModelSink::Type = "window";

ZLogModelSink::ZLogModelSink(size_t max_items) :
  mMaxItems(max_items)
{
}

#ifdef _USE_QSLOG_
void ZLogModelSink::write(const LogData& message)
{
  addEntry(message);
}

bool ZLogModelSink::isValid()
{
  return true;
}

QString ZLogModelSink::type() const
{
  return QString(Type);
}
#else
void ZLogModelSink::send(LogSeverity severity, const char *full_filename, const char *base_filename, int line,
                         const tm *tm_time, const char *message, size_t prefix_len, size_t message_len)
{
  addEntry(LogData(severity, full_filename, base_filename, line, tm_time, message, prefix_len, message_len));
}

#endif

void ZLogModelSink::addEntry(const LogData& message)
{
  const int next_idx = static_cast<int>(mLogDatas.size());
  beginInsertRows(QModelIndex(), next_idx, next_idx);
  {
    QWriteLocker lock(&mMessagesLock);
    mLogDatas.push_back(message);
  }
  endInsertRows();

  if (mMaxItems < std::numeric_limits<size_t>::max() && mLogDatas.size() > mMaxItems) {
    {
      QWriteLocker lock(&mMessagesLock);
      mLogDatas.pop_front();
    }
    // Every item changed
    const QModelIndex idx1 = index(0, 0);
    const QModelIndex idx2 = index(static_cast<int>(mLogDatas.size()), rowCount());
    emit dataChanged(idx1, idx2);
  }
}

void ZLogModelSink::clear()
{
  beginResetModel();
  {
    QWriteLocker lock(&mMessagesLock);
    mLogDatas.clear();
  }
  endResetModel();
}

LogData ZLogModelSink::at(size_t index)
{
  return mLogDatas[index];
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

  return static_cast<int>(mLogDatas.size());
}

QVariant ZLogModelSink::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  if (role == Qt::DisplayRole) {
    QReadLocker lock(&mMessagesLock);

    const LogData& item = mLogDatas.at(index.row());

    switch (index.column()) {
    case TimeColumn:
      return item.time.toLocalTime().toString();
    case LevelNameColumn:
      return levelToString(item.level);
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

    const LogData& item = mLogDatas.at(index.row());

    switch (item.level)
    {
    case WARNING:
      return QVariant(QColor(255, 255, 128));
    case ERROR:
      return QVariant(QColor(255, 128, 128));
    case FATAL:
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
