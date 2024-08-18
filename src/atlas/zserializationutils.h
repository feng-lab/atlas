#pragma once

#include "zjson.h"
#include <QColor>

inline QColor tag_invoke(const json::value_to_tag<QColor>&, const json::value& jv)
{
  QColor res;
  const auto& ja = jv.as_array();
  if (ja.size() >= 4) {
    res.setRgb(json::value_to<uint8_t>(ja[0]),
               json::value_to<uint8_t>(ja[1]),
               json::value_to<uint8_t>(ja[2]),
               json::value_to<uint8_t>(ja[3]));
  } else if (ja.size() >= 3) {
    res.setRgb(json::value_to<uint8_t>(ja[0]), json::value_to<uint8_t>(ja[1]), json::value_to<uint8_t>(ja[2]));
  } else {
    throw nim::ZException("json array too short for QColor");
  }
  return res;
}

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const QColor& v)
{
  auto& ja = jv.emplace_array();
  ja.reserve(4);
  ja.push_back(v.red());
  ja.push_back(v.green());
  ja.push_back(v.blue());
  ja.push_back(v.alpha());
}

namespace nim {

// [for backward compatibility, should not be used in new code] serialization support
#if 0
[[deprecated]] inline QString toQString(const QColor& v)
{
  return "[" + QString::number(v.red()) + ", " + QString::number(v.green()) + ", " + QString::number(v.blue()) + ", " +
         QString::number(v.alpha()) + "]";
}
#endif

inline void toVal(const QString& str, QColor& v)
{
  static QRegularExpression rx(R"((\ |\,|\[|\]|\;))"); // RegEx for ' ' or ',' or '[' or ']' or ';'
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
