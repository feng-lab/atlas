#include "zpunctadoc.h"

#include "zpunctadetectiondialog.h"
#include "zimgdoc.h"
#include "zanalysisworklistdialog.h"
#include "ztheme.h"
#include "zpunctawidget.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>

namespace nim {

ZPunctaDoc::ZPunctaDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZPunctaDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToPunctaPacks.at(id);
  if (ZPuncta::canWriteFile(pack->path())) {
    QString err;
    if (savePuncta(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save Error.\n" + err);
    return false;
  }
  return saveAs(id);
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
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save As Error.\n" + err);
  }
  return false;
}

bool ZPunctaDoc::canReadFile(const QString& fileName)
{
  return ZPuncta::canReadFile(fileName);
}

size_t ZPunctaDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToPunctaPacks) {
    if (idPack.second->path() == fileName)
      return idPack.first;
  }
  try {
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZPunctaDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (const auto& idPack : m_idToPunctaPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first)))
      return idPack.first;
  }
  QString fileName = jValue.toString();
  try {
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction*> ZPunctaDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadPunctaAction);
  return res;
}

QMenu* ZPunctaDoc::processObjMenu() const
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
  return m_idToPunctaPacks.at(id)->path();
}

bool ZPunctaDoc::objHasUnsavedChange(size_t id) const
{
  return !(ZPuncta::canReadFile(m_idToPunctaPacks.at(id)->path()) && objUndoStack(id)->isClean());
}

QString ZPunctaDoc::objInfo(size_t id) const
{
  return m_idToPunctaPacks.at(id)->info();
}

QString ZPunctaDoc::objTooltip(size_t id) const
{
  return m_idToPunctaPacks.at(id)->tooltip();
}

const QUndoStack* ZPunctaDoc::objUndoStack(size_t id) const
{
  return m_idToPunctaPacks.at(id)->undoStack();
}

QJsonValue ZPunctaDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToPunctaPacks.at(id)->path());
}

bool ZPunctaDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
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
  for (const auto& idPack : m_idToPunctaPacks) {
    if (idPack.first != id && idPack.second == pack)
      return true;
  }
  return false;
}

QWidget* ZPunctaDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToPunctaPacks.find(id) != m_idToPunctaPacks.end());

  return new ZPunctaWidget(punctaPack(id), m_doc);
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
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                              "Can not read puncta.\n" + errorMsg);
      }
    }
  }
}

void ZPunctaDoc::detectPuncta()
{
  ZPunctaDetectionDialog dlg(QApplication::activeWindow());
  dlg.exec();
}

void ZPunctaDoc::generateAnalysisTextFiles()
{
  ZAnalysisWorklistDialog dia(QApplication::activeWindow());
  dia.exec();
}

size_t ZPunctaDoc::addPuncta(ZPuncta puncta, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<ZPunctaPack>(puncta, path);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  connect(m_idToPunctaPacks[id].get(), &ZPunctaPack::undoStackCleanChanged,
          this, &ZPunctaDoc::setModified);
  return id;
}

void ZPunctaDoc::setModified(bool clean)
{
  if (auto ra = qobject_cast<ZPunctaPack*>(sender())) {
    for (const auto& idPack : m_idToPunctaPacks) {
      if (idPack.second.get() == ra) {
        idPack.second->updateDerivedData();
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

void ZPunctaDoc::createActions()
{
  m_loadPunctaAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Puncta..."), this);
  m_loadPunctaAction->setStatusTip(tr("Load one or more existing puncta files"));
  connect(m_loadPunctaAction, &QAction::triggered, this, &ZPunctaDoc::loadPuncta);

  m_detectPunctaAction = new QAction(tr("&Detect Puncta..."), this);
  m_detectPunctaAction->setStatusTip(tr("Auto Detect Puncta"));
  connect(m_detectPunctaAction, &QAction::triggered, this, &ZPunctaDoc::detectPuncta);

  m_generateAnalysisTextFilesAction = new QAction(tr("&Generate Analysis Text Files..."), this);
  m_generateAnalysisTextFilesAction->setStatusTip(tr("Generate Analysis Text Files from input list"));
  connect(m_generateAnalysisTextFilesAction, &QAction::triggered, this, &ZPunctaDoc::generateAnalysisTextFiles);
}

bool ZPunctaDoc::savePuncta(ZPunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format)
{
  try {
    pack->save(fileName, format);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZPunctaDoc::packInfoUpdated(ZPunctaPack* pack)
{
  for (const auto& idPack : m_idToPunctaPacks) {
    if (idPack.second.get() == pack)
      m_doc.updateObjInfo(idPack.first);
  }
}

} // namespace nim
