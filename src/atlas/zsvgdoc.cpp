#include "zsvgdoc.h"

#include "zlog.h"
#include "zexception.h"
#include "ztheme.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
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

bool ZSvgDoc::canReadFile(const QString& fileName) const
{
  return fileName.endsWith(".svg", Qt::CaseInsensitive);
}

size_t ZSvgDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToSvgPacks) {
    if (idPack.second->path == fileName) {
      return idPack.first;
    }
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

size_t ZSvgDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  if (!jValue.is_string() || asQString(jValue).trimmed().isEmpty()) {
    errorMsg = QString("File path is not string or is empty");
    return 0;
  }
  for (const auto& idPack : m_idToSvgPacks) {
    if (isSameObj(jValue, jsonValue(idPack.first))) {
      return idPack.first;
    }
  }
  QString fileName = asQString(jValue);
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

std::vector<QAction*> ZSvgDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadSvgAction);
  return res;
}

void ZSvgDoc::removeObj(size_t id)
{
  auto it = m_idToSvgPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToSvgPacks.erase(it);
  Q_EMIT objRemoved(id, this);
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

json::value ZSvgDoc::jsonValue(size_t id) const
{
  return json::value_from(m_idToSvgPacks.at(id)->path);
}

bool ZSvgDoc::isSameObj(const json::value& v1, const json::value& v2) const
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

size_t ZSvgDoc::makeAlias(size_t id)
{
  CHECK(m_idToSvgPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToSvgPacks[aliasId] = m_idToSvgPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZSvgDoc::isAlias(size_t id) const
{
  CHECK(m_idToSvgPacks.contains(id));

  return std::ranges::any_of(m_idToSvgPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToSvgPacks.at(id);
  });
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
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadFile(dialog.selectedFiles().at(i), errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(),
                              QApplication::applicationName(),
                              "Can not read svg.\n" + errorMsg);
      }
    }
  }
}

size_t ZSvgDoc::addSvg(std::unique_ptr<QSvgRenderer> svg, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToSvgPacks[id] = std::make_shared<SvgPack>(std::move(svg), path);
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

ZSvgDoc::SvgPack::SvgPack(std::unique_ptr<QSvgRenderer> svg_, const QString& path_)
  : svg(std::move(svg_))
  , path(QFileInfo(path_).canonicalFilePath())
{
  updateDerivedData();
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
  m_loadSvgAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Svg..."), this);
  m_loadSvgAction->setStatusTip(tr("Load an existing Svg file"));
  connect(m_loadSvgAction, &QAction::triggered, this, &ZSvgDoc::loadSvg);
}

void ZSvgDoc::packInfoUpdated(SvgPack* pack)
{
  for (const auto& idPack : m_idToSvgPacks) {
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
