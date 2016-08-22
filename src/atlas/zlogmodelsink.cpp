#include "zlogmodelsink.h"

#include <QColor>
#include <QTimer>
#include <iostream>

namespace nim {

ZLogModelSink& ZLogModelSink::instance()
{
  static ZLogModelSink modelDestination;
  return modelDestination;
}

const char* const ZLogModelSink::Type = "window";

ZLogModelSink::ZLogModelSink(int max_items)
  : m_maxItems(max_items)
  , m_timer(new QTimer(this))
{
  m_timer->setSingleShot(true);
  connect(m_timer, &QTimer::timeout, this, &ZLogModelSink::sendLogData);
}

void ZLogModelSink::send(LogSeverity severity, const char* full_filename, const char* base_filename, int line,
                         const tm* tm_time, const char* message, size_t prefix_len, size_t message_len)
{
  addEntry(LogData(severity, full_filename, base_filename, line, tm_time, message, prefix_len, message_len));
}

void ZLogModelSink::addEntry(const LogData& message)
{
  const int next_idx = m_logDatas.size();
  beginInsertRows(QModelIndex(), next_idx, next_idx);
  {
    QWriteLocker lock(&m_messagesLock);
    m_logDatas.push_back(message);
  }
  endInsertRows();
  if (!m_timer->isActive()) {
    m_timer->start(200);
  }

  if (m_maxItems < std::numeric_limits<int>::max() && m_logDatas.size() > m_maxItems) {
    {
      QWriteLocker lock(&m_messagesLock);
      m_logDatas.pop_front();
    }
    // Every item changed
    const QModelIndex idx1 = index(0, 0);
    const QModelIndex idx2 = index(m_logDatas.size(), rowCount());
    emit dataChanged(idx1, idx2);
  }
}

void ZLogModelSink::clear()
{
  beginResetModel();
  {
    QWriteLocker lock(&m_messagesLock);
    m_logDatas.clear();
  }
  endResetModel();
}

LogData ZLogModelSink::at(size_t index)
{
  return m_logDatas[index];
}

int ZLogModelSink::columnCount(const QModelIndex& parent) const
{
  Q_UNUSED(parent);
  return 3;
}

int ZLogModelSink::rowCount(const QModelIndex& parent) const
{
  Q_UNUSED(parent);
  QReadLocker lock(&m_messagesLock);

  return m_logDatas.size();
}

QVariant ZLogModelSink::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  if (role == Qt::DisplayRole) {
    QReadLocker lock(&m_messagesLock);

    const LogData& item = m_logDatas.at(index.row());

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
    QReadLocker lock(&m_messagesLock);

    const LogData& item = m_logDatas.at(index.row());

    switch (item.level) {
      case WarningLevel:
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

void ZLogModelSink::sendLogData()
{
  int start = m_unsendLogDataStart;
  m_unsendLogDataStart = m_logDatas.size();
  if (m_unsendLogDataStart > start) {
    emit logDataReady(&m_logDatas, start, m_unsendLogDataStart);
  }
}

} // namespace nim
