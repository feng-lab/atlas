#pragma once

#include "zglobal.h"
#include "zexception.h"
#include "zflags.h"

#ifdef _WIN32
#undef ERROR
#endif

// Atlas owns the LOG/CHECK macro namespace through Abseil. Some vendored Folly
// headers include <glog/logging.h>; predefine the stable include guard from our
// frozen glog copy so those includes cannot replace Abseil logging macros.
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif

#if defined(GOOGLE_LOG_INFO) && !defined(ATLAS_ALLOW_GLOG_LOGGING_H)
#error "glog/logging.h must not be included before zlog.h; include zlog.h or zfolly.h first."
#endif

#define GLOG_LOGGING_H

#include <absl/base/log_severity.h>
#include <absl/log/check.h>
#include <absl/log/die_if_null.h>
#include <absl/log/log.h>
#include <absl/log/log_entry.h>
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <absl/log/vlog_is_on.h>
#include <absl/strings/string_view.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/chrono.h>
#include <fmt/std.h>
#include <fmt/compile.h>
#include <QDebug>
#include <QList>
#include <QRect>
#include <QStringList>

#ifndef Q_MOC_RUN
#define NTEST
#include <reflect>
#endif

#include <functional>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef CHECK_NOTNULL
#define CHECK_NOTNULL(val) ABSL_DIE_IF_NULL(val)
#endif

#ifndef DCHECK_NOTNULL
#ifndef NDEBUG
#define DCHECK_NOTNULL(val) CHECK_NOTNULL(val)
#else
#define DCHECK_NOTNULL(val) (val)
#endif
#endif

namespace nim {

class ZLogInit
{
public:
  static const ZLogInit& instance(std::string programNameOrPath, const QString& filename = "");
  static bool isInitialized();

private:
  ZLogInit(std::string programNameOrPath, const QString& filename);

  ~ZLogInit();

  std::vector<std::shared_ptr<absl::LogSink>> m_logSinks;
};

struct LogData
{
  explicit LogData(const absl::LogEntry& entry);

  absl::LogSeverity level;
  std::chrono::system_clock::time_point time;
  std::string formatted; // Abseil-formatted log message including the active prefix, without trailing newline
};

// might return nullptr
std::shared_ptr<absl::LogSink> createFileLogSink(const QString& filename);

inline void addLogSink(absl::LogSink* sink)
{
  if (sink) {
    absl::AddLogSink(sink);
  }
}

inline void addLogSink(const std::shared_ptr<absl::LogSink>& sink)
{
  if (sink) {
    absl::AddLogSink(sink.get());
  }
}

inline void removeLogSink(absl::LogSink* sink)
{
  if (sink) {
    absl::RemoveLogSink(sink);
  }
}

inline void removeLogSink(const std::shared_ptr<absl::LogSink>& sink)
{
  if (sink) {
    absl::RemoveLogSink(sink.get());
  }
}

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
    auto errorMessage =
      fmt::format("invalid enum value {} of type {}", std::to_underlying(e), reflect::type_name<TEnum>());
    LOG(ERROR) << errorMessage;
    throw ZException(errorMessage);
  }
  return res;
}

// No-throw: return enum name or provided fallback if reflection has no name
template<typename TEnum>
  requires std::is_enum_v<TEnum>
std::string_view enumToStringOr(TEnum e, std::string_view fallback) noexcept
{
  auto res = reflect::enum_name(e);
#ifdef _MSC_VER
  using namespace std::literals;
  if (res.empty() || res.find("0x"sv) != res.npos) {
#else
  if (res.empty()) {
#endif
    return fallback;
  }
  return res;
}

// Qt convenience: return enum name or fallback (no throw)
template<typename TEnum>
  requires std::is_enum_v<TEnum>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
QString enumToQStringOr(TEnum e, QStringView fallback) noexcept
#else
QString enumToQStringOr(TEnum e, const QString& fallback) noexcept
#endif
{
  const auto sv = enumToStringOr(e, std::string_view{});
  if (!sv.empty()) {
    return QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
  }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
  return QString(fallback);
#else
  return fallback;
#endif
}

// Wrapper for logging: prints enum name if available, otherwise underlying value
template<typename TEnum>
struct EnumOrUnderlying
{
  static_assert(std::is_enum_v<TEnum>, "TEnum must be an enum type");
  TEnum value;
  int base = 10; // 10 for decimal, 16 for hex (0x...)
};

template<typename TEnum>
constexpr EnumOrUnderlying<TEnum> enumOrUnderlying(TEnum e, int base = 10) noexcept
{
  return {e, base};
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

inline std::string toUtf8String(const std::wstring& s)
{
  return QString::fromStdWString(s).toStdString();
}

inline std::string toUtf8String(std::wstring_view s)
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
  { a.toUtf8() } -> std::same_as<QByteArray>;
};

template<typename T>
concept HaveToStringFunction = requires(const T& a) {
  { a.toString() } -> std::same_as<std::string>;
};

template<typename T>
concept IsEigenDenseLike = requires {
  typename std::remove_cvref_t<T>::Scalar;
  typename std::remove_cvref_t<T>::StorageKind;
  typename std::remove_cvref_t<T>::StorageIndex;
  { std::remove_cvref_t<T>::RowsAtCompileTime } -> std::convertible_to<int>;
  { std::remove_cvref_t<T>::ColsAtCompileTime } -> std::convertible_to<int>;
};

template<typename T>
concept IsFmtTupleLoggable = (!IsEigenDenseLike<T>) && fmt::is_tuple_like<T>::value;

template<typename T>
concept IsFmtRangeLoggable =
  (!CanConvertToUtf8QByteArray<T>) && (!IsEigenDenseLike<T>) && fmt::is_range<T, char>::value;

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
  std::same_as<T, QRect> || std::same_as<T, QRectF> || std::same_as<T, QContiguousCache<typename T::value_type>> ||
  std::same_as<T, QSharedPointer<typename T::value_type>> || std::same_as<T, QFlags<typename T::enum_type>>
  ;

inline void logLongString(const std::string& q)
{
  static constexpr size_t maxLogLength = 30000;
  for (size_t i = 0; i < q.size(); i += maxLogLength) {
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

template<HaveToStringFunction T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  return (s << v.toString());
}

template<IsFmtTupleLoggable T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  return (s << fmt::format("{}", v));
}

template<IsFmtRangeLoggable T>
std::ostream& operator<<(std::ostream& s, const T& v)
{
  return (s << fmt::format("{}", v));
}

} // namespace nim

// fmt formatter for nim::EnumOrUnderlying<TEnum>
template<typename TEnum, typename Char>
struct fmt::formatter<nim::EnumOrUnderlying<TEnum>, Char>
{
  // No custom format specifiers; accept default {}
  constexpr auto parse(fmt::basic_format_parse_context<Char>& ctx)
  {
    return ctx.begin();
  }

  template<typename FormatContext>
  auto format(const nim::EnumOrUnderlying<TEnum>& w, FormatContext& ctx) const
  {
    auto sv = nim::enumToStringOr(w.value, std::string_view{});
    if (!sv.empty()) {
      return fmt::format_to(ctx.out(), FMT_STRING("{}"), sv);
    }
    using U = std::underlying_type_t<TEnum>;
    U v = static_cast<U>(w.value);
    if (w.base == 16) {
      using Uns = std::make_unsigned_t<U>;
      return fmt::format_to(ctx.out(), FMT_STRING("0x{:x}"), static_cast<Uns>(v));
    }
    return fmt::format_to(ctx.out(), FMT_STRING("{}"), v);
  }
};

// ostream << support for nim::EnumOrUnderlying<TEnum>
namespace nim {
template<typename TEnum>
inline std::ostream& operator<<(std::ostream& os, const EnumOrUnderlying<TEnum>& w)
{
  // Reuse the fmt::formatter specialization to avoid duplication.
  fmt::format_to(std::ostream_iterator<char>(os), FMT_STRING("{}"), w);
  return os;
}

} // namespace nim

namespace fmt {

// Prevent Qt UTF-8 byte containers from being treated as ranges by fmt/ranges.h.
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

template<typename T>
inline constexpr bool IsStringViewFormattableQtType = nim::CanConvertToUtf8QByteArray<T> || nim::IsUtf8ArrayType<T> ||
                                                      nim::HaveToStringFunction<T> || nim::IsSupportedQtTypeForPrint<T>;

// Generic fmt formatter for selected Qt-ish types.
//
// Important: we intentionally implement this as a *single* formatter partial
// specialization with SFINAE on the character type, rather than multiple
// constrained specializations of `fmt::formatter<T>`. Some toolchains (notably
// GCC 11 as used by the PyPI wheel build) reject multiple constrained partial
// specializations as a redefinition, even when the constraints are intended to
// be mutually exclusive.
template<typename T>
struct formatter<T, std::enable_if_t<IsStringViewFormattableQtType<T>, char>> : formatter<fmt::string_view>
{
  auto format(const T& v, format_context& ctx) const
  {
    if constexpr (nim::IsUtf8ArrayType<T>) {
      return formatter<fmt::string_view>::format(fmt::string_view(v.data(), v.size()), ctx);
    } else if constexpr (nim::CanConvertToUtf8QByteArray<T>) {
      auto u8 = v.toUtf8();
      return formatter<fmt::string_view>::format(fmt::string_view(u8.data(), u8.size()), ctx);
    } else if constexpr (nim::HaveToStringFunction<T>) {
      auto s = v.toString();
      return formatter<fmt::string_view>::format(fmt::string_view(s.data(), s.size()), ctx);
    } else if constexpr (nim::IsSupportedQtTypeForPrint<T>) {
      auto u8 = nim::qtTypeToQString(v).toUtf8();
      return formatter<fmt::string_view>::format(fmt::string_view(u8.data(), u8.size()), ctx);
    } else {
      // Should be unreachable because IsStringViewFormattableQtType<T> is in the specialization.
      return formatter<fmt::string_view>::format(fmt::string_view{}, ctx);
    }
  }
};

} // namespace fmt

// qt type iostream support
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

template<typename T>
  requires fmt::is_formattable<QList<T>>::value
std::ostream& operator<<(std::ostream& s, const QList<T>& v)
{
  return (s << fmt::format("{}", v));
}

static_assert(
  requires(std::ostream& s, const QList<QString>& v) { s << v; },
  "QList<QString> should be streamable from non-nim logging contexts");
