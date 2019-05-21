#include "zroidoc.h"

#include "zexception.h"
#include "zimg.h"
#include "zimgdoc.h"
#include "zimgsigneddistancemap.h"
#include "zlog.h"
#include "ztheme.h"
#include "zroiwidget.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <set>

namespace nim {

ZROIDoc::ZROIDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

ZROI& ZROIDoc::currentROI()
{
  if (m_idToROIPacks.empty()) {
    ZROI* roi = new ZROI(nullptr, this);
    addROI(roi, "");
  }
  return *m_idToROIPacks.begin()->second->roi;
}

void ZROIDoc::askToSave(const ZROI& roi, const QString& title)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZROI::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  if (title.isEmpty())
    dialog.setWindowTitle(tr("Save Mesh As"));
  else
    dialog.setWindowTitle(title);

  if (dialog.exec()) {
    try {
      roi.save(dialog.selectedFiles().at(0));

      ZSystemInfo::instance().addFileToRecentFileList(dialog.selectedFiles().at(0));
      setLastOpenedObjPath(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            QString("Save ROI Error:\n%1").arg(e.what()));
    }
  }
}

bool ZROIDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToROIPacks.at(id);
  if (ZROI::canWriteFile(pack->path)) {
    QString err;
    if (saveROI(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                          tr("Error saving %1 to file %2: %3").arg(objName(id)).arg(pack->path).arg(err));
    return false;
  }
  return saveAs(id);
}

bool ZROIDoc::saveAs(size_t id)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZROI::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save ROI %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToROIPacks.at(id);
    if (saveROI(pack.get(), dialog.selectedFiles().at(0), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                          tr("Error saving %1 as file %2: %3").arg(objName(id)).arg(dialog.selectedFiles().at(0)).arg(
                            err));
  }
  return false;
}

bool ZROIDoc::canReadFile(const QString& fileName)
{
  return ZROI::canReadFile(fileName);
}

size_t ZROIDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToROIPacks) {
    if (idPack.second->path == fileName)
      return idPack.first;
  }
  size_t id;
  try {
    auto roi = std::make_unique<ZROI>();
    roi->load(fileName);
    id = addROI(roi.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read animation from %1: %2")
      .arg(fileName).arg(e.what());
    return 0;
  }
}

size_t ZROIDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (const auto& idPack : m_idToROIPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first)))
      return idPack.first;
  }
  size_t id;
  QString fileName = jValue.toString();
  try {
    auto roi = std::make_unique<ZROI>();
    roi->load(fileName);
    id = addROI(roi.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read ROI from %1: %2")
      .arg(fileName).arg(e.what());
    return 0;
  }
}

QList<QAction*> ZROIDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadROIAction);
  return res;
}

QMenu* ZROIDoc::processObjMenu() const
{
  QMenu* res = new QMenu(typeName());
  res->addAction(m_importMaskImageAction);
  res->addAction(m_createMaskImageAction);
  return res;
}

void ZROIDoc::removeObj(size_t id)
{
  auto it = m_idToROIPacks.find(id);
  m_doc.undoGroup()->removeStack(objUndoStack(id));
  emit objAboutToBeRemoved(it->first, this);
  m_idToROIPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZROIDoc::objName(size_t id) const
{
  return m_idToROIPacks.at(id)->name();
}

QString ZROIDoc::objPath(size_t id) const
{
  return m_idToROIPacks.at(id)->path;
}

bool ZROIDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToROIPacks.at(id)->hasUnsavedChange;
}

QString ZROIDoc::objInfo(size_t id) const
{
  return m_idToROIPacks.at(id)->info();
}

QString ZROIDoc::objTooltip(size_t id) const
{
  return m_idToROIPacks.at(id)->tooltip();
}

QUndoStack* ZROIDoc::objUndoStack(size_t id)
{
  return m_idToROIPacks.at(id)->roi->undoStack();
}

QJsonValue ZROIDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToROIPacks.at(id)->path);
}

bool ZROIDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
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

size_t ZROIDoc::makeAlias(size_t /*id*/)
{
  return 0;
}

bool ZROIDoc::isAlias(size_t id) const
{
  CHECK(m_idToROIPacks.find(id) != m_idToROIPacks.end());

  auto& pack = m_idToROIPacks.at(id);
  for (const auto& idPack : m_idToROIPacks) {
    if (idPack.first != id && idPack.second == pack)
      return true;
  }
  return false;
}

QWidget* ZROIDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToROIPacks.find(id) != m_idToROIPacks.end());

  auto& pack = m_idToROIPacks.at(id);
  return new ZROIWidget(*pack->roi, m_doc);
}

void ZROIDoc::loadROI()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZROI::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load ROI File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                              "Can not read ROI.\n" + errorMsg);
      }
    }
  }
}

void ZROIDoc::setModified()
{
  if (ZROI* roi = qobject_cast<ZROI*>(sender())) {
    for (const auto& idPack : m_idToROIPacks) {
      if (idPack.second->roi.get() == roi) {
        idPack.second->updateDerivedData();
        idPack.second->hasUnsavedChange = true;
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

void ZROIDoc::importMaskImage()
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
    dialog.setWindowTitle("Import Mask Image File");
    if (dialog.exec()) {
      fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      fn = dialog.selectedFiles().at(0);
    }
    dialog.close();
  }
  QApplication::processEvents();
  if (fmtIdx >= 0 && !fn.isEmpty()) {
    try {
      auto roi = new ZROI();
      roi->importMaskImage(fn, formats[fmtIdx]);

      addROI(roi, QFileInfo(fn).baseName() + "_roi", true);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      ZSystemInfo::instance().setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            QString("Can not import mask image:\n%1").arg(e.what()));
    }
  }
}

void ZROIDoc::createMaskImage()
{
  if (m_idToROIPacks.empty()) {
    QMessageBox::information(QApplication::activeWindow(), qApp->applicationName(),
                             tr("No ROI"));
    return;
  }

  ZROI& roi = currentROI();
  if (roi.isEmpty()) {
    QMessageBox::information(QApplication::activeWindow(), qApp->applicationName(),
                             tr("Empty ROI"));
    return;
  }

  QStringList filters;
  QList<FileFormat> formats;
  QList<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(m_doc.imgDoc().lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save ROI as Mask Image"));

  if (dialog.exec()) {
    try {
      ZImg img = roi.toMaskImg();

      int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      img.save(dialog.selectedFiles().at(0), formats[fmtIdx], comps[fmtIdx]);
      img.clear();
      m_doc.loadFile(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Error saving mask image %1: %2").arg(dialog.selectedFiles().at(0)).arg(e.what()));
    }
  }
}

size_t ZROIDoc::addROI(ZROI* roi, const QString& path, bool unsaved)
{
  size_t id = m_doc.getNewObjId();
  m_idToROIPacks[id] = std::make_shared<ROIPack>(roi, path);
  if (unsaved) {
    m_idToROIPacks[id]->hasUnsavedChange = true;
  }
  m_doc.registerNewObj(id, this);
  m_doc.undoGroup()->addStack(roi->undoStack());

  emit objAdded(id, this);
  connect(roi, &ZROI::roiChanged, this, &ZROIDoc::setModified);
  connect(roi, &ZROI::roiDeleted, this, &ZROIDoc::setModified);
  connect(roi, &ZROI::roiMoved, this, &ZROIDoc::setModified);
  return id;
}

ZROIDoc::ROIPack::ROIPack(ZROI* roi_, const QString& path_)
  : roi(roi_), path(QFileInfo(path_).canonicalFilePath())
{
  updateDerivedData();
  if (path.isEmpty()) {
    hasUnsavedChange = true;
    m_name = path_.endsWith("_roi") ? path_ : generateUniqueName();
  }
}

void ZROIDoc::ROIPack::updateDerivedData()
{
  m_info.clear();
  if (!path.isEmpty())
    m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& ZROIDoc::ROIPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 slices").arg(roi->numSlices());
  }
  return m_info;
}

QString ZROIDoc::ROIPack::generateUniqueName()
{
  static size_t num = 1;
  return QString("Unsaved ROI %1").arg(num++);
}

void ZROIDoc::createActions()
{
  m_loadROIAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load ROI..."), this);
  m_loadROIAction->setStatusTip(tr("Load an existing ROI file"));
  connect(m_loadROIAction, &QAction::triggered, this, &ZROIDoc::loadROI);

  m_importMaskImageAction = new QAction(tr("&Import Mask Image..."), this);
  m_importMaskImageAction->setStatusTip(tr("Import ROI From Mask Image"));
  connect(m_importMaskImageAction, &QAction::triggered, this, &ZROIDoc::importMaskImage);

  m_createMaskImageAction = new QAction(tr("&To Mask Image..."), this);
  m_createMaskImageAction->setStatusTip(tr("Save ROI as Mask Image"));
  connect(m_createMaskImageAction, &QAction::triggered, this, &ZROIDoc::createMaskImage);
}

bool ZROIDoc::saveROI(ROIPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->roi->save(fileName);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not write animation to %1: %2")
      .arg(fileName).arg(e.what());
    return false;
  }
}

void ZROIDoc::packInfoUpdated(ROIPack* pack)
{
  for (const auto& idPack : m_idToROIPacks) {
    if (idPack.second.get() == pack)
      m_doc.updateObjInfo(idPack.first);
  }
}

} // namespace nim
