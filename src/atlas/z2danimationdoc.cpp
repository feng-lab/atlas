#include "z2danimationdoc.h"

#include "zanimationwidget.h"
#include "zexception.h"
#include "zlog.h"
#include "zview.h"
#include "ztheme.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <set>

namespace nim {

Z2DAnimationDoc::Z2DAnimationDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

void Z2DAnimationDoc::bindView(ZView* v)
{
  m_view = v;
  connect(m_view, &ZView::destroyed, this, &Z2DAnimationDoc::releaseView);
  for (const auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->bindView(m_view);
  }
}

void Z2DAnimationDoc::createNewAnimation(const QString& name)
{
  Z2DAnimation* animation = new Z2DAnimation(m_doc, this);
  addAnimation(animation, "", name);
  animation->addKeyFrame(0);
}

bool Z2DAnimationDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToAnimationPacks.at(id);
  if (pack->path.endsWith(".animation2d", Qt::CaseInsensitive)) {
    QString err;
    if (saveAnimation(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save Error.\n" + err);
    return false;
  }
  return saveAs(id);
}

bool Z2DAnimationDoc::saveAs(size_t id)
{
  QStringList filters;
  filters << "Animation files (*.animation2d)";

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save 2D Animation %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToAnimationPacks.at(id);
    if (saveAnimation(pack.get(), dialog.selectedFiles().at(0), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save As Error.\n" + err);
  }
  return false;
}

bool Z2DAnimationDoc::canReadFile(const QString& fileName) const
{
  return fileName.endsWith(".animation2d", Qt::CaseInsensitive);
}

size_t Z2DAnimationDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToAnimationPacks) {
    if (idPack.second->path == fileName)
      return idPack.first;
  }
  size_t id;
  try {
    auto animation = std::make_unique<Z2DAnimation>(m_doc);
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

size_t Z2DAnimationDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
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
    auto animation = std::make_unique<Z2DAnimation>(m_doc);
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

QList<QAction*> Z2DAnimationDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadAnimationsAction);
  return res;
}

void Z2DAnimationDoc::removeObj(size_t id)
{
  auto it = m_idToAnimationPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToAnimationPacks.erase(it);
  emit objRemoved(id, this);
}

QString Z2DAnimationDoc::objName(size_t id) const
{
  return m_idToAnimationPacks.at(id)->name();
}

QString Z2DAnimationDoc::objPath(size_t id) const
{
  return m_idToAnimationPacks.at(id)->path;
}

bool Z2DAnimationDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToAnimationPacks.at(id)->hasUnsavedChange;
}

QString Z2DAnimationDoc::objInfo(size_t id) const
{
  return m_idToAnimationPacks.at(id)->info();
}

QString Z2DAnimationDoc::objTooltip(size_t id) const
{
  return m_idToAnimationPacks.at(id)->tooltip();
}

const QUndoStack* Z2DAnimationDoc::objUndoStack(size_t id) const
{
  return m_idToAnimationPacks.at(id)->animation->undoStack();
}

QJsonValue Z2DAnimationDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToAnimationPacks.at(id)->path);
}

bool Z2DAnimationDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
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

size_t Z2DAnimationDoc::makeAlias(size_t /*id*/)
{
  return 0;
}

bool Z2DAnimationDoc::isAlias(size_t id) const
{
  CHECK(m_idToAnimationPacks.find(id) != m_idToAnimationPacks.end());

  auto& pack = m_idToAnimationPacks.at(id);
  for (const auto& idPack : m_idToAnimationPacks) {
    if (idPack.first != id && idPack.second == pack)
      return true;
  }
  return false;
}

QWidget* Z2DAnimationDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToAnimationPacks.find(id) != m_idToAnimationPacks.end());

  auto& pack = m_idToAnimationPacks.at(id);
  return new ZAnimationWidget(*pack->animation);
}

void Z2DAnimationDoc::loadAnimation()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter("*.animation2d");
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load 2D Animation File");
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

void Z2DAnimationDoc::setModified()
{
  if (Z2DAnimation* animation = qobject_cast<Z2DAnimation*>(sender())) {
    for (const auto& idPack : m_idToAnimationPacks) {
      if (idPack.second->animation.get() == animation) {
        if (!idPack.second->hasUnsavedChange) {
          idPack.second->updateDerivedData();
          idPack.second->hasUnsavedChange = true;
          m_doc.updateObjInfo(idPack.first);
        }
        return;
      }
    }
  }
}

void Z2DAnimationDoc::releaseView()
{
  for (const auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->releaseView();
  }
  m_view = nullptr;
}

size_t Z2DAnimationDoc::addAnimation(Z2DAnimation* animation, const QString& path, const QString& name)
{
  size_t id = m_doc.getNewObjId();
  m_idToAnimationPacks[id] = std::make_shared<AnimationPack>(animation, path, name);
  m_doc.registerNewObj(id, *this);
  animation->bindView(m_view);

  emit objAdded(id, this);
  connect(animation, &Z2DAnimation::durationChanged, this, &Z2DAnimationDoc::setModified);
  connect(animation, &Z2DAnimation::keysChanged, this, &Z2DAnimationDoc::setModified);
  connect(animation, &Z2DAnimation::objChanged, this, &Z2DAnimationDoc::setModified);
  connect(animation, &Z2DAnimation::keyChanged, this, &Z2DAnimationDoc::setModified);
  connect(animation, &Z2DAnimation::colorChanged, this, &Z2DAnimationDoc::setModified);
  connect(animation, &Z2DAnimation::keyAboutToDelete, this, &Z2DAnimationDoc::setModified);
  return id;
}

Z2DAnimationDoc::AnimationPack::AnimationPack(Z2DAnimation* animation_, const QString& path_, const QString& name)
  : animation(animation_), path(QFileInfo(path_).canonicalFilePath()), m_tmpName(name)
{
  if (path.isEmpty()) {
    hasUnsavedChange = true;
  }
  updateDerivedData();
}

void Z2DAnimationDoc::AnimationPack::updateDerivedData()
{
  m_info.clear();
  m_name = path.isEmpty() ? m_tmpName : QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& Z2DAnimationDoc::AnimationPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 secs").arg(animation->duration());
  }
  return m_info;
}

void Z2DAnimationDoc::createActions()
{
  m_loadAnimationsAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load 2D Animations..."), this);
  m_loadAnimationsAction->setStatusTip(tr("Load one or more existing Animation files"));
  connect(m_loadAnimationsAction, &QAction::triggered, this, &Z2DAnimationDoc::loadAnimation);
}

bool Z2DAnimationDoc::saveAnimation(AnimationPack* pack, const QString& fileName, QString& errorMsg)
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
