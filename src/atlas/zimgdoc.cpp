#include "zimgdoc.h"

#include "zloadimagesequencedialog.h"
#include "zstitchimagedialog.h"
#include "zsectionsregistrationdialog.h"
#include "zchromaticshiftcorrectiondialog.h"
#include "zlog.h"
#include "ztheme.h"
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <cmath>
#include <set>

namespace nim {

ZImgDoc::ZImgDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

void ZImgDoc::setImgChannelColor(size_t id, size_t c, col4 col)
{
  auto& pack = m_idToImgPacks.at(id);
  pack->setChannelColor(c, col);
}

bool ZImgDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id))
    return true;

  auto& pack = m_idToImgPacks.at(id);
  if (!pack->isSequence() && ZImg::fileExtensionWriteSupported(pack->paths()[0])) {
    QString err;
    if (saveImg(pack.get(), pack->paths()[0], FileFormat::Unknown, ZImgWriteParameters(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save Error.\n" + err);
    return false;
  }
  return saveAs(id);
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
    ZImgWriteParameters paras;
    paras.compression = comps[fmtIdx];
    if (saveImg(pack.get(), dialog.selectedFiles().at(0), formats[fmtIdx], paras, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), "Save As Error.\n" + err);
  }
  return false;
}

bool ZImgDoc::canReadFile(const QString& fileName) const
{
  return ZImg::fileExtensionReadSupported(fileName);
}

size_t ZImgDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  return loadImg(fileName, FileFormat::Unknown, errorMsg);
}

size_t ZImgDoc::loadFile(const QJsonValue& jValue, QString& errorMsg)
{
  return loadImg(ZImgSource(jValue), errorMsg);
}

QList<QAction*> ZImgDoc::loadFileActions() const
{
  QList<QAction*> res;
  res.push_back(m_loadImgAction);
  res.push_back(m_importImgSequenceAction);
  return res;
}

QMenu* ZImgDoc::processObjMenu() const
{
  QMenu* res = new QMenu(typeName());
  res->addAction(m_stitchImageAction);
  res->addAction(m_alignSectionsAction);
  res->addAction(m_correctChromaticShiftAction);
  return res;
}

void ZImgDoc::removeObj(size_t id)
{
  auto it = m_idToImgPacks.find(id);
  emit objAboutToBeRemoved(it->first, this);
  m_idToImgPacks.erase(it);
  emit objRemoved(id, this);
}

QString ZImgDoc::objName(size_t id) const
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

QString ZImgDoc::objInfo(size_t id) const
{
  return m_idToImgPacks.at(id)->sizeInfo();
}

QString ZImgDoc::objDetailedInfo(size_t id) const
{
  return m_idToImgPacks.at(id)->detailedInfo();
}

QString ZImgDoc::objTooltip(size_t id) const
{
  return m_idToImgPacks.at(id)->tooltip();
}

QJsonValue ZImgDoc::jsonValue(size_t id) const
{
  auto& pack = m_idToImgPacks.at(id);
  return pack->imgSource().toJson();
}

bool ZImgDoc::isSameObj(const QJsonValue& v1, const QJsonValue& v2) const
{
  CHECK(v1.isObject() && v2.isObject());
  if (v1 == v2) {
    return true;
  }
  if (ZImgSource(v1) == ZImgSource(v2)) {
    return true;
  }
  return false;
}

size_t ZImgDoc::makeAlias(size_t id)
{
  CHECK(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  size_t aliasId = m_doc.getNewObjId();
  m_idToImgPacks[aliasId] = m_idToImgPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  emit objAdded(aliasId, this);
  return aliasId;
}

bool ZImgDoc::isAlias(size_t id) const
{
  CHECK(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  auto& pack = m_idToImgPacks.at(id);
  for (const auto& idPack : m_idToImgPacks) {
    if (idPack.first != id && idPack.second == pack)
      return true;
  }
  return false;
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
    for (int i = 0; i < dialog.selectedFiles().size(); ++i) {
      if (!loadImg(dialog.selectedFiles().at(i), formats[fmtIdx], errorMsg)) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                              "Can not read image.\n" + errorMsg);
      }
    }
  }
}

void ZImgDoc::importImgSequence()
{
  ZLoadImageSequenceDialog dlg("Load Sequence Images", QApplication::activeWindow());
  if (dlg.exec() == QDialog::Accepted) {
    QStringList files = dlg.selectedFiles();
    if (files.empty())
      return;

    QString errorMsg;
    if (!loadImg(files, dlg.alongDimension(), dlg.catScences(),FileFormat::Unknown, errorMsg)) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            "Can not load image sequence.\n" + errorMsg);
    }
  }
}

void ZImgDoc::stitchImgs()
{
  ZStitchImageDialog stitchImageDialog(QApplication::activeWindow());
  connect(&stitchImageDialog, &ZStitchImageDialog::resultReady,
          &m_doc, qOverload<const QString&>(&ZDoc::loadFile));
  stitchImageDialog.exec();
}

void ZImgDoc::alignSections()
{
  ZSectionsRegistrationDialog alignSectionsDialog(QApplication::activeWindow());
  connect(&alignSectionsDialog, &ZSectionsRegistrationDialog::resultReady,
          &m_doc, qOverload<const QString&>(&ZDoc::loadFile));
  alignSectionsDialog.exec();
}

void ZImgDoc::correctChromaticShift()
{
  ZChromaticShiftCorrectionDialog chromaticShiftCorrectionDialog(QApplication::activeWindow());
  connect(&chromaticShiftCorrectionDialog, &ZChromaticShiftCorrectionDialog::resultReady,
          &m_doc, qOverload<const QString&>(&ZDoc::loadFile));
  chromaticShiftCorrectionDialog.exec();
}

size_t ZImgDoc::addImgPack(ZImgPack* imgPack)
{
  CHECK(imgPack);

  size_t id = m_doc.getNewObjId();
  m_idToImgPacks[id] = std::shared_ptr<ZImgPack>(imgPack);
  m_doc.registerNewObj(id, *this);

  emit objAdded(id, this);
  return id;
}

size_t ZImgDoc::loadImg(const QString& fileName, FileFormat format, QString& errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(fileName, &subBlocks, format);
    size_t id = 0;
    for (size_t s = 0; s < infos.size(); ++s) {
      id = loadImg(fileName, s, format, errorMsg, &infos[s], &subBlocks[s]);
      if (!id)
        return 0;
    }
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QString& fileName, size_t scene, FileFormat format, QString& errorMsg,
                        const ZImgInfo* info,
                        const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock)
{
  try {
    ZImgSource imgSource(fileName, ZImgRegion(), scene, format);
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource)
        return idPack.first;
    }

    size_t id = addImgPack(new ZImgPack(ZImgSource(fileName, ZImgRegion(), scene, format), info, subBlock));

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QStringList& files, Dimension catDim, bool catScenes, FileFormat format, QString& errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(files, catDim, catScenes, &subBlocks, format, true);
    size_t id = 0;
    for (size_t s = 0; s < infos.size(); ++s) {
      id = loadImg(files, catDim, catScenes, s, format, errorMsg, &infos[s], &subBlocks[s]);
      if (!id)
        return 0;
    }
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image sequence start from %1: %2")
      .arg(files[0]).arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QStringList& files, Dimension catDim, bool catScenes, size_t scene, FileFormat format, QString& errorMsg,
                        const ZImgInfo* info,
                        const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock)
{
  try {
    ZImgSource imgSource(files, catDim, catScenes, ZImgRegion(), scene, format);
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource)
        return idPack.first;
    }

    size_t id = addImgPack(new ZImgPack(imgSource, info, subBlock));

    ZSystemInfo::instance().addFileToRecentFileList(files[0]);
    setLastOpenedObjPath(files[0]);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image sequence start from %1: %2")
      .arg(files[0]).arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadImg(const ZImgSource& imgSource, QString& errorMsg)
{
  try {
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource)
        return idPack.first;
    }

    std::vector<std::shared_ptr<ZImgSubBlock>> subBlock;
    ZImgInfo info = ZImg::readImgInfo(imgSource, &subBlock);

    size_t id = addImgPack(new ZImgPack(imgSource, &info, &subBlock));

    ZSystemInfo::instance().addFileToRecentFileList(imgSource.filenames[0]);
    setLastOpenedObjPath(imgSource.filenames[0]);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image source start from %1: %2")
      .arg((!imgSource.filenames.empty()) ? imgSource.filenames[0] : "").arg(e.what());
    return 0;
  }
}

void ZImgDoc::sendChangedSignal(size_t id)
{
  CHECK(m_idToImgPacks.find(id) != m_idToImgPacks.end());

  auto& pack = m_idToImgPacks.at(id);
  for (const auto& idPack : m_idToImgPacks) {
    if (idPack.second == pack)
      emit imgChanged(id);
  }
}

void ZImgDoc::createActions()
{
  m_loadImgAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Image..."), this);
  m_loadImgAction->setStatusTip(tr("Load one or more existing image files"));
  connect(m_loadImgAction, &QAction::triggered, this, qOverload<>(&ZImgDoc::loadImg));

  m_importImgSequenceAction = new QAction(tr("&Import Sequence Images..."), this);
  m_importImgSequenceAction->setStatusTip(tr("Load sequence images"));
  connect(m_importImgSequenceAction, &QAction::triggered, this, &ZImgDoc::importImgSequence);

  m_stitchImageAction = new QAction(tr("&Stitch Images..."), this);
  m_stitchImageAction->setStatusTip(tr("Stitch Images"));
  connect(m_stitchImageAction, &QAction::triggered, this, &ZImgDoc::stitchImgs);

  m_alignSectionsAction = new QAction(tr("&Align Sections..."), this);
  m_alignSectionsAction->setStatusTip(tr("Align Sections"));
  connect(m_alignSectionsAction, &QAction::triggered, this, &ZImgDoc::alignSections);

  m_correctChromaticShiftAction = new QAction(tr("&Correct Chromatic Shift..."), this);
  m_correctChromaticShiftAction->setStatusTip(tr("Correct Chromatic Shift"));
  connect(m_correctChromaticShiftAction, &QAction::triggered, this, &ZImgDoc::correctChromaticShift);
}

bool ZImgDoc::saveImg(ZImgPack* pack, const QString& fileName, FileFormat format,
                      const ZImgWriteParameters& paras, QString& errorMsg)
{
  try {
    pack->save(fileName, format, paras);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZImgDoc::packInfoUpdated(ZImgPack* pack)
{
  for (const auto& idPack : m_idToImgPacks) {
    if (idPack.second.get() == pack)
      m_doc.updateObjInfo(idPack.first);
  }
}

} // namespace nim
