#include "zioutils.h"

#include "zglobal.h"
#include "zlog.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDir>
#include <vector>

namespace nim {

void openIFStream(std::ifstream& fs, const QString& filename, std::ios_base::openmode mode)
{
#ifdef _MSC_VER
  fs.open(filename.toStdWString().c_str(), mode); // use msvc extension
#else
  fs.open(QFile::encodeName(filename).constData(), mode);
#endif
  if (!fs.is_open()) {
    throw ZException(fmt::format("Could not open file {} for reading", filename), ZException::Option::CheckErrno);
  }
}

std::ifstream openIFStream(const QString& filename, std::ios_base::openmode mode)
{
  std::ifstream res;
  openIFStream(res, filename, mode);
  return res;
}

void openOFStream(std::ofstream& fs, const QString& filename, std::ios_base::openmode mode)
{
#ifdef _MSC_VER
  fs.open(filename.toStdWString().c_str(), mode); // use msvc extension
#else
  fs.open(QFile::encodeName(filename).constData(), mode);
#endif
  if (!fs.is_open()) {
    throw ZException(fmt::format("Could not open file {} for writing", filename), ZException::Option::CheckErrno);
  }
}

std::ofstream openOFStream(const QString& filename, std::ios_base::openmode mode)
{
  std::ofstream res;
  openOFStream(res, filename, mode);
  return res;
}

void readStream_impl(std::istream& fs, char* buf, size_t count)
{
#if defined(__APPLE__)
  if (count < 1024_uz * 1024 * 1024 * 2) {
    if (!fs.read(buf, count)) {
      throw ZException(fmt::format("Expect {} bytes, only read {} bytes.", count, fs.gcount()),
                       ZException::Option::CheckErrno);
    }
    return;
  }
  static size_t chunkSize = 1024_uz * 1024 * 1024;
  size_t bytesRemaining = count;
  while (bytesRemaining > 0) {
    size_t bytesToRead = std::min(bytesRemaining, chunkSize);
    if (!fs.read(buf, bytesToRead)) {
      throw ZException(fmt::format("Expect {} bytes, only read {} bytes.", bytesToRead, fs.gcount()),
                       ZException::Option::CheckErrno);
    }
    bytesRemaining -= bytesToRead;
    buf += bytesToRead;
  }
#else
  if (!fs.read(buf, count)) {
    throw ZException(fmt::format("Expect {} bytes, only read {} bytes.", count, fs.gcount()),
                     ZException::Option::CheckErrno);
  }
#endif
}

void writeStream_impl(std::ostream& fs, const char* buf, size_t count)
{
  if (!fs.write(buf, count)) {
    throw ZException("File write failed", ZException::Option::CheckErrno);
  }
}

#ifdef _MSC_VER

std::unique_ptr<std::FILE, decltype(&std::fclose)> openFile(const QString& filename, const QString& mode)
{
  errno = 0;
  std::FILE* tmpf = nullptr;
  if (_wfopen_s(&tmpf, filename.toStdWString().c_str(), mode.toStdWString().c_str()) != 0) {
    throw ZException(fmt::format("Could not open file {}", filename), ZException::Option::CheckErrno);
  }
  return std::unique_ptr<std::FILE, decltype(&std::fclose)>(tmpf, std::fclose);
}

#else

std::unique_ptr<std::FILE, decltype(&std::fclose)> openFile(const QString& filename, const char* mode)
{
  errno = 0;
  std::FILE* tmpf = std::fopen(QFile::encodeName(filename).constData(), mode);
  if (!tmpf) {
    throw ZException(fmt::format("Could not open file {}", filename), ZException::Option::CheckErrno);
  }
  return std::unique_ptr<std::FILE, decltype(&std::fclose)>(tmpf, std::fclose);
}

#endif

QString getTemporaryFilename(const QString& filename)
{
  QFileInfo fi(filename);
  return fi.dir().filePath(QString(QStringLiteral("~$%1")).arg(fi.fileName()));
}

void renameFile(const QString& oldName, const QString& newName)
{
  if (!QFile::exists(oldName)) {
    throw ZException(fmt::format("File {} does not exist", oldName), ZException::Option::CheckErrno);
  }
  if (QFile::exists(newName)) {
    if (!QFile::remove(newName)) {
      throw ZException(fmt::format("Can not remove existing file {}", newName), ZException::Option::CheckErrno);
    }
  }
  if (!QFile::rename(oldName, newName)) {
    throw ZException(fmt::format("Can not rename file {}", oldName), ZException::Option::CheckErrno);
  }
}

std::string readFileIntoString(const QString& filename, std::ios_base::openmode mode)
{
  std::string res;
  auto fileSize = QFileInfo(filename).size();
  if (fileSize == 0) {
    return res;
  }

  std::ifstream fs;
#ifdef _MSC_VER
  fs.open(filename.toStdWString().c_str(), mode); // use msvc extension
#else
  fs.open(QFile::encodeName(filename).constData(), mode);
#endif
  if (!fs.is_open()) {
    throw ZException(fmt::format("Could not open file {}", filename), ZException::Option::CheckErrno);
  }
  res.resize(fileSize);
  fs.read(&res[0], res.size());
  return res;
}

QByteArray readFileIntoByteArray(const QString& filename, QIODevice::OpenMode openMode)
{
  QFile loadFile(filename);

  if (!loadFile.open(openMode)) {
    throw ZException(fmt::format("Could not open file {}", filename), ZException::Option::CheckErrno);
  }

  return loadFile.readAll();
}

} // namespace nim
