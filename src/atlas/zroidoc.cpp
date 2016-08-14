#include "zroidoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include "zexception.h"
#include <set>
#include "zlog.h"
#include "zimg.h"
#include "zimgdoc.h"
#include "zimgsigneddistancemap.h"

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
                            "Save ROI Error.\n" + e.what());
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
    } else {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Error saving %1 to file %2: %3").arg(objName(id)).arg(pack->path).arg(err));
      return false;
    }
  } else {
    return saveAs(id);
  }
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
    } else {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Error saving %1 as file %2: %3").arg(objName(id)).arg(dialog.selectedFiles().at(0))
                              .arg(err));
    }
  }
  return false;
}

bool ZROIDoc::canReadFile(const QString& fileName)
{
  return ZROI::canReadFile(fileName);
}

size_t ZROIDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (auto it = m_idToROIPacks.begin(); it != m_idToROIPacks.end(); ++it) {
    if (it->second->path == fileName)
      return it->first;
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
  for (auto it = m_idToROIPacks.begin(); it != m_idToROIPacks.end(); ++it) {
    if (isSameObj(jValue, jsonValue(it->first)))
      return it->first;
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

size_t ZROIDoc::makeAlias(size_t)
{
  return 0;
}

bool ZROIDoc::isAlias(size_t id) const
{
  CHECK(m_idToROIPacks.find(id) != m_idToROIPacks.end());

  auto& pack = m_idToROIPacks.at(id);
  for (auto it = m_idToROIPacks.begin(); it != m_idToROIPacks.end(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
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
  ZROI* roi = qobject_cast<ZROI*>(sender());
  if (roi) {
    for (auto it = m_idToROIPacks.begin(); it != m_idToROIPacks.end(); ++it) {
      if (it->second->roi.get() == roi) {
        it->second->updateDerivedData();
        it->second->hasUnsavedChange = true;
        m_doc.updateObjInfo(it->first);
        return;
      }
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
      ZImg* tmpImg = new ZImg();
      tmpImg->swap(img);
      m_doc.imgDoc().showImg(tmpImg, dialog.selectedFiles().at(0));

      m_doc.imgDoc().setLastOpenedObjPath(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Error saving mask image %1: %2").arg(dialog.selectedFiles().at(0)).arg(e.what()));
    }
  }
}

size_t ZROIDoc::addROI(ZROI* roi, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToROIPacks[id] = std::make_shared<ROIPack>(roi, path);
  m_doc.registerNewObj(id, this);
  m_doc.undoGroup()->addStack(roi->undoStack());

  emit objAdded(id, this);
  connect(roi, &ZROI::roiChanged, this, &ZROIDoc::setModified);
  connect(roi, &ZROI::roiDeleted, this, &ZROIDoc::setModified);
  connect(roi, &ZROI::roiMoved, this, &ZROIDoc::setModified);
  return id;
}

ZROIDoc::ROIPack::ROIPack(ZROI* roi_, const QString& path_)
  : roi(roi_), path(QFileInfo(path_).canonicalFilePath()), hasUnsavedChange(false)
{
  updateDerivedData();
  if (path.isEmpty()) {
    hasUnsavedChange = true;
    m_name = generateUniqueName();
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
  m_loadROIAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load ROI..."), this);
  m_loadROIAction->setStatusTip(tr("Load an existing ROI file"));
  connect(m_loadROIAction, &QAction::triggered, this, &ZROIDoc::loadROI);

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
  for (auto it = m_idToROIPacks.begin(); it != m_idToROIPacks.end(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim
