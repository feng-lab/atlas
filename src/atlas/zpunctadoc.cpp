#include "zpunctadoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include "zpunctadetectiondialog.h"
#include "zimgdoc.h"
#include "zanalysisworklistdialog.h"

namespace nim {

ZPunctaDoc::ZPunctaDoc(ZDoc &doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZPunctaDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToPunctaPacks.at(id);
  if (ZPuncta::canWriteFile(pack->path)) {
    QString err;
    if (savePuncta(pack.get(), pack->path, err)) {
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

bool ZPunctaDoc::saveAs(size_t id)
{
  QStringList filters;
  QStringList formats;
  ZPuncta::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Puncta %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToPunctaPacks.at(id);
    int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    if (savePuncta(pack.get(), dialog.selectedFiles().at(0), err, formats[fmtIdx])) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save As Error", err);
    }
  }
  return false;
}

bool ZPunctaDoc::canReadFile(const QString &fileName)
{
  return ZPuncta::canReadFile(fileName);
}

size_t ZPunctaDoc::loadFile(const QString &fileName, QString &errorMsg)
{
  for (auto it = m_idToPunctaPacks.begin(); it != m_idToPunctaPacks.end(); ++it) {
    if (it->second->path == fileName)
      return it->first;
  }
  try {
    ZPuncta puncta(fileName);
    size_t id = addPuncta(puncta, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZPunctaDoc::loadFile(const QJsonValue &jValue, QString &errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (auto it = m_idToPunctaPacks.begin(); it != m_idToPunctaPacks.end(); ++it) {
    if (isSameObj(jValue, jsonValue(it->first)))
      return it->first;
  }
  QString fileName = jValue.toString();
  try {
    ZPuncta puncta(fileName);
    size_t id = addPuncta(puncta, fileName);
    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction *> ZPunctaDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadPunctaAction);
  return res;
}

QMenu *ZPunctaDoc::processObjMenu() const
{
  QMenu* res = new QMenu(typeName());
  res->addAction(m_detectPunctaAction);
  res->addAction(m_generateAnalysisTextFilesAction);
  return res;
}

void ZPunctaDoc::removeObj(size_t id)
{
  auto it = m_idToPunctaPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToPunctaPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZPunctaDoc::objName(size_t id) const
{
  return m_idToPunctaPacks.at(id)->name();
}

QString ZPunctaDoc::objPath(size_t id) const
{
  return m_idToPunctaPacks.at(id)->path;
}

bool ZPunctaDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToPunctaPacks.at(id)->hasUnsavedChange;
}

QString ZPunctaDoc::objInfo(size_t id) const
{
  return m_idToPunctaPacks.at(id)->info();
}

QString ZPunctaDoc::objTooltip(size_t id) const
{
  return m_idToPunctaPacks.at(id)->tooltip();
}

QJsonValue ZPunctaDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToPunctaPacks.at(id)->path);
}

bool ZPunctaDoc::isSameObj(const QJsonValue &v1, const QJsonValue &v2) const
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

size_t ZPunctaDoc::makeAlias(size_t id)
{
  CHECK(m_idToPunctaPacks.find(id) != m_idToPunctaPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToPunctaPacks[aliasId] = m_idToPunctaPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZPunctaDoc::isAlias(size_t id) const
{
  CHECK(m_idToPunctaPacks.find(id) != m_idToPunctaPacks.end());

  auto& pack = m_idToPunctaPacks.at(id);
  for (auto it = m_idToPunctaPacks.cbegin(); it != m_idToPunctaPacks.cend(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

void ZPunctaDoc::loadPuncta()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZPuncta::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Puncta File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i=0; i<dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), tr("Can not read puncta"),
                              errorMsg);
      }
    }
  }
}

void ZPunctaDoc::detectPuncta()
{
  ZPunctaDetectionDialog dlg(QApplication::activeWindow());
  connect(&dlg, &ZPunctaDetectionDialog::srcImgReady, &m_doc.imgDoc(), &ZImgDoc::showImg);
  dlg.exec();
}

void ZPunctaDoc::generateAnalysisTextFiles()
{
  ZAnalysisWorklistDialog dia(QApplication::activeWindow());
  dia.exec();
}

size_t ZPunctaDoc::addPuncta(ZPuncta &puncta, const QString &path)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<PunctaPack>(puncta, path);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  return id;
}

ZPunctaDoc::PunctaPack::PunctaPack(ZPuncta &punctaIn, const QString &path)
  : path(QFileInfo(path).canonicalFilePath()), hasUnsavedChange(false)
{
  puncta.swap(punctaIn);
  updateDerivedData();
}

ZPunctaDoc::PunctaPack::~PunctaPack()
{
}

void ZPunctaDoc::PunctaPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString &ZPunctaDoc::PunctaPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 puncta").arg(puncta.size());
  }
  return m_info;
}

void ZPunctaDoc::createActions()
{
  m_loadPunctaAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load Puncta..."), this);
  m_loadPunctaAction->setStatusTip(tr("Load one or more existing puncta files"));
  connect(m_loadPunctaAction, &QAction::triggered, this, &ZPunctaDoc::loadPuncta);

  m_detectPunctaAction = new QAction(tr("&Detect Puncta..."), this);
  m_detectPunctaAction->setStatusTip(tr("Auto Detect Puncta"));
  connect(m_detectPunctaAction, &QAction::triggered, this, &ZPunctaDoc::detectPuncta);

  m_generateAnalysisTextFilesAction = new QAction(tr("&Generate Analysis Text Files..."), this);
  m_generateAnalysisTextFilesAction->setStatusTip(tr("Generate Analysis Text Files from input list"));
  connect(m_generateAnalysisTextFilesAction, &QAction::triggered, this, &ZPunctaDoc::generateAnalysisTextFiles);
}

bool ZPunctaDoc::savePuncta(PunctaPack *pack, const QString &fileName, QString &errorMsg, const QString& format)
{
  try {
    pack->puncta.save(fileName, format);
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

void ZPunctaDoc::packInfoUpdated(PunctaPack *pack)
{
  for (auto it = m_idToPunctaPacks.begin(); it != m_idToPunctaPacks.end(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim
