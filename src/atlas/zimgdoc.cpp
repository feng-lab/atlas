#include "zimgdoc.h"

#include "zloadimagesequencedialog.h"
#include "zstitchimagedialog.h"
#include "zsectionsregistrationdialog.h"
#include "zchromaticshiftcorrectiondialog.h"
#include "zloadneuroglancerprecomputeddialog.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputeddatasetlist.h"
#include "zneuroglancerstate.h"
#include "zlog.h"
#include "zproxygenhttpclient.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include "zautotracedialog.h"
#include "zbackgroundtaskmanager.h"
#include "zsysteminfo.h"
#include "ztracesettings.h"
#include "zswcdoc.h"
#include "zswcwriter.h"

#include "zneutubetraceauto.h"
#include "zneutubetraceconfig.h"
#include "zcancellation.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QUuid>
#include <boost/json.hpp>
#include <folly/CancellationToken.h>
#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Invoke.h>
#include <folly/executors/GlobalExecutor.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

namespace nim {

namespace {

struct AutoTraceAsyncResult
{
  QString error;
  bool cancelled = false;
  bool hasResult = false;
};

[[nodiscard]] AutoTraceAsyncResult runAutoTraceLegacyLike(const std::shared_ptr<ZImgPack>& imgPack,
                                                          size_t sc,
                                                          size_t t,
                                                          int traceLevel,
                                                          const QString& traceCfgPath,
                                                          bool haveAlgoConfig,
                                                          const ZTraceSettings::AlgoConfig& algoCfg,
                                                          bool doResample,
                                                          bool docHasAnySwc,
                                                          const QString& outputSwcPath,
                                                          const QString& outputLogPath,
                                                          folly::CancellationToken cancellationToken)
{
  AutoTraceAsyncResult res;

  try {
    maybeCancel(cancellationToken);

    {
      const QDir dir = QFileInfo(outputLogPath).dir();
      if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        res.error = QStringLiteral("Auto Trace failed: can not create log directory: %1").arg(dir.absolutePath());
        return res;
      }
    }

    auto fileSink = createFileLogSink(outputLogPath);
    if (!fileSink) {
      res.error = QStringLiteral("Auto Trace failed: can not open log file for writing: %1").arg(outputLogPath);
      return res;
    }

    addLogSink(fileSink);
    auto sinkGuard = folly::makeGuard([&fileSink]() {
      removeLogSink(fileSink);
    });

    LOG(INFO) << "Atlas Auto Trace";
    LOG(INFO) << "Output SWC: " << outputSwcPath;
    LOG(INFO) << "Trace config path: " << traceCfgPath;
    LOG(INFO) << "Output log file: " << outputLogPath;
    LOG(INFO) << "Selected channel (0-based): " << static_cast<qulonglong>(sc);
    LOG(INFO) << "Selected time (0-based): " << static_cast<qulonglong>(t);
    LOG(INFO) << "Budget level override (0=default): " << traceLevel;
    LOG(INFO) << "Optimal node resampling: " << (doResample ? "enabled" : "disabled");

    const ZImgInfo info = imgPack->imgInfo();
    if (sc >= info.numChannels || t >= info.numTimes) {
      res.error = QStringLiteral("Auto Trace failed: invalid channel/time selection (c=%1, t=%2).")
                    .arg(static_cast<qulonglong>(sc))
                    .arg(static_cast<qulonglong>(t));
      return res;
    }

    LOG(INFO) << "Signal info: " << info;

    const ZImg* fullSignalPtr = nullptr;
    ZImg ownedSignal;
    if (imgPack->isDiskCached()) {
      LOG(INFO) << "Loading whole image (disk cached)...";
      ownedSignal = imgPack->wholeImg();
      fullSignalPtr = &ownedSignal;
    } else {
      LOG(INFO) << "Using in-memory image...";
      fullSignalPtr = &imgPack->img();
    }
    CHECK(fullSignalPtr != nullptr);
    const ZImg& fullSignal = *fullSignalPtr;

    if (fullSignal.isEmpty()) {
      res.error = QStringLiteral("Auto Trace failed: the image is empty.");
      return res;
    }

    maybeCancel(cancellationToken);
    LOG(INFO) << "Extracting selected channel/time...";
    ZImg signal = fullSignal.extractChannel(sc, static_cast<index_t>(t));

    TraceConfig cfg;
    if (!traceCfgPath.isEmpty()) {
      const bool ok = loadTraceConfigLegacyLike(traceCfgPath.toStdString(), cfg);
      if (!ok) {
        cfg = TraceConfig{};
      }
    } else {
      cfg = TraceConfig{};
    }

    if (traceLevel > 0) {
      if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(cfg, traceLevel)) {
        applyTraceConfigOverridesLegacyLike(*levelOverride, cfg);
      }
    }

    if (haveAlgoConfig) {
      cfg.minAutoScore = algoCfg.minAutoScore;
      cfg.minManualScore = algoCfg.minManualScore;
      cfg.minSeedScore = algoCfg.minSeedScore;
      cfg.min2dScore = algoCfg.min2dScore;
      cfg.refit = algoCfg.refit;
      cfg.spTest = algoCfg.spTest;
      cfg.crossoverTest = algoCfg.crossoverTest;
      cfg.tuneEnd = algoCfg.tuneEnd;
      cfg.edgePath = algoCfg.edgePath;
      cfg.enhanceMask = algoCfg.enhanceMask;
      cfg.seedMethod = algoCfg.seedMethod;
      cfg.recover = algoCfg.recover;
      cfg.chainScreenCount = algoCfg.chainScreenCount;
      cfg.maxEucDist = algoCfg.maxEucDist;
    }

    if (docHasAnySwc) {
      cfg.recover = 0;
    }

    LOG(INFO) << "Final TraceConfig:";
    LOG(INFO) << "  minAutoScore=" << cfg.minAutoScore;
    LOG(INFO) << "  minManualScore=" << cfg.minManualScore;
    LOG(INFO) << "  minSeedScore=" << cfg.minSeedScore;
    LOG(INFO) << "  min2dScore=" << cfg.min2dScore;
    LOG(INFO) << "  refit=" << (cfg.refit ? "true" : "false");
    LOG(INFO) << "  spTest=" << (cfg.spTest ? "true" : "false");
    LOG(INFO) << "  crossoverTest=" << (cfg.crossoverTest ? "true" : "false");
    LOG(INFO) << "  tuneEnd=" << (cfg.tuneEnd ? "true" : "false");
    LOG(INFO) << "  edgePath=" << (cfg.edgePath ? "true" : "false");
    LOG(INFO) << "  enhanceMask=" << (cfg.enhanceMask ? "true" : "false");
    LOG(INFO) << "  seedMethod=" << cfg.seedMethod;
    LOG(INFO) << "  recover=" << cfg.recover;
    LOG(INFO) << "  chainScreenCount=" << cfg.chainScreenCount;
    LOG(INFO) << "  maxEucDist=" << cfg.maxEucDist;

    maybeCancel(cancellationToken);
    LOG(INFO) << "Tracing...";
    std::unique_ptr<ZSwc> swc = traceNeuronAutoLegacyLike(std::move(signal),
                                                          cfg,
                                                          /*diagnosis=*/false,
                                                          /*verbose=*/false,
                                                          /*doResampleAfterTracing=*/doResample,
                                                          /*predefinedMask=*/nullptr,
                                                          cancellationToken);
    maybeCancel(cancellationToken);

    if (!swc) {
      LOG(INFO) << "No SWC generated.";
      res.hasResult = false;
      return res;
    }

    {
      const QDir dir = QFileInfo(outputSwcPath).dir();
      if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        res.error = QStringLiteral("Auto Trace failed: can not create output directory: %1").arg(dir.absolutePath());
        return res;
      }
    }

    const QString tmpSwcPath =
      outputSwcPath + QStringLiteral(".tmp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    LOG(INFO) << "Writing SWC...";
    writeSwcLegacyNeuTuOrThrow(*swc, tmpSwcPath.toStdString(), {});
    if (QFile::exists(outputSwcPath) && !QFile::remove(outputSwcPath)) {
      res.error = QStringLiteral("Auto Trace failed: can not overwrite output SWC: %1").arg(outputSwcPath);
      (void)QFile::remove(tmpSwcPath);
      return res;
    }
    if (!QFile::rename(tmpSwcPath, outputSwcPath)) {
      res.error = QStringLiteral("Auto Trace failed: can not move temp SWC into place.\nTemp: %1\nFinal: %2")
                    .arg(tmpSwcPath, outputSwcPath);
      (void)QFile::remove(tmpSwcPath);
      return res;
    }

    res.hasResult = true;
    LOG(INFO) << "Finished.";
  }
  catch (const ZCancellationException&) {
    res.cancelled = true;
  }
  catch (const std::exception& e) {
    res.error = QString("Auto Trace failed: %1").arg(QString::fromUtf8(e.what()));
  }

  return res;
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
    if (!files.isEmpty() && ZImgDoc::looksLikeNeuroglancerPrecomputedUrl(files[0])) {
      return files[0];
    }
  }

  return std::nullopt;
}

} // namespace

bool ZImgDoc::looksLikeNeuroglancerPrecomputedUrl(const QString& s)
{
  const QString trimmed = s.trimmed();
  return trimmed.startsWith("precomputed://", Qt::CaseInsensitive) ||
         trimmed.startsWith("gs://", Qt::CaseInsensitive) || trimmed.startsWith("s3://", Qt::CaseInsensitive) ||
         trimmed.startsWith("http://", Qt::CaseInsensitive) || trimmed.startsWith("https://", Qt::CaseInsensitive);
}

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
      const size_t id = loadNeuroglancerPrecomputed(*urlOpt, errorMsg);
      if (id == 0) {
        return 0;
      }

      // Restore per-dataset Neuroglancer mesh/skeleton source overrides (if present).
      // These are optional and may be absent for most datasets.
      if (jValue.is_object()) {
        ZImgPack& pack = imgPack(id);
        const auto& jo = jValue.as_object();

        auto applyOverride = [&](const char* key,
                                 auto setter,
                                 auto clearFn) {
          auto it = jo.find(key);
          if (it == jo.end()) {
            return;
          }
          if (!it->value().is_string()) {
            LOG(WARNING) << "Ignoring invalid neuroglancer_precomputed JSON field '" << key
                         << "' (expected string).";
            return;
          }
          const QString text = json::value_to<QString>(it->value()).trimmed();
          if (text.isEmpty()) {
            clearFn();
            return;
          }
          QString err;
          if (!setter(text, &err)) {
            LOG(WARNING) << "Failed to apply neuroglancer_precomputed override '" << key
                         << "': " << err.toStdString();
          }
        };

        applyOverride(
          "mesh_source_override_url",
          [&](const QString& s, QString* err) { return pack.setNeuroglancerMeshSourceOverride(s, err); },
          [&]() { pack.clearNeuroglancerMeshSourceOverride(); });
        applyOverride(
          "skeleton_source_override_url",
          [&](const QString& s, QString* err) { return pack.setNeuroglancerSkeletonSourceOverride(s, err); },
          [&]() { pack.clearNeuroglancerSkeletonSourceOverride(); });
        applyOverride(
          "annotations_source_override_url",
          [&](const QString& s, QString* err) { return pack.setNeuroglancerAnnotationsSourceOverride(s, err); },
          [&]() { pack.clearNeuroglancerAnnotationsSourceOverride(); });
      }

      return id;
    }
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
  return loadImg(json::value_to<ZImgSource>(jValue), errorMsg);
}

bool ZImgDoc::canPrepareLoadAsync(const json::value& jValue) const
{
  if (!jValue.is_object()) {
    return false;
  }

  // Keep Neuroglancer precomputed sources synchronous for now. They can involve
  // network IO and shared caches, and are not part of the common local-file
  // startup path for animation exports.
  try {
    if (neuroglancerPrecomputedUrlFromJson(jValue).has_value()) {
      return false;
    }
  }
  catch (const ZException&) {
    // Invalid JSON; keep it synchronous so loadFile() can produce a consistent error.
    return false;
  }

  return true;
}

folly::coro::Task<ZObjDoc::PreparedLoadResult> ZImgDoc::prepareLoadAsync(const json::value& jValue,
                                                                         const ZObjDoc::AsyncLoadContext& ctx) const
{
  PreparedLoadResult out;
  if (!jValue.is_object()) {
    out.errorMsg = QString("Invalid image JSON: expected object");
    co_return out;
  }
  if (!ctx.commitThread) {
    out.errorMsg = QString("Internal error: missing commit thread for async image load");
    co_return out;
  }

  ZImgSource imgSource;
  try {
    imgSource = json::value_to<ZImgSource>(jValue);
  }
  catch (const ZException& e) {
    out.errorMsg = QString::fromUtf8(e.what());
    co_return out;
  }
  catch (const std::exception& e) {
    out.errorMsg = QString::fromUtf8(e.what());
    co_return out;
  }

  if (imgSource.filenames.isEmpty()) {
    out.errorMsg = QString("Invalid image JSON: filenames list is empty");
    co_return out;
  }

  const QString recentPath = imgSource.filenames[0];

  try {
    auto pack = std::make_unique<ZImgPack>(std::move(imgSource));

    // ZImgPack is a QObject; move thread affinity to the doc thread before returning.
    // moveToThread() must be called from the object's current thread (this prepare task).
    pack->moveToThread(ctx.commitThread);

    ZImgDoc* self = const_cast<ZImgDoc*>(this);
    out.commit = [self, this, recentPath, pack = std::move(pack)](QString& errorMsg) mutable -> size_t {
      try {
        CHECK(pack);
        const size_t id = self->addImgPack(pack.release());
        ZSystemInfo::instance().addFileToRecentFileList(recentPath);
        setLastOpenedObjPath(recentPath);
        return id;
      }
      catch (const ZException& e) {
        errorMsg = QString("Can not read image source start from %1: %2").arg(recentPath).arg(e.what());
        return 0;
      }
      catch (const std::exception& e) {
        errorMsg = QString("Can not read image source start from %1: %2").arg(recentPath).arg(e.what());
        return 0;
      }
    };
  }
  catch (const ZException& e) {
    out.errorMsg = QString("Can not read image source start from %1: %2").arg(recentPath).arg(e.what());
  }
  catch (const std::exception& e) {
    out.errorMsg = QString("Can not read image source start from %1: %2").arg(recentPath).arg(e.what());
  }
  co_return out;
}

std::vector<QAction*> ZImgDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadImgAction);
  res.push_back(m_loadNeuroglancerPrecomputedAction);
  res.push_back(m_loadNeuroglancerStateAction);
  res.push_back(m_importImgSequenceAction);
  return res;
}

QMenu* ZImgDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_autoTraceAction);
  res->addSeparator();
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
    if (pack->hasNeuroglancerMeshSourceOverride()) {
      jo["mesh_source_override_url"] = json::value_from(pack->neuroglancerMeshSourceOverrideUrl());
    }
    if (pack->hasNeuroglancerSkeletonSourceOverride()) {
      jo["skeleton_source_override_url"] = json::value_from(pack->neuroglancerSkeletonSourceOverrideUrl());
    }
    if (pack->hasNeuroglancerAnnotationsSourceOverride()) {
      jo["annotations_source_override_url"] = json::value_from(pack->neuroglancerAnnotationsSourceOverrideUrl());
    }
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

    // Apply any per-dataset mesh/skeleton source overrides stored in history. This makes the "history"
    // entry act as the persistent dataset configuration for mesh/skeleton loading.
    const QString rootUrl = pack->neuroglancerRootUrl();
    const QString normalizedRoot = ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(rootUrl);

    auto historyIt =
      std::find_if(entries.begin(), entries.end(), [&](const ZNeuroglancerPrecomputedDatasetList::Entry& e) {
        return ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(e.url) == normalizedRoot;
      });
    if (historyIt != entries.end()) {
      auto applyOverride = [&](const QString& overrideText,
                               auto setter,
                               const char* kind) {
        const QString text = overrideText.trimmed();
        if (text.isEmpty()) {
          return;
        }
        QString err;
        if (!setter(text, &err)) {
          LOG(WARNING) << "Failed to apply Neuroglancer " << kind << " source override from history: "
                       << err.toStdString();
        }
      };

      applyOverride(historyIt->meshSourceOverrideUrl,
                    [&](const QString& s, QString* err) { return pack->setNeuroglancerMeshSourceOverride(s, err); },
                    "mesh");
      applyOverride(historyIt->skeletonSourceOverrideUrl,
                    [&](const QString& s, QString* err) { return pack->setNeuroglancerSkeletonSourceOverride(s, err); },
                    "skeleton");
      applyOverride(historyIt->annotationsSourceOverrideUrl,
                    [&](const QString& s, QString* err) { return pack->setNeuroglancerAnnotationsSourceOverride(s, err); },
                    "annotations");
    }

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
      if (historyIt != entries.end()) {
        // Preserve per-dataset configuration when recording a new "most recent" entry.
        e.kind = historyIt->kind;
        e.meshSourceOverrideUrl = historyIt->meshSourceOverrideUrl;
        e.skeletonSourceOverrideUrl = historyIt->skeletonSourceOverrideUrl;
        e.annotationsSourceOverrideUrl = historyIt->annotationsSourceOverrideUrl;
      }
    ZNeuroglancerPrecomputedDatasetList::upsertMostRecent(&entries, std::move(e));

    QString saveErr;
    if (!ZNeuroglancerPrecomputedDatasetList::saveUserHistory(entries, &saveErr)) {
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr.toStdString();
    }
  }
}

void ZImgDoc::loadNeuroglancerState()
{
  QString prefill;
  const QString clip = QApplication::clipboard()->text().trimmed();
  if (!clip.isEmpty()) {
    prefill = clip;
  }

  const QString userText =
    QInputDialog::getText(QApplication::activeWindow(),
                          QApplication::applicationName(),
                          QStringLiteral("Neuroglancer state URL or JSON:\n"
                                         "- Paste a Neuroglancer share link (contains '#!{...}'), OR\n"
                                         "- Paste a URL to a .json state file, OR\n"
                                         "- Paste raw state JSON text."),
                          QLineEdit::Normal,
                          prefill)
      .trimmed();
  if (userText.isEmpty()) {
    return;
  }

  constexpr std::chrono::milliseconds kDefaultTimeout{30000};

  auto mapCloudUrlToHttps = [](QString u) -> QString {
    QString s = u.trimmed();
    if (s.startsWith("gs://", Qt::CaseInsensitive)) {
      const QString rest = s.mid(QStringLiteral("gs://").size());
      return QStringLiteral("https://storage.googleapis.com/") + rest;
    }
    if (s.startsWith("s3://", Qt::CaseInsensitive)) {
      const QString rest = s.mid(QStringLiteral("s3://").size());
      const int slash = rest.indexOf('/');
      const QString bucket = (slash < 0) ? rest : rest.left(slash);
      const QString key = (slash < 0) ? QString{} : rest.mid(slash + 1);
      if (bucket.isEmpty()) {
        return s;
      }

      // Prefer virtual-hosted-style URLs for compatibility with newer AWS regions, but fall back to
      // path-style when the bucket name contains dots (TLS wildcard mismatch with e.g. "a.b.s3.amazonaws.com").
      const bool bucketHasDot = bucket.contains('.');
      if (bucketHasDot) {
        QString out = QStringLiteral("https://s3.amazonaws.com/") + bucket;
        if (!key.isEmpty()) {
          out += '/';
          out += key;
        }
        return out;
      }
      QString out = QStringLiteral("https://") + bucket + QStringLiteral(".s3.amazonaws.com");
      if (!key.isEmpty()) {
        out += '/';
        out += key;
      }
      return out;
    }
    return s;
  };

  auto tryExtractJsonFromUrlFragment = [](QString u) -> std::optional<QString> {
    const QUrl url(u.trimmed());
    if (!url.isValid()) {
      return std::nullopt;
    }
    QString frag = url.fragment(QUrl::FullyDecoded).trimmed();
    if (frag.isEmpty()) {
      return std::nullopt;
    }
    if (frag.startsWith('!')) {
      frag = frag.mid(1).trimmed();
    }
    if (!frag.startsWith('{') && !frag.startsWith('[') && frag.contains('%')) {
      frag = QString::fromUtf8(QByteArray::fromPercentEncoding(frag.toUtf8())).trimmed();
    }
    if (frag.startsWith('{') || frag.startsWith('[')) {
      return frag;
    }
    return std::nullopt;
  };

  // Load/parse state JSON.
  boost::json::value stateJson;
  try {
    if (userText.startsWith('{') || userText.startsWith('[')) {
      stateJson = boost::json::parse(userText.toStdString());
    } else if (const auto fragOpt = tryExtractJsonFromUrlFragment(userText)) {
      stateJson = boost::json::parse(fragOpt->toStdString());
    } else if (userText.contains("://") || userText.startsWith("gs://", Qt::CaseInsensitive)) {
      const QString urlStr = mapCloudUrlToHttps(userText);
      auto resOpt = folly::coro::blockingWait(
        ZProxygenHttpClient::instance().getBytes(urlStr.toStdString(), kDefaultTimeout));
      if (!resOpt) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("Failed to fetch Neuroglancer state JSON (HTTP 404 or network failure):\n%1")
                                   .arg(urlStr));
        return;
      }
      if (resOpt->status != 200) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("Failed to fetch Neuroglancer state JSON:\n%1\n\nHTTP status: %2")
                                   .arg(urlStr)
                                   .arg(resOpt->status));
        return;
      }
      const std::string text(reinterpret_cast<const char*>(resOpt->body.data()), resOpt->body.size());
      stateJson = boost::json::parse(text);
    } else {
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               QStringLiteral("Unrecognized input. Paste a Neuroglancer share link, a JSON URL, or raw JSON."));
      return;
    }
  }
  catch (const std::exception& e) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("Failed to parse Neuroglancer state JSON:\n%1").arg(QString::fromUtf8(e.what())));
    return;
  }

  const ZNeuroglancerState::ParseResult parsed = ZNeuroglancerState::parse(stateJson);
  if (parsed.layers.empty()) {
    QString msg = QStringLiteral("No supported precomputed layers were found in this Neuroglancer state.");
    if (!parsed.warnings.isEmpty()) {
      msg += QStringLiteral("\n\nDetails:\n") + parsed.warnings.join("\n");
    }
    QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), msg);
    return;
  }

  QString historyLoadErr;
  auto historyEntries = ZNeuroglancerPrecomputedDatasetList::loadUserHistory(&historyLoadErr);
  if (!historyLoadErr.isEmpty()) {
    LOG(WARNING) << "Failed to load Neuroglancer history: " << historyLoadErr.toStdString();
  }

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

  auto applyOverridesFromHistoryIfPresent = [&](ZImgPack& pack) {
    if (!pack.isNeuroglancerPrecomputed() || !pack.neuroglancerVolumeShared()->isSegmentation()) {
      return;
    }
    const QString rootUrl = pack.neuroglancerRootUrl();
    const QString normalizedRoot = ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(rootUrl);
    if (normalizedRoot.isEmpty()) {
      return;
    }

    auto it = std::find_if(historyEntries.begin(),
                           historyEntries.end(),
                           [&](const ZNeuroglancerPrecomputedDatasetList::Entry& e) {
                             return ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(e.url) == normalizedRoot;
                           });
    if (it == historyEntries.end()) {
      return;
    }

    auto apply = [&](const QString& overrideText,
                     auto setter,
                     const char* kind) {
      const QString text = overrideText.trimmed();
      if (text.isEmpty()) {
        return;
      }
      QString err;
      if (!setter(text, &err)) {
        LOG(WARNING) << "Failed to apply Neuroglancer " << kind << " source override from history: "
                     << err.toStdString();
      }
    };

    apply(it->meshSourceOverrideUrl,
          [&](const QString& s, QString* err) { return pack.setNeuroglancerMeshSourceOverride(s, err); },
          "mesh");
    apply(it->skeletonSourceOverrideUrl,
          [&](const QString& s, QString* err) { return pack.setNeuroglancerSkeletonSourceOverride(s, err); },
          "skeleton");
    apply(it->annotationsSourceOverrideUrl,
          [&](const QString& s, QString* err) { return pack.setNeuroglancerAnnotationsSourceOverride(s, err); },
          "annotations");
  };

  // We want to update history exactly once at the end, and avoid duplicating entries if multiple layers
  // refer to the same dataset.
  std::set<QString> touchedNormalizedRoots;
  std::map<QString, QString> normalizedRootToNameHint;

  auto upsertHistoryForPack = [&](const ZImgPack& pack, QString displayNameHint, QString kindHint) {
    const QString rootUrl = pack.neuroglancerRootUrl();
    const QString normalizedRoot = ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(rootUrl);
    if (normalizedRoot.isEmpty()) {
      return;
    }

    // Only use the first non-empty hint per dataset (so the history name is stable).
    if (!displayNameHint.trimmed().isEmpty() && !normalizedRootToNameHint.contains(normalizedRoot)) {
      normalizedRootToNameHint[normalizedRoot] = displayNameHint.trimmed();
    }

    ZNeuroglancerPrecomputedDatasetList::Entry entry;
    auto it = std::find_if(historyEntries.begin(),
                           historyEntries.end(),
                           [&](const ZNeuroglancerPrecomputedDatasetList::Entry& e) {
                             return ZNeuroglancerPrecomputedDatasetList::normalizedUrlForMatch(e.url) == normalizedRoot;
                           });
    if (it != historyEntries.end()) {
      entry = *it;
    }

    entry.url = rootUrl;
    const QString nameHintFinal = normalizedRootToNameHint.contains(normalizedRoot) ? normalizedRootToNameHint[normalizedRoot] : QString{};
    if (!nameHintFinal.isEmpty()) {
      entry.name = nameHintFinal;
    } else if (entry.name.trimmed().isEmpty()) {
      entry.name = defaultNameFromUrl(rootUrl);
    }
    if (!kindHint.trimmed().isEmpty()) {
      entry.kind = kindHint.trimmed();
    }

    entry.meshSourceOverrideUrl = pack.neuroglancerMeshSourceOverrideUrl();
    entry.skeletonSourceOverrideUrl = pack.neuroglancerSkeletonSourceOverrideUrl();
    entry.annotationsSourceOverrideUrl = pack.neuroglancerAnnotationsSourceOverrideUrl();

    ZNeuroglancerPrecomputedDatasetList::upsertMostRecent(&historyEntries, std::move(entry));
    touchedNormalizedRoots.insert(normalizedRoot);
  };

  // Open supported precomputed volumes from the state.
  // Note: this is a blocking operation; we keep UI feedback consistent with existing "Load Neuroglancer" behavior.
  QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

  QStringList warnings = parsed.warnings;
  std::map<QString, size_t> segmentationLayerNameToObjId;

  for (const auto& layer : parsed.layers) {
    QString err;
    const size_t id = loadNeuroglancerPrecomputed(layer.volumeUrl, err);
    if (!id) {
      warnings << QStringLiteral("Failed to open precomputed dataset '%1': %2").arg(layer.volumeUrl).arg(err);
      continue;
    }

    // Apply doc-level visibility (maps to the standard object visible toggle).
    m_doc.setObjVisible(id, layer.visible);

    ZImgPack& pack = imgPack(id);

    // Apply any persisted per-dataset overrides before applying state overrides.
    applyOverridesFromHistoryIfPresent(pack);

    if (layer.type == ZNeuroglancerState::LayerType::Segmentation) {
      if (!layer.meshSourceOverrideUrl.trimmed().isEmpty()) {
        QString setErr;
        (void)pack.setNeuroglancerMeshSourceOverride(layer.meshSourceOverrideUrl, &setErr);
      }
      if (!layer.skeletonSourceOverrideUrl.trimmed().isEmpty()) {
        QString setErr;
        (void)pack.setNeuroglancerSkeletonSourceOverride(layer.skeletonSourceOverrideUrl, &setErr);
      }
      if (!layer.name.trimmed().isEmpty()) {
        segmentationLayerNameToObjId[layer.name.trimmed()] = id;
      }
    }

    QString kindHint = QStringLiteral("image");
    if (pack.isNeuroglancerPrecomputed() && pack.neuroglancerVolumeShared()->isSegmentation()) {
      kindHint = QStringLiteral("segmentation");
    }
    upsertHistoryForPack(pack, layer.name, kindHint);
  }

  // Apply annotations bindings to the linked segmentation datasets (configuration only; no objects created).
  for (const auto& b : parsed.annotationsBindings) {
    for (const QString& segLayerName : b.linkedSegmentationLayerNames) {
      auto it = segmentationLayerNameToObjId.find(segLayerName.trimmed());
      if (it == segmentationLayerNameToObjId.end()) {
        continue;
      }
      const size_t objId = it->second;
      ZImgPack& pack = imgPack(objId);
      QString setErr;
      if (!pack.setNeuroglancerAnnotationsSourceOverride(b.annotationsRootUrl, &setErr)) {
        warnings << QStringLiteral("Failed to apply annotations source override '%1' to dataset '%2': %3")
                      .arg(b.annotationsRootUrl)
                      .arg(pack.neuroglancerRootUrl())
                      .arg(setErr);
      }
      upsertHistoryForPack(pack,
                           /*displayNameHint=*/segLayerName,
                           /*kindHint=*/QStringLiteral("segmentation"));
    }
  }

  QApplication::restoreOverrideCursor();

  // Persist updated history (including any overrides we applied from state).
  {
    QString saveErr;
    ZNeuroglancerPrecomputedDatasetList::normalizeAndDeduplicate(&historyEntries);
    if (!ZNeuroglancerPrecomputedDatasetList::saveUserHistory(historyEntries, &saveErr)) {
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr.toStdString();
    }
  }

  if (!warnings.isEmpty()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("Neuroglancer state import completed with warnings:\n\n%1")
                               .arg(warnings.join("\n")));
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

void ZImgDoc::autoTrace()
{
  if (objs().empty()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("No images are loaded."));
    return;
  }

  ZAutoTraceDialog dlg(m_doc, QApplication::activeWindow());
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const std::optional<size_t> imgIdOpt = dlg.selectedImageId();
  if (!imgIdOpt.has_value()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("Please select an image to trace."));
    return;
  }

  const size_t imgId = *imgIdOpt;
  if (!m_doc.imgDoc().hasObjWithID(imgId)) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("The selected image no longer exists."));
    return;
  }

  const std::shared_ptr<ZImgPack> imgPack = m_doc.imgDoc().imgPackShared(imgId);
  CHECK(imgPack);

  const size_t sc = dlg.selectedChannel();
  const size_t t = dlg.selectedTime();
  const int traceLevel = dlg.traceLevel();
  const bool doResample = dlg.optimalResamplingEnabled();
  const QString outputSwcPath = dlg.outputSwcPath();
  const QString outputLogPath = dlg.outputLogPath();
  const bool loadResult = dlg.loadResultEnabled();

  if (outputSwcPath.isEmpty()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("Please select an output SWC file."));
    return;
  }
  if (outputLogPath.isEmpty()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("Please select an output log file."));
    return;
  }

  // Keep selection explicit and shared across 2D/3D trace UIs: the dialog is just another view
  // of the shared trace settings state.
  m_doc.traceSettings().setSourceSelection(imgId, sc);

  const bool haveAlgoConfig = m_doc.traceSettings().algoConfigInitialized();
  const ZTraceSettings::AlgoConfig algoCfg = m_doc.traceSettings().algoConfig();

  const bool docHasAnySwc = !m_doc.swcDoc().objs().empty();

  const ZImgInfo info = imgPack->imgInfo();
  const QString channelLabel = (sc < info.channelNames.size())
                                 ? info.displayChannelName(sc)
                                 : QStringLiteral("Ch%1").arg(static_cast<qulonglong>(sc + 1));
  const QString timeLabel = QStringLiteral("T%1").arg(static_cast<qulonglong>(t + 1));
  const QString outputSwcLabel = QFileInfo(outputSwcPath).fileName();

  ZBackgroundTaskManager& tm = m_doc.backgroundTaskManager();
  auto cancellationSource = std::make_shared<folly::CancellationSource>();
  const folly::CancellationToken cancellationToken = cancellationSource->getToken();
  ZBackgroundTaskManager::TaskOptions taskOptions;
  taskOptions.useFakeProgress = true;
  taskOptions.cancelCallback = [cancellationSource]() {
    cancellationSource->requestCancellation();
  };

  const QString taskTitle = QStringLiteral("Auto Trace: %1, %2, %3 -> %4")
                              .arg(m_doc.objNameWithModifiedMarkerAndID(imgId))
                              .arg(channelLabel)
                              .arg(timeLabel)
                              .arg(outputSwcLabel.isEmpty() ? outputSwcPath : outputSwcLabel);

  auto* task = tm.createTask(taskTitle, std::move(taskOptions));
  tm.startTask(task, QStringLiteral("running"));
  m_doc.showBackgroundTasksPanel();

  const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");

  QPointer<ZDoc> docPtr(&m_doc);
  QPointer<ZBackgroundTask> taskPtr(task);

  tm.spawnDetachedTask(
    folly::getGlobalCPUExecutor(),
    folly::coro::co_invoke([docPtr,
                            taskPtr,
                            imgPack,
                            imgId,
                            sc,
                            t,
                            traceLevel,
                            traceCfgPath,
                            haveAlgoConfig,
                            algoCfg,
                            doResample,
                            docHasAnySwc,
                            outputSwcPath,
                            outputLogPath,
                            loadResult,
                            cancellationToken]() mutable -> folly::coro::Task<void> {
      const AutoTraceAsyncResult res = runAutoTraceLegacyLike(imgPack,
                                                              sc,
                                                              t,
                                                              traceLevel,
                                                              traceCfgPath,
                                                              haveAlgoConfig,
                                                              algoCfg,
                                                              doResample,
                                                              docHasAnySwc,
                                                              outputSwcPath,
                                                              outputLogPath,
                                                              cancellationToken);

      if (docPtr != nullptr && taskPtr != nullptr) {
        QMetaObject::invokeMethod(
          docPtr,
          [docPtr, taskPtr, imgId, sc, outputSwcPath, outputLogPath, loadResult, res]() mutable {
            if (docPtr == nullptr || taskPtr == nullptr) {
              return;
            }

            ZBackgroundTaskManager& tm = docPtr->backgroundTaskManager();
            if (res.cancelled) {
              tm.cancelTask(taskPtr, QStringLiteral("cancelled"));
              return;
            }
            if (!res.error.isEmpty()) {
              tm.failTask(taskPtr, res.error);
              return;
            }
            if (!res.hasResult) {
              tm.succeedTask(taskPtr, QStringLiteral("no trace result"));
              return;
            }
            if (!loadResult) {
              tm.succeedTask(taskPtr, QStringLiteral("wrote %1").arg(QFileInfo(outputSwcPath).fileName()));
              return;
            }

            ZSwcDoc& swcDoc = docPtr->swcDoc();
            QString loadError;
            const size_t newSwcId = swcDoc.loadFile(outputSwcPath, loadError);
            if (newSwcId == 0) {
              tm.failTask(taskPtr,
                          QStringLiteral("Auto Trace finished but failed to load output SWC.\nSWC: %1\nLog: %2\n\n%3")
                            .arg(outputSwcPath, outputLogPath, loadError));
              return;
            }

            tm.succeedTask(taskPtr, QStringLiteral("created SWC #%1").arg(static_cast<qulonglong>(newSwcId)));

            // UX policy: if the user had not explicitly mapped this source to an existing SWC, promote
            // the (image, channel) pair to target the freshly created SWC so follow-up seed traces
            // naturally continue on the auto-trace result.
            docPtr->traceSettings().promoteNewSwcTargetToExistingIfStillNew(imgId, sc, newSwcId);
          },
          Qt::QueuedConnection);
      }

      co_return;
    }),
    "Auto Trace");
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
    return addNeuroglancerPrecomputedVolume(std::move(vol), errorMsg);
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZImgDoc::addNeuroglancerPrecomputedVolume(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, QString& errorMsg)
{
  try {
    errorMsg.clear();
    CHECK(vol);

    const QString rootUrl = vol->rootUrl();
    for (const auto& idPack : m_idToImgPacks) {
      const auto& pack = idPack.second;
      if (pack->isNeuroglancerPrecomputed() && pack->neuroglancerRootUrl() == rootUrl) {
        return idPack.first;
      }
    }

    return addImgPack(new ZImgPack(std::move(vol)));
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

  m_loadNeuroglancerStateAction =
    new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("Load Neuroglancer (&State JSON)..."), this);
  m_loadNeuroglancerStateAction->setStatusTip(tr("Load supported precomputed layers from a Neuroglancer viewer state URL/JSON"));
  connect(m_loadNeuroglancerStateAction, &QAction::triggered, this, &ZImgDoc::loadNeuroglancerState);

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

  m_autoTraceAction = new QAction(ZTheme::instance().icon(ZTheme::AutoTraceIcon), tr("&Auto Trace..."), this);
  m_autoTraceAction->setStatusTip(tr("Automatically trace neurons in a selected image/channel"));
  connect(m_autoTraceAction, &QAction::triggered, this, &ZImgDoc::autoTrace);
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
