#pragma once

#include "zglobal.h"

#ifdef _WIN32
#undef ERROR
#endif

#define GOOGLE_STRIP_LOG 0

#include <glog/logging.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <QDateTime>
#include <QDebug>
#include <QPoint>
#include <QRect>
#include <memory>
#include <functional>
#include <iosfwd>
#include <type_traits>

namespace nim {

void initLogging(const char* argv0, const QString& filename = "");

void shutdownLogging();

using LogSink = google::LogSink;
using LogSinkPtr = std::shared_ptr<google::LogSink>;
using LogSeverity = google::LogSeverity;
const LogSeverity InfoLevel = google::GLOG_INFO;
const LogSeverity WarningLevel = google::GLOG_WARNING;
const LogSeverity ErrorLevel = google::GLOG_ERROR;
const LogSeverity FatalLevel = google::GLOG_FATAL;

struct LogData
{
  LogData(LogSeverity severity,
          const char* full_filename,
          const char* base_filename,
          int line_,
          const std::tm& tm,
          const char* msg,
          size_t message_len,
          std::string formatted_msg)
    : level(severity)
    , fullFilename(full_filename)
    , baseFilename(base_filename)
    , line(line_)
    , time(QDate(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday), QTime(tm.tm_hour, tm.tm_min, tm.tm_sec))
    , message(msg, message_len)
    , formatted(QString::fromStdString(formatted_msg))
  {}

  LogSeverity level;
  std::string fullFilename;
  std::string baseFilename;
  int line;
  QDateTime time;
  QByteArray message; // main log message
  QString formatted; // formatted log message with level, time, threadid, filename, line, and message
};

using LogFunction = std::function<void(const LogData&)>;

// might return nullptr
LogSinkPtr createFileLogSink(const QString& filename);

LogSinkPtr createFunctorLogSink(const LogFunction& f);

inline void addLogSink(LogSink* sink)
{
  if (sink) {
    google::AddLogSink(sink);
  }
}

inline void addLogSink(const LogSinkPtr& sink)
{
  if (sink) {
    google::AddLogSink(sink.get());
  }
}

inline void removeLogSink(LogSink* sink)
{
  if (sink) {
    google::RemoveLogSink(sink);
  }
}

inline void removeLogSink(const LogSinkPtr& sink)
{
  if (sink) {
    google::RemoveLogSink(sink.get());
  }
}

QString levelToString(LogSeverity theLevel);

#define LINFOF(file, line) google::LogMessage(file, line, google::GLOG_INFO).stream()
#define LWARNF(file, line) google::LogMessage(file, line, google::GLOG_WARNING).stream()
#define LERRORF(file, line) google::LogMessage(file, line, google::GLOG_ERROR).stream()
#define LFATALF(file, line) google::LogMessage(file, line, google::GLOG_FATAL).stream()

inline std::ostream& operator<<(std::ostream& s, const QByteArray& q)
{
  return (s << q.constData());
}

inline std::ostream& operator<<(std::ostream& s, const QString& q)
{
  return (s << q.toUtf8().constData());
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
inline std::ostream& operator<<(std::ostream& s, QStringView q)
{
  return (s << q.toUtf8().constData());
}
inline std::ostream& operator<<(std::ostream& s, QUtf8StringView q)
{
  return (s << q.data());
}
inline std::ostream& operator<<(std::ostream& s, QByteArrayView q)
{
  return (s << q.constData());
}
#else
inline std::ostream& operator<<(std::ostream& s, QStringRef q)
{
  return (s << q.toUtf8().constData());
}
#endif

// template<class T, std::size_t N>
// inline std::ostream& operator<<(std::ostream& s, const std::array<T, N>& arr)
//{
//   std::copy(arr.cbegin(), arr.cend(), std::ostream_iterator<T>(s, " "));
//   return s;
// }

inline void logLongString(const QString& q)
{
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
  for (qsizetype i = 0; i < q.size(); i += 10000) { // glog limit is 30000
    auto length = std::min(qsizetype(10000), q.size() - i);
    QStringView qView(q.data() + i, length);
    LOG(INFO) << qView;
  }
#else
  for (int i = 0; i < q.size(); i += 10000) { // glog limit is 30000
    int length = std::min(10000, q.size() - i);
    QStringRef qRef(&q, i, length);
    LOG(INFO) << qRef;
  }
#endif
}

template<typename T>
inline QString qtTypeToQString(const T& v)
{
  QString buffer;
  QDebug out(&buffer);
  out << v;
  return buffer;
}

inline std::ostream& operator<<(std::ostream& s, const QPoint& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

inline std::ostream& operator<<(std::ostream& s, const QPointF& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

inline std::ostream& operator<<(std::ostream& s, const QRect& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

inline std::ostream& operator<<(std::ostream& s, const QRectF& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QList<T>& list)
{
  return (s << qtTypeToQString(list).toUtf8().constData());
}

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QVector<T>& vec)
{
  return (s << qtTypeToQString(vec).toUtf8().constData());
}
#endif

// template<typename T, typename Alloc>
// inline std::ostream& operator<<(std::ostream& s, const std::vector<T, Alloc>& vec)
//{
//   return (s << qtTypeToQString(vec).toUtf8().constData());
// }
//
// template<typename T, typename Alloc>
// inline std::ostream& operator<<(std::ostream& s, const std::list<T, Alloc>& vec)
//{
//   return (s << qtTypeToQString(vec).toUtf8().constData());
// }
//
// template<typename Key, typename T, typename Compare, typename Alloc>
// inline std::ostream& operator<<(std::ostream& s, const std::map<Key, T, Compare, Alloc>& map)
//{
//   return (s << qtTypeToQString(map).toUtf8().constData());
// }
//
// template<typename Key, typename T, typename Compare, typename Alloc>
// inline std::ostream& operator<<(std::ostream& s, const std::multimap<Key, T, Compare, Alloc>& map)
//{
//   return (s << qtTypeToQString(map).toUtf8().constData());
// }

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QMap<Key, T>& map)
{
  return (s << qtTypeToQString(map).toUtf8().constData());
}

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QMultiMap<Key, T>& map)
{
  return (s << qtTypeToQString(map).toUtf8().constData());
}

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QHash<Key, T>& hash)
{
  return (s << qtTypeToQString(hash).toUtf8().constData());
}

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QMultiHash<Key, T>& hash)
{
  return (s << qtTypeToQString(hash).toUtf8().constData());
}

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
template<class T1, class T2>
inline std::ostream& operator<<(std::ostream& s, const QPair<T1, T2>& pair)
{
  return (s << qtTypeToQString(pair).toUtf8().constData());
}
#endif

// template<class T1, class T2>
// inline std::ostream& operator<<(std::ostream& s, const std::pair<T1, T2>& pair)
//{
//   return (s << qtTypeToQString(pair).toUtf8().constData());
// }

template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QSet<T>& set)
{
  return (s << qtTypeToQString(set).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QContiguousCache<T>& cache)
{
  return (s << qtTypeToQString(cache).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QSharedPointer<T>& ptr)
{
  return (s << qtTypeToQString(ptr).toUtf8().constData());
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<typename T, typename Tag>
inline std::ostream& operator<<(std::ostream& s, const QTaggedPointer<T, Tag>& ptr)
{
  return (s << qtTypeToQString(ptr).toUtf8().constData());
}
#endif

template<typename T>
inline typename std::enable_if<QtPrivate::IsQEnumHelper<T>::Value, std::ostream&>::Type operator<<(std::ostream& s,
                                                                                                   T value)
{
  return (s << qtTypeToQString(value).toUtf8().constData());
}

template<typename T,
         typename A = typename std::enable_if<std::is_enum<T>::value, void>::type,
         typename B = typename std::enable_if<sizeof(T) <= sizeof(int), void>::type,
         typename C = typename std::enable_if<!QtPrivate::IsQEnumHelper<T>::Value, void>::type,
         typename D = typename std::enable_if<QtPrivate::IsQEnumHelper<QFlags<T>>::Value, void>::type>
inline std::ostream& operator<<(std::ostream& s, T value)
{
  return (s << qtTypeToQString(value).toUtf8().constData());
}

template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QFlags<T>& flags)
{
  return (s << qtTypeToQString(flags).toUtf8().constData());
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
inline std::ostream& operator<<(std::ostream& s, QKeyCombination combination)
{
  return (s << qtTypeToQString(combination).toUtf8().constData());
}
#endif

} // namespace nim

template<>
struct fmt::formatter<QString> : fmt::formatter<const char*>
{
  auto format(const QString& s, format_context& ctx)
  {
    return formatter<const char*>::format(s.toUtf8().constData(), ctx);
  }
};

template<>
struct fmt::formatter<QByteArray> : fmt::formatter<const char*>
{
  auto format(const QByteArray& s, format_context& ctx)
  {
    return formatter<const char*>::format(s.constData(), ctx);
  }
};

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<>
struct fmt::formatter<QStringView> : fmt::formatter<const char*>
{
  auto format(QStringView s, format_context& ctx)
  {
    return formatter<const char*>::format(s.toUtf8().constData(), ctx);
  }
};
template<>
struct fmt::formatter<QUtf8StringView> : fmt::formatter<const char*>
{
  auto format(QUtf8StringView s, format_context& ctx)
  {
    return formatter<const char*>::format(s.data(), ctx);
  }
};
template<>
struct fmt::formatter<QByteArrayView> : fmt::formatter<const char*>
{
  auto format(QByteArrayView s, format_context& ctx)
  {
    return formatter<const char*>::format(s.constData(), ctx);
  }
};
#else
template<>
struct fmt::formatter<QStringRef> : fmt::formatter<const char*>
{
  auto format(QStringRef s, format_context& ctx)
  {
    return formatter<const char*>::format(s.toUtf8().constData(), ctx);
  }
};
#endif

#if (QT_VERSION < QT_VERSION_CHECK(5, 14, 0))
namespace std {

template<>
struct hash<QString>
{
  size_t operator()(const QString& s) const noexcept
  {
    return qHash(s, qHash(std::hash<int>{}(0)));
  }
};

} // namespace std
#endif
