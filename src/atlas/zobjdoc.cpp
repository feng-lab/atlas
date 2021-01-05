#include "zobjdoc.h"

#include "zdoc.h"
#include "zlog.h"
#include "zfileutils.h"
#include "zchooseobjdialog.h"
#include "zsysteminfo.h"
#include <QMessageBox>
#include <QApplication>
#include <QFileInfo>
#include <set>

namespace nim {

ZObjDoc::ZObjDoc(ZDoc& doc)
  : QObject(&doc)
  , m_doc(doc)
{
}

void ZObjDoc::showObjInGraphicalShell(size_t id) const
{
  ZFileUtils::showInGraphicalShell(objPath(id));
}

size_t ZObjDoc::chooseOneObjWithWidget(const QString& title, QWidget* parent) const
{
  if (hasObj()) {
    ZChooseObjDialog dlg(*this, false, parent);
    if (!title.isEmpty()) {
      dlg.setWindowTitle(title);
    }
    if (dlg.exec() == QDialog::Accepted) {
      return dlg.selectedID();
    }
  }
  return 0;
}

QString ZObjDoc::objNameWithModifiedMarker(size_t id) const
{
  if (objHasUnsavedChange(id))
    return QString("%1*").arg(objName(id));

  return objName(id);
}

QString ZObjDoc::objNameWithModifiedMarkerAndID(size_t id) const
{
  if (objHasUnsavedChange(id))
    return QString("%1* (id: %2)").arg(objName(id)).arg(id);

  return QString("%1 (id: %2)").arg(objName(id)).arg(id);
}

std::map<size_t, size_t> ZObjDoc::read(const std::vector<std::pair<QString, json::value>>& docKeyValueList, QString& err)
{
  std::map<size_t, size_t> idmap;
  auto allObjs = objs();

  std::map<size_t, json::value> idToJsonValue;
  for (const auto& keyValue : docKeyValueList) {
    QString keyString = keyValue.first;
    CHECK(keyString.startsWith(typeName()));
    bool ok = false;
    size_t id = 0;
    if (keyString.length() > typeName().length() + 1) {
      keyString.remove(0, typeName().length() + 1);
      if (keyString.trimmed().isEmpty()) {
        LOG(WARNING) << "Invalid object key " << keyValue.first;
        continue;
      }
      id = keyString.toLongLong(&ok);
    }
    if (ok && id > 0) {
      idToJsonValue[id] = keyValue.second;
    } else {
      LOG(WARNING) << "Invalid object key " << keyValue.first;
    }
  }

  while (!idToJsonValue.empty()) {
    auto it = idToJsonValue.begin();
    const auto& jv = it->second;
    std::set<size_t> ids; // collect all ids that are pointing to jv
    ids.insert(it->first);
    it = idToJsonValue.erase(it);
    while (it != idToJsonValue.end()) {
      if (it->second == jv) {
        ids.insert(it->first);
        it = idToJsonValue.erase(it);
      } else {
        ++it;
      }
    }

    // check existing objects that are pointing to jv
    std::set<size_t> existingIds;
    for (auto obj : allObjs) {
      if (isSameObj(jv, jsonValue(obj))) {
        existingIds.insert(obj);
      }
    }

    if (existingIds.empty()) {
      QString errMsg;
      size_t id = loadFile(jv, errMsg);
      if (id == 0) {
        err += QString("%1\n").arg(errMsg);
        continue;
      }
      existingIds.insert(id);
    }

    if (ids.size() > existingIds.size()) {
      size_t firstId = *existingIds.begin();
      size_t num = ids.size() - existingIds.size();
      for (size_t i = 0; i < num; ++i)
        existingIds.insert(makeAlias(firstId));
    }
    auto it1 = ids.begin();
    auto it2 = existingIds.begin();
    while (it2 != existingIds.end()) {
      idmap[*it1] = *it2;
      ++it1;
      ++it2;
    }
  }

  QApplication::processEvents();
  return idmap;
}

void ZObjDoc::write(json::object& json) const
{
  for (auto id : objs()) {
    json[QString("%1 %2").arg(typeName()).arg(id).toStdString()] = jsonValue(id);
  }
}

QString ZObjDoc::lastOpenedObjPath() const
{
  return ZSystemInfo::instance().lastOpenedObjPath(typeName());
}

void ZObjDoc::setLastOpenedObjPath(const QString& path) const
{
  ZSystemInfo::instance().setLastOpenedObjPath(typeName(), path);
}

QString ZObjDoc::strippedName(const QString& fullFileName)
{
  return QFileInfo(fullFileName).fileName();
}

} // namespace nim
