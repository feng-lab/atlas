#pragma once

#include <QString>
#include <fmt/base.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

// all exceptions are defined here

namespace nim {

using namespace std::literals::string_view_literals;

using namespace std::literals::string_literals;

class ZException : public std::exception
{
public:
  enum class Option
  {
    Default = 0,
    CheckErrno,
  };

  explicit ZException(std::string what, Option option = Option::Default)
    : m_what(std::move(what))
  {
    if (option == Option::CheckErrno && errno != 0) {
      fmt::format_to(std::back_inserter(m_what), "  <errno: {}>", std::strerror(errno));
      errno = 0;
    }
  }

  explicit ZException(const char* what = "", Option option = Option::Default)
    : ZException(std::string(what), option)
  {}

  explicit ZException(std::string_view what, Option option = Option::Default)
    : ZException(std::string(what), option)
  {}

  explicit ZException(QStringView what, Option option = Option::Default)
  {
    auto u8 = what.toUtf8();
    std::string(u8.data(), u8.size()).swap(m_what);
    if (option == Option::CheckErrno && errno != 0) {
      fmt::format_to(std::back_inserter(m_what), "  <errno: {}>", std::strerror(errno));
      errno = 0;
    }
  }

  ZException(ZException&&) = default;

  ZException& operator=(ZException&&) = default;

  ZException(const ZException&) = default;

  ZException& operator=(const ZException&) = default;

  ~ZException() noexcept override = default;

  [[nodiscard]] const char* what() const noexcept override
  {
    return m_what.c_str();
  }

protected:
  std::string m_what;
};

class ZCancellationException : public ZException
{
  using ZException::ZException;
};

} // namespace nim
