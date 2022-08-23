#pragma once

#include <QColor>

namespace nim {

inline QString toQString(const QColor& v)
{
  return "[" + QString::number(v.red()) + ", " + QString::number(v.green()) + ", " + QString::number(v.blue()) + ", " +
         QString::number(v.alpha()) + "]";
}

inline void toVal(const QString& str, QColor& v)
{
  QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); // RegEx for ' ' or ',' or '[' or ']' or ';'
  QStringList numList = str.split(rx, Qt::SkipEmptyParts);
  for (qsizetype i = 0; i < std::min(qsizetype(4), numList.size()); ++i) {
    int c;
    toVal(numList[i], c);
    if (i == 0) {
      v.setRed(c);
    } else if (i == 1) {
      v.setGreen(c);
    } else if (i == 2) {
      v.setBlue(c);
    } else if (i == 3) {
      v.setAlpha(c);
    }
  }
}

} // namespace nim
