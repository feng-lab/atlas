#pragma once

#include "zglobal.h"
#include "zexception.h"

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
#include <reflect>
#include <functional>
#include <iosfwd>
#include <type_traits>

namespace nim {

void initLogging(const char* argv0, const QString& filename = "");

void shutdownLogging();

using LogSink = google::LogSink;
using LogSinkPtr = std::shared_ptr<google::LogSink>;
using LogSeverity = google::LogSeverity;
constexpr LogSeverity InfoLevel = google::GLOG_INFO;
constexpr LogSeverity WarningLevel = google::GLOG_WARNING;
constexpr LogSeverity ErrorLevel = google::GLOG_ERROR;
constexpr LogSeverity FatalLevel = google::GLOG_FATAL;

struct LogData
{
  LogData(LogSeverity severity,
          const char* full_filename,
          const char* base_filename,
          int line_,
          const std::tm& tm,
          const char* msg,
          size_t message_len,
          const std::string& formatted_msg)
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

// enum related
template<typename TEnum>
  requires std::is_enum_v<TEnum>
std::string_view enumToString(TEnum e)
{
  auto res = reflect::enum_name(e);
  if (res.empty()) {
    throw ZException(fmt::format("invalid enum value: {}", std::to_underlying(e)));
  }
  return res;
}

template<typename TEnum>
  requires std::is_enum_v<TEnum>
TEnum stringToEnum(std::string_view s)
{
  static constexpr auto enumerators =
    reflect::enumerators<TEnum, reflect::enum_min(TEnum{}), reflect::enum_max(TEnum{})>;
  for (size_t i = 0; i < enumerators.size(); ++i) {
    if (s == enumerators[i].second) {
      return static_cast<TEnum>(enumerators[i].first);
    }
  }
  throw ZException(fmt::format("invalid enum string: {}", s));
}

template<typename TEnum>
  requires std::is_enum_v<TEnum>
QString enumToQString(TEnum e)
{
  auto str = enumToString(e);
  return QString::fromUtf8(str.data(), str.size());
}

template<typename TEnum>
  requires std::is_enum_v<TEnum>
TEnum stringToEnum(QStringView s)
{
  auto str = s.toUtf8();
  return stringToEnum<TEnum>(std::string_view(str.data(), str.size()));
}

template<typename TEnum>
  requires std::is_scoped_enum_v<TEnum>
auto format_as(TEnum f)
{
  return enumToString(f);
}

// stream support for qt types
template<typename T>
QString qtTypeToQString(const T& v)
{
  QString buffer;
  QDebug out(&buffer);
  out << v;
  return buffer;
}

#define OUTPUT_QT_TYPE_VALUE(v)          \
  auto u8 = qtTypeToQString(v).toUtf8(); \
  return (s << std::string_view(u8.data(), u8.size()));

#define OUTPUT_QSTRING(v) \
  auto u8 = v.toUtf8();   \
  return (s << std::string_view(u8.data(), u8.size()));

#define OUTPUT_QBYTEARRAY(v) return (s << std::string_view(v.data(), v.size()));

inline std::ostream& operator<<(std::ostream& s, const QByteArray& v)
{
  OUTPUT_QBYTEARRAY(v)
}

inline std::ostream& operator<<(std::ostream& s, const QString& v)
{
  OUTPUT_QSTRING(v)
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
inline std::ostream& operator<<(std::ostream& s, QStringView v)
{
  OUTPUT_QSTRING(v)
}
inline std::ostream& operator<<(std::ostream& s, QUtf8StringView v)
{
  OUTPUT_QBYTEARRAY(v)
}
inline std::ostream& operator<<(std::ostream& s, QByteArrayView v)
{
  OUTPUT_QBYTEARRAY(v)
}
#else
inline std::ostream& operator<<(std::ostream& s, QStringRef v)
{
  OUTPUT_QSTRING(v)
}
#endif

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

inline std::ostream& operator<<(std::ostream& s, const QPoint& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

inline std::ostream& operator<<(std::ostream& s, const QPointF& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

inline std::ostream& operator<<(std::ostream& s, const QRect& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

inline std::ostream& operator<<(std::ostream& s, const QRectF& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class T>
std::ostream& operator<<(std::ostream& s, const QList<T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class Key, class T>
std::ostream& operator<<(std::ostream& s, const QMap<Key, T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class Key, class T>
std::ostream& operator<<(std::ostream& s, const QMultiMap<Key, T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class Key, class T>
std::ostream& operator<<(std::ostream& s, const QHash<Key, T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class Key, class T>
std::ostream& operator<<(std::ostream& s, const QMultiHash<Key, T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<typename T>
std::ostream& operator<<(std::ostream& s, const QSet<T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class T>
std::ostream& operator<<(std::ostream& s, const QContiguousCache<T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class T>
std::ostream& operator<<(std::ostream& s, const QSharedPointer<T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<typename T, typename Tag>
std::ostream& operator<<(std::ostream& s, const QTaggedPointer<T, Tag>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}
#endif

template<typename T>
typename std::enable_if<QtPrivate::IsQEnumHelper<T>::Value, std::ostream&>::Type operator<<(std::ostream& s, T v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<typename T,
         typename A = std::enable_if_t<std::is_enum_v<T>, void>,
         typename B = std::enable_if_t<sizeof(T) <= sizeof(int), void>,
         typename C = std::enable_if_t<!QtPrivate::IsQEnumHelper<T>::Value, void>,
         typename D = std::enable_if_t<QtPrivate::IsQEnumHelper<QFlags<T>>::Value, void>>
std::ostream& operator<<(std::ostream& s, T v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<typename T>
std::ostream& operator<<(std::ostream& s, const QFlags<T>& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
inline std::ostream& operator<<(std::ostream& s, QKeyCombination v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}
#endif

} // namespace nim

template<>
struct fmt::formatter<QString> : formatter<string_view>
{
  auto format(const QString& s, format_context& ctx)
  {
    auto u8 = s.toUtf8();
    return formatter<string_view>::format(string_view(u8.data(), u8.size()), ctx);
  }
};

template<>
struct fmt::formatter<QByteArray> : formatter<string_view>
{
  auto format(const QByteArray& s, format_context& ctx)
  {
    return formatter<string_view>::format(string_view(s.data(), s.size()), ctx);
  }
};

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<>
struct fmt::formatter<QStringView> : formatter<string_view>
{
  auto format(QStringView s, format_context& ctx)
  {
    auto u8 = s.toUtf8();
    return formatter<string_view>::format(string_view(u8.data(), u8.size()), ctx);
  }
};
template<>
struct fmt::formatter<QUtf8StringView> : formatter<string_view>
{
  auto format(QUtf8StringView s, format_context& ctx)
  {
    return formatter<string_view>::format(string_view(s.data(), s.size()), ctx);
  }
};
template<>
struct fmt::formatter<QByteArrayView> : formatter<string_view>
{
  auto format(QByteArrayView s, format_context& ctx)
  {
    return formatter<string_view>::format(string_view(s.data(), s.size()), ctx);
  }
};
#else
template<>
struct fmt::formatter<QStringRef> : formatter<string_view>
{
  auto format(QStringRef s, format_context& ctx)
  {
    auto u8 = s.toUtf8();
    return formatter<string_view>::format(string_view(u8.data(), u8.size()), ctx);
  }
};
#endif
