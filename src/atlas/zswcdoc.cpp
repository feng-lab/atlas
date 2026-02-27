#include "zswcdoc.h"

#include "zexception.h"
#include "zlog.h"
#include "ztheme.h"
#include "zswcwidget.h"
#include "zsubtractswcsdialog.h"
#include "zmessageboxhelpers.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>
#include <QMenu>
#include <set>

namespace nim {

ZSwcDoc::ZSwcDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

size_t ZSwcDoc::addSwcFromMemory(ZSwc tree, const QString& path)
{
  return addSwc(std::move(tree), path);
}

bool ZSwcDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToSwcPacks.at(id);
  if (ZSwc::canWriteFile(pack->path())) {
    QString err;
    if (saveSwc(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(),
                            tr("Can not save SWC %1").arg(pack->path()),
                            tr("Object: %1\n%2").arg(objName(id), err));
    return false;
  }
  return saveAs(id);
}

bool ZSwcDoc::saveAs(size_t id)
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilter(ZSwc::getQtWriteNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Swc %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToSwcPacks.at(id);
    const QString targetPath = dialog.selectedFiles().at(0);
    if (saveSwc(pack.get(), targetPath, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save SWC %1").arg(targetPath), err);
  }
  return false;
}

bool ZSwcDoc::canReadFile(const QString& fileName) const
{
  return ZSwc::canReadFile(fileName);
}

size_t ZSwcDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToSwcPacks) {
    if (idPack.second->path() == fileName) {
      return idPack.first;
    }
  }
  try {
    ZSwc tree(fileName);
    size_t id = addSwc(std::move(tree), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);

    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZSwcDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }
    for (const auto& idPack : m_idToSwcPacks) {
      if (isSameObj(jValue, jsonValue(idPack.first))) {
        return idPack.first;
      }
    }
    QString fileName = asQString(jValue);

    ZSwc tree(fileName);
    size_t id = addSwc(std::move(tree), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

bool ZSwcDoc::canPrepareLoadAsync(const json::value& jValue) const
{
  if (!jValue.is_string()) {
    return false;
  }
  return !asQString(jValue).trimmed().isEmpty();
}

folly::coro::Task<ZObjDoc::PreparedLoadResult> ZSwcDoc::prepareLoadAsync(const json::value& jValue,
                                                                         const ZObjDoc::AsyncLoadContext&) const
{
  PreparedLoadResult out;
  const QString fileName = asQString(jValue);
  if (fileName.trimmed().isEmpty()) {
    out.errorMsg = QString("File path is not string or is empty");
    co_return out;
  }

  try {
    ZSwc tree(fileName);
    ZSwcDoc* self = const_cast<ZSwcDoc*>(this);
    out.commit = [self, this, fileName, tree = std::move(tree)](QString& errorMsg) mutable -> size_t {
      try {
        const size_t id = self->addSwc(std::move(tree), fileName);
        ZSystemInfo::instance().addFileToRecentFileList(fileName);
        setLastOpenedObjPath(fileName);
        return id;
      }
      catch (const ZException& e) {
        errorMsg = e.what();
        return 0;
      }
      catch (const std::exception& e) {
        errorMsg = e.what();
        return 0;
      }
    };
  }
  catch (const ZException& e) {
    out.errorMsg = e.what();
  }
  catch (const std::exception& e) {
    out.errorMsg = e.what();
  }
  co_return out;
}

std::vector<QAction*> ZSwcDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadSwcAction);
  return res;
}

QMenu* ZSwcDoc::processObjMenu() const
{
  auto* res = new QMenu(typeName());
  res->addAction(m_editSwcAction);
  res->addAction(m_subtractSwcsAction);
  return res;
}

void ZSwcDoc::removeObj(size_t id)
{
  auto it = m_idToSwcPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToSwcPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZSwcDoc::objName(size_t id) const
{
  return m_idToSwcPacks.at(id)->name();
}

QString ZSwcDoc::objPath(size_t id) const
{
  return m_idToSwcPacks.at(id)->path();
}

bool ZSwcDoc::objHasUnsavedChange(size_t id) const
{
  return !(canReadFile(m_idToSwcPacks.at(id)->path()) && objUndoStack(id)->isClean());
}

QString ZSwcDoc::objInfo(size_t id) const
{
  return m_idToSwcPacks.at(id)->info();
}

QString ZSwcDoc::objTooltip(size_t id) const
{
  return m_idToSwcPacks.at(id)->tooltip();
}

const QUndoStack* ZSwcDoc::objUndoStack(size_t id) const
{
  return m_idToSwcPacks.at(id)->undoStack();
}

json::value ZSwcDoc::jsonValue(size_t id) const
{
  return json::value_from(m_idToSwcPacks.at(id)->path());
}

bool ZSwcDoc::isSameObj(const json::value& v1, const json::value& v2) const
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

size_t ZSwcDoc::makeAlias(size_t id)
{
  CHECK(m_idToSwcPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToSwcPacks[aliasId] = m_idToSwcPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZSwcDoc::isAlias(size_t id) const
{
  CHECK(m_idToSwcPacks.contains(id));

  return std::ranges::any_of(m_idToSwcPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToSwcPacks.at(id);
  });
}

QWidget* ZSwcDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToSwcPacks.contains(id));

  return new ZSwcWidget(swcPack(id), m_doc);
}

void ZSwcDoc::loadSwc()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZSwc::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Swc File");
  if (dialog.exec()) {
    QString errorMsg;
    // auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadFile(filePath, errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load SWC %1").arg(filePath), errorMsg);
      }
    }
  }
}

size_t ZSwcDoc::addSwc(ZSwc tree, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToSwcPacks[id] = std::make_shared<ZSwcPack>(std::move(tree), path, id, *this);
  m_doc.registerNewObj(m_idToSwcPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(m_idToSwcPacks[id].get(), &ZSwcPack::undoStackCleanChanged, this, &ZSwcDoc::setModified);
  return id;
}

void ZSwcDoc::setModified(bool)
{
  if (auto ra = qobject_cast<ZSwcPack*>(sender())) {
    for (const auto& idPack : m_idToSwcPacks) {
      if (idPack.second.get() == ra) {
        idPack.second->updateDerivedData();
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

void ZSwcDoc::createActions()
{
  m_loadSwcAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Swc..."), this);
  m_loadSwcAction->setStatusTip(tr("Load an existing Swc file"));
  connect(m_loadSwcAction, &QAction::triggered, this, &ZSwcDoc::loadSwc);

  m_editSwcAction = new QAction(tr("&Edit SWC..."), this);
  m_editSwcAction->setStatusTip(tr("Open SWC editor"));
  connect(m_editSwcAction, &QAction::triggered, this, &ZSwcDoc::editSwc);

  m_subtractSwcsAction = new QAction(tr("&Subtract SWCs..."), this);
  m_subtractSwcsAction->setStatusTip(tr("Subtract SWC trees from an input SWC"));
  connect(m_subtractSwcsAction, &QAction::triggered, this, &ZSwcDoc::subtractSwcs);
}

void ZSwcDoc::editSwc()
{
  const size_t id = chooseOneObjWithWidget(tr("Edit SWC"), QApplication::activeWindow());
  if (id == 0) {
    return;
  }

  m_doc.requestOpenEditWidget(id);
}

void ZSwcDoc::subtractSwcs()
{
  ZSubtractSwcsDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
}

bool ZSwcDoc::saveSwc(ZSwcPack* pack, const QString& fileName, QString& errorMsg)
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

void ZSwcDoc::packInfoUpdated(ZSwcPack* pack)
{
  for (const auto& idPack : m_idToSwcPacks) {
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
