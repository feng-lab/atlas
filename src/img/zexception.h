#pragma once

#include <QString>
#include <fmt/core.h>
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
  explicit inline ZException(const char* what)
    : m_what(what)
  {}

  explicit inline ZException(std::string what)
    : m_what(std::move(what))
  {}

  explicit inline ZException(const QString& what)
    : m_what(what.toStdString())
  {}

  ZException(ZException&&) = default;

  ZException& operator=(ZException&&) = default;

  ZException(const ZException&) = default;

  ZException& operator=(const ZException&) = default;

  ~ZException() noexcept override;

  inline const char* what() const noexcept override
  { return m_what.c_str(); }

protected:
  std::string m_what;
};

// io exception, generated while reading or writing
class ZIOException : public ZException
{
public:
  explicit inline ZIOException(const char* what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }

  explicit inline ZIOException(std::string what)
    : ZException(std::move(what))
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }

  explicit inline ZIOException(const QString& what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what + " <errno: " + std::strerror(errno) + ">";
      errno = 0;
    }
  }
};

// img operation exception
class ZImgException : public ZException
{
public:
  using ZException::ZException;
};

class ZProcessAbortException : public ZException
{
public:
  using ZException::ZException;
};

class ZGLException : public ZException
{
public:
  using ZException::ZException;
};

} // namespace nim
