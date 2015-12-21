#include "zregionontology.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include "zexception.h"

namespace nim {

void readMouseBrainAtlasOntology(ZTree<RegionNode> &ontology)
{
  ontology.clear();
  QString ontologyFilename = ":/Resources/ontology/mouse_brain_atlas.json";
  QFile file(ontologyFilename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(QObject::tr("Can not open ontology file"));
  }

  QByteArray saveData = file.readAll();
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(QObject::tr("File format is incorrect"));
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("msg") || !loadObj["msg"].isArray() || !loadObj["msg"].toArray().first().isObject()) {
    throw ZIOException(QObject::tr("File is not %1 format").arg("ontology"));
  }
  QJsonObject rootObj = loadObj["msg"].toArray().first().toObject();
  RegionNode node;
  QJsonArray children;
  for (QJsonObject::const_iterator it = rootObj.constBegin(); it != rootObj.constEnd(); ++it) {
    if (it.key() == "id") {
      node.id = it.value().toInt(-1);
    } else if (it.key() == "parent_structure_id") {
      node.parentID = -1;
    } else if (it.key() == "acronym") {
      node.abbreviation = it.value().toString();
    } else if (it.key() == "name") {
      node.name = it.value().toString();
    } else if (it.key() == "color_hex_triplet") {
      QString colorStr = it.value().toString();
      assert(colorStr.size() == 6);
      bool ok;
      node.red = colorStr.mid(0,2).toInt(&ok, 16);
      assert(ok);
      node.green = colorStr.mid(2,2).toInt(&ok, 16);
      assert(ok);
      node.blue = colorStr.mid(4,2).toInt(&ok, 16);
      assert(ok);
    } else if (it.key() == "children") {
      assert(it.value().isArray());
      children = it.value().toArray();
    }
  }
  ZTree<RegionNode>::Iterator currIt = ontology.appendRoot(node);
  for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
    assert((*it).isObject());
    readOntology((*it).toObject(), currIt, QStringList(), ontology);
  }
}

void readMouseBrainAtlasOntology(const QStringList &regionAbbrevs, ZTree<RegionNode> &ontology)
{
  ontology.clear();
  QString ontologyFilename = ":/Resources/ontology/mouse_brain_atlas.json";
  QFile file(ontologyFilename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(QObject::tr("Can not open ontology file"));
  }

  QByteArray saveData = file.readAll();
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(QObject::tr("File format is incorrect"));
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("msg") || !loadObj["msg"].isArray() || !loadObj["msg"].toArray().first().isObject()) {
    throw ZIOException(QObject::tr("File is not %1 format").arg("ontology"));
  }
  QJsonObject rootObj = loadObj["msg"].toArray().first().toObject();
  RegionNode node;
  QJsonArray children;
  for (QJsonObject::const_iterator it = rootObj.constBegin(); it != rootObj.constEnd(); ++it) {
    if (it.key() == "id") {
      node.id = it.value().toInt(-1);
    } else if (it.key() == "parent_structure_id") {
      node.parentID = -1;
    } else if (it.key() == "acronym") {
      node.abbreviation = it.value().toString();
    } else if (it.key() == "name") {
      node.name = it.value().toString();
    } else if (it.key() == "color_hex_triplet") {
      QString colorStr = it.value().toString();
      assert(colorStr.size() == 6);
      bool ok;
      node.red = colorStr.mid(0,2).toInt(&ok, 16);
      assert(ok);
      node.green = colorStr.mid(2,2).toInt(&ok, 16);
      assert(ok);
      node.blue = colorStr.mid(4,2).toInt(&ok, 16);
      assert(ok);
    } else if (it.key() == "children") {
      assert(it.value().isArray());
      children = it.value().toArray();
    }
  }
  ZTree<RegionNode>::Iterator nullIt;
  for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
    assert((*it).isObject());
    readOntology((*it).toObject(), nullIt, regionAbbrevs, ontology);
  }
}

int64_t idOfRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology)
{
  for (auto it = ontology.begin(); it != ontology.end(); ++it) {
    if (it->abbreviation == abbreviation) {
      return it->id;
    }
  }
  throw ZException(QString("can not find region %1").arg(abbreviation));
  return 0;
}

std::vector<int64_t> allIDsWithinRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology)
{
  std::vector<int64_t> res;
  for (auto it = ontology.begin(); it != ontology.end(); ++it) {
    if (it->abbreviation == abbreviation) {
      for (auto ait = ontology.begin(it); ait != ontology.end(it); ++ait) {
        res.push_back(ait->id);
      }
    }
  }
  if (res.empty()) {
    throw ZException(QString("can not find region %1").arg(abbreviation));
  }
  return res;
}

void readOntology(const QJsonObject &obj, ZTree<RegionNode>::Iterator &parentIt,
                  const QStringList& regionAbbrevs, ZTree<RegionNode> &ontology)
{
  RegionNode node;
  QJsonArray children;
  for (QJsonObject::const_iterator it = obj.constBegin(); it != obj.constEnd(); ++it) {
    if (it.key() == "id") {
      node.id = it.value().toInt(-1);
    } else if (it.key() == "parent_structure_id") {
      node.parentID = it.value().toInt(-1);
    } else if (it.key() == "acronym") {
      node.abbreviation = it.value().toString();
    } else if (it.key() == "name") {
      node.name = it.value().toString();
    } else if (it.key() == "color_hex_triplet") {
      QString colorStr = it.value().toString();
      assert(colorStr.size() == 6);
      bool ok;
      node.red = colorStr.mid(0,2).toInt(&ok, 16);
      assert(ok);
      node.green = colorStr.mid(2,2).toInt(&ok, 16);
      assert(ok);
      node.blue = colorStr.mid(4,2).toInt(&ok, 16);
      assert(ok);
    } else if (it.key() == "children") {
      assert(it.value().isArray());
      children = it.value().toArray();
    }
  }
  if (!ontology.isNull(parentIt)) {
    ZTree<RegionNode>::Iterator currIt = ontology.appendChild(parentIt, node);
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt, regionAbbrevs, ontology);
    }
  } else {
    ZTree<RegionNode>::Iterator currIt;
    if (regionAbbrevs.contains(node.abbreviation, Qt::CaseInsensitive)) {
      currIt = ontology.appendRoot(node);
    }
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt, regionAbbrevs, ontology);
    }
  }
}

} // namespace nim
