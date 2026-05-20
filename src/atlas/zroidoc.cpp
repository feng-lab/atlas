#include "zroidoc.h"

#include "zexception.h"
#include "zimg.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "ztheme.h"
#include "zroiwidget.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <set>

namespace nim {

ZROIDoc::ZROIDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

ZROIPack& ZROIDoc::currentROIPack()
{
  for (auto& [id, roiPack] : m_idToROIPacks) {
    if (!roiPack->isLocked()) {
      return *roiPack;
    }
  }
  auto roi = new ZROI(nullptr, this);
  auto id = addROI(roi, "");
  CHECK(!m_idToROIPacks.at(id)->isLocked());
  return *m_idToROIPacks.at(id);
}

void ZROIDoc::askToSave(const ZROI& roi, const QString& title)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZROI::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  if (title.isEmpty()) {
    dialog.setWindowTitle(tr("Save ROI As"));
  } else {
    dialog.setWindowTitle(title);
  }

  if (dialog.exec()) {
    try {
      roi.save(dialog.selectedFiles().at(0));

      ZSystemInfo::instance().addFileToRecentFileList(dialog.selectedFiles().at(0));
      setLastOpenedObjPath(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Save ROI Error:\n%1").arg(e.what()));
    }
  }
}

bool ZROIDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToROIPacks.at(id);
  if (ZROI::canWriteFile(pack->path())) {
    QString err;
    if (saveROI(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(),
                          QApplication::applicationName(),
                          tr("Error saving %1 to file %2: %3").arg(objName(id)).arg(pack->path()).arg(err));
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
    QMessageBox::critical(
      QApplication::activeWindow(),
      QApplication::applicationName(),
      tr("Error saving %1 as file %2: %3").arg(objName(id)).arg(dialog.selectedFiles().at(0)).arg(err));
  }
  return false;
}

bool ZROIDoc::canReadFile(const QString& fileName) const
{
  return ZROI::canReadFile(fileName);
}

size_t ZROIDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToROIPacks) {
    if (idPack.second->path() == fileName) {
      return idPack.first;
    }
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
    errorMsg = QString("Can not read animation from %1: %2").arg(fileName).arg(e.what());
    return 0;
  }
}

size_t ZROIDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }
    for (const auto& idPack : m_idToROIPacks) {
      if (isSameObj(jValue, jsonValue(idPack.first))) {
        return idPack.first;
      }
    }
    size_t id;
    QString fileName = asQString(jValue);

    auto roi = std::make_unique<ZROI>();
    roi->load(fileName);
    id = addROI(roi.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read ROI from %1: %2").arg(jsonToFormattedQString(jValue)).arg(e.what());
    return 0;
  }
}

std::vector<QAction*> ZROIDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadROIAction);
  return res;
}

QMenu* ZROIDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_importMaskImageAction);
  res->addAction(m_createMaskImageAction);
  return res;
}

void ZROIDoc::removeObj(size_t id)
{
  auto it = m_idToROIPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToROIPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZROIDoc::objName(size_t id) const
{
  return m_idToROIPacks.at(id)->name();
}

QString ZROIDoc::objPath(size_t id) const
{
  return m_idToROIPacks.at(id)->path();
}

bool ZROIDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToROIPacks.at(id)->hasUnsavedChange();
}

QString ZROIDoc::objInfo(size_t id) const
{
  return m_idToROIPacks.at(id)->info();
}

QString ZROIDoc::objTooltip(size_t id) const
{
  return m_idToROIPacks.at(id)->tooltip();
}

const QUndoStack* ZROIDoc::objUndoStack(size_t id) const
{
  return m_idToROIPacks.at(id)->undoStack();
}

json::value ZROIDoc::jsonValue(size_t id) const
{
  return json::value_from(m_idToROIPacks.at(id)->path());
}

bool ZROIDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
  CHECK(v1.is_string() && v2.is_string());
  if (v1 == v2) {
    return true;
  }
  QString f1 = asQString(v1);
  QString f2 = asQString(v2);
  if (!QFile::exists(f1) || !QFile::exists(f2)) {
    return false;
  }
  return QFileInfo(f1).canonicalFilePath() == QFileInfo(f2).canonicalFilePath();
}

size_t ZROIDoc::makeAlias(size_t /*id*/)
{
  return 0;
}

bool ZROIDoc::isAlias(size_t id) const
{
  CHECK(m_idToROIPacks.contains(id));

  return std::ranges::any_of(m_idToROIPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToROIPacks.at(id);
  });
}

QWidget* ZROIDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToROIPacks.contains(id));

  auto& pack = m_idToROIPacks.at(id);
  return new ZROIWidget(*pack, m_doc);
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
    // auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(),
                              QApplication::applicationName(),
                              "Can not read ROI.\n" + errorMsg);
      }
    }
  }
}

void ZROIDoc::setModified()
{
  if (ZROI* roi = qobject_cast<ZROI*>(sender())) {
    for (const auto& idPack : m_idToROIPacks) {
      if (&(idPack.second->roi()) == roi) {
        idPack.second->updateDerivedData();
        idPack.second->setHasUnsavedChange(true);
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

// void ZROIDoc::setModified(bool clean)
//{
//   if (ZROI* roi = qobject_cast<ZROI*>(sender())) {
//     for (const auto& idPack : m_idToROIPacks) {
//       if (&(idPack.second->roi()) == roi) {
//         idPack.second->updateDerivedData();
//         m_doc.updateObjInfo(idPack.first);
//         return;
//       }
//     }
//   }
// }

void ZROIDoc::importMaskImage()
{
  QStringList filters;
  std::vector<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);

  index_t fmtIdx = -1;
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

  if (fmtIdx >= 0 && !fn.isEmpty()) {
    try {
      auto roi = new ZROI();
      roi->importMaskImage(fn, formats[fmtIdx]);

      addROI(roi, QFileInfo(fn).baseName() + "_roi", true);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      ZSystemInfo::instance().setLastOpenedImagePath(fn);
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Can not import mask image:\n%1").arg(e.what()));
    }
  }
}

void ZROIDoc::createMaskImage()
{
  if (m_idToROIPacks.empty()) {
    QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), tr("No ROI"));
    return;
  }

  ZROI& roi = currentROIPack().roi();
  if (roi.isEmpty()) {
    QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), tr("Empty ROI"));
    return;
  }

  QStringList filters;
  std::vector<FileFormat> formats;
  std::vector<Compression> comps;
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

      auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
      ZImgWriteParameters paras;
      paras.compression = comps[fmtIdx];
      img.save(dialog.selectedFiles().at(0), formats[fmtIdx], paras);
      img.clear();
      m_doc.loadFile(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            tr("Error saving mask image %1: %2").arg(dialog.selectedFiles().at(0)).arg(e.what()));
    }
  }
}

size_t ZROIDoc::addROI(ZROI* roi, const QString& path, bool unsaved)
{
  size_t id = m_doc.getNewObjId();
  m_idToROIPacks[id] = std::make_shared<ZROIPack>(roi, path, id, *this);
  if (unsaved) {
    m_idToROIPacks[id]->setHasUnsavedChange(true);
  }
  m_doc.registerNewObj(m_idToROIPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(roi, &ZROI::roiChanged, this, qOverload<>(&ZROIDoc::setModified));
  connect(roi, &ZROI::roiDeleted, this, qOverload<>(&ZROIDoc::setModified));
  connect(roi, &ZROI::roiMoved, this, qOverload<>(&ZROIDoc::setModified));
  //  connect(roi, &ZROI::undoStackCleanChanged,
  //          this, qOverload<bool>(&ZROIDoc::setModified));
  return id;
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

bool ZROIDoc::saveROI(ZROIPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->save(fileName);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not write animation to %1: %2").arg(fileName).arg(e.what());
    return false;
  }
}

void ZROIDoc::packInfoUpdated(ZROIPack* pack)
{
  for (const auto& idPack : m_idToROIPacks) {
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
