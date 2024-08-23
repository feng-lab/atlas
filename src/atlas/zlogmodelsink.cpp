#include "zlogmodelsink.h"

#include <QColor>

namespace nim {

const char* const ZLogModelSink::Type = "window";

ZLogModelSink::ZLogModelSink(size_t max_items)
  : m_maxItems(max_items)
{}

void ZLogModelSink::addEntry(const LogData& message)
{
  const int next_idx = m_logDatas.size();
  beginInsertRows(QModelIndex(), next_idx, next_idx);
  {
    QWriteLocker lock(&m_messagesLock);
    m_logDatas.push_back(message);
  }
  endInsertRows();

  if (m_maxItems < std::numeric_limits<int>::max() && m_logDatas.size() > m_maxItems) {
    {
      QWriteLocker lock(&m_messagesLock);
      m_logDatas.pop_front();
    }
    // Every item changed
    const QModelIndex idx1 = index(0, 0);
    const QModelIndex idx2 = index(m_logDatas.size(), rowCount(QModelIndex()));
    Q_EMIT dataChanged(idx1, idx2);
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

int ZLogModelSink::columnCount(const QModelIndex& /*parent*/) const
{
  return 3;
}

int ZLogModelSink::rowCount(const QModelIndex& /*parent*/) const
{
  QReadLocker lock(&m_messagesLock);

  return m_logDatas.size();
}

QVariant ZLogModelSink::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) {
    return {};
  }

  if (role == Qt::DisplayRole) {
    QReadLocker lock(&m_messagesLock);

    const LogData& item = m_logDatas.at(index.row());

    switch (index.column()) {
      case TimeColumn:
        return QByteArray::fromStdString(fmt::format("{}", item.time));
      case LevelNameColumn:
        return GetLogSeverityName(item.level);
      case MessageColumn:
        return QByteArray(item.formatted.data(), item.formatted.size());
      default:
        return {};
    }
  }

  if (role == Qt::BackgroundRole) {
    QReadLocker lock(&m_messagesLock);

    const LogData& item = m_logDatas.at(index.row());

    switch (item.level) {
      case google::GLOG_WARNING:
        return QVariant(QColor(255, 255, 128));
      case google::GLOG_ERROR:
        return QVariant(QColor(255, 128, 128));
      case google::GLOG_FATAL:
        return QVariant(QColor(255, 0, 0));
      default:
        break;
    }
  }

  return {};
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

  return {};
}

} // namespace nim
