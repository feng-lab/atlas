#include "zjson.h"

namespace nim {

void modifyJsonValue(QJsonObject& obj, const QString& path, const QJsonValue& newValue)
{
  const int indexOfDot = path.indexOf('.');
  const QString propertyName = path.left(indexOfDot);
  const QString subPath = indexOfDot > 0 ? path.mid(indexOfDot + 1) : QString();

  QJsonValue subValue = obj[propertyName];

  if (subPath.isEmpty()) {
    subValue = newValue;
  } else {
    if (subValue.isArray()) {
      QJsonArray subArray = subValue.toArray();
      modifyJsonValue(subArray, subPath, newValue);
      subValue = subArray;
    } else {
      QJsonObject lobj = subValue.toObject();
      modifyJsonValue(lobj, subPath, newValue);
      subValue = lobj;
    }
  }

  obj[propertyName] = subValue;
}

void modifyJsonValue(QJsonArray& array, const QString& path, const QJsonValue& newValue)
{
  const int indexOfSquareBracketOpen = path.indexOf('[');
  const int indexOfSquareBracketClose = path.indexOf(']');

  const int arrayIndex = path.mid(indexOfSquareBracketOpen + 1,
                                  indexOfSquareBracketClose - indexOfSquareBracketOpen - 1).toInt();

  const QString subPath =
    indexOfSquareBracketClose > 0 ? (path.mid(indexOfSquareBracketClose + 1)[0] == '.' ? path.mid(
      indexOfSquareBracketClose + 2) : path.mid(indexOfSquareBracketClose + 1)) : QString();

  QJsonValue subValue = array[arrayIndex];

  if (subPath.isEmpty()) {
    subValue = newValue;
  } else {
    if (subValue.isArray()) {
      QJsonArray subArray = subValue.toArray();
      modifyJsonValue(subArray, subPath, newValue);
      subValue = subArray;
    } else {
      QJsonObject lobj = subValue.toObject();
      modifyJsonValue(lobj, subPath, newValue);
      subValue = lobj;
    }
  }

  array[arrayIndex] = subValue;
}

void modifyJsonValue(QJsonDocument& doc, const QString& path, const QJsonValue& newValue)
{
  if (doc.isArray()) {
    auto array = doc.array();
    modifyJsonValue(array, path, newValue);
    doc = QJsonDocument(array);
  } else {
    QJsonObject obj = doc.object();
    modifyJsonValue(obj, path, newValue);
    doc = QJsonDocument(obj);
  }
}

void removeJsonValue(QJsonObject& obj, const QString& path)
{
  const int indexOfDot = path.indexOf('.');
  const QString propertyName = path.left(indexOfDot);
  const QString subPath = indexOfDot > 0 ? path.mid(indexOfDot + 1) : QString();

  if (subPath.isEmpty()) {
    obj.remove(propertyName);
  } else {
    QJsonValue subValue = obj[propertyName];
    QJsonObject lobj = subValue.toObject();
    removeJsonValue(lobj, subPath);
    subValue = lobj;
    obj[propertyName] = subValue;
  }
}

void removeJsonValue(QJsonDocument& doc, const QString& path)
{
  QJsonObject obj = doc.object();
  removeJsonValue(obj, path);
  doc = QJsonDocument(obj);
}

} // namespace nim
