#pragma once

#include "zglobal.h"
#include "zexception.h"
#include "zlog.h"
#include <QString>
#include <QStringView>
// #include <boost/charconv.hpp>
#include <fast_float/fast_float.h>
#include <absl/strings/str_split.h>
#include <concepts>

namespace nim {

// Space: ( ) - U+0020
// Tab: (\t) - U+0009
// Newline: (\n) - U+000A
// Carriage return: (\r) - U+000D
// Vertical tab: (\v) - U+000B
// Form feed: (\f) - U+000C
static constexpr auto spaces_literal = " \t\n\r\v\f"sv;

static constexpr auto delimiter_literal = " ,\t:;[]\n\r\v\f"sv;

#if 0
template<std::integral Integral>
__forceinline void stringToValue(std::string_view sv, Integral& value, int base = 10)
{
  auto res = boost::charconv::from_chars(sv.data(), sv.data() + sv.size(), value, base);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}
#else
template<std::integral Integral>
__forceinline void stringToValue(std::string_view sv, Integral& value, int base = 10)
{
  auto res = fast_float::from_chars(sv.data(), sv.data() + sv.size(), value, base);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}

template<std::integral Integral, class iterator>
__forceinline void stringToValue(iterator first, iterator last, Integral& value, int base = 10)
{
  auto res = fast_float::from_chars(&(*first), &(*last), value, base);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(
      fmt::format("error: invalid_argument when converting {} to Integer", std::string_view(first, last)));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(
      fmt::format("error: result_out_of_range when converting {} to Integer", std::string_view(first, last)));
  }
}
#endif

template<std::integral Integral>
__forceinline bool stringToValueNoThrow(std::string_view sv, Integral& value, int base = 10)
{
  auto res = fast_float::from_chars(sv.data(), sv.data() + sv.size(), value, base);
  return res.ec == std::errc();
}

template<std::integral Integral, class iterator>
__forceinline bool stringToValueNoThrow(iterator first, iterator last, Integral& value, int base = 10)
{
  auto res = fast_float::from_chars(&(*first), &(*last), value, base);
  return res.ec == std::errc();
}

template<std::integral Integral>
__forceinline void stringToValue(QStringView sv, Integral& value, int base = 10)
{
  auto data = sv.utf16();
  auto res = fast_float::from_chars(data, data + sv.size(), value, base);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}

template<std::integral Integral>
__forceinline bool stringToValueNoThrow(QStringView sv, Integral& value, int base = 10)
{
  auto data = sv.utf16();
  auto res = fast_float::from_chars(data, data + sv.size(), value, base);
  return res.ec == std::errc();
}

#if 0
template<std::floating_point Real>
__forceinline void stringToValue(std::string_view sv,
                                 Real& value,
                                 boost::charconv::chars_format fmt = boost::charconv::chars_format::general)
{
  auto res = boost::charconv::from_chars_erange(sv.data(), sv.data() + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
}

template<std::floating_point Real>
__forceinline bool stringToValueNoThrow(std::string_view sv,
                                        Real& value,
                                        boost::charconv::chars_format fmt = boost::charconv::chars_format::general)
{
  auto res = boost::charconv::from_chars_erange(sv.data(), sv.data() + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
  return res.ec == std::errc();
}
#else
template<std::floating_point Real>
__forceinline void
stringToValue(std::string_view sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto res = fast_float::from_chars(sv.data(), sv.data() + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
}

template<std::floating_point Real, class iterator>
__forceinline void stringToValue(iterator first,
                                 iterator last,
                                 Real& value,
                                 fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto res = fast_float::from_chars(&(*first), &(*last), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(
      fmt::format("error: invalid_argument when converting {} to Real number", std::string_view(first, last)));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number",
    // std::string_view(first, last)));
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number",
                                std::string_view(first, last));
  }
}

template<std::floating_point Real>
__forceinline bool
stringToValueNoThrow(std::string_view sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto res = fast_float::from_chars(sv.data(), sv.data() + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
  return res.ec == std::errc();
}

template<std::floating_point Real, class iterator>
__forceinline bool stringToValueNoThrow(iterator first,
                                        iterator last,
                                        Real& value,
                                        fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto res = fast_float::from_chars(&(*first), &(*last), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number",
                                std::string_view(first, last));
  }
  return res.ec == std::errc();
}
#endif

template<std::floating_point Real>
__forceinline void
stringToValue(QStringView sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto data = sv.utf16();
  auto res = fast_float::from_chars(data, data + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
}

template<std::floating_point Real>
__forceinline bool
stringToValueNoThrow(QStringView sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto data = sv.utf16();
  auto res = fast_float::from_chars(data, data + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
  return res.ec == std::errc();
}

// checkSpecialNumber takes care of 1.#qnan, 1.#ind ... when "#" is start of comment
std::string_view
removeComment(std::string_view line, std::string_view commentStart = "#"sv, bool checkSpecialNumber = true);

QString randomString(index_t minLength, index_t maxLength);

inline QString randomString(index_t size)
{
  return randomString(size, size);
}

bool naturalSortLessThan(const QString& s1, const QString& s2);

bool lastIntegerLessThan(const QString& s1, const QString& s2);

QString replaceLastInteger(const QString& str, const QString& replacement = "");

void removeComment(QString& line, const QString& commentStart = "#", bool checkSpecialNumber = true);

class QStringNaturalCompare
{
public:
  bool operator()(const QString& s1, const QString& s2) const
  {
    return naturalSortLessThan(s1, s2);
  }
};

} // namespace nim
