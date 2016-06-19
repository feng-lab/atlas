#include "zioutils.h"
#include "zexception.h"
#include <vector>
#include <QFile>
#include <QTextStream>

namespace nim {

void openFileStream(std::ifstream &fs, const QString &filename, std::ios_base::openmode mode)
{
#ifdef _MSC_VER
  fs.open(filename.toStdWString().c_str(), mode);   // use msvc extension
#else
  fs.open(QFile::encodeName(filename).constData(), mode);
#endif
  if (!fs.is_open()) {
    throw ZIOException("Can not open file for reading.");
  }
}

void openFileStream(std::ofstream &fs, const QString &filename, std::ios_base::openmode mode)
{
#ifdef _MSC_VER
  fs.open(filename.toStdWString().c_str(), mode);   // use msvc extension
#else
  fs.open(QFile::encodeName(filename).constData(), mode);
#endif
  if (!fs.is_open()) {
    throw ZIOException("Can not open file for writing.");
  }
}

void readStream_impl(std::istream &fs, char *buf, size_t count)
{
#if defined(__APPLE__)
  if (count < size_t(1024)*1024*1024*2) {
    if (!fs.read(buf, count)) {
      throw ZIOException(QString("Expect %1 bytes, only read %2 bytes.").arg(count).arg(fs.gcount()));
    }
    return;
  }
  static size_t chunkSize = size_t(1024)*1024*1024;
  size_t bytesRemaining = count;
  while (bytesRemaining > 0) {
    size_t bytesToRead = std::min(bytesRemaining, chunkSize);
    if (!fs.read(buf, bytesToRead)) {
      throw ZIOException(QString("Expect %1 bytes, only read %2 bytes.").arg(bytesToRead).arg(fs.gcount()));
    }
    bytesRemaining -= bytesToRead;
    buf += bytesToRead;
  }
#else
  if (!fs.read(buf, count)) {
    throw ZIOException(QString("Expect %1 bytes, only read %2 bytes.").arg(count).arg(fs.gcount()));
  }
#endif
}

void writeStream_impl(std::ostream &fs, const char *buf, size_t count)
{
  if (!fs.write(buf, count)) {
    throw ZIOException("File write failed.");
  }
}

QList<QStringList> readCSV(const QString filename, QChar separator, QTextCodec *codec)
{
  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(QString("Can not open file %1 for reading.").arg(filename));
  }

  QList<QStringList> res;

  bool DoubleQuote = true;
  bool SingleQuote = true;
  bool BackslashEscape = true;
  bool TwoQuoteEscape = false;

  QStringList row;
  QString field;
  QChar quote;
  QChar ch, buffer(0);
  bool readCR = false;
  QTextStream stream(&file);
  if(codec) {
    stream.setCodec(codec);
  } else {
    stream.setAutoDetectUnicode(true);
  }
  while (!stream.atEnd()) {
    if (buffer != QChar(0)) {
      ch = buffer;
      buffer = QChar(0);
    } else {
      stream >> ch;
    }
    if (ch == '\n' && readCR)
      continue;
    else if (ch == '\r')
      readCR = true;
    else
      readCR = false;
    if (ch != separator && (ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph || ch.category() == QChar::Other_Control)) {
      row << field;
      field.clear();
      if (!row.isEmpty()) {
        res.append(row);
      }
      row.clear();
    } else if ((DoubleQuote && ch == '"') || (SingleQuote && ch == '\'')) {
      quote = ch;
      do {
        stream >> ch;
        if (ch == '\\' && BackslashEscape) {
          stream >> ch;
        } else if (ch == quote) {
          if (TwoQuoteEscape) {
            stream >> buffer;
            if (buffer == quote) {
              buffer = QChar(0);
              field.append(ch);
              continue;
            }
          }
          break;
        }
        field.append(ch);
      } while (!stream.atEnd());
    } else if (ch == separator) {
      row << field;
      field.clear();
    } else {
      field.append(ch);
    }

    if (stream.status() != QTextStream::Ok) {
      throw ZIOException(QString("Error while reading file %1.").arg(filename));
    }
  }
  if (!field.isEmpty())
    row << field;
  if (!row.isEmpty()) {
    res.append(row);
  }
  file.close();

  return res;
}

}
