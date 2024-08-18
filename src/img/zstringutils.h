#pragma once

#include "zglobal.h"
#include "zexception.h"
#include "zlog.h"
#include <QString>
#include <boost/charconv.hpp>
#include <absl/strings/str_split.h>
#include <concepts>

namespace nim {

template<std::integral Integral>
__forceinline void stringToValue(std::string_view sv, Integral& value, int base = 10)
{
  auto res = boost::charconv::from_chars(sv.data(), sv.data() + sv.size(), value, base);
  if (res.ec == std::errc::invalid_argument) {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Integer", sv));
  } else if (res.ec == std::errc::result_out_of_range) {
    throw ZException(fmt::format("error: result_out_of_range when converting {} to Integer", sv));
  }
}

template<std::floating_point Real>
__forceinline void stringToValue(std::string_view sv,
                                 Real& value,
                                 boost::charconv::chars_format fmt = boost::charconv::chars_format::general)
{
  auto res = boost::charconv::from_chars_erange(sv.data(), sv.data() + sv.size(), value, fmt);
  if (res.ec == std::errc::invalid_argument) {
    throw ZException(fmt::format("error: invalid_argument when converting {} to Real number", sv));
  } else if (res.ec == std::errc::result_out_of_range) {
    // throw ZException(fmt::format("error: result_out_of_range when converting {} to Real number", sv));
    LOG(WARNING) << fmt::format("warning: result_out_of_range when converting {} to Real number", sv);
  }
}

QString randomString(index_t minLength, index_t maxLength);

inline QString randomString(index_t size)
{
  return randomString(size, size);
}

bool naturalSortLessThan(const QString& s1, const QString& s2);

bool lastIntegerLessThan(const QString& s1, const QString& s2);

QString replaceLastInteger(const QString& str, const QString& replacement = "");

// checkSpecialNumber takes care of 1.#qnan, 1.#ind ... when "#" is start of comment
void removeComment(std::string& line, const std::string& commentStart = "#", bool checkSpecialNumber = true);

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
