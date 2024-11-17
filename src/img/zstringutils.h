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
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = boost::charconv::from_chars(first, last, value, base);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}

template<std::integral Integral>
__forceinline bool stringToValueNoThrow(std::string_view sv, Integral& value, int base = 10)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = boost::charconv::from_chars(first, last, value, base);
  return res.ptr == last && res.ec == std::errc();
}
#else
template<std::integral Integral>
__forceinline void stringToValue(std::string_view sv, Integral& value, int base = 10)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, base);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}

template<std::integral Integral>
__forceinline bool stringToValueNoThrow(std::string_view sv, Integral& value, int base = 10)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, base);
  return res.ptr == last && res.ec == std::errc();
}
#endif

template<std::integral Integral, class Iterator>
__forceinline void stringToValue(Iterator begin, Iterator end, Integral& value, int base = 10)
{
  auto first = &(*begin);
  auto last = &(*end);
  auto res = fast_float::from_chars(first, last, value, base);

  using CharType = typename std::iterator_traits<Iterator>::value_type;
  if constexpr (sizeof(CharType) == 1) {
    if (res.ptr != last) [[unlikely]] {
      throw ZException(fmt::format("error: garbage left when converting {} to Integer", std::string_view(begin, end)));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(
        fmt::format("error: invalid_argument when converting {} to Integer", std::string_view(begin, end)));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(
        fmt::format("error: result_out_of_range when converting {} to Integer", std::string_view(begin, end)));
    }
  } else if constexpr (sizeof(CharType) == 2) {
    // For wchar_t-like types, use QStringView
    if (res.ptr != last) [[unlikely]] {
      throw ZException(fmt::format("error: garbage left when converting {} to Integer", QStringView(first, last)));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", QStringView(first, last)));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(
        fmt::format("error: result_out_of_range when converting {} to Integer", QStringView(first, last)));
    }
  } else {
    if (res.ptr != last) [[unlikely]] {
      throw ZException(fmt::format("error: garbage left when converting string to Integer"));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(fmt::format("error: invalid_argument when converting string to Integer"));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(fmt::format("error: result_out_of_range when converting string to Integer"));
    }
  }
}

template<std::integral Integral, class Iterator>
__forceinline bool stringToValueNoThrow(Iterator begin, Iterator end, Integral& value, int base = 10)
{
  auto first = &(*begin);
  auto last = &(*end);
  auto res = fast_float::from_chars(first, last, value, base);
  return res.ptr == last && res.ec == std::errc();
}

template<std::integral Integral>
__forceinline void stringToValue(QStringView sv, Integral& value, int base = 10)
{
  auto first = sv.utf16();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, base);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Integer", sv));
  }
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
  auto first = sv.utf16();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, base);
  return res.ptr == last && res.ec == std::errc();
}

#if 0
template<std::floating_point Real>
__forceinline void stringToValue(std::string_view sv,
                                 Real& value,
                                 boost::charconv::chars_format fmt = boost::charconv::chars_format::general)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = boost::charconv::from_chars_erange(first, last, value, fmt);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
  }
}

template<std::floating_point Real>
__forceinline bool stringToValueNoThrow(std::string_view sv,
                                        Real& value,
                                        boost::charconv::chars_format fmt = boost::charconv::chars_format::general)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = boost::charconv::from_chars_erange(first, last, value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  return res.ptr == last && res.ec == std::errc();
}
#else
template<std::floating_point Real>
__forceinline void
stringToValue(std::string_view sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, fmt);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
  }
}

template<std::floating_point Real>
__forceinline bool
stringToValueNoThrow(std::string_view sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = sv.data();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  return res.ptr == last && res.ec == std::errc();
}
#endif

template<std::floating_point Real, class Iterator>
__forceinline void stringToValue(Iterator begin,
                                 Iterator end,
                                 Real& value,
                                 fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = &(*begin);
  auto last = &(*end);
  auto res = fast_float::from_chars(first, last, value, fmt);

  using CharType = typename std::iterator_traits<Iterator>::value_type;
  if constexpr (sizeof(CharType) == 1) {
    if (res.ptr != last) [[unlikely]] {
      throw ZException(
        fmt::format("error: garbage left when converting {} to Real number", std::string_view(begin, end)));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(
        fmt::format("error: invalid_argument when converting {} to Real number", std::string_view(begin, end)));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(
        fmt::format("error: result_out_of_range when converting {} to Real number", std::string_view(begin, end)));
    }
  } else if constexpr (sizeof(CharType) == 2) {
    // For wchar_t-like types, use QStringView
    if (res.ptr != last) [[unlikely]] {
      throw ZException(fmt::format("error: garbage left when converting {} to Real number", QStringView(first, last)));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(
        fmt::format("error: invalid_argument when converting {} to Real number", QStringView(first, last)));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(
        fmt::format("error: result_out_of_range when converting {} to Real number", QStringView(first, last)));
    }
  } else {
    if (res.ptr != last) [[unlikely]] {
      throw ZException(fmt::format("error: garbage left when converting string to Real number"));
    }
    if (res.ec == std::errc::invalid_argument) [[unlikely]] {
      throw ZException(fmt::format("error: invalid_argument when converting string to Real number"));
    }
    if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
      throw ZException(fmt::format("error: result_out_of_range when converting string to Real number"));
    }
  }
}

template<std::floating_point Real, class Iterator>
__forceinline bool stringToValueNoThrow(Iterator begin,
                                        Iterator end,
                                        Real& value,
                                        fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = &(*begin);
  auto last = &(*end);
  auto res = fast_float::from_chars(first, last, value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  return res.ptr == last && res.ec == std::errc();
}

template<std::floating_point Real>
__forceinline void
stringToValue(QStringView sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = sv.utf16();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, fmt);

  if (res.ptr != last) [[unlikely]] {
    throw ZException(fmt::format("error: garbage left when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  }
  if (res.ec == std::errc::result_out_of_range) [[unlikely]] {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
  }
}

template<std::floating_point Real>
__forceinline bool
stringToValueNoThrow(QStringView sv, Real& value, fast_float::chars_format fmt = fast_float::chars_format::general)
{
  auto first = sv.utf16();
  auto last = first + sv.size();
  auto res = fast_float::from_chars(first, last, value, fmt);
  if (res.ec == std::errc::invalid_argument) [[unlikely]] {
    value = std::numeric_limits<Real>::quiet_NaN();
  }
  return res.ptr == last && res.ec == std::errc();
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
