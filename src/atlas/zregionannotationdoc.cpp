#include "zregionannotationdoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include "zimgdoc.h"
#include <set>
#include "zregionannotationwidget.h"
#include "zsysteminfo.h"
#include "zlog.h"

namespace nim {

ZRegionAnnotationDoc::ZRegionAnnotationDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZRegionAnnotationDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToRegionAnnotationPacks.at(id);
  if (ZRegionAnnotation::canWriteFile(pack->path)) {
    QString err;
    if (saveRegionAnnotation(pack.get(), pack->path, err)) {
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

bool ZRegionAnnotationDoc::saveAs(size_t id)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZRegionAnnotation::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Region Annotation %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToRegionAnnotationPacks.at(id);
    if (saveRegionAnnotation(pack.get(), dialog.selectedFiles().at(0), err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save As Error", err);
    }
  }
  return false;
}

bool ZRegionAnnotationDoc::canReadFile(const QString& fileName)
{
  return ZRegionAnnotation::canReadFile(fileName);
}

size_t ZRegionAnnotationDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
    if (it->second->path == fileName)
      return it->first;
  }
  try {
    ZRegionAnnotation* regionAnnotation = new ZRegionAnnotation(fileName);
    size_t id = addRegionAnnotation(regionAnnotation, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZRegionAnnotationDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
    if (isSameObj(jValue, jsonValue(it->first)))
      return it->first;
  }
  QString fileName = jValue.toString();
  try {
    ZRegionAnnotation* regionAnnotation = new ZRegionAnnotation(fileName);
    size_t id = addRegionAnnotation(regionAnnotation, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction*> ZRegionAnnotationDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadRegionAnnotationAction);
  return res;
}

QMenu* ZRegionAnnotationDoc::processObjMenu() const
{
  QMenu* res = new QMenu(typeName());
  res->addAction(m_importLabelImageAction);
  //res->addAction(m_exportLabelImageAction);
  return res;
}

void ZRegionAnnotationDoc::removeObj(size_t id)
{
  auto it = m_idToRegionAnnotationPacks.find(id);
  m_doc.undoGroup()->removeStack(objUndoStack(id));
  emit objAboutToBeRemoved(it->first, this);
  m_idToRegionAnnotationPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZRegionAnnotationDoc::objName(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->name();
}

QString ZRegionAnnotationDoc::objPath(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->path;
}

bool ZRegionAnnotationDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->hasUnsavedChange;
}

QString ZRegionAnnotationDoc::objInfo(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->info();
}

QString ZRegionAnnotationDoc::objTooltip(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->tooltip();
}

QUndoStack* ZRegionAnnotationDoc::objUndoStack(size_t id)
{
  return m_idToRegionAnnotationPacks.at(id)->regionAnnotation->undoStack();
}

QJsonValue ZRegionAnnotationDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToRegionAnnotationPacks.at(id)->path);
}

bool ZRegionAnnotationDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
{
  CHECK(v1.isString() && v2.isString());
  if (v1 == v2)
    return true;
  QString f1 = v1.toString();
  QString f2 = v2.toString();
  if (!QFile::exists(f1) || !QFile::exists(f2))
    return false;
  return QFileInfo(f1).canonicalFilePath() == QFileInfo(f2).canonicalFilePath();
}

size_t ZRegionAnnotationDoc::makeAlias(size_t id)
{
  CHECK(m_idToRegionAnnotationPacks.find(id) != m_idToRegionAnnotationPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToRegionAnnotationPacks[aliasId] = m_idToRegionAnnotationPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZRegionAnnotationDoc::isAlias(size_t id) const
{
  CHECK(m_idToRegionAnnotationPacks.find(id) != m_idToRegionAnnotationPacks.end());

  auto& pack = m_idToRegionAnnotationPacks.at(id);
  for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

QWidget* ZRegionAnnotationDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToRegionAnnotationPacks.find(id) != m_idToRegionAnnotationPacks.end());

  auto& pack = m_idToRegionAnnotationPacks.at(id);
  return new ZRegionAnnotationWidget(*pack->regionAnnotation, m_doc);
}

void ZRegionAnnotationDoc::loadRegionAnnotation()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZRegionAnnotation::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load RegionAnnotation File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), tr("Can not read regionAnnotation"),
                              errorMsg);
      }
    }
  }
}

void ZRegionAnnotationDoc::importLabelImage()
{
  QStringList filters;
  QList<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);

  int fmtIdx = -1;
  QString fn;
  {
    QFileDialog dialog(QApplication::activeWindow());
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(filters);
    dialog.setDirectory(lastOpenedObjPath());
    dialog.setWindowTitle("Import Label Image File");
    if (dialog.exec()) {
      fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      fn = dialog.selectedFiles().at(0);
    }
    dialog.close();
  }
  QApplication::processEvents();
  if (fmtIdx >= 0 && !fn.isEmpty()) {
    try {
      ZRegionAnnotation* regionAnnotation = new ZRegionAnnotation();
      regionAnnotation->importLabelImage(fn, formats[fmtIdx]);

      addRegionAnnotation(regionAnnotation, QFileInfo(fn).baseName() + "_anno", true);
      ZSystemInfoInstance.addFileToRecentFileList(fn);
      ZSystemInfoInstance.setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), tr("Can not import label image"),
                            e.what());
    }
  }
}

void ZRegionAnnotationDoc::exportLabelImage()
{
  QStringList filters;
  QList<FileFormat> formats;
  QList<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  if (m_idToRegionAnnotationPacks.size() == 0) {
    QMessageBox::critical(QApplication::activeWindow(), tr("No RegionAnnotation to Export"),
                          tr("No RegionAnnotation to Export"));
    return;
  } else if (m_idToRegionAnnotationPacks.size() > 1) {
    QMessageBox::critical(QApplication::activeWindow(), tr("Two many RegionAnnotations"),
                          tr(
                            "Two many RegionAnnotations, don't know which one to export. Right now this function only works when there is only one regionannotation object"));
    return;
  }
  int id = m_idToRegionAnnotationPacks.begin()->first;
  if (m_idToRegionAnnotationPacks.begin()->second->hasUnsavedChange) {
    QMessageBox::critical(QApplication::activeWindow(), tr("RegionAnnotation is not saved yet"),
                          tr("Please Save RegionAnnotation First"));
    return;
  }

  int fmtIdx = -1;
  QString fn;
  {
    QFileDialog dialog(QApplication::activeWindow());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    for (int i = 0; i < formats.size(); ++i) {
      if (formats[i] == FileFormat::MetaImage) {
        dialog.selectNameFilter(filters[i]);
      }
    }
    dialog.setDirectory(lastOpenedObjPath());
    dialog.setWindowTitle(tr("Export Region Annotation %1 As Label Image").arg(objName(id)));
    if (dialog.exec()) {
      fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      fn = dialog.selectedFiles().at(0);
    }
    dialog.close();
  }
  QApplication::processEvents();
  if (fmtIdx >= 0 && !fn.isEmpty()) {
    try {
      m_idToRegionAnnotationPacks.begin()->second->regionAnnotation->exportLabelImage(fn, formats[fmtIdx],
                                                                                      comps[fmtIdx]);
      ZSystemInfoInstance.addFileToRecentFileList(fn);
      ZSystemInfoInstance.setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), tr("Can not export label image"),
                            e.what());
    }
  }
}

void ZRegionAnnotationDoc::setModified()
{
  ZRegionAnnotation* ra = qobject_cast<ZRegionAnnotation*>(sender());
  if (ra) {
    for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
      if (it->second->regionAnnotation == ra && !it->second->hasUnsavedChange) {
        it->second->updateDerivedData();
        it->second->hasUnsavedChange = true;
        m_doc.updateObjInfo(it->first);
        return;
      }
    }
  }
}

void ZRegionAnnotationDoc::setModified(bool clean)
{
  ZRegionAnnotation* ra = qobject_cast<ZRegionAnnotation*>(sender());
  if (ra) {
    for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
      if (it->second->regionAnnotation == ra) {
        if (clean && it->second->path.endsWith(ZRegionAnnotation::fileExtension(), Qt::CaseInsensitive)) {
          it->second->updateDerivedData();
          it->second->hasUnsavedChange = false;
          m_doc.updateObjInfo(it->first);
        } else if (!it->second->hasUnsavedChange) {
          it->second->updateDerivedData();
          it->second->hasUnsavedChange = true;
          m_doc.updateObjInfo(it->first);
        }
        return;
      }
    }
  }
}

size_t ZRegionAnnotationDoc::addRegionAnnotation(ZRegionAnnotation* regionAnnotation, const QString& path, bool unsaved)
{
  size_t id = m_doc.getNewObjId();
  m_idToRegionAnnotationPacks[id] = std::make_shared<RegionAnnotationPack>(regionAnnotation, path, unsaved);
  m_doc.registerNewObj(id, this);
  m_doc.undoGroup()->addStack(regionAnnotation->undoStack());

  emit objAdded(id, this);
  connect(regionAnnotation, &ZRegionAnnotation::undoStackCleanChanged, this,
          qOverload<bool>(&ZRegionAnnotationDoc::setModified));
  return id;
}

ZRegionAnnotationDoc::RegionAnnotationPack::RegionAnnotationPack(ZRegionAnnotation* regionAnnotationIn,
                                                                 const QString& pathIn, bool unsaved)
  : path(QFileInfo(pathIn).canonicalFilePath()), hasUnsavedChange(unsaved)
{
  if (path.isEmpty() && pathIn.endsWith("_anno")) {
    path = pathIn;
  }
  regionAnnotation = regionAnnotationIn;
  updateDerivedData();
}

ZRegionAnnotationDoc::RegionAnnotationPack::~RegionAnnotationPack()
{
}

void ZRegionAnnotationDoc::RegionAnnotationPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& ZRegionAnnotationDoc::RegionAnnotationPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 regions").arg(regionAnnotation->numRegions());
  }
  return m_info;
}

void ZRegionAnnotationDoc::createActions()
{
  m_loadRegionAnnotationAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load RegionAnnotation..."), this);
  m_loadRegionAnnotationAction->setStatusTip(tr("Load one or more existing regionAnnotation files"));
  connect(m_loadRegionAnnotationAction, &QAction::triggered, this, &ZRegionAnnotationDoc::loadRegionAnnotation);

  m_importLabelImageAction = new QAction(tr("&Import Label Image..."), this);
  m_importLabelImageAction->setStatusTip(tr("Import Annotation From Label Image"));
  connect(m_importLabelImageAction, &QAction::triggered, this, &ZRegionAnnotationDoc::importLabelImage);

  m_exportLabelImageAction = new QAction(tr("&Export Label Image..."), this);
  m_exportLabelImageAction->setStatusTip(tr("Export Annotation To Label Image"));
  connect(m_exportLabelImageAction, &QAction::triggered, this, &ZRegionAnnotationDoc::exportLabelImage);
}

bool ZRegionAnnotationDoc::saveRegionAnnotation(RegionAnnotationPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->regionAnnotation->save(fileName);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->regionAnnotation->undoStack()->setClean();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZRegionAnnotationDoc::packInfoUpdated(RegionAnnotationPack* pack)
{
  for (auto it = m_idToRegionAnnotationPacks.begin(); it != m_idToRegionAnnotationPacks.end(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim

