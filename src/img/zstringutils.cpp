#include "zstringutils.h"

#include <vector>

namespace {

inline bool isNumber(QChar c)
{
  return c >= QChar('0') && c <= QChar('9');
}

int lastInteger(const QString& str)
{
  int size = str.size();
  int endNumPos = -1;
  int startNumPos = -1;
  int index = size - 1;
  while (index >= 0) {
    if (isNumber(str[index])) {
      if (endNumPos == -1) {
        endNumPos = index;
        startNumPos = index;
      } else {
        startNumPos = index;
      }
    } else {
      if (endNumPos >= 0) {
        break;
      }
    }
    --index;
  }

  if (startNumPos == -1)
    return 0;

  int res = str.mid(startNumPos, endNumPos - startNumPos + 1).toInt();
  if (startNumPos > 0 && str[startNumPos - 1] == QChar('-'))
    res = -res;
  return res;
}

}

namespace nim {

bool naturalSortLessThan(const QString& s1, const QString& s2)
{
  if (s1 == "" || s2 == "") return s1 < s2;

  // Move to the first difference between the strings
  int startIndex = -1;
  int length = s1.length() > s2.length() ? s2.length() : s1.length();
  for (int i = 0; i < length; i++) {
    QChar c1 = s1[i];
    QChar c2 = s2[i];
    if (c1 != c2) {
      startIndex = i;
      break;
    }
  }

  // If the strings are the same, exit now.
  if (startIndex < 0) return s1 < s2;

  // Now extract the numbers, if any, from the two strings.
  QString sn1;
  QString sn2;
  bool done1 = false;
  bool done2 = false;
  length = s1.length() < s2.length() ? s2.length() : s1.length();

  for (int i = startIndex; i < length; i++) {
    if (!done1 && i < s1.length()) {
      if (isNumber(s1[i])) {
        sn1 += QString(s1[i]);
      } else {
        done1 = true;
      }
    }

    if (!done2 && i < s2.length()) {
      if (isNumber(s2[i])) {
        sn2 += QString(s2[i]);
      } else {
        done2 = true;
      }
    }

    if (done1 && done2) break;
  }

  // If none of the strings contain a number, use a regular comparison.
  if (sn1 == "" && sn2 == "") return s1 < s2;

  // If one of the strings doesn't contain a number at that position,
  // we put the string without number first so that, for example,
  // "example.bin" is before "example1.bin"
  if (sn1 == "" && sn2 != "") return true;
  if (sn1 != "" && sn2 == "") return false;

  return sn1.toInt() < sn2.toInt();
}

bool lastIntegerLessThan(const QString& s1, const QString& s2)
{
  return lastInteger(s1) < lastInteger(s2);
}

QString replaceLastInteger(const QString& str, const QString& replacement)
{
  int size = str.size();
  int endNumPos = -1;
  int startNumPos = -1;
  int index = size - 1;
  while (index >= 0) {
    if (isNumber(str[index])) {
      if (endNumPos == -1) {
        endNumPos = index;
        startNumPos = index;
      } else {
        startNumPos = index;
      }
    } else {
      if (endNumPos >= 0) {
        break;
      }
    }
    --index;
  }

  if (startNumPos == -1)
    return str;

  if (startNumPos > 0 && str[startNumPos - 1] == QChar('-'))
    startNumPos -= 1;

  QString res = str;
  res.replace(startNumPos, endNumPos - startNumPos + 1, replacement);
  return res;
}

void removeComment(std::string& line, const std::string& commentStart, bool checkSpecialNumber)
{
  if (commentStart == "#" && checkSpecialNumber) {
    std::vector<size_t> poses;

    std::string lineCopy = line;
    std::transform(lineCopy.begin(), lineCopy.end(), lineCopy.begin(), ::tolower);
    std::string str("1.#inf");
    size_t idx = std::string::npos;
    do {
      idx = lineCopy.find(str, idx + 1);
      if (idx != std::string::npos) {
        poses.push_back(idx + 2);
        line[idx + 2] = '&';
      }
    } while (idx != std::string::npos);

    str = std::string("1.#ind");
    idx = std::string::npos;
    do {
      idx = lineCopy.find(str, idx + 1);
      if (idx != std::string::npos) {
        poses.push_back(idx + 2);
        line[idx + 2] = '&';
      }
    } while (idx != std::string::npos);

    str = std::string("1.#qnan");
    idx = std::string::npos;
    do {
      idx = lineCopy.find(str, idx + 1);
      if (idx != std::string::npos) {
        poses.push_back(idx + 2);
        line[idx + 2] = '&';
      }
    } while (idx != std::string::npos);

    str = std::string("1.#snan");
    idx = std::string::npos;
    do {
      idx = lineCopy.find(str, idx + 1);
      if (idx != std::string::npos) {
        poses.push_back(idx + 2);
        line[idx + 2] = '&';
      }
    } while (idx != std::string::npos);

    idx = line.find(commentStart);
    if (idx != std::string::npos) {
      line = line.substr(0, idx);
    }

    for (size_t i = 0; i < poses.size(); ++i) {
      if (poses[i] < line.size())
        line[poses[i]] = '#';
    }
  } else {
    size_t idx = line.find(commentStart);
    if (idx != std::string::npos) {
      line = line.substr(0, idx);
    }
  }
}

void removeComment(QString& line, const QString& commentStart, bool checkSpecialNumber)
{
  if (commentStart == "#" && checkSpecialNumber) {
    std::vector<int> poses;

    QString str("1.#inf");
    int idx = -1;
    do {
      idx = line.indexOf(str, idx + 1, Qt::CaseInsensitive);
      if (idx >= 0) {
        poses.push_back(idx + 2);
        line[idx + 2] = QChar('&');
      }
    } while (idx >= 0);

    str = QString("1.#ind");
    idx = -1;
    do {
      idx = line.indexOf(str, idx + 1, Qt::CaseInsensitive);
      if (idx >= 0) {
        poses.push_back(idx + 2);
        line[idx + 2] = QChar('&');
      }
    } while (idx >= 0);

    str = QString("1.#qnan");
    idx = -1;
    do {
      idx = line.indexOf(str, idx + 1, Qt::CaseInsensitive);
      if (idx >= 0) {
        poses.push_back(idx + 2);
        line[idx + 2] = QChar('&');
      }
    } while (idx >= 0);

    str = QString("1.#snan");
    idx = -1;
    do {
      idx = line.indexOf(str, idx + 1, Qt::CaseInsensitive);
      if (idx >= 0) {
        poses.push_back(idx + 2);
        line[idx + 2] = QChar('&');
      }
    } while (idx >= 0);

    idx = line.indexOf(commentStart);
    if (idx >= 0) {
      line.truncate(idx);
    }

    for (size_t i = 0; i < poses.size(); ++i) {
      if (poses[i] < line.size())
        line[poses[i]] = QChar('#');
    }
  } else {
    int idx = line.indexOf(commentStart);
    if (idx >= 0) {
      line.truncate(idx);
    }
  }
}

} // namespace nim
