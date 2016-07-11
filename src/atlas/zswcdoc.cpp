#include "zswcdoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <cassert>
#include "QsLog.h"
#include "zexception.h"
#include <set>

namespace nim {

ZSwcDoc::ZSwcDoc(ZDoc &doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZSwcDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToSwcPacks.at(id);
  if (ZSwc::canWriteFile(pack->path)) {
    QString err;
    if (saveSwc(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save Error",
                            tr("Error saving %1 to file %2: %3").arg(objName(id)).arg(pack->path).arg(err));
      return false;
    }
  } else {
    return saveAs(id);
  }
}

bool ZSwcDoc::saveAs(size_t id)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZSwc::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Swc %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToSwcPacks.at(id);
    if (saveSwc(pack.get(), dialog.selectedFiles().at(0), err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save As Error", err);
    }
  }
  return false;
}

bool ZSwcDoc::canReadFile(const QString &fileName)
{
  return ZSwc::canReadFile(fileName);
}

size_t ZSwcDoc::loadFile(const QString &fileName, QString &errorMsg)
{
  for (auto it = m_idToSwcPacks.begin(); it != m_idToSwcPacks.end(); ++it) {
    if (it->second->path == fileName)
      return it->first;
  }
  try {
    ZSwc tree(fileName);
    size_t id = addSwc(tree, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);

    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZSwcDoc::loadFile(const QJsonValue &jValue, QString &errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (auto it = m_idToSwcPacks.begin(); it != m_idToSwcPacks.end(); ++it) {
    if (isSameObj(jValue, jsonValue(it->first)))
      return it->first;
  }
  QString fileName = jValue.toString();
  try {
    ZSwc tree(fileName);
    size_t id = addSwc(tree, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction *> ZSwcDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadSwcAction);
  return res;
}

void ZSwcDoc::removeObj(size_t id)
{
  auto it = m_idToSwcPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToSwcPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZSwcDoc::objName(size_t id) const
{
  return m_idToSwcPacks.at(id)->name();
}

QString ZSwcDoc::objPath(size_t id) const
{
  return m_idToSwcPacks.at(id)->path;
}

bool ZSwcDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToSwcPacks.at(id)->hasUnsavedChange;
}

QString ZSwcDoc::objInfo(size_t id) const
{
  return m_idToSwcPacks.at(id)->info();
}

QString ZSwcDoc::objTooltip(size_t id) const
{
  return m_idToSwcPacks.at(id)->tooltip();
}

QJsonValue ZSwcDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToSwcPacks.at(id)->path);
}

bool ZSwcDoc::isSameObj(const QJsonValue &v1, const QJsonValue &v2) const
{
  assert(v1.isString() && v2.isString());
  if (v1 == v2)
    return true;
  QString f1 = v1.toString();
  QString f2 = v2.toString();
  if (!QFile::exists(f1) || !QFile::exists(f2))
    return false;
  return QFileInfo(f1).canonicalFilePath() == QFileInfo(f2).canonicalFilePath();
}

size_t ZSwcDoc::makeAlias(size_t id)
{
  assert(m_idToSwcPacks.find(id) != m_idToSwcPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToSwcPacks[aliasId] = m_idToSwcPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZSwcDoc::isAlias(size_t id) const
{
  auto& pack = m_idToSwcPacks.at(id);
  for (auto it = m_idToSwcPacks.begin(); it != m_idToSwcPacks.end(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

void ZSwcDoc::loadSwc()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZSwc::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Swc File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i=0; i<dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), tr("Can not read swc"),
                              errorMsg);
      }
    }
  }
}

size_t ZSwcDoc::addSwc(ZSwc &tree, const QString &path)
{
  size_t id = m_doc.getNewObjId();
  m_idToSwcPacks[id] = std::make_shared<SwcPack>(tree, path);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  return id;
}

ZSwcDoc::SwcPack::SwcPack(ZSwc &tree, const QString &path)
  : path(QFileInfo(path).canonicalFilePath()), hasUnsavedChange(false)
{
  swc.swap(tree);
  updateDerivedData();
}

ZSwcDoc::SwcPack::~SwcPack()
{
}

void ZSwcDoc::SwcPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString &ZSwcDoc::SwcPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("size %1").arg(swc.size());
  }
  return m_info;
}

void ZSwcDoc::createActions()
{
  m_loadSwcAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load Swc..."), this);
  m_loadSwcAction->setStatusTip(tr("Load an existing Swc file"));
  connect(m_loadSwcAction, &QAction::triggered, this, &ZSwcDoc::loadSwc);
}

bool ZSwcDoc::saveSwc(SwcPack *pack, const QString &fileName, QString &errorMsg)
{
  try {
    pack->swc.resortID();
    pack->swc.save(fileName);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return false;
  }
}

void ZSwcDoc::packInfoUpdated(SwcPack *pack)
{
  for (auto it = m_idToSwcPacks.begin(); it != m_idToSwcPacks.end(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim
