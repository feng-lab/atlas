#pragma once

#include <exception>
#include <string>
#include <string_view>

// all exceptions are defined here

class QStringView;

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

  explicit ZException(std::string what, Option option = Option::Default);

  explicit ZException(const char* what = "", Option option = Option::Default)
    : ZException(std::string(what), option)
  {}

  explicit ZException(std::string_view what, Option option = Option::Default)
    : ZException(std::string(what), option)
  {}

  explicit ZException(QStringView what, Option option = Option::Default);

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

// Indicates a missing optional resource at an I/O boundary (e.g. HTTP 404 for a
// Neuroglancer mesh that doesn't exist for a given segment).
class ZNotFoundException : public ZException
{
  using ZException::ZException;
};

} // namespace nim
