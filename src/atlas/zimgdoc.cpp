#include "zimgdoc.h"

#include "zloadimagesequencedialog.h"
#include "zstitchimagedialog.h"
#include "zsectionsregistrationdialog.h"
#include "zchromaticshiftcorrectiondialog.h"
#include "zloadneuroglancerprecomputeddialog.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputeddatasetlist.h"
#include "zlog.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include <QApplication>
#include <QFileDialog>
#include <QSettings>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>

namespace nim {

namespace {

bool looksLikeNeuroglancerPrecomputedUrl(const QString& s)
{
  const QString trimmed = s.trimmed();
  return trimmed.startsWith("precomputed://", Qt::CaseInsensitive) || trimmed.startsWith("gs://", Qt::CaseInsensitive) ||
         trimmed.startsWith("http://", Qt::CaseInsensitive) || trimmed.startsWith("https://", Qt::CaseInsensitive);
}

std::optional<QString> neuroglancerPrecomputedUrlFromJson(const json::value& jValue)
{
  if (!jValue.is_object()) {
    return std::nullopt;
  }
  const auto& jo = jValue.as_object();
  if (auto it = jo.find("dataSource"); it != jo.end() && it->value().is_string()) {
    const auto ds = it->value().as_string();
    if (ds == "neuroglancer_precomputed") {
      auto urlIt = jo.find("url");
      if (urlIt == jo.end() || !urlIt->value().is_string()) {
        throw ZException("Invalid neuroglancer_precomputed image JSON: missing 'url'");
      }
      return json::value_to<QString>(urlIt->value());
    }
  }

  // Backward-compat: previous attempts may have serialized as a ZImgSource with a URL in filenames[0].
  if (auto it = jo.find("filenames"); it != jo.end() && it->value().is_array()) {
    const auto files = json::value_to<QStringList>(it->value());
    if (!files.isEmpty() && looksLikeNeuroglancerPrecomputedUrl(files[0])) {
      return files[0];
    }
  }

  return std::nullopt;
}

} // namespace

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
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToImgPacks.at(id);
  if (!pack->isSequence() && ZImg::fileExtensionWriteSupported(pack->paths()[0])) {
    QString err;
    if (saveImg(pack.get(), pack->paths()[0], FileFormat::Unknown, ZImgWriteParameters(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save image %1").arg(pack->paths()[0]), err);
    return false;
  }
  return saveAs(id);
}

bool ZImgDoc::saveAs(size_t id)
{
  QStringList filters;
  std::vector<FileFormat> formats;
  std::vector<Compression> comps;
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
    auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    ZImgWriteParameters paras;
    paras.compression = comps[fmtIdx];
    if (saveImg(pack.get(), dialog.selectedFiles().at(0), formats[fmtIdx], paras, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(),
                            tr("Can not save image %1").arg(dialog.selectedFiles().at(0)),
                            err);
  }
  return false;
}

bool ZImgDoc::canReadFile(const QString& fileName) const
{
  if (looksLikeNeuroglancerPrecomputedUrl(fileName)) {
    return true;
  }
  return ZImg::fileExtensionReadSupported(fileName);
}

size_t ZImgDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  if (looksLikeNeuroglancerPrecomputedUrl(fileName)) {
    return loadNeuroglancerPrecomputed(fileName, errorMsg);
  }
  return loadImg(fileName, FileFormat::Unknown, errorMsg);
}

size_t ZImgDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (auto urlOpt = neuroglancerPrecomputedUrlFromJson(jValue)) {
      return loadNeuroglancerPrecomputed(*urlOpt, errorMsg);
    }
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
  return loadImg(json::value_to<ZImgSource>(jValue), errorMsg);
}

std::vector<QAction*> ZImgDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadImgAction);
  res.push_back(m_loadNeuroglancerPrecomputedAction);
  res.push_back(m_importImgSequenceAction);
  return res;
}

QMenu* ZImgDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_stitchImageAction);
  res->addAction(m_alignSectionsAction);
  res->addAction(m_correctChromaticShiftAction);
  return res;
}

void ZImgDoc::removeObj(size_t id)
{
  auto it = m_idToImgPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToImgPacks.erase(it);
  Q_EMIT objRemoved(id, this);
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

json::value ZImgDoc::jsonValue(size_t id) const
{
  auto& pack = m_idToImgPacks.at(id);
  if (pack->isNeuroglancerPrecomputed()) {
    json::value jv;
    auto& jo = jv.emplace_object();
    jo["dataSource"] = "neuroglancer_precomputed";
    jo["url"] = json::value_from(pack->neuroglancerRootUrl());
    return jv;
  }
  return json::value_from(pack->imgSource());
}

bool ZImgDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
  CHECK(v1.is_object() && v2.is_object());
  if (v1 == v2) {
    return true;
  }
  try {
    auto url1 = neuroglancerPrecomputedUrlFromJson(v1);
    auto url2 = neuroglancerPrecomputedUrlFromJson(v2);
    if (url1 || url2) {
      return url1 && url2 && (url1->trimmed() == url2->trimmed());
    }
  }
  catch (const ZException&) {
    return false;
  }
  if (json::value_to<ZImgSource>(v1) == json::value_to<ZImgSource>(v2)) {
    return true;
  }
  return false;
}

size_t ZImgDoc::makeAlias(size_t id)
{
  CHECK(m_idToImgPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToImgPacks[aliasId] = m_idToImgPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZImgDoc::isAlias(size_t id) const
{
  CHECK(m_idToImgPacks.contains(id));

  return std::ranges::any_of(m_idToImgPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToImgPacks.at(id);
  });
}

void ZImgDoc::loadImg()
{
  QStringList filters;
  std::vector<FileFormat> formats;
  ZImg::getQtReadNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Image File");
  if (dialog.exec()) {
    QString errorMsg;
    auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadImg(filePath, formats[fmtIdx], errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load image %1").arg(filePath), errorMsg);
      }
    }
  }
}

void ZImgDoc::loadNeuroglancerPrecomputed()
{
  QSettings settings;
  constexpr const char* kLastUrlKey = "neuroglancer_precomputed/last_url";
  const QString lastUrl = settings.value(kLastUrlKey).toString();

  ZLoadNeuroglancerPrecomputedDialog dlg(QApplication::activeWindow());
  dlg.setInitialUrl(lastUrl);
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const QString url = dlg.selectedUrl().trimmed();
  const QString displayName = dlg.selectedName().trimmed();
  if (url.isEmpty()) {
    return;
  }
  settings.setValue(kLastUrlKey, url);

  // Persist any history edits performed in the dialog (rename/remove).
  {
    QString saveErr;
    auto entries = dlg.userHistoryEntries();
    ZNeuroglancerPrecomputedDatasetList::normalizeAndDeduplicate(&entries);
    if (!ZNeuroglancerPrecomputedDatasetList::saveUserHistory(entries, &saveErr)) {
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr.toStdString();
    }
  }

  QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
  QString errorMsg;
  const size_t id = loadNeuroglancerPrecomputed(url, errorMsg);
  QApplication::restoreOverrideCursor();

  if (!id) {
    showCriticalWithDetails(QApplication::activeWindow(),
                            tr("Can not load Neuroglancer precomputed volume %1").arg(url),
                            errorMsg);
    return;
  }

  // Record successful opens into a dedicated (named) Neuroglancer history list in the user config directory.
  {
    QString loadErr;
    auto entries = ZNeuroglancerPrecomputedDatasetList::loadUserHistory(&loadErr);
    if (!loadErr.isEmpty()) {
      LOG(WARNING) << "Failed to load Neuroglancer history: " << loadErr.toStdString();
    }

    const auto& pack = m_idToImgPacks.at(id);
    CHECK(pack);
    CHECK(pack->isNeuroglancerPrecomputed());

    auto defaultNameFromUrl = [](QString u) -> QString {
      QString s = u.trimmed();
      // Strip nested schemes like "precomputed://gs://..."
      for (int i = 0; i < 2; ++i) {
        const int idx = s.indexOf("://");
        if (idx < 0) {
          break;
        }
        s = s.mid(idx + 3);
      }
      QStringList parts = s.split('/', Qt::SkipEmptyParts);
      if (parts.size() >= 2) {
        return parts[parts.size() - 2] + "/" + parts[parts.size() - 1];
      }
      if (!parts.isEmpty()) {
        return parts.front();
      }
      return u.trimmed();
    };

    ZNeuroglancerPrecomputedDatasetList::Entry e;
    e.url = pack->neuroglancerRootUrl();
    e.name = !displayName.isEmpty() ? displayName : defaultNameFromUrl(e.url);
    ZNeuroglancerPrecomputedDatasetList::upsertMostRecent(&entries, std::move(e));

    QString saveErr;
    if (!ZNeuroglancerPrecomputedDatasetList::saveUserHistory(entries, &saveErr)) {
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr.toStdString();
    }
  }
}

void ZImgDoc::importImgSequence()
{
  ZLoadImageSequenceDialog dlg("Load Sequence Images", QApplication::activeWindow());
  if (dlg.exec() == QDialog::Accepted) {
    QStringList files = dlg.selectedFiles();
    if (files.empty()) {
      return;
    }

    QString errorMsg;
    if (!loadImg(files, dlg.alongDimension(), dlg.catScences(), FileFormat::Unknown, errorMsg)) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load image sequence"), errorMsg);
    }
  }
}

void ZImgDoc::stitchImgs()
{
  ZStitchImageDialog stitchImageDialog(QApplication::activeWindow());
  connect(&stitchImageDialog, &ZStitchImageDialog::resultReady, &m_doc, qOverload<const QString&>(&ZDoc::loadFile));
  stitchImageDialog.exec();
}

void ZImgDoc::alignSections()
{
  ZSectionsRegistrationDialog alignSectionsDialog(QApplication::activeWindow());
  connect(&alignSectionsDialog,
          &ZSectionsRegistrationDialog::resultReady,
          &m_doc,
          qOverload<const QString&>(&ZDoc::loadFile));
  alignSectionsDialog.exec();
}

void ZImgDoc::correctChromaticShift()
{
  ZChromaticShiftCorrectionDialog chromaticShiftCorrectionDialog(QApplication::activeWindow());
  connect(&chromaticShiftCorrectionDialog,
          &ZChromaticShiftCorrectionDialog::resultReady,
          &m_doc,
          qOverload<const QString&>(&ZDoc::loadFile));
  chromaticShiftCorrectionDialog.exec();
}

size_t ZImgDoc::addImgPack(ZImgPack* imgPack)
{
  CHECK(imgPack);

  size_t id = m_doc.getNewObjId();
  m_idToImgPacks[id] = std::shared_ptr<ZImgPack>(imgPack);
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

size_t ZImgDoc::loadImg(const QString& fileName, FileFormat format, QString& errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(fileName, &subBlocks, format);
    size_t id = 0;
    for (size_t s = 0; s < infos.size(); ++s) {
      id = loadImg(fileName, s, format, errorMsg, infos[s], subBlocks[s]);
      if (!id) {
        return 0;
      }
    }
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QString& fileName,
                        size_t scene,
                        FileFormat format,
                        QString& errorMsg,
                        ZImgInfo& info,
                        std::vector<std::shared_ptr<ZImgSubBlock>>& sceneSubBlocks)
{
  try {
    ZImgSource imgSource(fileName, ZImgRegion(), scene, format);
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource) {
        return idPack.first;
      }
    }

    size_t id = addImgPack(new ZImgPack(imgSource, &info, &sceneSubBlocks));

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t
ZImgDoc::loadImg(const QStringList& files, Dimension catDim, bool catScenes, FileFormat format, QString& errorMsg)
{
  try {
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(files, catDim, catScenes, &subBlocks, format, true);
    size_t id = 0;
    for (size_t s = 0; s < infos.size(); ++s) {
      id = loadImg(files, catDim, catScenes, s, format, errorMsg, infos[s], subBlocks[s]);
      if (!id) {
        return 0;
      }
    }
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image sequence start from %1: %2").arg(files[0]).arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadImg(const QStringList& files,
                        Dimension catDim,
                        bool catScenes,
                        size_t scene,
                        FileFormat format,
                        QString& errorMsg,
                        ZImgInfo& info,
                        std::vector<std::shared_ptr<ZImgSubBlock>>& sceneSubBlocks)
{
  try {
    ZImgSource imgSource(files, catDim, catScenes, ZImgRegion(), scene, format);
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource) {
        return idPack.first;
      }
    }

    size_t id = addImgPack(new ZImgPack(imgSource, &info, &sceneSubBlocks));

    ZSystemInfo::instance().addFileToRecentFileList(files[0]);
    setLastOpenedObjPath(files[0]);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image sequence start from %1: %2").arg(files[0]).arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadImg(const ZImgSource& imgSource, QString& errorMsg)
{
  try {
    for (const auto& idPack : m_idToImgPacks) {
      if (idPack.second->imgSource() == imgSource) {
        return idPack.first;
      }
    }

    size_t id = addImgPack(new ZImgPack(imgSource));

    ZSystemInfo::instance().addFileToRecentFileList(imgSource.filenames[0]);
    setLastOpenedObjPath(imgSource.filenames[0]);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = QString("Can not read image source start from %1: %2")
                 .arg((!imgSource.filenames.empty()) ? imgSource.filenames[0] : "")
                 .arg(e.what());
    return 0;
  }
}

size_t ZImgDoc::loadNeuroglancerPrecomputed(const QString& url, QString& errorMsg)
{
  try {
    constexpr std::chrono::milliseconds defaultTimeout{30000};
    auto vol = ZNeuroglancerPrecomputedVolume::open(url, defaultTimeout);
    CHECK(vol);

    const QString rootUrl = vol->rootUrl();
    for (const auto& idPack : m_idToImgPacks) {
      const auto& pack = idPack.second;
      if (pack->isNeuroglancerPrecomputed() && pack->neuroglancerRootUrl() == rootUrl) {
        return idPack.first;
      }
    }

    size_t id = addImgPack(new ZImgPack(std::move(vol)));
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

void ZImgDoc::sendChangedSignal(size_t id)
{
  CHECK(m_idToImgPacks.contains(id));

  auto& pack = m_idToImgPacks.at(id);
  for (const auto& idPack : m_idToImgPacks) {
    if (idPack.second == pack) {
      Q_EMIT imgChanged(id);
    }
  }
}

void ZImgDoc::createActions()
{
  m_loadImgAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Image..."), this);
  m_loadImgAction->setStatusTip(tr("Load one or more existing image files"));
  connect(m_loadImgAction, &QAction::triggered, this, qOverload<>(&ZImgDoc::loadImg));

  m_loadNeuroglancerPrecomputedAction =
    new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("Load &Neuroglancer (Precomputed)..."), this);
  m_loadNeuroglancerPrecomputedAction->setStatusTip(tr("Load a Neuroglancer precomputed volume via URL"));
  connect(m_loadNeuroglancerPrecomputedAction,
          &QAction::triggered,
          this,
          qOverload<>(&ZImgDoc::loadNeuroglancerPrecomputed));

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

bool ZImgDoc::saveImg(ZImgPack* pack,
                      const QString& fileName,
                      FileFormat format,
                      const ZImgWriteParameters& paras,
                      QString& errorMsg)
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
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
