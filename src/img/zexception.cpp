#include "zexception.h"

#include <QString>
#include <fmt/base.h>
#include <system_error>

namespace nim {

ZException::ZException(std::string what, Option option)
    : m_what(std::move(what))
{
  if (option == Option::CheckErrno && errno != 0) {
    fmt::format_to(std::back_inserter(m_what), "  <errno: {}>", std::make_error_code(std::errc(errno)).message());
    errno = 0;
  }
}

ZException::ZException(QStringView what, Option option)
{
  auto u8 = what.toUtf8();
  std::string(u8.data(), u8.size()).swap(m_what);
  if (option == Option::CheckErrno && errno != 0) {
    fmt::format_to(std::back_inserter(m_what), "  <errno: {}>", std::make_error_code(std::errc(errno)).message());
    errno = 0;
  }
}

} // namespace nim
