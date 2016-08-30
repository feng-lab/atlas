#include "z3danimationdoc.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <set>
#include "zexception.h"
#include "zanimationwidget.h"
#include "z3dview.h"
#include "zlog.h"

namespace nim {

Z3DAnimationDoc::Z3DAnimationDoc(ZDoc& doc)
  : ZObjDoc(doc), m_view(nullptr)
{
  createActions();
}

void Z3DAnimationDoc::bindView(Z3DView* v)
{
  m_view = v;
  connect(m_view, &Z3DView::destroyed, this, &Z3DAnimationDoc::releaseView);
  for (auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->bindView(m_view);
  }
}

void Z3DAnimationDoc::createNewAnimation(const QString& name)
{
  Z3DAnimation* animation = new Z3DAnimation(m_doc, this);
  addAnimation(animation, "", name);
  animation->addKeyFrame(0);
}

bool Z3DAnimationDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToAnimationPacks.at(id);
  if (pack->path.endsWith(".animation3d", Qt::CaseInsensitive)) {
    QString err;
    if (saveAnimation(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save Error.\n" + err);
      return false;
    }
  } else {
    return saveAs(id);
  }
}

bool Z3DAnimationDoc::saveAs(size_t id)
{
  QStringList filters;
  filters << "3D Animation files (*.animation3d)";

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save 3D Animation %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToAnimationPacks.at(id);
    if (saveAnimation(pack.get(), dialog.selectedFiles().at(0), err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save As Error.\n" + err);
    }
  }
  return false;
}

bool Z3DAnimationDoc::canReadFile(const QString& fileName)
{
  return fileName.endsWith(".animation3d", Qt::CaseInsensitive);
}

size_t Z3DAnimationDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToAnimationPacks) {
    if (idPack.second->path == fileName)
      return idPack.first;
  }
  size_t id;
  try {
    auto animation = std::make_unique<Z3DAnimation>(m_doc);
    animation->load(fileName);
    id = addAnimation(animation.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t Z3DAnimationDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (const auto& idPack : m_idToAnimationPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first)))
      return idPack.first;
  }
  size_t id;
  QString fileName = jValue.toString();
  try {
    auto animation = std::make_unique<Z3DAnimation>(m_doc);
    animation->load(fileName);
    id = addAnimation(animation.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

QList<QAction*> Z3DAnimationDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadAnimationsAction);
  return res;
}

void Z3DAnimationDoc::removeObj(size_t id)
{
  auto it = m_idToAnimationPacks.find(id);
  m_doc.undoGroup()->removeStack(objUndoStack(id));
  emit objAboutToBeRemoved(it->first, this);
  m_idToAnimationPacks.erase(it);
  emit objRemoved(id, this);
}

QString Z3DAnimationDoc::objName(size_t id) const
{
  return m_idToAnimationPacks.at(id)->name();
}

QString Z3DAnimationDoc::objPath(size_t id) const
{
  return m_idToAnimationPacks.at(id)->path;
}

bool Z3DAnimationDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToAnimationPacks.at(id)->hasUnsavedChange;
}

QString Z3DAnimationDoc::objInfo(size_t id) const
{
  return m_idToAnimationPacks.at(id)->info();
}

QString Z3DAnimationDoc::objTooltip(size_t id) const
{
  return m_idToAnimationPacks.at(id)->tooltip();
}

QUndoStack* Z3DAnimationDoc::objUndoStack(size_t id)
{
  return m_idToAnimationPacks.at(id)->animation->undoStack();
}

QJsonValue Z3DAnimationDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToAnimationPacks.at(id)->path);
}

bool Z3DAnimationDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
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

size_t Z3DAnimationDoc::makeAlias(size_t)
{
  return 0;
}

bool Z3DAnimationDoc::isAlias(size_t id) const
{
  CHECK(m_idToAnimationPacks.find(id) != m_idToAnimationPacks.end());

  auto& pack = m_idToAnimationPacks.at(id);
  for (auto it = m_idToAnimationPacks.cbegin(); it != m_idToAnimationPacks.cend(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

QWidget* Z3DAnimationDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToAnimationPacks.find(id) != m_idToAnimationPacks.end());

  auto& pack = m_idToAnimationPacks.at(id);
  return new ZAnimationWidget(*pack->animation);
}

void Z3DAnimationDoc::loadAnimation()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter("*.animation3d");
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load 3D Animation File");
  if (dialog.exec()) {
    QString errorMsg;
    //int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                              "Can not read Animation.\n" + errorMsg);
      }
    }
  }
}

void Z3DAnimationDoc::setModified()
{
  if (Z3DAnimation* animation = qobject_cast<Z3DAnimation*>(sender())) {
    for (auto& idPack : m_idToAnimationPacks) {
      if (idPack.second->animation.get() == animation) {
        idPack.second->updateDerivedData();
        idPack.second->hasUnsavedChange = true;
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

void Z3DAnimationDoc::releaseView()
{
  for (auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->releaseView();
  }
  m_view = nullptr;
}

size_t Z3DAnimationDoc::addAnimation(Z3DAnimation* animation, const QString& path, const QString& name)
{
  size_t id = m_doc.getNewObjId();
  m_idToAnimationPacks[id] = std::make_shared<AnimationPack>(animation, path, name);
  m_doc.registerNewObj(id, this);
  m_doc.undoGroup()->addStack(animation->undoStack());
  animation->bindView(m_view);

  emit objAdded(id, this);
  connect(animation, &Z3DAnimation::durationChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::keysChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::objChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::keyChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::colorChanged, this, &Z3DAnimationDoc::setModified);
  return id;
}

Z3DAnimationDoc::AnimationPack::AnimationPack(Z3DAnimation* animation_, const QString& path_, const QString& name)
  : animation(animation_), path(QFileInfo(path_).canonicalFilePath()), hasUnsavedChange(false), m_tmpName(name)
{
  if (path.isEmpty()) {
    hasUnsavedChange = true;
  }
  updateDerivedData();
}

void Z3DAnimationDoc::AnimationPack::updateDerivedData()
{
  m_info.clear();
  m_name = path.isEmpty() ? m_tmpName : QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& Z3DAnimationDoc::AnimationPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 secs").arg(animation->duration());
  }
  return m_info;
}

void Z3DAnimationDoc::createActions()
{
  m_loadAnimationsAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load 3D Animations..."), this);
  m_loadAnimationsAction->setStatusTip(tr("Load one or more existing Animation files"));
  connect(m_loadAnimationsAction, &QAction::triggered, this, &Z3DAnimationDoc::loadAnimation);
}

bool Z3DAnimationDoc::saveAnimation(AnimationPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->animation->save(fileName);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

} // namespace nim
