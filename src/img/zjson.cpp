#include "zjson.h"

#include "zexception.h"
#include "zlog.h"
#include "zioutils.h"
#include <QRegularExpression>
#include <QFile>
#include <boost/json/src.hpp>
#include <fmt/core.h>
#include <sstream>

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

QJsonObject loadQJsonObject(const QString& file)
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
  for (auto&& i : array) {
    if (!i.isString()) {
      throw ZIOException("not string");
    }
    res.append(i.toString());
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
  for (auto&& i : array) {
    if (!i.isDouble()) {
      throw ZIOException("not number");
    }
    res.push_back(i.toDouble());
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

void pretty_print(std::ostream& os, const json::value& jv, std::string* indent)
{
  using namespace boost;
  std::string indent_;
  if (!indent) {
    indent = &indent_;
  }
  switch (jv.kind()) {
    case json::kind::object: {
      os << "{\n";
      indent->append(4, ' ');
      auto const& obj = jv.get_object();
      if (!obj.empty()) {
        auto it = obj.begin();
        for (;;) {
          os << *indent << json::serialize(it->key()) << " : ";
          pretty_print(os, it->value(), indent);
          if (++it == obj.end()) {
            break;
          }
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "}";
      break;
    }

    case json::kind::array: {
      os << "[\n";
      indent->append(4, ' ');
      auto const& arr = jv.get_array();
      if (!arr.empty()) {
        auto it = arr.begin();
        for (;;) {
          os << *indent;
          pretty_print(os, *it, indent);
          if (++it == arr.end()) {
            break;
          }
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "]";
      break;
    }

    case json::kind::string: {
      os << json::serialize(jv.get_string());
      break;
    }

    case json::kind::uint64:
      os << jv.get_uint64();
      break;

    case json::kind::int64:
      os << jv.get_int64();
      break;

    case json::kind::double_:
      os << jv.get_double();
      break;

    case json::kind::bool_:
      if (jv.get_bool()) {
        os << "true";
      } else {
        os << "false";
      }
      break;

    case json::kind::null:
      os << "null";
      break;
  }

  if (indent->empty()) {
    os << "\n";
  }
}

QString formatJsonToQString(const json::value& jv)
{
  std::ostringstream oss;
  pretty_print(oss, jv);
  return QString::fromStdString(oss.str());
}

json::object loadJsonObject(const QString& file)
{
  QFile loadFile(file);

  if (!loadFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException("could not open: " + file);
  }

  QByteArray saveData = loadFile.readAll();

  try {
    json::parse_options opt;  // all extensions default to off
    opt.allow_comments = true;  // permit C and C++ style comments to appear in whitespace
    opt.allow_trailing_commas = true; // allow an additional trailing comma in object and array element lists

    auto jv = json::parse(json::string_view(saveData.constData(), saveData.size()),
                                 json::storage_ptr(),
                                 opt);
    return jv.as_object();
  }
  catch (const std::exception& e) {
    throw ZIOException(fmt::format("json parse error: {}", e.what()));
  }
}

void saveJsonObject(const json::object& jo, const QString& file)
{
  std::ofstream fs;
  openFileStream(fs, file, std::ios_base::out);
  try {
    pretty_print(fs, jo);
  }
  catch (const std::exception& e) {
    throw ZIOException(fmt::format("json error: {}", e.what()));
  }
}

} // namespace nim
