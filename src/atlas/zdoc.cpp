#include "zdoc.h"

#include "zobjdoc.h"
#include "zobjmodel.h"
#include "zitemselectionmodel.h"
#include "zobjwidget.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "zsaveobjsdialog.h"
#include "zimgdoc.h"
#include "z2danimationdoc.h"
#include "z3danimationdoc.h"
#include "zroidoc.h"
#include "zpunctadoc.h"
#include "zswcdoc.h"
#include "zsvgdoc.h"
#include "zregionannotationdoc.h"
#include "zmeshdoc.h"
#include "zskeletondoc.h"
#include "z3drenderglobalstate.h"
#include "ztheme.h"
#include "zchooseobjdialog.h"
#include "zmessageboxhelpers.h"
#include "ztracesettings.h"
#include "zbackgroundtaskmanager.h"
#include <QUndoGroup>
#include <QAction>
#include <QApplication>
#include <QMessageBox>
#include <QFile>
#include <QSettings>
#include <QInputDialog>
#include <QDir>
#include <QMenu>
#include <QClipboard>

namespace nim {

namespace {

bool looksLikeNetworkUrl(const QString& s)
{
  const QString trimmed = s.trimmed();
  return trimmed.startsWith("precomputed://", Qt::CaseInsensitive) || trimmed.startsWith("gs://", Qt::CaseInsensitive) ||
         trimmed.startsWith("s3://", Qt::CaseInsensitive) ||
         trimmed.startsWith("http://", Qt::CaseInsensitive) || trimmed.startsWith("https://", Qt::CaseInsensitive);
}

} // namespace

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

  m_punctaDoc = new ZPunctaDoc(*this);
  registerObjDoc(m_punctaDoc);

  m_swcDoc = new ZSwcDoc(*this);
  registerObjDoc(m_swcDoc);

  m_svgDoc = new ZSvgDoc(*this);
  registerObjDoc(m_svgDoc);

  m_meshDoc = new ZMeshDoc(*this);
  registerObjDoc(m_meshDoc);

  m_skeletonDoc = new ZSkeletonDoc(*this);
  registerObjDoc(m_skeletonDoc);

  m_regionAnnotationDoc = new ZRegionAnnotationDoc(*this);
  registerObjDoc(m_regionAnnotationDoc);

  m_traceSettings = new ZTraceSettings(this);
  m_backgroundTaskManager = new ZBackgroundTaskManager(this);
}

void ZDoc::cancelAllBackgroundTasksAndWait()
{
  CHECK(m_backgroundTaskManager != nullptr);
  m_backgroundTaskManager->cancelAllTasksAndWait();
}

std::vector<size_t> ZDoc::chooseObjsWithWidget(const QString& title, QWidget* parent) const
{
  if (hasObj()) {
    ZChooseObjDialog dlg(*this, true, parent);
    if (!title.isEmpty()) {
      dlg.setWindowTitle(title);
    }
    if (dlg.exec() == QDialog::Accepted) {
      return dlg.selectedIDs();
    }
  }
  return {};
}

bool ZDoc::hasObj() const
{
  return m_objModel->hasObj();
}

size_t ZDoc::numObjs() const
{
  return m_objModel->numObjs();
}

std::vector<size_t> ZDoc::objs() const
{
  return m_objModel->objs();
}

size_t ZDoc::numSelectedObjs() const
{
  return m_objSelectionModel->numSelectedObjs();
}

std::vector<size_t> ZDoc::selectedObjs() const
{
  return m_objSelectionModel->selectedObjs();
}

std::vector<ZObjDoc*> ZDoc::objDocs()
{
  std::vector<ZObjDoc*> res;
  for (auto& docPack : m_docPacks) {
    res.push_back(docPack.doc);
  }
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

void ZDoc::setObjLocked(size_t id, bool v)
{
  m_objModel->setObjLocked(id, v);
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
    return {"Background"};
  }
  if (id == 2) {
    return {"Axis"};
  }
  if (id == 3) {
    return {"Lighting"};
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
    return {"Background"};
  }
  if (id == 2) {
    return {"Axis"};
  }
  if (id == 3) {
    return {"Lighting"};
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
    return {"Background"};
  }
  if (id == 2) {
    return {"Axis"};
  }
  if (id == 3) {
    return {"Lighting"};
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

std::vector<size_t> ZDoc::objsOfDoc(const ZObjDoc* objD) const
{
  return m_objModel->objsOfDoc(objD);
}

std::vector<size_t> ZDoc::selectedObjsOfDoc(const ZObjDoc* objD) const
{
  return m_objSelectionModel->selectedObjsOfDoc(objD);
}

void ZDoc::activateEmptyUndoStack()
{
  m_undoGroup->setActiveStack(m_emptyUndoStack);
}

std::vector<QAction*> ZDoc::fileActions() const
{
  std::vector<QAction*> res;
  for (auto& docPack : m_docPacks) {
    auto acts = docPack.doc->loadFileActions();
    res.insert(res.end(), acts.begin(), acts.end());
    res.push_back(docPack.removeAllAction);
  }
  res.push_back(m_removeAllAction);
  return res;
}

std::vector<QAction*> ZDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  for (auto& docPack : m_docPacks) {
    auto acts = docPack.doc->loadFileActions();
    res.insert(res.end(), acts.begin(), acts.end());
  }
  return res;
}

std::vector<QMenu*> ZDoc::processObjMenu() const
{
  std::vector<QMenu*> res;
  for (auto& docPack : m_docPacks) {
    QMenu* menu = docPack.doc->processObjMenu();
    if (menu) {
      res.push_back(menu);
    }
  }
  return res;
}

ZObjWidget* ZDoc::createObjWidget(QWidget* parent)
{
  auto wdt = new ZObjWidget(this, m_objModel, m_objSelectionModel, parent);
  return wdt;
}

QWidget* ZDoc::createObjEditWidget(size_t id) const
{
  if (id == 0) {
    return nullptr;
  }
  return idToDoc(id)->createObjEditWidget(id);
}

void ZDoc::updateObjInfo(size_t id)
{
  m_objModel->updateObj(id);
  Q_EMIT objInfoChanged(id);
}

void ZDoc::registerObjDoc(ZObjDoc* objD)
{
  DocPack docPack{};
  docPack.doc = objD;
  auto docRemoveAllAction = new QAction(tr("&Remove All %1").arg(objD->typePluralName()), this);
  docRemoveAllAction->setStatusTip(tr("&Remove All %1").arg(objD->typePluralName()));
  connect(docRemoveAllAction, &QAction::triggered, this, &ZDoc::removeAllObjs);
  docPack.removeAllAction = docRemoveAllAction;
  m_docPacks.push_back(docPack);
  connect(objD, &ZObjDoc::objAboutToBeRemoved, this, &ZDoc::onObjAboutToBeRemoved);
  connect(objD, &ZObjDoc::objAdded, this, &ZDoc::onObjAdded);
  connect(objD, &ZObjDoc::objAdded, this, &ZDoc::objAdded);
  connect(objD, &ZObjDoc::objRemoved, this, &ZDoc::objRemoved);
}

void ZDoc::registerNewObj(size_t id, ZObjDoc& objD)
{
  m_objModel->addObj(id, objD);
}

void ZDoc::registerNewObj(const std::shared_ptr<ZObjPack>& op)
{
  m_objModel->addObj(op);
}

void ZDoc::removeObj(size_t id)
{
  ZObjDoc* doc = m_objModel->idToDoc(id);
  if (saveOrDiscard(id)) {
    // Object removal can be triggered while the 3D rendering thread is in the middle of building
    // preview volumes or paging blocks. Proactively request cancellation so the rendering thread
    // can unwind quickly and detach filters before object data is destroyed.
    Z3DRenderGlobalState::instance().requestCancellation();
    m_objModel->removeObj(id);
    doc->removeObj(id);
  }
}

void ZDoc::removeObjsNoPrompt(const std::vector<size_t>& ids)
{
  CHECK(!ids.empty());

  // Match removeObj(): object removal can overlap with 3D preview/paging work on the rendering thread.
  // Proactively request cancellation so the rendering thread can unwind quickly and detach filters
  // before object data is destroyed.
  Z3DRenderGlobalState::instance().requestCancellation();

  for (auto id : ids) {
    ZObjDoc* doc = m_objModel->idToDoc(id);
    CHECK(doc);
    m_objModel->removeObj(id);
    doc->removeObj(id);
  }
}

void ZDoc::removeAllObjsOfDoc(ZObjDoc* doc)
{
  auto objs = objsOfDoc(doc);
  if (saveOrDiscard(objs)) {
    Z3DRenderGlobalState::instance().requestCancellation();
    for (auto obj : objs) {
      m_objModel->removeObj(obj);
      doc->removeObj(obj);
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
  auto ret = msgBox.exec();

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
    case QMessageBox::Cancel:
      // Cancel was clicked
      VLOG(1) << "cancel clicked";
      break;
    default:
      // should never be reached
      break;
  }
  return false;
}

bool ZDoc::saveOrDiscard(const std::vector<size_t>& objs)
{
  std::vector<size_t> unsavedObjs;
  for (auto obj : objs) {
    if (m_objModel->idToDoc(obj)->objHasUnsavedChange(obj)) {
      unsavedObjs.push_back(obj);
    }
  }
  if (unsavedObjs.empty()) {
    return true;
  }

  ZSaveObjsDialog dlg(*this, unsavedObjs, QApplication::activeWindow());
  auto ret = dlg.exec();
  switch (ret) {
    case QDialog::Accepted: {
      // VLOG(1) << "Save or Discard was clicked";
      bool saveSuccess = true;
      const auto& sobjs = dlg.objsToSave();
      for (auto sobj : sobjs) {
        // VLOG(1) << "save " << m_objModel->idToDoc(sobjs[i])->objName(sobjs[i]);
        saveSuccess = saveSuccess && m_objModel->idToDoc(sobj)->save(sobj);
      }
      return saveSuccess;
    }
    case QDialog::Rejected:
      // VLOG(1) << "Cancel was clicked";
      break;
    default:
      // should never be reached
      LOG(FATAL) << "crash";
      break;
  }
  return false;
}

void ZDoc::loadFile(const QString& fileName)
{
  QString error;
  if (!loadFile(fileName, error)) {
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load file %1").arg(fileName), error);
  }
}

void ZDoc::loadFileList(const QStringList& fileList)
{
  QString error;
  for (const auto& i : fileList) {
    QString tmpErr;
    if (!loadFile(i, tmpErr)) {
      if (!error.isEmpty()) {
        error += "\n\n";
      }
      error += QString("Can not read file %1. Error: %2").arg(i).arg(tmpErr);
    }
  }
  if (!error.isEmpty()) {
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load one or more files"), error);
  }
}

size_t ZDoc::viewSettingId()
{
  if (!contains(objs(), m_viewSettingId)) {
    m_viewSettingId = 0;
  }
  return m_viewSettingId;
}

std::map<size_t, size_t> ZDoc::read(const json::object& jo, QString& err)
{
  std::map<size_t, size_t> res;
  if (!jo.empty()) {
    ZBenchTimer bt("ZDoc::read");
    for (auto& docPack : m_docPacks) {
      std::vector<std::pair<QString, json::value>> docKeyValueList;
      for (const auto& [key, value] : jo) {
        QString qkey = QString::fromUtf8(key.data(), key.size());
        if (qkey.startsWith(docPack.doc->typeName())) {
          docKeyValueList.emplace_back(qkey, value);
        }
      }
      // VLOG(1) << json::value_from(docKeyValueList);
      if (!docKeyValueList.empty()) {
        const size_t beforeSize = res.size();
        for (const auto& idid : docPack.doc->read(docKeyValueList, err)) {
          res[idid.first] = idid.second;
        }
        bt.recordEvent(fmt::format("{} ids={}", docPack.doc->typeName().toStdString(), res.size() - beforeSize));
      }
    }
    bt.stop();
    VLOG(1) << bt;
  }
  return res;
}

void ZDoc::write(json::object& json, bool includeAnimation) const
{
  for (auto& docPack : m_docPacks) {
    if (!includeAnimation && docPack.doc == m_animation3DDoc) {
      continue;
    }
    if (!includeAnimation && docPack.doc == m_animation2DDoc) {
      continue;
    }
    docPack.doc->write(json);
  }
}

QString ZDoc::lastOpenedFilePath()
{
  QSettings settings;
  return settings.value(QString("doc/lastOpenedPath")).toString();
}

void ZDoc::setLastOpenedFilePath(const QString& path)
{
  if (path.isEmpty()) {
    return;
  }
  QDir dir = QFileInfo(path).dir();
  QSettings settings;
  settings.setValue(QString("doc/lastOpenedPath"), dir.absolutePath());
}

void ZDoc::hideAnimation3DView()
{
  auto a3ds = m_animation3DDoc->objs();
  for (auto a3d : a3ds) {
    setObjVisible(a3d, false);
    setObjSelected(a3d, false);
  }
}

void ZDoc::deselectAllObjs()
{
  for (auto id : selectedObjs()) {
    setObjSelected(id, false);
  }
}

void ZDoc::removeAllObjs()
{
  auto action = qobject_cast<QAction*>(sender());
  if (!action || action == m_removeAllAction) {
    for (auto& docPack : m_docPacks) {
      removeAllObjsOfDoc(docPack.doc);
    }
    if (!hasObj()) {
      m_nextObjId = 100;
    }
  } else {
    for (auto& docPack : m_docPacks) {
      if (docPack.removeAllAction == action) {
        removeAllObjsOfDoc(docPack.doc);
        break;
      }
    }
  }
}

void ZDoc::showSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    setObjVisible(obj, true);
  }
}

void ZDoc::hideSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    setObjVisible(obj, false);
  }
}

void ZDoc::lockSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    setObjLocked(obj, true);
  }
}

void ZDoc::unlockSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    setObjLocked(obj, false);
  }
}

void ZDoc::makeAliasOfSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    ZObjDoc* doc = idToDoc(obj);
    doc->makeAlias(obj);
  }
}

void ZDoc::removeSelectedObjs()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    removeObj(obj);
  }
}

bool ZDoc::saveSelectedObjs()
{
  bool res = true;
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    res = res && idToDoc(obj)->save(obj);
  }
  return res;
}

bool ZDoc::saveSelectedObjsAs()
{
  bool res = true;
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    res = res && idToDoc(obj)->saveAs(obj);
  }
  return res;
}

bool ZDoc::saveAllObjs() const
{
  bool res = true;
  auto allObjs = objs();
  for (auto obj : allObjs) {
    res = res && idToDoc(obj)->save(obj);
  }
  return res;
}

void ZDoc::showSelectedObjsInGraphicalShell()
{
  auto objs = m_objSelectionModel->selectedObjs();
  for (auto obj : objs) {
    idToDoc(obj)->showObjInGraphicalShell(obj);
  }
}

void ZDoc::copySelectedObjsPathToClipboard()
{
  auto objs = m_objSelectionModel->selectedObjs();
  if (!objs.empty()) {
    QString path = idToDoc(objs[0])->objPath(objs[0]);
    for (auto id : objs) {
      path += QString("\n");
      path += idToDoc(id)->objPath(id);
    }
    QApplication::clipboard()->setText(path);
  }
}

void ZDoc::requestOpenEditWidget(size_t id)
{
  sendOpenEditWidgetSignal(id);
}

void ZDoc::onObjAboutToBeRemoved(size_t id, ZObjDoc* doc)
{
  if (auto us = doc->objUndoStack(id); us) {
    m_undoGroup->removeStack(us);
  }
  Q_EMIT objAboutToBeRemoved(id, doc);
}

void ZDoc::onObjAdded(size_t id, ZObjDoc* doc)
{
  if (auto us = doc->objUndoStack(id); us) {
    m_undoGroup->addStack(us);
  }
}

void ZDoc::create2DAnimation()
{
  bool ok;
  QString text = QInputDialog::getText(QApplication::activeWindow(),
                                       tr("Animation Name:"),
                                       tr("Name of 2D Animation:"),
                                       QLineEdit::Normal,
                                       "Unnamed_2D_Animation",
                                       &ok);
  if (ok && !text.isEmpty()) {
    m_animation2DDoc->createNewAnimation(text);
  }
}

void ZDoc::create3DAnimation()
{
  bool ok;
  QString text = QInputDialog::getText(QApplication::activeWindow(),
                                       tr("Animation Name:"),
                                       tr("Name of 3D Animation:"),
                                       QLineEdit::Normal,
                                       "Unnamed_3D_Animation",
                                       &ok);
  if (ok && !text.isEmpty()) {
    m_animation3DDoc->createNewAnimation(text);
  }
}

void ZDoc::createActions()
{
  m_undoAction = m_undoGroup->createUndoAction(this, tr("&Undo"));
  m_undoAction->setIcon(ZTheme::instance().icon(ZTheme::UndoIcon));
  m_undoAction->setShortcuts(QKeySequence::Undo);

  m_redoAction = m_undoGroup->createRedoAction(this, tr("&Redo"));
  m_redoAction->setIcon(ZTheme::instance().icon(ZTheme::RedoIcon));
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
  const QString trimmed = fileName.trimmed();
  const bool isNetworkUrl = looksLikeNetworkUrl(trimmed);
  if (QFile::exists(trimmed) || isNetworkUrl) {
    for (auto& docPack : m_docPacks) {
      if (docPack.doc->canReadFile(trimmed)) {
        QString tmpErr;
        if (docPack.doc->loadFile(trimmed, tmpErr)) {
          errMsg.clear();
          return true;
        }
        errMsg += QString("\nRead As %1, failed: %2 ").arg(docPack.doc->typeName()).arg(tmpErr);
      }
    }
    if (errMsg.isEmpty()) {
      errMsg = isNetworkUrl ? tr("URL is not supported") : tr("File is not supported");
    }
  } else {
    errMsg = tr("File does not exist");
  }
  return false;
}

} // namespace nim
