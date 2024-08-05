#pragma once

#include "zglobal.h"
#include "zexception.h"

#ifdef _WIN32
#undef ERROR
#endif

#define GOOGLE_STRIP_LOG 0

#include <glog/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <QDateTime>
#include <QDebug>
#include <QPoint>
#include <QRect>

#ifndef Q_MOC_RUN
#define NTEST
#include <reflect>
#endif

#include <functional>
#include <iosfwd>

namespace nim {

void initLogging(const char* argv0, const QString& filename = "");

void shutdownLogging();

struct LogData
{
  LogData(google::LogSeverity severity,
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

  google::LogSeverity level;
  std::string fullFilename;
  std::string baseFilename;
  int line;
  QDateTime time;
  QByteArray message; // main log message
  QString formatted; // formatted log message with level, time, threadid, filename, line, and message
};

using LogFunction = std::function<void(const LogData&)>;

// might return nullptr
std::shared_ptr<google::LogSink> createFileLogSink(const QString& filename);

std::shared_ptr<google::LogSink> createFunctorLogSink(const LogFunction& f);

inline void addLogSink(google::LogSink* sink)
{
  if (sink) {
    google::AddLogSink(sink);
  }
}

inline void addLogSink(const std::shared_ptr<google::LogSink>& sink)
{
  if (sink) {
    google::AddLogSink(sink.get());
  }
}

inline void removeLogSink(google::LogSink* sink)
{
  if (sink) {
    google::RemoveLogSink(sink);
  }
}

inline void removeLogSink(const std::shared_ptr<google::LogSink>& sink)
{
  if (sink) {
    google::RemoveLogSink(sink.get());
  }
}

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

inline QByteArray toUtf8QByteArray(const std::wstring& s)
{
  return QString::fromStdWString(s).toUtf8();
}

inline QByteArray toUtf8QByteArray(const std::wstring_view& s)
{
  return QString::fromStdWString(std::wstring(s)).toUtf8();
}

inline QByteArray toUtf8QByteArray(const wchar_t* s)
{
  return QString::fromStdWString(std::wstring(s)).toUtf8();
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

template<typename T>
concept CanConvertToUtf8QByteArray = requires(const T& a) {
  {
    a.toUtf8()
  } -> std::same_as<QByteArray>;
};

template<class T>
concept IsUtf8ArrayType = IsAnyOf<T,
                                  QByteArray
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
                                  ,
                                  QByteArrayView,
                                  QUtf8StringView
#endif
                                  >;

} // namespace nim

template<nim::CanConvertToUtf8QByteArray T>
struct fmt::formatter<T> : fmt::formatter<fmt::string_view>
{
  auto format(const T& s, format_context& ctx) const
  {
    auto u8 = s.toUtf8();
    return fmt::formatter<fmt::string_view>::format(fmt::string_view(u8.data(), u8.size()), ctx);
  }
};

template<nim::IsUtf8ArrayType T>
struct fmt::formatter<T> : fmt::formatter<fmt::string_view>
{
  auto format(const T& s, format_context& ctx) const
  {
    return fmt::formatter<fmt::string_view>::format(fmt::string_view(s.data(), s.size()), ctx);
  }
};

template<nim::CanConvertToUtf8QByteArray T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  auto u8 = v.toUtf8();
  return (s << std::string_view(u8.data(), u8.size()));
}

template<nim::IsUtf8ArrayType T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  return (s << std::string_view(v.data(), v.size()));
}

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

#define OUTPUT_QT_TYPE_VALUE(v)               \
  auto u8 = nim::qtTypeToQString(v).toUtf8(); \
  return (s << std::string_view(u8.data(), u8.size()));

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

inline std::ostream& operator<<(std::ostream& s, const QSize& v)
{
  OUTPUT_QT_TYPE_VALUE(v)
}

template<class T>
std::ostream& operator<<(std::ostream& s, const QList<T>& v)
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
