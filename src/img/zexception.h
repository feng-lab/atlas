#ifndef ZEXCEPTION_H
#define ZEXCEPTION_H

#include <string>
#include <QString>
#include <stdexcept>
#include <cstring>
#include <cerrno>

// all exceptions are defined here
// always catch ZException unless you can make sure of the possible exception type

namespace nim {

class ZException
{
public:
  explicit inline ZException(const char* what)
    : m_what(what)
  {}
  explicit inline ZException(const std::string& what)
  {
    m_what = QString::fromUtf8(what.c_str());
  }
  explicit inline ZException(const QString& what)
    : m_what(what)
  {}
  virtual ~ZException() throw() {}

  inline const QString& what() const throw() {return m_what;}
protected:
  QString m_what;
};

// io exception, generated while reading or writing
class ZIOException : public ZException
{
public:
  explicit inline ZIOException(const char* what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what % QString(" <errno: %1>").arg(std::strerror(errno));
      errno = 0;
    }
  }
  explicit inline ZIOException(const std::string& what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what % QString(" <errno: %1>").arg(std::strerror(errno));
      errno = 0;
    }
  }
  explicit inline ZIOException(const QString& what)
    : ZException(what)
  {
    if (errno != 0) {
      m_what = m_what % QString(" <errno: %1>").arg(std::strerror(errno));
      errno = 0;
    }
  }
};

// img operation exception
class ZImgException : public ZException
{
public:
  explicit inline ZImgException(const char* what)
    : ZException(what)
  {}
  explicit inline ZImgException(const std::string& what)
    : ZException(what)
  {}
  explicit inline ZImgException(const QString& what)
    : ZException(what)
  {}
};

class ZProcessAbortException : public ZException
{
public:
  explicit inline ZProcessAbortException(const char* what)
    : ZException(what)
  {}
  explicit inline ZProcessAbortException(const std::string& what)
    : ZException(what)
  {}
  explicit inline ZProcessAbortException(const QString& what)
    : ZException(what)
  {}
};

class ZGLException : public ZException
{
public:
  explicit inline ZGLException(const char* what)
    : ZException(what)
  {}
  explicit inline ZGLException(const std::string& what)
    : ZException(what)
  {}
  explicit inline ZGLException(const QString& what)
    : ZException(what)
  {}
};

}

#endif // ZEXCEPTION_H
