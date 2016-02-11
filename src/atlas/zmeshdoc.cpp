#include "zmeshdoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <set>
#include "zexception.h"

namespace nim {

ZMeshDoc::ZMeshDoc(ZDoc &doc)
  : ZObjDoc(doc)
{
  createActions();
}

void ZMeshDoc::askToSave(const ZMesh &msh, const QString &title)
{
  QStringList filters;
  QList<std::string> formats;
  ZMesh::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  if (title.isEmpty())
    dialog.setWindowTitle(tr("Save Mesh As"));
  else
    dialog.setWindowTitle(title);

  if (dialog.exec()) {
    int fmtIdx = filters.indexOf(dialog.selectedNameFilter());

    try {
      msh.save(dialog.selectedFiles().at(0), formats[fmtIdx]);

      ZSystemInfoInstance.addFileToRecentFileList(dialog.selectedFiles().at(0));
      setLastOpenedObjPath(dialog.selectedFiles().at(0));
    }
    catch (const ZException & e) {
      QMessageBox::critical(QApplication::activeWindow(), "Save Mesh Error", e.what());
    }
  }
}

bool ZMeshDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToMeshPacks.at(id);
  if (ZMesh::canWriteFile(pack->path)) {
    QString err;
    if (saveMesh(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save Error", err);
      return false;
    }
  } else {
    return saveAs(id);
  }
}

bool ZMeshDoc::saveAs(size_t id)
{
  QStringList filters;
  QList<std::string> formats;
  ZMesh::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Mesh %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToMeshPacks.at(id);
    int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    if (saveMesh(pack.get(), dialog.selectedFiles().at(0), err, formats[fmtIdx])) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save As Error", err);
    }
  }
  return false;
}

bool ZMeshDoc::canReadFile(const QString &fileName)
{
  return ZMesh::canReadFile(fileName);
}

size_t ZMeshDoc::loadFile(const QString &fileName, QString &errorMsg)
{
  for (auto it = m_idToMeshPacks.begin(); it != m_idToMeshPacks.end(); ++it) {
    if (it->second->path == fileName)
      return it->first;
  }
  try {
    ZMesh mesh(fileName);
    size_t id = addMesh(mesh, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZMeshDoc::loadFile(const QJsonValue &jValue, QString &errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (auto it = m_idToMeshPacks.begin(); it != m_idToMeshPacks.end(); ++it) {
    if (isSameObj(jValue, jsonValue(it->first)))
      return it->first;
  }
  QString fileName = jValue.toString();
  try {
    ZMesh mesh(fileName);
    size_t id = addMesh(mesh, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction *> ZMeshDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadMeshAction);
  return res;
}

void ZMeshDoc::removeObj(size_t id)
{
  auto it = m_idToMeshPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToMeshPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZMeshDoc::objName(size_t id) const
{
  return m_idToMeshPacks.at(id)->name();
}

QString ZMeshDoc::objPath(size_t id) const
{
  return m_idToMeshPacks.at(id)->path;
}

bool ZMeshDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToMeshPacks.at(id)->hasUnsavedChange;
}

QString ZMeshDoc::objInfo(size_t id) const
{
  return m_idToMeshPacks.at(id)->info();
}

QString ZMeshDoc::objDetailedInfo(size_t id) const
{
  return m_idToMeshPacks.at(id)->detailedInfo();
}

QString ZMeshDoc::objTooltip(size_t id) const
{
  return m_idToMeshPacks.at(id)->tooltip();
}

QJsonValue ZMeshDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToMeshPacks.at(id)->path);
}

bool ZMeshDoc::isSameObj(const QJsonValue &v1, const QJsonValue &v2) const
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

size_t ZMeshDoc::makeAlias(size_t id)
{
  assert(m_idToMeshPacks.find(id) != m_idToMeshPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToMeshPacks[aliasId] = m_idToMeshPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZMeshDoc::isAlias(size_t id) const
{
  assert(m_idToMeshPacks.find(id) != m_idToMeshPacks.end());

  auto& pack = m_idToMeshPacks.at(id);
  for (auto it = m_idToMeshPacks.cbegin(); it != m_idToMeshPacks.cend(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

void ZMeshDoc::loadMesh()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZMesh::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Mesh File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i=0; i<dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), tr("Can not read mesh"),
                              errorMsg);
      }
    }
  }
}

size_t ZMeshDoc::addMesh(ZMesh &mesh, const QString &path)
{
  size_t id = m_doc.getNewObjId();
  m_idToMeshPacks[id] = std::make_shared<MeshPack>(mesh, path);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  return id;
}

ZMeshDoc::MeshPack::MeshPack(ZMesh &imesh, const QString &path)
  : path(QFileInfo(path).canonicalFilePath()), hasUnsavedChange(false)
{
  mesh.swap(imesh);
  meshList.push_back(&mesh);
  updateDerivedData();
}

ZMeshDoc::MeshPack::~MeshPack()
{
  meshList.clear();
}

void ZMeshDoc::MeshPack::updateDerivedData()
{
  m_info.clear();
  m_detailedInfo.clear();
  m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString &ZMeshDoc::MeshPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 vertices, %2 triangles").arg(mesh.numVertices()).arg(mesh.numTriangles());
  }
  return m_info;
}

const QString &ZMeshDoc::MeshPack::detailedInfo() const
{
  if (m_detailedInfo.isEmpty()) {
    QStringList info;
    ZMeshProperties prop = mesh.properties();
    info << QString("Number of Vertices: %1").arg(prop.numVertices);
    info << QString("Number of Triangles: %1").arg(prop.numTriangles);
    info << QString("Surface Area: %1").arg(prop.surfaceArea);
    info << QString("Min Triangle Area: %1").arg(prop.minTriangleArea);
    info << QString("Max Triangle Area: %1").arg(prop.maxTriangleArea);
    info << QString("Volume: %1").arg(prop.volume);
    info << QString("Volume Projected: %1").arg(prop.volumeProjected);
    info << QString("Volume X: %1").arg(prop.volumeX);
    info << QString("Volume Y: %1").arg(prop.volumeY);
    info << QString("Volume Z: %1").arg(prop.volumeZ);
    info << QString("Kx: %1").arg(prop.kx);
    info << QString("Ky: %1").arg(prop.ky);
    info << QString("Kz: %1").arg(prop.kz);
    info << QString("Normalized Shape Index: %1").arg(prop.normalizedShapeIndex);
    m_detailedInfo = info.join("\n");
  }
  return m_detailedInfo;
}

void ZMeshDoc::createActions()
{
  m_loadMeshAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load Mesh..."), this);
  m_loadMeshAction->setStatusTip(tr("Load an existing mesh file"));
  connect(m_loadMeshAction, SIGNAL(triggered()), this, SLOT(loadMesh()));
}

bool ZMeshDoc::saveMesh(MeshPack *pack, const QString &fileName, QString &errorMsg, const std::string& format)
{
  try {
    pack->mesh.save(fileName, format);
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

void ZMeshDoc::packInfoUpdated(MeshPack *pack)
{
  for (auto it = m_idToMeshPacks.begin(); it != m_idToMeshPacks.end(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim
