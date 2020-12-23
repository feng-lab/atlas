#include "zregionannotationdoc.h"

#include "zimgdoc.h"
#include "zregionannotationwidget.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "ztheme.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <set>

namespace nim {

ZRegionAnnotationDoc::ZRegionAnnotationDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

ZRegionAnnotationPack& ZRegionAnnotationDoc::currentRegionAnnotationPack()
{
  for (auto& [id, raPack] : m_idToRegionAnnotationPacks) {
    if (!raPack->isLocked()) {
      return *raPack;
    }
  }
  auto ra = new ZRegionAnnotation(this);
  auto id = addRegionAnnotation(ra, "");
  CHECK(!m_idToRegionAnnotationPacks.at(id)->isLocked());
  return *m_idToRegionAnnotationPacks.at(id);
}

bool ZRegionAnnotationDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToRegionAnnotationPacks.at(id);
  if (ZRegionAnnotation::canWriteFile(pack->path())) {
    QString err;
    if (saveRegionAnnotation(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), "Save Error.\n" + err);
    return false;
  }
  return saveAs(id);
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
    }
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), "Save As Error.\n" + err);
  }
  return false;
}

bool ZRegionAnnotationDoc::canReadFile(const QString& fileName) const
{
  return ZRegionAnnotation::canReadFile(fileName);
}

size_t ZRegionAnnotationDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToRegionAnnotationPacks) {
    if (idPack.second->path() == fileName)
      return idPack.first;
  }
  try {
    auto regionAnnotation = new ZRegionAnnotation(fileName);
    size_t id = addRegionAnnotation(regionAnnotation, fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
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
  for (const auto& idPack : m_idToRegionAnnotationPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first)))
      return idPack.first;
  }
  QString fileName = jValue.toString();
  try {
    auto regionAnnotation = new ZRegionAnnotation(fileName);
    size_t id = addRegionAnnotation(regionAnnotation, fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

std::vector<QAction*> ZRegionAnnotationDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadRegionAnnotationAction);
  return res;
}

QMenu* ZRegionAnnotationDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_importLabelImageAction);
  //res->addAction(m_exportLabelImageAction);
  return res;
}

void ZRegionAnnotationDoc::removeObj(size_t id)
{
  auto it = m_idToRegionAnnotationPacks.find(id);
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
  return m_idToRegionAnnotationPacks.at(id)->path();
}

bool ZRegionAnnotationDoc::objHasUnsavedChange(size_t id) const
{
  return !(ZRegionAnnotation::canReadFile(m_idToRegionAnnotationPacks.at(id)->path()) && objUndoStack(id)->isClean());
}

QString ZRegionAnnotationDoc::objInfo(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->info();
}

QString ZRegionAnnotationDoc::objTooltip(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->tooltip();
}

const QUndoStack* ZRegionAnnotationDoc::objUndoStack(size_t id) const
{
  return m_idToRegionAnnotationPacks.at(id)->undoStack();
}

QJsonValue ZRegionAnnotationDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToRegionAnnotationPacks.at(id)->path());
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
  m_doc.registerNewObj(aliasId, *this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZRegionAnnotationDoc::isAlias(size_t id) const
{
  CHECK(m_idToRegionAnnotationPacks.find(id) != m_idToRegionAnnotationPacks.end());

  return std::any_of(m_idToRegionAnnotationPacks.begin(), m_idToRegionAnnotationPacks.end(),
                     [&, this](const auto& idPack) {
                       return idPack.first != id && idPack.second == m_idToRegionAnnotationPacks.at(id);
                     }
  );
}

QWidget* ZRegionAnnotationDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToRegionAnnotationPacks.find(id) != m_idToRegionAnnotationPacks.end());

  auto& pack = m_idToRegionAnnotationPacks.at(id);
  return new ZRegionAnnotationWidget(*pack, m_doc);
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
        QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                              "Can not read regionAnnotation.\n" + errorMsg);
      }
    }
  }
}

void ZRegionAnnotationDoc::importLabelImage()
{
  QStringList filters;
  std::vector<FileFormat> formats;
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
      auto regionAnnotation = new ZRegionAnnotation();
      regionAnnotation->importLabelImage(fn, formats[fmtIdx]);

      addRegionAnnotation(regionAnnotation, QFileInfo(fn).baseName() + "_anno");
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      ZSystemInfo::instance().setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                            QString("Can not import label image:\n%1").arg(e.what()));
    }
  }
}

void ZRegionAnnotationDoc::exportLabelImage()
{
  QStringList filters;
  std::vector<FileFormat> formats;
  std::vector<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  if (m_idToRegionAnnotationPacks.empty()) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                          tr("No RegionAnnotation to Export"));
    return;
  }
  if (m_idToRegionAnnotationPacks.size() > 1) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                          tr("Two many RegionAnnotations, don't know which one to export. "
                             "Right now this function only works when there is only one regionannotation object"));
    return;
  }
  auto id = m_idToRegionAnnotationPacks.begin()->first;
  if (objHasUnsavedChange(id)) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
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
    for (size_t i = 0; i < formats.size(); ++i) {
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
      ZImgWriteParameters paras;
      paras.compression = comps[fmtIdx];
      currentRegionAnnotationPack().regionAnnotation().exportLabelImage(fn, formats[fmtIdx],
                                                                        paras);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      ZSystemInfo::instance().setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(),
                            QString("Can not export label image:\n%1").arg(e.what()));
    }
  }
}

//void ZRegionAnnotationDoc::setModified()
//{
//  if (auto ra = qobject_cast<ZRegionAnnotation*>(sender())) {
//    for (const auto& idPack : m_idToRegionAnnotationPacks) {
//      if (idPack.second->regionAnnotation == ra) {
//        if (!idPack.second->hasUnsavedChange) {
//          idPack.second->updateDerivedData();
//          idPack.second->hasUnsavedChange = true;
//          m_doc.updateObjInfo(idPack.first);
//        }
//        return;
//      }
//    }
//  }
//}

void ZRegionAnnotationDoc::setModified(bool)
{
  if (auto ra = qobject_cast<ZRegionAnnotationPack*>(sender())) {
    for (const auto& idPack : m_idToRegionAnnotationPacks) {
      if (idPack.second.get() == ra) {
        idPack.second->updateDerivedData();
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

size_t ZRegionAnnotationDoc::addRegionAnnotation(ZRegionAnnotation* regionAnnotation, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToRegionAnnotationPacks[id] = std::make_shared<ZRegionAnnotationPack>(regionAnnotation, path, id, *this);
  m_doc.registerNewObj(m_idToRegionAnnotationPacks[id]);

  emit objAdded(id, this);
  connect(m_idToRegionAnnotationPacks[id].get(), &ZRegionAnnotationPack::undoStackCleanChanged,
          this, &ZRegionAnnotationDoc::setModified);
  return id;
}

void ZRegionAnnotationDoc::createActions()
{
  m_loadRegionAnnotationAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon),
                                             tr("&Load RegionAnnotation..."), this);
  m_loadRegionAnnotationAction->setStatusTip(tr("Load one or more existing regionAnnotation files"));
  connect(m_loadRegionAnnotationAction, &QAction::triggered, this, &ZRegionAnnotationDoc::loadRegionAnnotation);

  m_importLabelImageAction = new QAction(tr("&Import Label Image..."), this);
  m_importLabelImageAction->setStatusTip(tr("Import Annotation From Label Image"));
  connect(m_importLabelImageAction, &QAction::triggered, this, &ZRegionAnnotationDoc::importLabelImage);

  m_exportLabelImageAction = new QAction(tr("&Export Label Image..."), this);
  m_exportLabelImageAction->setStatusTip(tr("Export Annotation To Label Image"));
  connect(m_exportLabelImageAction, &QAction::triggered, this, &ZRegionAnnotationDoc::exportLabelImage);
}

bool ZRegionAnnotationDoc::saveRegionAnnotation(ZRegionAnnotationPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->save(fileName);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZRegionAnnotationDoc::packInfoUpdated(ZRegionAnnotationPack* pack)
{
  for (const auto& idPack : m_idToRegionAnnotationPacks) {
    if (idPack.second.get() == pack)
      m_doc.updateObjInfo(idPack.first);
  }
}

} // namespace nim

