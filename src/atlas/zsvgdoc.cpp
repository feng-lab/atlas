#include "zsvgdoc.h"

#include "zlog.h"
#include "zexception.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QIcon>
#include <set>

namespace nim {

ZSvgDoc::ZSvgDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZSvgDoc::save(size_t /*id*/)
{
  return false;
}

bool ZSvgDoc::saveAs(size_t /*id*/)
{
  return false;
}

bool ZSvgDoc::canReadFile(const QString& fileName)
{
  return fileName.endsWith(".svg", Qt::CaseInsensitive);
}

size_t ZSvgDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToSvgPacks) {
    if (idPack.second->path == fileName)
      return idPack.first;
  }
  auto svg = std::make_unique<QSvgRenderer>(fileName);
  if (svg->isValid()) {
    size_t id = addSvg(std::move(svg), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);

    return id;
  }
  errorMsg = QString("invalid svg: %1").arg(fileName);
  return 0;
}

size_t ZSvgDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  if (!jValue.isString() || jValue.toString().trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (const auto& idPack : m_idToSvgPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first)))
      return idPack.first;
  }
  QString fileName = jValue.toString();
  auto svg = std::make_unique<QSvgRenderer>(fileName);
  if (svg->isValid()) {
    size_t id = addSvg(std::move(svg), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);

    return id;
  }
  errorMsg = QString("invalid svg: %1").arg(fileName);
  return 0;
}

QList<QAction*> ZSvgDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadSvgAction);
  return res;
}

void ZSvgDoc::removeObj(size_t id)
{
  auto it = m_idToSvgPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToSvgPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZSvgDoc::objName(size_t id) const
{
  return m_idToSvgPacks.at(id)->name();
}

QString ZSvgDoc::objPath(size_t id) const
{
  return m_idToSvgPacks.at(id)->path;
}

bool ZSvgDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToSvgPacks.at(id)->hasUnsavedChange;
}

QString ZSvgDoc::objInfo(size_t id) const
{
  return m_idToSvgPacks.at(id)->info();
}

QString ZSvgDoc::objTooltip(size_t id) const
{
  return m_idToSvgPacks.at(id)->tooltip();
}

QJsonValue ZSvgDoc::jsonValue(size_t id) const
{
  return QJsonValue(m_idToSvgPacks.at(id)->path);
}

bool ZSvgDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
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

size_t ZSvgDoc::makeAlias(size_t id)
{
  CHECK(m_idToSvgPacks.find(id) != m_idToSvgPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToSvgPacks[aliasId] = m_idToSvgPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZSvgDoc::isAlias(size_t id) const
{
  auto& pack = m_idToSvgPacks.at(id);
  for (const auto& idPack : m_idToSvgPacks) {
    if (idPack.first != id && idPack.second == pack)
      return true;
  }
  return false;
}

void ZSvgDoc::loadSvg()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(QString("SVG files (*.svg)"));
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Svg File");
  if (dialog.exec()) {
    QString errorMsg;
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                              "Can not read svg.\n" + errorMsg);
      }
    }
  }
}

size_t ZSvgDoc::addSvg(std::unique_ptr<QSvgRenderer> svg, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToSvgPacks[id] = std::make_shared<SvgPack>(std::move(svg), path);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  return id;
}

ZSvgDoc::SvgPack::SvgPack(std::unique_ptr<QSvgRenderer> svg_, const QString& path_)
  : svg(std::move(svg_)), path(QFileInfo(path_).canonicalFilePath())
{
  updateDerivedData();
}

ZSvgDoc::SvgPack::~SvgPack()
{
}

void ZSvgDoc::SvgPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& ZSvgDoc::SvgPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("size (%1, %2)").arg(svg->defaultSize().width()).arg(svg->defaultSize().height());
  }
  return m_info;
}

void ZSvgDoc::createActions()
{
  m_loadSvgAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load Svg..."), this);
  m_loadSvgAction->setStatusTip(tr("Load an existing Svg file"));
  connect(m_loadSvgAction, &QAction::triggered, this, &ZSvgDoc::loadSvg);
}

void ZSvgDoc::packInfoUpdated(SvgPack* pack)
{
  for (const auto& idPack : m_idToSvgPacks) {
    if (idPack.second.get() == pack)
      m_doc.updateObjInfo(idPack.first);
  }
}

} // namespace nim
