#include "zjson.h"

#include "zexception.h"
#include "zlog.h"
#include <QRegularExpression>
#include <QFile>

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

QJsonObject loadJsonObject(const QString& file)
{
  QFile loadFile(file);

  if (!loadFile.open(QIODevice::ReadOnly)) {
    throw ZIOException("could not open: " + file);
  }

  QByteArray saveData = loadFile.readAll();

  QJsonDocument doc = QJsonDocument::fromJson(saveData);

  CHECK(doc.isObject());

  return doc.object();
}

void saveJsonObject(const QJsonObject& json, const QString& file)
{
  QFile saveFile(file);

  if (!saveFile.open(QIODevice::WriteOnly)) {
    throw ZIOException(QString("could not open save file: ") + file);
  }

  QJsonDocument saveDoc(json);
  saveFile.write(saveDoc.toJson(QJsonDocument::Indented));
}

QStringList readStringList(const QJsonObject& json, const QString& key)
{
  if (!json.contains(key)) {
    throw ZIOException(QString("Key %1 required").arg(key));
  }
  QJsonValue value = json[key];

  if (!value.isArray()) {
    throw ZIOException("not stringlist");
  }
  QStringList res;
  QJsonArray array = value.toArray();
  for (int i = 0; i < array.size(); ++i) {
    if (!array[i].isString()) {
      throw ZIOException("not string");
    }
    res.append(array[i].toString());
  }
  return res;
}

QString readString(const QJsonObject& json, const QString& key)
{
  if (!json.contains(key)) {
    throw ZIOException(QString("Key %1 required").arg(key));
  }
  QJsonValue value = json[key];

  if (!value.isString()) {
    throw ZIOException("not string");
  }
  return value.toString();
}

double readNumber(const QJsonObject& json, const QString& key)
{
  if (!json.contains(key)) {
    throw ZIOException(QString("Key %1 required").arg(key));
  }
  QJsonValue value = json[key];

  if (!value.isDouble()) {
    throw ZIOException("not number");
  }
  return value.toDouble();
}

std::vector<double> readNumberArray(const QJsonObject& json, const QString& key)
{
  if (!json.contains(key)) {
    throw ZIOException(QString("Key %1 required").arg(key));
  }
  QJsonValue value = json[key];

  if (!value.isArray()) {
    throw ZIOException("not number array");
  }
  std::vector<double> res;
  QJsonArray array = value.toArray();
  for (int i = 0; i < array.size(); ++i) {
    if (!array[i].isDouble()) {
      throw ZIOException("not number");
    }
    res.push_back(array[i].toDouble());
  }
  return res;
}

bool readBool(const QJsonObject& json, const QString& key)
{
  if (!json.contains(key)) {
    throw ZIOException(QString("Key %1 required").arg(key));
  }
  QJsonValue value = json[key];

  if (!value.isBool()) {
    throw ZIOException("not bool");
  }
  return value.toBool();
}

} // namespace nim
