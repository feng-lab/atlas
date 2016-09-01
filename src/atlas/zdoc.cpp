#include "zdoc.h"

#include "zobjdoc.h"
#include "zobjmodel.h"
#include "zitemselectionmodel.h"
#include "zobjwidget.h"
#include "zlog.h"
#include "zsaveobjsdialog.h"
#include "zimgdoc.h"
#include "z2danimationdoc.h"
#include "z3danimationdoc.h"
#include "zroidoc.h"
#include "zmeshdoc.h"
#include <QUndoGroup>
#include <QAction>
#include <QMessageBox>
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QInputDialog>
#include <QDir>
#include <QMenu>
#include <QClipboard>

namespace nim {

ZDoc::ZDoc(QObject* parent)
  : QObject(parent)
  , m_nextObjId(100)
  , m_viewSettingId(0)
{
  m_objModel = new ZObjModel(this);
  m_objSelectionModel = new ZItemSelectionModel(m_objModel, this);
  m_undoGroup = new QUndoGroup(this);
  m_emptyUndoStack = new QUndoStack(m_undoGroup);
  createActions();

  m_imgDoc = new ZImgDoc(*this);
  registerObjDoc(m_imgDoc);

  m_animation2DDoc = new Z2DAnimationDoc(*this);
  registerObjDoc(m_animation2DDoc);

  m_animation3DDoc = new Z3DAnimationDoc(*this);
  registerObjDoc(m_animation3DDoc);

  m_roiDoc = new ZROIDoc(*this);
  registerObjDoc(m_roiDoc);

  m_meshDoc = new ZMeshDoc(*this);
  registerObjDoc(m_meshDoc);
}

bool ZDoc::hasObj() const
{
  return m_objModel->hasObj();
}

size_t ZDoc::numObjs() const
{
  return m_objModel->numObjs();
}

QList<size_t> ZDoc::objs() const
{
  return m_objModel->objs();
}

size_t ZDoc::numSelectedObjs() const
{
  return m_objSelectionModel->numSelectedObjs();
}

QList<size_t> ZDoc::selectedObjs() const
{
  return m_objSelectionModel->selectedObjs();
}

QList<ZObjDoc*> ZDoc::objDocs()
{
  QList<ZObjDoc*> res;
  for (int i = 0; i < m_docPacks.size(); ++i)
    res.push_back(m_docPacks[i].doc);
  return res;
}

ZObjDoc* ZDoc::idToDoc(size_t id) const
{
  return m_objModel->idToDoc(id);
}

bool ZDoc::isObjVisible(size_t id) const
{
  return m_objModel->isObjVisible(id);
}

bool ZDoc::isObjSelected(size_t id) const
{
  return m_objSelectionModel->isObjSelected(id);
}

void ZDoc::setObjVisible(size_t id, bool v)
{
  m_objModel->setObjVisible(id, v);
}

void ZDoc::setObjSelected(size_t id, bool v)
{
  m_objSelectionModel->setObjSelected(id, v);
}

void ZDoc::deselectObj(size_t id)
{
  m_objSelectionModel->deselectObj(id);
}

void ZDoc::clearAndSelectObj(size_t id)
{
  m_objSelectionModel->clearAndSelectObj(id);
}

void ZDoc::appendSelectObj(size_t id)
{
  m_objSelectionModel->appendSelectObj(id);
}

size_t ZDoc::indexToId(const QModelIndex& index)
{
  return m_objModel->indexToId(index);
}

QString ZDoc::objName(size_t id) const
{
  if (id >= 100) {
    return idToDoc(id)->objName(id);
  }
  if (id == 1) {
    return QString("Background");
  }
  if (id == 2) {
    return QString("Axis");
  }
  if (id == 3) {
    return QString("Lighting");
  }
  CHECK(false);
  return "";
}

QString ZDoc::objNameWithModifiedMarker(size_t id) const
{
  if (id >= 100) {
    return idToDoc(id)->objNameWithModifiedMarker(id);
  }
  if (id == 1) {
    return QString("Background");
  }
  if (id == 2) {
    return QString("Axis");
  }
  if (id == 3) {
    return QString("Lighting");
  }
  CHECK(false);
  return "";
}

QString ZDoc::objNameWithModifiedMarkerAndID(size_t id) const
{
  if (id >= 100) {
    return idToDoc(id)->objNameWithModifiedMarkerAndID(id);
  }
  if (id == 1) {
    return QString("Background");
  }
  if (id == 2) {
    return QString("Axis");
  }
  if (id == 3) {
    return QString("Lighting");
  }
  CHECK(false);
  return "";
}

QString ZDoc::objDetailedInfo(size_t id) const
{
  if (id >= 100) {
    return idToDoc(id)->objDetailedInfo(id);
  }
  CHECK(false);
  return "";
}

QList<size_t> ZDoc::objsOfDoc(const ZObjDoc* objD) const
{
  return m_objModel->objsOfDoc(objD);
}

QList<size_t> ZDoc::selectedObjsOfDoc(const ZObjDoc* objD) const
{
  return m_objSelectionModel->selectedObjsOfDoc(objD);
}

void ZDoc::activateEmptyUndoStack()
{
  m_undoGroup->setActiveStack(m_emptyUndoStack);
}

QList<QAction*> ZDoc::fileActions() const
{
  QList<QAction*> res;
  for (int i = 0; i < m_docPacks.size(); ++i) {
    res.append(m_docPacks[i].doc->loadFileActions());
    res.append(m_docPacks[i].removeAllAction);
  }
  res.push_back(m_removeAllAction);
  return res;
}

QList<QAction*> ZDoc::loadFileActions() const
{
  QList<QAction*> res;
  for (int i = 0; i < m_docPacks.size(); ++i) {
    res.append(m_docPacks[i].doc->loadFileActions());
  }
  return res;
}

QList<QMenu*> ZDoc::processObjMenu() const
{
  QList<QMenu*> res;
  for (int i = 0; i < m_docPacks.size(); ++i) {
    QMenu* menu = m_docPacks[i].doc->processObjMenu();
    if (menu)
      res.append(menu);
  }
  return res;
}

ZObjWidget* ZDoc::createObjWidget(QWidget* parent)
{
  ZObjWidget* wdt = new ZObjWidget(this, m_objModel, m_objSelectionModel, parent);
  return wdt;
}

QWidget* ZDoc::createObjEditWidget(size_t id)
{
  if (id == 0) {
    return nullptr;
  }
  return idToDoc(id)->createObjEditWidget(id);
}

void ZDoc::updateObjInfo(size_t id)
{
  m_objModel->updateObj(id);
  emit objInfoChanged(id);
}

void ZDoc::registerObjDoc(ZObjDoc* objD)
{
  DocPack docPack;
  docPack.doc = objD;
  QAction* docRemoveAllAction = new QAction(tr("&Remove All %1").arg(objD->typePluralName()), this);
  docRemoveAllAction->setStatusTip(tr("&Remove All %1").arg(objD->typePluralName()));
  connect(docRemoveAllAction, &QAction::triggered, this, &ZDoc::removeAllObjs);
  docPack.removeAllAction = docRemoveAllAction;
  m_docPacks.push_back(docPack);
  connect(objD, &ZObjDoc::objAboutToBeRemoved, this, &ZDoc::objAboutToBeRemoved);
  connect(objD, &ZObjDoc::objAdded, this, &ZDoc::objAdded);
  connect(objD, &ZObjDoc::objRemoved, this, &ZDoc::objRemoved);
}

void ZDoc::registerNewObj(size_t id, ZObjDoc* objD)
{
  m_objModel->addObj(id, objD);
}

void ZDoc::removeObj(size_t id)
{
  ZObjDoc* doc = m_objModel->idToDoc(id);
  if (saveOrDiscard(id)) {
    m_objModel->removeObj(id);
    doc->removeObj(id);
  }
}

void ZDoc::removeAllObjsOfDoc(ZObjDoc* doc)
{
  QList<size_t> objs = objsOfDoc(doc);
  if (saveOrDiscard(objs)) {
    for (int i = 0; i < objs.size(); ++i) {
      m_objModel->removeObj(objs[i]);
      doc->removeObj(objs[i]);
    }
  }
}

bool ZDoc::saveOrDiscard(size_t id)
{
  ZObjDoc* doc = m_objModel->idToDoc(id);
  if (!doc->objHasUnsavedChange(id)) {
    return true;
  }

  QMessageBox msgBox(QApplication::activeWindow());
  msgBox.setText(tr("Do you want to save the changes you made to %1?").arg(doc->objName(id)));
  msgBox.setInformativeText("Your changes will be lost if you don't save them.");
  msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  msgBox.setDefaultButton(QMessageBox::Save);
  int ret = msgBox.exec();

  switch (ret) {
    case QMessageBox::Save:
      // Save was clicked
      if (doc->save(id)) {
        return true;
      }
      break;
    case QMessageBox::Discard:
      // Don't Save was clicked
      return true;
      break;
    case QMessageBox::Cancel:
      // Cancel was clicked
      break;
    default:
      // should never be reached
      break;
  }
  return false;
}

bool ZDoc::saveOrDiscard(const QList<size_t>& objs)
{
  QList<size_t> unsavedObjs;
  for (int i = 0; i < objs.size(); ++i) {
    if (m_objModel->idToDoc(objs[i])->objHasUnsavedChange(objs[i])) {
      unsavedObjs.push_back(objs[i]);
    }
  }
  if (unsavedObjs.empty())
    return true;

  ZSaveObjsDialog dlg(*this, unsavedObjs, QApplication::activeWindow());
  int ret = dlg.exec();
  switch (ret) {
    case QDialog::Accepted: {
      //LOG(INFO) << "Save or Discard was clicked";
      bool saveSuccess = true;
      const QList<size_t>& sobjs = dlg.objsToSave();
      for (int i = 0; i < sobjs.size(); ++i) {
        //LOG(INFO) << "save " << m_objModel->idToDoc(sobjs[i])->objName(sobjs[i]);
        saveSuccess = saveSuccess && m_objModel->idToDoc(sobjs[i])->save(sobjs[i]);
      }
      return saveSuccess;
      break;
    }
    case QDialog::Rejected:
      //LOG(INFO) << "Cancel was clicked";
      break;
    default:
      // should never be reached
      break;
  }
  return false;
}

void ZDoc::loadFile(const QString& fileName)
{
  QString error;
  if (!loadFile(fileName, error)) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                          tr("Can not read file %1. Error: %2")
                            .arg(fileName).arg(error));
  }
}

void ZDoc::loadFileList(const QStringList& fileList)
{
  QString error;
  for (int i = 0; i < fileList.size(); ++i) {
    QString tmpErr;
    if (!loadFile(fileList[i], tmpErr)) {
      if (!error.isEmpty()) {
        error += "\n\n";
      }
      error += QString("Can not read file %1. Error: %2").arg(fileList[i]).arg(tmpErr);
    }
  }
  if (!error.isEmpty()) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), error);
  }
}

size_t ZDoc::viewSettingId()
{
  if (!objs().contains(m_viewSettingId)) {
    m_viewSettingId = 0;
  }
  return m_viewSettingId;
}

std::map<size_t, size_t> ZDoc::read(const QJsonObject& json, QString& err)
{
  std::map<size_t, size_t> res;
  if (!json.isEmpty()) {
    for (int i = 0; i < m_docPacks.size(); ++i) {
      QList<QPair<QString, QJsonValue>> docKeyValueList;
      for (QJsonObject::const_iterator it = json.begin(); it != json.end(); ++it) {
        if (it.key().startsWith(m_docPacks[i].doc->typeName())) {
          docKeyValueList.push_back(qMakePair(it.key(), it.value()));
        }
      }
      if (!docKeyValueList.empty()) {
        for (const auto& idid : m_docPacks[i].doc->read(docKeyValueList, err)) {
          res[idid.first] = idid.second;
        }
      }
    }
  }
  return res;
}

void ZDoc::write(QJsonObject& json, bool includeAnimation) const
{
  for (int i = 0; i < m_docPacks.size(); ++i) {
    if (!includeAnimation && m_docPacks[i].doc == m_animation3DDoc)
      continue;
    if (!includeAnimation && m_docPacks[i].doc == m_animation2DDoc)
      continue;
    m_docPacks[i].doc->write(json);
  }
}

QString ZDoc::lastOpenedFilePath()
{
  QSettings settings;
  return settings.value(QString("doc/lastOpenedPath")).toString();
}

void ZDoc::setLastOpenedFilePath(const QString& path)
{
  if (path.isEmpty())
    return;
  QDir dir = QFileInfo(path).dir();
  QSettings settings;
  settings.setValue(QString("doc/lastOpenedPath"), dir.absolutePath());
}

void ZDoc::hideAnimation3DView()
{
  QList<size_t> a3ds = m_animation3DDoc->objs();
  for (int i = 0; i < a3ds.size(); ++i) {
    setObjVisible(a3ds[i], false);
    setObjSelected(a3ds[i], false);
  }
}

void ZDoc::removeAllObjs()
{
  QAction* action = qobject_cast<QAction*>(sender());
  if (!action || action == m_removeAllAction) {
    for (int i = 0; i < m_docPacks.size(); ++i) {
      removeAllObjsOfDoc(m_docPacks[i].doc);
    }
    if (!hasObj())
      m_nextObjId = 100;
  } else {
    for (int i = 0; i < m_docPacks.size(); ++i) {
      if (m_docPacks[i].removeAllAction == action) {
        removeAllObjsOfDoc(m_docPacks[i].doc);
        break;
      }
    }
  }
}

void ZDoc::showSelectedObjs()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    setObjVisible(objs[i], true);
  }
}

void ZDoc::hideSelectedObjs()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    setObjVisible(objs[i], false);
  }
}

void ZDoc::makeAliasOfSelectedObjs()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    ZObjDoc* doc = idToDoc(objs[i]);
    doc->makeAlias(objs[i]);
  }
}

void ZDoc::removeSelectedObjs()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    removeObj(objs[i]);
  }
}

bool ZDoc::saveSelectedObjs()
{
  bool res = true;
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    res = res && idToDoc(objs[i])->save(objs[i]);
  }
  return res;
}

bool ZDoc::saveSelectedObjsAs()
{
  bool res = true;
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    res = res && idToDoc(objs[i])->saveAs(objs[i]);
  }
  return res;
}

bool ZDoc::saveAllObjs()
{
  bool res = true;
  QList<size_t> allObjs = objs();
  for (int i = 0; i < allObjs.size(); ++i) {
    res = res && idToDoc(allObjs[i])->save(allObjs[i]);
  }
  return res;
}

void ZDoc::showSelectedObjsInGraphicalShell()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  for (int i = 0; i < objs.size(); ++i) {
    idToDoc(objs[i])->showObjInGraphicalShell(objs[i]);
  }
}

void ZDoc::copySelectedObjsPathToClipboard()
{
  QList<size_t> objs = m_objSelectionModel->selectedObjs();
  if (!objs.isEmpty()) {
    QString path = idToDoc(objs[0])->objPath(objs[0]);
    for (int i = 1; i < objs.size(); ++i) {
      path += QString("\n");
      path += idToDoc(objs[i])->objPath(objs[i]);
    }
    QApplication::clipboard()->setText(path);
  }
}

void ZDoc::create2DAnimation()
{
  bool ok;
  QString text = QInputDialog::getText(QApplication::activeWindow(), tr("Animation Name:"),
                                       tr("Name of 2D Animation:"), QLineEdit::Normal,
                                       "Unnamed_2D_Animation", &ok);
  if (ok && !text.isEmpty())
    m_animation2DDoc->createNewAnimation(text);
}

void ZDoc::create3DAnimation()
{
  bool ok;
  QString text = QInputDialog::getText(QApplication::activeWindow(), tr("Animation Name:"),
                                       tr("Name of 3D Animation:"), QLineEdit::Normal,
                                       "Unnamed_3D_Animation", &ok);
  if (ok && !text.isEmpty())
    m_animation3DDoc->createNewAnimation(text);
}

void ZDoc::createActions()
{
  m_undoAction = m_undoGroup->createUndoAction(this, tr("&Undo"));
  m_undoAction->setIcon(QIcon(":/icons/undo-512.png"));
  m_undoAction->setShortcuts(QKeySequence::Undo);

  m_redoAction = m_undoGroup->createRedoAction(this, tr("&Redo"));
  m_redoAction->setIcon(QIcon(":/icons/redo-512.png"));
  m_redoAction->setShortcuts(QKeySequence::Redo);

  m_removeAllAction = new QAction(tr("&Remove Everything"), this);
  m_removeAllAction->setStatusTip(tr("Remove everything from workspace"));
  connect(m_removeAllAction, &QAction::triggered, this, &ZDoc::removeAllObjs);

  m_make2DAnimationAction = new QAction(tr("&Make 2D Animation"), this);
  m_make2DAnimationAction->setStatusTip(tr("Create 2D Animation starting from current scene"));
  connect(m_make2DAnimationAction, &QAction::triggered, this, &ZDoc::create2DAnimation);

  m_make3DAnimationAction = new QAction(tr("Make 3D &Animation"), this);
  m_make3DAnimationAction->setStatusTip(tr("Create 3D Animation starting from current scene"));
  connect(m_make3DAnimationAction, &QAction::triggered, this, &ZDoc::create3DAnimation);

  m_changeAnimationSettingAction = new QAction(tr("&Change Animation Settings..."), this);
  m_changeAnimationSettingAction->setStatusTip(tr("Open Animation Settings Dialog"));
}

bool ZDoc::loadFile(const QString& fileName, QString& errMsg)
{
  if (QFile::exists(fileName)) {
    for (int i = 0; i < m_docPacks.size(); ++i) {
      if (m_docPacks[i].doc->canReadFile(fileName)) {
        QString tmpErr;
        if (m_docPacks[i].doc->loadFile(fileName, tmpErr)) {
          errMsg.clear();
          return true;
        }
        errMsg += QString("\nRead As %1, failed: %2 ").arg(m_docPacks[i].doc->typeName()).arg(tmpErr);
      }
    }
    if (errMsg.isEmpty()) {
      errMsg = tr("File is not supported");
    }
  } else {
    errMsg = tr("File does not exist");
  }
  return false;
}

} // namespace nim
