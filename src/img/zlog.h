#pragma once

#include "zglobal.h"
#include "zexception.h"
#include "zflags.h"

#ifdef _WIN32
#undef ERROR
#endif

#define GOOGLE_STRIP_LOG 0

#include <glog/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/chrono.h>
#include <fmt/std.h>
#include <fmt/compile.h>
#include <QDebug>
#include <QPoint>
#include <QRect>

#ifndef Q_MOC_RUN
#define NTEST
#include <reflect>
#endif

#include <functional>
#include <iosfwd>
#include <concepts>

namespace nim {

class ZLogInit
{
public:
  static const ZLogInit& instance(const std::string& appName, const QString& filename = "");

private:
  ZLogInit(const std::string& appName, const QString& filename);

  ~ZLogInit();
};

struct LogData
{
  LogData(google::LogSeverity severity, const google::LogMessageTime& logmsgtime, std::string formatted_msg)
    : level(severity)
    , time(logmsgtime.when())
    , formatted(std::move(formatted_msg))
  {}

  google::LogSeverity level;
  std::chrono::system_clock::time_point time;
  std::string formatted; // formatted log message with level, time, threadid, filename, line, and message
};

__forceinline std::string formatLogMessage(google::LogSeverity severity,
                                           const char* file,
                                           int line,
                                           const google::LogMessageTime& time,
                                           const char* message,
                                           size_t message_len)
{
  return fmt::format(FMT_COMPILE("{}{:%Y%m%d %H:%M:%S} {} {}:{}] {}"),
                     google::GetLogSeverityName(severity)[0],
                     time.when(),
                     std::this_thread::get_id(),
                     file,
                     line,
                     fmt::string_view(message, message_len));
}

// might return nullptr
std::shared_ptr<google::LogSink> createFileLogSink(const QString& filename);

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

void relayLogToQtGUI();

// enum related
template<typename TEnum>
  requires std::is_enum_v<TEnum>
std::string_view enumToString(TEnum e)
{
  auto res = reflect::enum_name(e);
#ifdef _MSC_VER
  using namespace std::literals;
  if (res.empty() || res.find("0x"sv) != res.npos) {
#else
  if (res.empty()) {
#endif
    throw ZException(fmt::format("invalid enum value: {}", std::to_underlying(e)));
  }
  return res;
}

template<typename TEnum>
  requires std::is_enum_v<TEnum>
TEnum stringToEnum(std::string_view s)
{
#ifdef _MSC_VER
  using namespace std::literals;
#endif
  static constexpr auto enumerators =
    reflect::enumerators<TEnum, reflect::enum_min(TEnum{}), reflect::enum_max(TEnum{})>;
  for (size_t i = 0; i < enumerators.size(); ++i) {
#ifdef _MSC_VER
    if (std::string_view(enumerators[i].second).find("0x"sv) != std::string_view::npos) {
      continue;
    }
#endif
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
  requires std::is_scoped_enum_v<TEnum>
auto format_as(TEnum f)
{
  return enumToString(f);
}

// flags
template<typename TEnum>
  requires IsFlags<TEnum>::value
std::string flagsToString(TEnum e)
{
  std::string res = std::string(reflect::enum_name(e));
#ifdef _MSC_VER
  using namespace std::literals;
  if (res.empty() || res.find("0x") != res.npos) {
    res.clear();
#else
  if (res.empty()) {
#endif
    static constexpr auto enumerators =
      reflect::enumerators<TEnum, reflect::enum_min(TEnum{}), reflect::enum_max(TEnum{})>;
    for (size_t i = 0; i < enumerators.size(); ++i) {
#ifdef _MSC_VER
      if (std::string_view(enumerators[i].second).find("0x"sv) != std::string_view::npos) {
        continue;
      }
#endif
      if (enumerators[i].first != 0 && isFlagSet(e, static_cast<TEnum>(enumerators[i].first))) {
        if (res.empty()) {
          res = std::string(enumerators[i].second);
        } else {
          fmt::format_to(std::back_inserter(res), " | {}", enumerators[i].second);
        }
      }
    }
  }
  return res;
}

// inline QByteArray toUtf8QByteArray(const std::wstring& s)
// {
//   return QString::fromStdWString(s).toUtf8();
// }
//
// inline QByteArray toUtf8QByteArray(const std::wstring_view& s)
// {
//   return QString::fromStdWString(std::wstring(s)).toUtf8();
// }
//
// inline QByteArray toUtf8QByteArray(const wchar_t* s)
// {
//   return QString::fromStdWString(std::wstring(s)).toUtf8();
// }

inline std::string toUtf8String(const std::wstring& s)
{
  return QString::fromStdWString(s).toStdString();
}

inline std::string toUtf8String(const std::wstring_view& s)
{
  return QString::fromStdWString(std::wstring(s)).toStdString();
}

inline std::string toUtf8String(const wchar_t* s)
{
  return QString::fromStdWString(std::wstring(s)).toStdString();
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

template<typename T>
concept IsSupportedQtTypeForPrint =
  std::same_as<T, QPoint> || std::same_as<T, QPointF> || std::same_as<T, QRect> || std::same_as<T, QRectF> ||
  std::same_as<T, QSize> || std::same_as<T, QStringList> || std::same_as<T, QList<typename T::value_type>> ||
  std::same_as<T, QContiguousCache<typename T::value_type>> ||
  std::same_as<T, QSharedPointer<typename T::value_type>> || std::same_as<T, QFlags<typename T::enum_type>>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
  || std::same_as<T, QKeyCombination> || std::same_as<T, QTaggedPointer<typename T::Type, typename T::TagType>>
#endif
  ;

inline void logLongString(const std::string& q)
{
  static const size_t maxLogLength = google::LogMessage::kMaxLogMessageLen - 500;
  for (size_t i = 0; i < q.size(); i += maxLogLength) { // glog limit is 30000
    LOG(INFO) << std::string_view(q.data() + i, std::min(maxLogLength, q.size() - i));
  }
}

template<typename TEnum, CanConvertToUtf8QByteArray T>
  requires std::is_enum_v<TEnum>
TEnum stringToEnum(const T& s)
{
  auto u8 = s.toUtf8();
  return stringToEnum<TEnum>(std::string_view(u8.data(), u8.size()));
}

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

namespace fmt {

template<>
struct is_range<QByteArray, char> : std::false_type
{};
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
template<>
struct is_range<QByteArrayView, char> : std::false_type
{};
template<>
struct is_range<QUtf8StringView, char> : std::false_type
{};
#endif

} // namespace fmt

template<nim::IsUtf8ArrayType T>
struct fmt::formatter<T> : fmt::formatter<fmt::string_view>
{
  auto format(const T& s, format_context& ctx) const
  {
    return fmt::formatter<fmt::string_view>::format(fmt::string_view(s.data(), s.size()), ctx);
  }
};

#define DEFINE_FMT_SPECIALIAZATION_FOR_HAVE_TOSTRING_TYPE(T)    \
  template<>                                                    \
  struct fmt::formatter<T> : fmt::formatter<std::string>        \
  {                                                             \
    auto format(const T& a, fmt::format_context& ctx) const     \
    {                                                           \
      return formatter<std::string>::format(a.toString(), ctx); \
    }                                                           \
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

template<nim::IsSupportedQtTypeForPrint T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  auto u8 = nim::qtTypeToQString(v).toUtf8();
  return (s << std::string_view(u8.data(), u8.size()));
}
