#pragma once

#include <QString>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

// all exceptions are defined here
// always catch ZException unless you can make sure of the possible exception type

namespace nim {

class ZException : public std::exception
{
public:
  explicit ZException(const char* what)
    : m_what(what)
  {}

  explicit ZException(std::string_view what)
    : m_what(what)
  {}

  explicit ZException(const QString& what)
    : m_what(what.toStdString())
  {}

  ZException(ZException&&) = default;

  ZException& operator=(ZException&&) = default;

  ZException(const ZException&) = default;

  ZException& operator=(const ZException&) = default;

  ~ZException() noexcept override;

  [[nodiscard]] const char* what() const noexcept override
  {
    return m_what.c_str();
  }

protected:
  std::string m_what;
};

// io exception, generated while reading or writing
class ZIOException : public ZException
{
public:
  explicit ZIOException(const char* what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }

  explicit ZIOException(std::string_view what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }

  explicit ZIOException(const QString& what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }
};

class ZCancellationException : public ZException
{
  using ZException::ZException;
};

} // namespace nim
