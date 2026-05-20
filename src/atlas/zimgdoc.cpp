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
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include "zautotracedialog.h"
#include "zbinarizeimagedialog.h"
#include "zbinarytoswcdialog.h"
#include "zenhancelinedialog.h"
#include "zsubtractbackgroundadaptivedialog.h"
#include "zsubtractbackgrounddialog.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

namespace nim {

namespace {

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
            LOG(WARNING) << "Failed to apply neuroglancer_precomputed override '" << key << "': " << err;
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
  res->addAction(m_binaryToSwcAction);
  res->addSeparator();
  res->addAction(m_binarizeImageAction);
  res->addAction(m_subtractBackgroundAction);
  res->addAction(m_subtractBackgroundAdaptiveAction);
  res->addAction(m_enhanceLineAction);
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
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr;
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
      LOG(WARNING) << "Failed to load Neuroglancer history: " << loadErr;
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
          LOG(WARNING) << "Failed to apply Neuroglancer " << kind << " source override from history: " << err;
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
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr;
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
  const ZNeuroglancerState::InputParseResult input = ZNeuroglancerState::parseInputText(userText, kDefaultTimeout);
  if (input.status == ZNeuroglancerState::InputStatus::NotRecognized) {
    QMessageBox::information(
      QApplication::activeWindow(),
      QApplication::applicationName(),
      QStringLiteral("Unrecognized input. Paste a Neuroglancer share link, a JSON URL, or raw JSON."));
    return;
  }
  if (input.status == ZNeuroglancerState::InputStatus::Error) {
    QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), input.error);
    return;
  }
  CHECK(input.status == ZNeuroglancerState::InputStatus::Parsed);
  const boost::json::value& stateJson = input.stateJson;

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
    LOG(WARNING) << "Failed to load Neuroglancer history: " << historyLoadErr;
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
        LOG(WARNING) << "Failed to apply Neuroglancer " << kind << " source override from history: " << err;
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
      LOG(WARNING) << "Failed to save Neuroglancer history: " << saveErr;
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
  ZStitchImageDialog stitchImageDialog(m_doc, QApplication::activeWindow());
  stitchImageDialog.exec();
}

void ZImgDoc::alignSections()
{
  ZSectionsRegistrationDialog alignSectionsDialog(m_doc, QApplication::activeWindow());
  alignSectionsDialog.exec();
}

void ZImgDoc::correctChromaticShift()
{
  ZChromaticShiftCorrectionDialog chromaticShiftCorrectionDialog(m_doc, QApplication::activeWindow());
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
  dlg.exec();
}

void ZImgDoc::binaryToSwc()
{
  ZBinaryToSwcDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
}

void ZImgDoc::binarizeImage()
{
  ZBinarizeImageDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
}

void ZImgDoc::subtractBackground()
{
  ZSubtractBackgroundDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
}

void ZImgDoc::subtractBackgroundAdaptive()
{
  ZSubtractBackgroundAdaptiveDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
}

void ZImgDoc::enhanceLine()
{
  ZEnhanceLineDialog dlg(m_doc, QApplication::activeWindow());
  dlg.exec();
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
  ZTheme::instance().bindIcon(m_loadImgAction, ZTheme::LoadObjectIcon);
  m_loadImgAction->setStatusTip(tr("Load one or more existing image files"));
  connect(m_loadImgAction, &QAction::triggered, this, qOverload<>(&ZImgDoc::loadImg));

  m_loadNeuroglancerPrecomputedAction =
    new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("Load &Neuroglancer (Precomputed)..."), this);
  ZTheme::instance().bindIcon(m_loadNeuroglancerPrecomputedAction, ZTheme::LoadObjectIcon);
  m_loadNeuroglancerPrecomputedAction->setStatusTip(tr("Load a Neuroglancer precomputed volume via URL"));
  connect(m_loadNeuroglancerPrecomputedAction,
          &QAction::triggered,
          this,
          qOverload<>(&ZImgDoc::loadNeuroglancerPrecomputed));

  m_loadNeuroglancerStateAction =
    new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("Load Neuroglancer (&State JSON)..."), this);
  ZTheme::instance().bindIcon(m_loadNeuroglancerStateAction, ZTheme::LoadObjectIcon);
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
  ZTheme::instance().bindIcon(m_autoTraceAction, ZTheme::AutoTraceIcon);
  m_autoTraceAction->setStatusTip(tr("Automatically trace neurons in a selected image/channel"));
  connect(m_autoTraceAction, &QAction::triggered, this, &ZImgDoc::autoTrace);

  m_binaryToSwcAction = new QAction(tr("&Binary -> SWC..."), this);
  m_binaryToSwcAction->setStatusTip(tr("Convert a binary image to an SWC skeleton"));
  connect(m_binaryToSwcAction, &QAction::triggered, this, &ZImgDoc::binaryToSwc);

  m_binarizeImageAction = new QAction(tr("&Binarize..."), this);
  m_binarizeImageAction->setStatusTip(tr("Binarize an image channel by a threshold"));
  connect(m_binarizeImageAction, &QAction::triggered, this, &ZImgDoc::binarizeImage);

  m_subtractBackgroundAction = new QAction(tr("Subtract &Background..."), this);
  m_subtractBackgroundAction->setStatusTip(tr("Subtract background from an image channel (neuTube-style)"));
  connect(m_subtractBackgroundAction, &QAction::triggered, this, &ZImgDoc::subtractBackground);

  m_subtractBackgroundAdaptiveAction = new QAction(tr("Subtract Background (&Adaptive)..."), this);
  m_subtractBackgroundAdaptiveAction->setStatusTip(
    tr("Subtract background from an image channel using adaptive sampling (neuTube-style)"));
  connect(m_subtractBackgroundAdaptiveAction, &QAction::triggered, this, &ZImgDoc::subtractBackgroundAdaptive);

  m_enhanceLineAction = new QAction(tr("&Enhance Line..."), this);
  m_enhanceLineAction->setStatusTip(tr("Enhance line-like structures in an image channel (neuTube-style)"));
  connect(m_enhanceLineAction, &QAction::triggered, this, &ZImgDoc::enhanceLine);
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
