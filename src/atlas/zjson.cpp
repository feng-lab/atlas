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
  }
  else {
    QJsonObject lobj = subValue.toObject();
    modifyJsonValue(lobj, subPath, newValue);
    subValue = lobj;
  }

  obj[propertyName] = subValue;
}

void modifyJsonValue(QJsonDocument& doc, const QString& path, const QJsonValue& newValue)
{
  QJsonObject obj = doc.object();
  modifyJsonValue(obj, path, newValue);
  doc = QJsonDocument(obj);
}

} // namespace nim
