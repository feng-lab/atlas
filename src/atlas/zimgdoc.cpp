#include "zimgdoc.h"

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <cmath>
#include "zloadimagesequencedialog.h"
#include "zstitchimagedialog.h"
#include <set>
#include <QJsonArray>
#include "zsectionsregistrationdialog.h"

namespace nim {

ZImgDoc::ZImgDoc(ZDoc &doc)
  : ZObjDoc(doc)
{
  createActions();
}

void ZImgDoc::setImgOffset(size_t id, int offx, int offy, int offz, int offt)
{
  auto& pack = m_idToImgPacks.at(id);
  pack->setOffsetX(offx);
  pack->setOffsetY(offy);
  pack->setOffsetZ(offz);
  pack->setOffsetT(offt);
}

void ZImgDoc::setImgChannelColor(size_t id, size_t c, col4 col)
{
  auto& pack = m_idToImgPacks.at(id);
  pack->imgInfoRef().channelColors[c] = col;
}

bool ZImgDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToImgPacks.at(id);
  if (!pack->isSequence() && ZImg::fileExtensionWriteSupported(pack->paths()[0])) {
    QString err;
    if (saveImg(pack.get(), pack->paths()[0], FileFormat::Unknown, Compression::AUTO, err)) {
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

bool ZImgDoc::saveAs(size_t id)
{
  QStringList filters;
  QList<FileFormat> formats;
  QList<Compression> comps;
  ZImg::getQtWriteNameFilter(filters, formats, comps);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Image %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToImgPacks.at(id);
    int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    if (saveImg(pack.get(), dialog.selectedFiles().at(0), formats[fmtIdx], comps[fmtIdx], err)) {
      m_doc.updateObjInfo(id);
      return true;
    } else {
      QMessageBox::critical(QApplication::activeWindow(), "Save As Error", err);
    }
  }
  return false;
}

bool ZImgDoc::canReadFile(const QString &fileName)
{
  return ZImg::fileExtensionReadSupported(fileName);
}

size_t ZImgDoc::loadFile(const QString &fileName, QString &errorMsg)
{
  return loadImg(fileName, FileFormat::Unknown, errorMsg);
}

size_t ZImgDoc::loadFile(const QJsonValue &jValue, QString &errorMsg)
{
  if (!jValue.isObject()) {
    errorMsg = QString("No valid image file path");
    return 0;
  }
  QJsonObject obj = jValue.toObject();
  if (!obj.contains("Path") || !obj["Path"].isArray()) {
    errorMsg = QString("No valid image file path, no valid sequence or path key");
    return 0;
  }
  QJsonArray pathArray = obj["Path"].toArray();
  QStringList paths;
  for (int i=0; i<pathArray.size(); ++i) {
    paths.push_back(pathArray.at(i).toString());
  }
  Dimension catDim = Dimension::Z;
  size_t tileIdx = 0;
  if (obj.contains("CatDimension")) {
    if (!obj["CatDimension"].isString()) {
      errorMsg = QString("Invalid CatDimension Key");
      return 0;
    } else {
      QString txt = obj["CatDimension"].toString();
      if (txt == enumToString(Dimension::Z)) {
        catDim = Dimension::Z;
      } else if (txt == enumToString(Dimension::T)) {
        catDim = Dimension::T;
      } else {
        errorMsg = QString("Wrong CatDimension String %1").arg(txt);
        return 0;
      }
    }
  }
  if (obj.contains("TileIndex")) {
    if (!obj["TileIndex"].isDouble()) {
      errorMsg = QString("Invalid TileIndex Key");
      return 0;
    } else {
      tileIdx = obj.value("TileIndex").toInt(-1);
    }
  }
  if (paths.size() > 1) {
    return loadImg(paths, catDim, tileIdx, FileFormat::Unknown, errorMsg);
  } else {
    return loadImg(paths[0], tileIdx, FileFormat::Unknown, errorMsg);
  }
}

QList<QAction *> ZImgDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadImgAction);
  res.push_back(m_importImgZSequenceAction);
  res.push_back(m_importImgTimeSequenceAction);
  return res;
}

QMenu *ZImgDoc::processObjMenu() const
{
  QMenu* res = new QMenu(typeName());
  res->addAction(m_stitchImageAction);
  res->addAction(m_alignSectionsAction);
  return res;
}

void ZImgDoc::removeObj(size_t id)
{
  auto it = m_idToImgPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToImgPacks.erase(it);
  emit objRemoved(id, this);
}

const QString &ZImgDoc::objName(size_t id) const
{
  return m_idToImgPacks.at(id)->name();
}

QString ZImgDoc::objPath(size_t id) const
{
#if 0
  ZImgPack* pack = m_idToImgPacks.at(id);
  if (pack->isSequence()) {
    return pack->paths()[0] + QString(" ...");
  } else {
    return pack->paths()[0];
  }
#else
  return m_idToImgPacks.at(id)->paths()[0];
#endif
}

bool ZImgDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToImgPacks.at(id)->hasUnsavedChange();
}

const QString &ZImgDoc::objInfo(size_t id) const
{
  return m_idToImgPacks.at(id)->sizeInfo();
}

const QString &ZImgDoc::objTooltip(size_t id) const
{
  return m_idToImgPacks.at(id)->tooltip();
}

QJsonValue ZImgDoc::jsonValue(size_t id) const
{
  QJsonObject obj;
  auto& pack = m_idToImgPacks.at(id);
  QJsonArray pathArray;
  for (int i=0; i<pack->paths().size(); ++i) {
    pathArray.append(pack->paths()[i]);
  }
  obj.insert("Path", pathArray);
  if (pack->isSequence()) {
    obj["CatDimension"] = QJsonValue(enumToString(pack->catDim()));
  }
  if (pack->numScenes() > 1) {
    obj["TileIndex"] = QJsonValue(int(pack->sceneIdx()));
  }
  return obj;
}

bool ZImgDoc::isSameObj(const QJsonValue &v1, const QJsonValue &v2) const
{
  assert(v1.isObject() && v2.isObject());
  if (v1 == v2)
    return true;
  QJsonObject f1 = v1.toObject();
  QJsonObject f2 = v2.toObject();
  if (f1.size() != f2.size())
    return false;
  QJsonObject::const_iterator it1 = f1.begin();
  QJsonObject::const_iterator it2 = f2.begin();
  while (it1 != f1.end()) {
    if (it1.key() != it2.key())
      return false;
    if (it1.key() == "TileIndex" && it1.value() != it2.value())
      return false;
    if (it1.key() == "CatDimension" && it1.value() != it2.value())
      return false;
    if (it1.key() == "Path") {
      if (!it1.value().isArray() || !it2.value().isArray())
        return false;
      QJsonArray a1 = it1.value().toArray();
      QJsonArray a2 = it2.value().toArray();
      if (a1.size() != a2.size())
        return false;
      QJsonArray::const_iterator ait1 = a1.begin();
      QJsonArray::const_iterator ait2 = a2.begin();
      while (ait1 != a1.end()) {
        if (!(*ait1).isString() || !(*ait2).isString())
          return false;
        QString fn1 = (*ait1).toString();
        QString fn2 = (*ait2).toString();
        if (!QFile::exists(fn1) || !QFile::exists(fn2))
          return false;
        if (QFileInfo(fn1).canonicalFilePath() != QFileInfo(fn2).canonicalFilePath())
          return false;
        ++ait1;
        ++ait2;
      }
    }
    ++it1;
    ++it2;
  }
  return true;
}

size_t ZImgDoc::makeAlias(size_t id)
{
  assert(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToImgPacks[aliasId] = m_idToImgPacks[id];
  m_doc.registerNewObj(aliasId, this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZImgDoc::isAlias(size_t id) const
{
  assert(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  auto& pack = m_idToImgPacks.at(id);
  for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
    if (it->first != id && it->second == pack)
      return true;
  }
  return false;
}

void ZImgDoc::showImg(ZImg *img, const QString &path)
{
  QStringList files;
  files.push_back(QFileInfo(path).canonicalFilePath());
  for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
    if (it->second->paths() == files) {
      //it->second->setImg(*img);
      sendChangedSignal(it->first);
      return;
    }
  }

  try {
    addImgPack(new ZImgPack(*img, path));
    ZSystemInfoInstance.addFileToRecentFileList(path);
    setLastOpenedObjPath(path);
  }
  catch (const ZException & e) {
    QMessageBox::critical(QApplication::activeWindow(), tr("Can not show image"),
                          e.what());
  }
}

void ZImgDoc::loadImg()
{
  QStringList filters;
  QList<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Image File");
  if (dialog.exec()) {
    QString errorMsg;
    int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (int i=0; i<dialog.selectedFiles().size(); ++i) {
      if (!loadImg(dialog.selectedFiles().at(i), formats[fmtIdx], errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), tr("Can not read image"),
                              errorMsg);
      }
    }
  }
}

void ZImgDoc::importImgZSequence()
{
  ZLoadImageSequenceDialog dlg("Load Z Sequence Images", lastOpenedObjPath(), QApplication::activeWindow());
  if (dlg.exec() == QDialog::Accepted) {
    QStringList files = dlg.getSelectedFiles();
    if (files.empty())
      return;

    QString errorMsg;
    if (!loadImg(files, Dimension::Z, FileFormat::Unknown, errorMsg)) {
      QMessageBox::critical(QApplication::activeWindow(), tr("Can not load image sequence"),
                            errorMsg);
    }
  }
}

void ZImgDoc::importImgTimeSequence()
{
  ZLoadImageSequenceDialog dlg("Load Time Sequence Images", lastOpenedObjPath(), QApplication::activeWindow());
  if (dlg.exec() == QDialog::Accepted) {
    QStringList files = dlg.getSelectedFiles();
    if (files.empty())
      return;

    QString errorMsg;
    if (!loadImg(files, Dimension::T, FileFormat::Unknown, errorMsg)) {
      QMessageBox::critical(QApplication::activeWindow(), tr("Can not load image sequence"),
                            errorMsg);
    }
  }
}

void ZImgDoc::stitchImgs()
{
  ZStitchImageDialog stitchImageDialog(QApplication::activeWindow());
  connect(&stitchImageDialog, SIGNAL(resultReady(ZImg*,QString)), this, SLOT(showImg(ZImg*,QString)));
  stitchImageDialog.exec();
}

void ZImgDoc::alignSections()
{
  ZSectionsRegistrationDialog alignSectionsDialog(QApplication::activeWindow());
  connect(&alignSectionsDialog, SIGNAL(resultReady(ZImg*,QString)), this, SLOT(showImg(ZImg*,QString)));
  alignSectionsDialog.exec();
}

size_t ZImgDoc::addImgPack(ZImgPack *imgPack)
{
  assert(imgPack);

  size_t id = m_doc.getNewObjId();
  m_idToImgPacks[id] = std::shared_ptr<ZImgPack>(imgPack);
  m_doc.registerNewObj(id, this);

  emit objAdded(id, this);
  return id;
}

size_t ZImgDoc::loadImg(const QString &fileName, FileFormat format, QString &errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfo(fileName, &subBlocks, format);
    size_t id = 0;
    for (size_t s=0; s<infos.size(); ++s) {
      id = loadImg(fileName, s, format, errorMsg, infos.size(), &infos[s], &subBlocks[s]);
      if (!id)
        return 0;
    }
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QString &fileName, size_t scene, FileFormat format, QString &errorMsg,
                        size_t numScene, const ZImgInfo *info, const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock)
{
  try {
    ZImgSource imgSource(fileName, ZImgRegion(), scene, format);
    for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
      if (it->second->imgSource() == imgSource)
        return it->first;
    }

    size_t id = addImgPack(new ZImgPack(fileName, scene, format, numScene, info, subBlock));

    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QStringList &files, Dimension catDim, FileFormat format, QString &errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfo(files, catDim, &subBlocks, format, true);
    size_t id = 0;
    for (size_t s=0; s<infos.size(); ++s) {
      id = loadImg(files, catDim, s, format, errorMsg, infos.size(), &infos[s], &subBlocks[s]);
      if (!id)
        return 0;
    }
    return id;
  }
  catch (const ZException & e) {
    errorMsg = QString("Can not read image sequence start from %1: %2")
        .arg(files[0]).arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QStringList &files, Dimension catDim, size_t scene, FileFormat format, QString &errorMsg,
                        size_t numScene, const ZImgInfo *info, const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock)
{
  try {
    ZImgSource imgSource(files, catDim, ZImgRegion(), scene, format);
    for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
      if (it->second->imgSource() == imgSource)
        return it->first;
    }

    size_t id = addImgPack(new ZImgPack(files, catDim, scene, format, numScene, info, subBlock));

    ZSystemInfoInstance.addFileToRecentFileList(files[0]);
    setLastOpenedObjPath(files[0]);
    return id;
  }
  catch (const ZException & e) {
    errorMsg = QString("Can not read image sequence start from %1: %2")
        .arg(files[0]).arg(e.what());
    return 0;
  }
}

void ZImgDoc::sendChangedSignal(size_t id)
{
  assert(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  auto& pack = m_idToImgPacks.at(id);
  for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
    if (it->second == pack)
      emit imgChanged(id);
  }
}

void ZImgDoc::createActions()
{
  m_loadImgAction = new QAction(QIcon(":/icons/add_image-512.png"), tr("&Load Image..."), this);
  m_loadImgAction->setStatusTip(tr("Load one or more existing image files"));
  connect(m_loadImgAction, SIGNAL(triggered()), this, SLOT(loadImg()));

  m_importImgZSequenceAction = new QAction(tr("&Import Z Sequence Images..."), this);
  m_importImgZSequenceAction->setStatusTip(tr("Load sequence images as 3d image stack"));
  connect(m_importImgZSequenceAction, SIGNAL(triggered()), this, SLOT(importImgZSequence()));

  m_importImgTimeSequenceAction = new QAction(tr("&Import Time Sequence Images..."), this);
  m_importImgTimeSequenceAction->setStatusTip(tr("Load sequence images as time sequence image"));
  connect(m_importImgTimeSequenceAction, SIGNAL(triggered()), this, SLOT(importImgTimeSequence()));

  m_stitchImageAction = new QAction(tr("&Stitch Images..."), this);
  m_stitchImageAction->setStatusTip(tr("Stitch Images"));
  connect(m_stitchImageAction, SIGNAL(triggered()), this, SLOT(stitchImgs()));

  m_alignSectionsAction = new QAction(tr("&Align Sections..."), this);
  m_alignSectionsAction->setStatusTip(tr("Align Sections"));
  connect(m_alignSectionsAction, SIGNAL(triggered()), this, SLOT(alignSections()));
}

bool ZImgDoc::saveImg(ZImgPack *pack, QString fileName, FileFormat format, Compression comp, QString &errorMsg)
{
  try {
    pack->save(fileName, format, comp);

    ZSystemInfoInstance.addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException & e) {
    errorMsg = e.what();
    return false;
  }
}

void ZImgDoc::packInfoUpdated(ZImgPack *pack)
{
  for (auto it = m_idToImgPacks.cbegin(); it != m_idToImgPacks.cend(); ++it) {
    if (it->second.get() == pack)
      m_doc.updateObjInfo(it->first);
  }
}

} // namespace nim
