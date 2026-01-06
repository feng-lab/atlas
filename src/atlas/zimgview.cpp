#include "zimgview.h"

#include "zmeshdoc.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancersegmentpropertiesdialog.h"

#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_set>

namespace nim {

namespace {

struct NeuroglancerMeshLoadResult
{
  QString error;
  std::shared_ptr<ZMesh> mesh;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
};

struct NeuroglancerMeshSourceKey
{
  QString rootUrl;
  QString meshKey;
  uint64_t segmentId = 0;
};

[[nodiscard]] QString neuroglancerMeshKeyString(const QString& rootUrl, const QString& meshKey, uint64_t segmentId)
{
  return QString("%1|%2|%3").arg(rootUrl).arg(meshKey).arg(static_cast<qulonglong>(segmentId));
}

[[nodiscard]] std::optional<uint64_t> parseUint64Base10(const QString& s)
{
  const QString trimmed = s.trimmed();
  if (trimmed.isEmpty()) {
    return std::nullopt;
  }
  bool ok = false;
  const qulonglong v = trimmed.toULongLong(&ok, 10);
  if (!ok) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(v);
}

[[nodiscard]] std::vector<uint64_t> parseUint64ListFromText(const QString& text)
{
  // Accept any mix of whitespace / punctuation; extract all digit runs.
  // No truncation: returns every parsed value in order (deduping can be done by caller).
  std::vector<uint64_t> out;
  const QRegularExpression re(QStringLiteral("(\\d+)"));
  auto it = re.globalMatch(text);
  while (it.hasNext()) {
    const auto m = it.next();
    const auto vOpt = parseUint64Base10(m.captured(1));
    if (vOpt) {
      out.push_back(*vOpt);
    }
  }
  return out;
}

[[nodiscard]] std::optional<NeuroglancerMeshSourceKey> parseNeuroglancerMeshSourceKey(const json::value& v)
{
  if (!v.is_object()) {
    return std::nullopt;
  }
  const auto& o = v.as_object();
  auto itType = o.find("type");
  if (itType == o.end() || !itType->value().is_string()) {
    return std::nullopt;
  }
  const QString type = json::value_to<QString>(itType->value()).trimmed();
  if (type != "neuroglancer_precomputed_mesh") {
    return std::nullopt;
  }

  auto itRoot = o.find("segmentation_root_url");
  auto itSeg = o.find("segment_id");
  if (itRoot == o.end() || !itRoot->value().is_string() || itSeg == o.end() || !itSeg->value().is_string()) {
    return std::nullopt;
  }

  QString root = json::value_to<QString>(itRoot->value());
  try {
    root = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(root));
  }
  catch (...) {
    return std::nullopt;
  }

  const QString segStr = json::value_to<QString>(itSeg->value());
  const auto segOpt = parseUint64Base10(segStr);
  if (!segOpt) {
    return std::nullopt;
  }

  QString meshKey;
  if (auto itMeshKey = o.find("mesh_key"); itMeshKey != o.end() && itMeshKey->value().is_string()) {
    meshKey = json::value_to<QString>(itMeshKey->value()).trimmed();
  }

  NeuroglancerMeshSourceKey out;
  out.rootUrl = std::move(root);
  out.meshKey = std::move(meshKey);
  out.segmentId = *segOpt;
  return out;
}

struct CollectVisibleSegIdsResult
{
  std::vector<uint64_t> ids;
  QString error;
};

} // namespace

ZImgView::ZImgView(ZImgDoc& doc, ZView& view)
  : ZFilterView<ZImgDoc, ZImgFilter>(doc, view)
{
  docImgsAdded(m_doc.objs());
  connect(&m_doc, &ZImgDoc::objAdded, this, &ZImgView::docImgAdded);
  connect(&m_doc, &ZImgDoc::imgChanged, this, &ZImgView::docImgChanged);
}

void ZImgView::appendContextMenuActions(QMenu& menu,
                                        size_t /*activeObjId*/,
                                        const QPointF& scenePos,
                                        Qt::KeyboardModifiers /*modifiers*/)
{
  if (m_view.isMaxZProjView()) {
    return;
  }

  struct Candidate
  {
    size_t imgObjId = 0;
    ZImgFilter* filter = nullptr;
    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
    bool hasMeshDirectory = false;
    bool hasSegmentPropertiesDirectory = false;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(m_idToFilter.size());
  for (const auto& idFilter : m_idToFilter) {
    const size_t imgObjId = idFilter.first;
    ZImgFilter* filter = idFilter.second.get();
    if (!filter || !filter->isVisible()) {
      continue;
    }
    const ZImgPack& imgPack = m_doc.imgPack(imgObjId);
    if (!imgPack.isNeuroglancerPrecomputed()) {
      continue;
    }
    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
    if (!vol || !vol->isSegmentation()) {
      continue;
    }
    Candidate c;
    c.imgObjId = imgObjId;
    c.filter = filter;
    c.vol = std::move(vol);
    c.hasMeshDirectory = c.vol && c.vol->hasMeshDirectory();
    c.hasSegmentPropertiesDirectory = c.vol && c.vol->hasSegmentPropertiesDirectory();
    candidates.push_back(std::move(c));
  }
  if (candidates.empty()) {
    return;
  }

  const bool hasAnyMesh = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasMeshDirectory;
  });

  auto startMeshLoad = [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, uint64_t segmentId) {
    CHECK(vol);
    CHECK(vol->hasMeshDirectory());

    json::object sourceObj;
    sourceObj["type"] = "neuroglancer_precomputed_mesh";
    sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
    sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
    if (!vol->meshKey().isEmpty()) {
      sourceObj["mesh_key"] = json::value_from(vol->meshKey());
    }
    const json::value sourceJson = sourceObj;

    // If already loaded, do not re-fetch.
    if (m_doc.doc().meshDoc().findMeshByExternalSource(sourceJson)) {
      return;
    }

    auto* coarseWatcher = new QFutureWatcher<NeuroglancerMeshLoadResult>(this);
    connect(coarseWatcher,
            &QFutureWatcher<NeuroglancerMeshLoadResult>::finished,
            this,
            [this, coarseWatcher, vol, segmentId]() {
              const NeuroglancerMeshLoadResult coarse = coarseWatcher->result();
              coarseWatcher->deleteLater();

              if (!coarse.error.isEmpty()) {
                QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), coarse.error);
                return;
              }
              if (!coarse.mesh || coarse.mesh->empty()) {
                QMessageBox::information(QApplication::activeWindow(),
                                         QApplication::applicationName(),
                                         QString("Neuroglancer mesh load returned an empty mesh for segment %1")
                                           .arg(static_cast<qulonglong>(segmentId)));
                return;
              }

              const size_t meshObjId = m_doc.doc().meshDoc().addMeshFromExternalSource(*coarse.mesh,
                                                                                       coarse.displayName,
                                                                                       coarse.tooltip,
                                                                                       coarse.sourceJson);

              auto* fineWatcher = new QFutureWatcher<NeuroglancerMeshLoadResult>(this);
              connect(fineWatcher,
                      &QFutureWatcher<NeuroglancerMeshLoadResult>::finished,
                      this,
                      [this, fineWatcher, meshObjId]() {
                        const NeuroglancerMeshLoadResult fine = fineWatcher->result();
                        fineWatcher->deleteLater();

                        if (!fine.error.isEmpty()) {
                          // Keep the coarse mesh; refinement failures should not be fatal.
                          VLOG(1) << fmt::format("Neuroglancer mesh refinement failed: {}", fine.error.toStdString());
                          return;
                        }
                        if (!fine.mesh || fine.mesh->empty()) {
                          return;
                        }
                        if (!m_doc.doc().meshDoc().hasObjWithID(meshObjId)) {
                          return;
                        }
                        m_doc.doc().meshDoc().replaceMeshGeometry(meshObjId, *fine.mesh);
                      });

              fineWatcher->setFuture(QtConcurrent::run([vol, segmentId, coarse]() -> NeuroglancerMeshLoadResult {
                NeuroglancerMeshLoadResult out;
                // Reuse metadata (sourceJson/displayName/tooltip) from coarse for consistency.
                out.sourceJson = coarse.sourceJson;
                out.displayName = coarse.displayName;
                out.tooltip = coarse.tooltip;
                try {
                  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source = vol->loadMeshSourceBlocking();
                  CHECK(source);
                  out.mesh = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest);
                  if (!out.mesh || out.mesh->empty()) {
                    out.error = QString("Neuroglancer mesh refinement returned an empty mesh for segment %1")
                                  .arg(static_cast<qulonglong>(segmentId));
                  }
                }
                catch (const ZNotFoundException&) {
                  // Fine LOD missing: keep the coarse mesh.
                }
                catch (const ZException& e) {
                  out.error = QString::fromUtf8(e.what());
                }
                catch (const std::exception& e) {
                  out.error = QString::fromUtf8(e.what());
                }
                return out;
              }));
            });

    coarseWatcher->setFuture(QtConcurrent::run([vol, segmentId, sourceJson]() -> NeuroglancerMeshLoadResult {
      NeuroglancerMeshLoadResult out;
      out.sourceJson = sourceJson;
      try {
        std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source = vol->loadMeshSourceBlocking();
        CHECK(source);
        out.mesh = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest);
        if (!out.mesh || out.mesh->empty()) {
          out.error = QString("Neuroglancer mesh load returned an empty mesh for segment %1")
                        .arg(static_cast<qulonglong>(segmentId));
          return out;
        }

        QString label;
        QString description;
        if (const auto props = vol->segmentPropertiesShared()) {
          if (const auto l = props->labelForId(segmentId)) {
            label = *l;
          }
          if (const auto d = props->descriptionForId(segmentId)) {
            description = *d;
          }
        }

        out.displayName = label.isEmpty() ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
                                          : QString("NG Mesh %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

        out.tooltip = QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                        .arg(vol->rootUrl())
                        .arg(static_cast<qulonglong>(segmentId));
        if (!label.isEmpty()) {
          out.tooltip += QString("\nLabel: %1").arg(label);
        }
        if (!description.isEmpty()) {
          out.tooltip += QString("\nDescription: %1").arg(description);
        }
      }
      catch (const ZNotFoundException&) {
        out.error = QString("No neuroglancer mesh found for segment %1").arg(static_cast<qulonglong>(segmentId));
      }
      catch (const ZException& e) {
        out.error = QString::fromUtf8(e.what());
      }
      catch (const std::exception& e) {
        out.error = QString::fromUtf8(e.what());
      }
      return out;
    }));
  };

  auto startMeshBatchLoad = [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol,
                                   std::vector<uint64_t> ids) {
    CHECK(vol);
    CHECK(vol->hasMeshDirectory());

    if (ids.empty()) {
      return;
    }

    // Dedup early and strip background ID 0 (it is typically "unlabeled" and rarely has a mesh).
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
    if (ids.empty()) {
      return;
    }

    QPointer<ZMeshDoc> meshDocPtr = &m_doc.doc().meshDoc();
    CHECK(meshDocPtr);
    ZMeshDoc* meshDoc = meshDocPtr.data();

    std::set<QString> existingKeys;
    for (const size_t meshId : meshDoc->objs()) {
      const auto keyOpt = parseNeuroglancerMeshSourceKey(meshDoc->jsonValue(meshId));
      if (!keyOpt) {
        continue;
      }
      existingKeys.insert(neuroglancerMeshKeyString(keyOpt->rootUrl, keyOpt->meshKey, keyOpt->segmentId));
    }

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [watcher]() {
      const QString msg = watcher->result();
      watcher->deleteLater();
      if (!msg.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), msg);
      }
    });

    watcher->setFuture(
      QtConcurrent::run([vol, meshDocPtr, ids = std::move(ids), existingKeys = std::move(existingKeys)]() mutable {
      size_t loaded = 0;
      size_t missing = 0;
      size_t skipped = 0;
      size_t errors = 0;

      if (QCoreApplication::closingDown() || !meshDocPtr) {
        return QString{};
      }

      auto enqueueToUi = [](auto fn) {
        if (QCoreApplication::closingDown()) {
          return;
        }
        if (auto* app = QCoreApplication::instance()) {
          QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
        }
      };

      std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
      try {
        source = vol->loadMeshSourceBlocking();
      }
      catch (const std::exception& e) {
        return QString("Failed to load neuroglancer mesh source: %1").arg(QString::fromUtf8(e.what()));
      }
      CHECK(source);

      const std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props = vol->segmentPropertiesShared();

      const QString rootUrl = vol->rootUrl();
      const QString meshKey = vol->meshKey();

      for (const uint64_t segmentId : ids) {
        if (QCoreApplication::closingDown() || !meshDocPtr) {
          break;
        }

        const QString keyStr = neuroglancerMeshKeyString(rootUrl, meshKey, segmentId);
        if (existingKeys.contains(keyStr)) {
          ++skipped;
          continue;
        }

        std::shared_ptr<ZMesh> coarse;
        try {
          coarse = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest);
        }
        catch (const ZNotFoundException&) {
          ++missing;
          continue;
        }
        catch (const std::exception& e) {
          ++errors;
          VLOG(1) << fmt::format("Failed to load neuroglancer mesh for segment {}: {}", segmentId, e.what());
          continue;
        }

        if (!coarse || coarse->empty()) {
          ++errors;
          continue;
        }

        QString label;
        QString description;
        if (props) {
          if (const auto l = props->labelForId(segmentId)) {
            label = *l;
          }
          if (const auto d = props->descriptionForId(segmentId)) {
            description = *d;
          }
        }

        const QString displayName = label.isEmpty()
                                      ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
                                      : QString("NG Mesh %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

        QString tooltip = QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                            .arg(rootUrl)
                            .arg(static_cast<qulonglong>(segmentId));
        if (!label.isEmpty()) {
          tooltip += QString("\nLabel: %1").arg(label);
        }
        if (!description.isEmpty()) {
          tooltip += QString("\nDescription: %1").arg(description);
        }

        json::object sourceObj;
        sourceObj["type"] = "neuroglancer_precomputed_mesh";
        sourceObj["segmentation_root_url"] = json::value_from(rootUrl);
        sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
        if (!meshKey.isEmpty()) {
          sourceObj["mesh_key"] = json::value_from(meshKey);
        }
        const json::value sourceJson = sourceObj;

        enqueueToUi([meshDocPtr, coarse, displayName, tooltip, sourceJson]() mutable {
          if (QCoreApplication::closingDown() || !meshDocPtr) {
            return;
          }
          // Avoid duplicates if another async task already added the same external-source mesh.
          if (meshDocPtr->findMeshByExternalSource(sourceJson)) {
            return;
          }
          (void)meshDocPtr->addMeshFromExternalSource(*coarse, displayName, tooltip, sourceJson);
        });

        existingKeys.insert(keyStr);
        ++loaded;

        // Refine to the highest-detail LOD (best effort).
        std::shared_ptr<ZMesh> fine;
        try {
          fine = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest);
        }
        catch (const ZNotFoundException&) {
          continue;
        }
        catch (const std::exception& e) {
          VLOG(1) << fmt::format("Neuroglancer mesh refinement failed for segment {}: {}", segmentId, e.what());
          continue;
        }
        if (!fine || fine->empty()) {
          continue;
        }

        enqueueToUi([meshDocPtr, fine, sourceJson]() mutable {
          if (QCoreApplication::closingDown() || !meshDocPtr) {
            return;
          }
          const auto idOpt = meshDocPtr->findMeshByExternalSource(sourceJson);
          if (!idOpt) {
            return;
          }
          meshDocPtr->replaceMeshGeometry(*idOpt, *fine);
        });
      }

      if (loaded == 0 && missing == 0 && skipped == 0 && errors == 0) {
        return QString{};
      }
      return QString("Neuroglancer mesh batch load finished.\n\nLoaded: %1\nMissing mesh: %2\nSkipped (already loaded): %3\nErrors: %4")
        .arg(static_cast<qulonglong>(loaded))
        .arg(static_cast<qulonglong>(missing))
        .arg(static_cast<qulonglong>(skipped))
        .arg(static_cast<qulonglong>(errors));
    }));
  };

  menu.addSeparator();

  auto* copySegIdAct = menu.addAction("Copy Neuroglancer Segment ID Under Cursor");
  connect(copySegIdAct, &QAction::triggered, this, [scenePos, candidates]() {
    int bestPrecedence = std::numeric_limits<int>::min();
    std::optional<uint64_t> bestSeg;
    for (const auto& c : candidates) {
      CHECK(c.filter);
      const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
      if (!segOpt) {
        continue;
      }
      const int precedence = c.filter->viewPrecedence();
      if (!bestSeg || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestSeg = segOpt;
      }
    }
    if (!bestSeg) {
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                              "Wait for the visible tiles to finish loading, then try again."));
      return;
    }
    QApplication::clipboard()->setText(QString::number(static_cast<qulonglong>(*bestSeg)));
  });

  if (hasAnyMesh) {
    auto* loadMeshAct = menu.addAction("Load Neuroglancer Mesh for Segment Under Cursor");
    connect(loadMeshAct, &QAction::triggered, this, [scenePos, startMeshLoad, candidates]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      for (const auto& c : candidates) {
        if (!c.hasMeshDirectory) {
          continue;
        }
        CHECK(c.filter);
        const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
        if (!segOpt) {
          continue;
        }
        const int precedence = c.filter->viewPrecedence();
        if (!bestSeg || precedence > bestPrecedence) {
          bestPrecedence = precedence;
          bestSeg = segOpt;
          bestVol = c.vol;
        }
      }
      if (!bestSeg || !bestVol) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startMeshLoad(bestVol, *bestSeg);
    });
  }

  // Choose the top-most (highest view precedence) visible segmentation layer for dataset-scoped actions.
  auto pickTopmostSegmentationVolume = [&]() -> std::shared_ptr<ZNeuroglancerPrecomputedVolume> {
    int bestPrecedence = std::numeric_limits<int>::min();
    std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
    for (const auto& c : candidates) {
      if (!c.hasMeshDirectory) {
        continue;
      }
      CHECK(c.filter);
      const int precedence = c.filter->viewPrecedence();
      if (!bestVol || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestVol = c.vol;
      }
    }
    return bestVol;
  };

  if (hasAnyMesh) {
    if (auto topVol = pickTopmostSegmentationVolume()) {
      auto* loadMeshByIdAct =
        menu.addAction(QString("Load Neuroglancer Mesh for Segment ID… (%1)").arg(topVol->rootUrl()));
    connect(loadMeshByIdAct, &QAction::triggered, this, [topVol, startMeshLoad]() {
      QString prefill;
      if (const auto clipOpt = parseUint64Base10(QApplication::clipboard()->text())) {
        prefill = QString::number(static_cast<qulonglong>(*clipOpt));
      }
      const QString s = QInputDialog::getText(QApplication::activeWindow(),
                                              QApplication::applicationName(),
                                              QStringLiteral("Neuroglancer segment ID (uint64, base-10):"),
                                              QLineEdit::Normal,
                                              prefill)
                          .trimmed();
      if (s.isEmpty()) {
        return;
      }
      const auto idOpt = parseUint64Base10(s);
      if (!idOpt) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("Invalid segment ID. Expected a base-10 uint64 value."));
        return;
      }
      startMeshLoad(topVol, *idOpt);
    });

    auto* loadMeshesFromClipboardAct =
      menu.addAction(QString("Load Neuroglancer Meshes for Segment IDs from Clipboard… (%1)").arg(topVol->rootUrl()));
    connect(loadMeshesFromClipboardAct, &QAction::triggered, this, [topVol, startMeshBatchLoad]() {
      const QString text = QApplication::clipboard()->text();
      const std::vector<uint64_t> ids = parseUint64ListFromText(text);
      if (ids.empty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("Clipboard does not contain any base-10 uint64 segment IDs."));
        return;
      }

      std::set<uint64_t> unique(ids.begin(), ids.end());
      const int response = QMessageBox::question(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QString("Load meshes for %1 segment IDs from clipboard?\n\nThis will download mesh data and may take time.")
          .arg(static_cast<qulonglong>(unique.size())),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);
      if (response != QMessageBox::Ok) {
        return;
      }

      startMeshBatchLoad(topVol, std::vector<uint64_t>(unique.begin(), unique.end()));
    });

    auto* loadVisibleMeshesAct =
      menu.addAction(QString("Load Neuroglancer Meshes for Visible Segments (cached)… (%1)").arg(topVol->rootUrl()));
    connect(loadVisibleMeshesAct, &QAction::triggered, this, [this, topVol, startMeshBatchLoad]() {
      CHECK(topVol);

      const QRectF viewport = m_view.currentViewport();
      const double scale = m_view.currentScale();
      const int z = m_view.currentSlice();

      auto* watcher = new QFutureWatcher<CollectVisibleSegIdsResult>(this);
      connect(watcher, &QFutureWatcher<CollectVisibleSegIdsResult>::finished, this, [watcher, topVol, startMeshBatchLoad]() {
        const CollectVisibleSegIdsResult r = watcher->result();
        watcher->deleteLater();

        if (!r.error.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
          return;
        }
        if (r.ids.empty()) {
          QMessageBox::information(
            QApplication::activeWindow(),
            QApplication::applicationName(),
            QStringLiteral("No cached Neuroglancer segmentation IDs are available at the current zoom level yet.\n\n"
                           "This action only considers cached tiles at the target (final) LOD for the current viewport scale "
                           "and ignores coarse fallback tiles.\n\n"
                           "Wait for refinement to complete for the current zoom level, then try again."));
          return;
        }

        const int response = QMessageBox::question(
          QApplication::activeWindow(),
          QApplication::applicationName(),
          QString(
            "Load meshes for %1 segments visible in the current viewport (cached tiles only)?\n\n"
            "This will download mesh data and may take time.\n"
            "Note: segment ID 0 is treated as background and will be ignored.")
            .arg(static_cast<qulonglong>(r.ids.size())),
          QMessageBox::Ok | QMessageBox::Cancel,
          QMessageBox::Cancel);
        if (response != QMessageBox::Ok) {
          return;
        }

        startMeshBatchLoad(topVol, r.ids);
      });

      watcher->setFuture(QtConcurrent::run([topVol, z, viewport, scale]() -> CollectVisibleSegIdsResult {
        CollectVisibleSegIdsResult out;
        try {
          const auto pack =
            topVol->sliceTilePackFor2DViewportCacheBestEffort(static_cast<size_t>(z), /*t=*/0, viewport, scale);

          if (pack.imgs.empty()) {
            return out;
          }
          CHECK(pack.ratios.size() == pack.imgs.size());

          std::unordered_set<uint64_t> ids;
          for (size_t i = 0; i < pack.imgs.size(); ++i) {
            if (pack.ratios[i] != pack.targetRatio) {
              // Visible segment meshes are defined as IDs from cached tiles at the "final" LOD for the
              // current viewport scale. Ignore coarser fallback tiles to avoid accidental bulk loads.
              continue;
            }

            const auto& img = pack.imgs[i];
            if (!img || img->isEmpty()) {
              continue;
            }
            const ZImgInfo& info = img->info();
            if (info.numChannels != 1 || info.depth != 1) {
              out.error = QString("Unexpected neuroglancer segmentation slice type: %1 channels, depth %2")
                            .arg(info.numChannels)
                            .arg(info.depth);
              return out;
            }
            if (info.voxelFormat != VoxelFormat::Unsigned) {
              out.error = QString("Unsupported neuroglancer segmentation voxel format: expected unsigned, got '%1'")
                            .arg(info.voxelFormat == VoxelFormat::Signed ? "signed" : "float");
              return out;
            }

            const size_t n = info.width * info.height;
            switch (info.bytesPerVoxel) {
            case 1: {
              const auto* p = img->timeData<uint8_t>(0);
              for (size_t pix = 0; pix < n; ++pix) {
                ids.insert(static_cast<uint64_t>(p[pix]));
              }
              break;
            }
            case 2: {
              const auto* p = img->timeData<uint16_t>(0);
              for (size_t pix = 0; pix < n; ++pix) {
                ids.insert(static_cast<uint64_t>(p[pix]));
              }
              break;
            }
            case 4: {
              const auto* p = img->timeData<uint32_t>(0);
              for (size_t pix = 0; pix < n; ++pix) {
                ids.insert(static_cast<uint64_t>(p[pix]));
              }
              break;
            }
            case 8: {
              const auto* p = img->timeData<uint64_t>(0);
              for (size_t pix = 0; pix < n; ++pix) {
                ids.insert(p[pix]);
              }
              break;
            }
            default:
              out.error = QString("Unsupported neuroglancer segmentation data type: %1 bytes/voxel").arg(info.bytesPerVoxel);
              return out;
            }
          }

          out.ids.assign(ids.begin(), ids.end());
          std::sort(out.ids.begin(), out.ids.end());
        }
        catch (const ZException& e) {
          out.error = QString::fromUtf8(e.what());
        }
        catch (const std::exception& e) {
          out.error = QString::fromUtf8(e.what());
        }
        return out;
      }));
    });
  }
  }

  // Bulk load (segment_properties). Potentially heavy; expose per dataset.
  std::vector<std::shared_ptr<ZNeuroglancerPrecomputedVolume>> volsWithSegProps;
  volsWithSegProps.reserve(candidates.size());
  for (const auto& c : candidates) {
    if (!c.hasSegmentPropertiesDirectory) {
      continue;
    }
    volsWithSegProps.push_back(c.vol);
  }
  std::sort(volsWithSegProps.begin(), volsWithSegProps.end(), [](const auto& a, const auto& b) {
    return a->rootUrl() < b->rootUrl();
  });
  volsWithSegProps.erase(std::unique(volsWithSegProps.begin(),
                                     volsWithSegProps.end(),
                                     [](const auto& a, const auto& b) {
                                       return a->rootUrl() == b->rootUrl();
                                     }),
                         volsWithSegProps.end());

  for (const auto& vol : volsWithSegProps) {
    auto* showPropsAct = menu.addAction(QString("Show Neuroglancer Segment Properties… (%1)").arg(vol->rootUrl()));
    connect(showPropsAct,
            &QAction::triggered,
            this,
            [meshDocPtr = QPointer<ZMeshDoc>(&m_doc.doc().meshDoc()), vol, startMeshLoad]() {
      auto* dlg = new ZNeuroglancerSegmentPropertiesDialog(vol, QApplication::activeWindow());
      dlg->setAttribute(Qt::WA_DeleteOnClose, true);
      if (vol->hasMeshDirectory()) {
        dlg->setMeshLoadCallback([startMeshLoad, vol](uint64_t id) {
          startMeshLoad(vol, id);
        });
      }
      dlg->setAfterPropertiesLoadedCallback([meshDocPtr,
                                             propsRootUrl = vol->rootUrl(),
                                             propsMeshKey = vol->meshKey()](auto props) {
        if (QCoreApplication::closingDown() || !meshDocPtr || !props) {
          return;
        }
        for (const size_t meshId : meshDocPtr->objs()) {
          const auto srcOpt = parseNeuroglancerMeshSourceKey(meshDocPtr->jsonValue(meshId));
          if (!srcOpt) {
            continue;
          }
          if (srcOpt->rootUrl != propsRootUrl) {
            continue;
          }
          if (!propsMeshKey.isEmpty() && srcOpt->meshKey != propsMeshKey) {
            continue;
          }

          const uint64_t segmentId = srcOpt->segmentId;
          const auto labelOpt = props->labelForId(segmentId);
          const auto descOpt = props->descriptionForId(segmentId);
          if (!labelOpt && !descOpt) {
            continue;
          }

          const QString label = labelOpt ? *labelOpt : QString{};
          const QString description = descOpt ? *descOpt : QString{};

          const QString displayName = label.isEmpty()
                                        ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
                                        : QString("NG Mesh %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

          QString tooltip = QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                              .arg(propsRootUrl)
                              .arg(static_cast<qulonglong>(segmentId));
          if (!label.isEmpty()) {
            tooltip += QString("\nLabel: %1").arg(label);
          }
          if (!description.isEmpty()) {
            tooltip += QString("\nDescription: %1").arg(description);
          }

          meshDocPtr->updateExternalMeshMetadata(meshId, displayName, tooltip);
        }
      });
      dlg->open();
    });
  }

  for (const auto& vol : volsWithSegProps) {
    if (!vol->hasMeshDirectory()) {
      continue;
    }
    auto* loadAllMeshesAct = menu.addAction(
      QString("Load Neuroglancer Meshes for All Segments (segment_properties)… (%1)").arg(vol->rootUrl()));
    connect(loadAllMeshesAct, &QAction::triggered, this, [this, vol]() {
      const auto response = QMessageBox::question(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QString(
          "Load meshes for all segments listed in segment_properties?\n\nThis will download segment properties (if not cached) and may take a long time and consume significant memory."),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);
      if (response != QMessageBox::Ok) {
        return;
      }

      auto* watcher = new QFutureWatcher<QString>(this);
      connect(watcher, &QFutureWatcher<QString>::finished, this, [watcher]() {
        const QString msg = watcher->result();
        watcher->deleteLater();
        if (!msg.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), msg);
        }
      });

      QPointer<ZMeshDoc> meshDocPtr = &m_doc.doc().meshDoc();
      watcher->setFuture(QtConcurrent::run([vol, meshDocPtr]() -> QString {
        size_t loaded = 0;
        size_t missing = 0;
        size_t errors = 0;

        if (QCoreApplication::closingDown() || !meshDocPtr) {
          return QString{};
        }

        auto enqueueToUi = [](auto fn) {
          if (QCoreApplication::closingDown()) {
            return;
          }
          if (auto* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
          }
        };

        std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props;
        try {
          props = vol->loadSegmentPropertiesBlocking();
        }
        catch (const std::exception& e) {
          return QString("Failed to load neuroglancer segment properties: %1").arg(QString::fromUtf8(e.what()));
        }
        CHECK(props);

        std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
        try {
          source = vol->loadMeshSourceBlocking();
        }
        catch (const std::exception& e) {
          return QString("Failed to load neuroglancer mesh source: %1").arg(QString::fromUtf8(e.what()));
        }
        CHECK(source);

        const auto& ids = props->ids();
        for (size_t i = 0; i < ids.size(); ++i) {
          const uint64_t segmentId = ids[i];
          std::shared_ptr<ZMesh> coarse;
          try {
            coarse = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest);
          }
          catch (const ZNotFoundException&) {
            ++missing;
            continue;
          }
          catch (const std::exception& e) {
            ++errors;
            VLOG(1) << fmt::format("Failed to load neuroglancer mesh for segment {}: {}", segmentId, e.what());
            continue;
          }

          if (!coarse || coarse->empty()) {
            ++errors;
            continue;
          }

          QString label;
          QString description;
          if (const auto l = props->labelForId(segmentId)) {
            label = *l;
          }
          if (const auto d = props->descriptionForId(segmentId)) {
            description = *d;
          }

          const QString displayName = label.isEmpty()
                                        ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
                                        : QString("NG Mesh %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

          QString tooltip = QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                              .arg(vol->rootUrl())
                              .arg(static_cast<qulonglong>(segmentId));
          if (!label.isEmpty()) {
            tooltip += QString("\nLabel: %1").arg(label);
          }
          if (!description.isEmpty()) {
            tooltip += QString("\nDescription: %1").arg(description);
          }

          json::object sourceObj;
          sourceObj["type"] = "neuroglancer_precomputed_mesh";
          sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
          sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
          if (!vol->meshKey().isEmpty()) {
            sourceObj["mesh_key"] = json::value_from(vol->meshKey());
          }
          const json::value sourceJson = sourceObj;

          enqueueToUi([meshDocPtr, coarse, displayName, tooltip, sourceJson]() mutable {
            if (QCoreApplication::closingDown() || !meshDocPtr) {
              return;
            }
            if (meshDocPtr->findMeshByExternalSource(sourceJson)) {
              return;
            }
            (void)meshDocPtr->addMeshFromExternalSource(*coarse, displayName, tooltip, sourceJson);
          });

          ++loaded;

          // Refine to the highest-detail LOD.
          std::shared_ptr<ZMesh> fine;
          try {
            fine = source->loadMeshBlocking(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest);
          }
          catch (const ZNotFoundException&) {
            continue;
          }
          catch (const std::exception& e) {
            ++errors;
            VLOG(1) << fmt::format("Failed to refine neuroglancer mesh for segment {}: {}", segmentId, e.what());
            continue;
          }

          if (!fine || fine->empty()) {
            continue;
          }
          enqueueToUi([meshDocPtr, fine, sourceJson]() mutable {
            if (QCoreApplication::closingDown() || !meshDocPtr) {
              return;
            }
            const auto idOpt = meshDocPtr->findMeshByExternalSource(sourceJson);
            if (!idOpt) {
              return;
            }
            meshDocPtr->replaceMeshGeometry(*idOpt, *fine);
          });
        }

        return QString("Neuroglancer mesh bulk load finished.\nLoaded: %1\nMissing: %2\nErrors: %3")
          .arg(static_cast<qulonglong>(loaded))
          .arg(static_cast<qulonglong>(missing))
          .arg(static_cast<qulonglong>(errors));
      }));
    });
  }
}

QString ZImgView::infoOfPos(double x, double y)
{
  QString info;
  try {
    for (const auto& idFilter : m_idToFilter) {
      const ZImgFilter* viewControl = idFilter.second.get();
      if (!viewControl->isVisible()) {
        continue;
      }
      size_t id = idFilter.first;
      const ZImgPack& imgPack = m_doc.imgPack(id);
      QPointF p = idFilter.second->mapFromScene(QPointF(x, y));
      index_t lx = p.x();
      index_t ly = p.y();
      if (lx >= 0 && lx < imgPack.imgInfo().sWidth() && ly >= 0 && ly < imgPack.imgInfo().sHeight()) {
        auto lz = m_view.isMaxZProjView() ? 0 : viewControl->imgSlice();
        auto lt = viewControl->imgTime();
        info += imgPack.sizeInfo();
        if (imgPack.imgInfo().numTimes == 1) {
          info += QString(" Coord: (%1,%2,%3)").arg(lx).arg(ly).arg(lz);
        } else {
          info += QString(" Coord: (%1,%2,%3,%4)").arg(lx).arg(ly).arg(lz).arg(lt);
        }
        info += QString(" Intensity: (%1")
                  .arg(imgPack.displayValue(lx, ly, lz, 0, lt, m_view.isMaxZProjView()),
                       0,
                       'g',
                       QLocale::FloatingPointShortest);
        for (size_t c = 1; c < imgPack.imgInfo().numChannels; ++c) {
          info += QString(",%1").arg(imgPack.displayValue(lx, ly, lz, c, lt, m_view.isMaxZProjView()),
                                     0,
                                     'g',
                                     QLocale::FloatingPointShortest);
        }
        info += ")";

        // Neuroglancer segmentation: enrich hover readout with label/description when already available,
        // without triggering any network I/O (cache-only).
        if (!m_view.isMaxZProjView() && imgPack.isNeuroglancerPrecomputed()) {
          const std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
          if (vol && vol->isSegmentation()) {
            if (const auto props = vol->segmentPropertiesShared()) {
              const auto segOpt = imgPack.tryGetCachedNeuroglancerSegmentationId(static_cast<size_t>(lx),
                                                                                 static_cast<size_t>(ly),
                                                                                 static_cast<size_t>(lz));
              if (segOpt) {
                if (const auto labelOpt = props->labelForId(*segOpt)) {
                  info += QString(" Label: %1").arg(*labelOpt);
                }
                if (const auto descOpt = props->descriptionForId(*segOpt)) {
                  info += QString(" Desc: %1").arg(*descOpt);
                }
              }
            }
          }
        }

        info += "      ";
      }
    }
  }
  catch (const ZException& e) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), e.what());
  }
  return info;
}

void ZImgView::docImgsAdded(const std::vector<size_t>& objs)
{
  for (auto id : objs) {
    auto viewControl = new ZImgFilter(m_view);
    viewControl->setData(m_doc.imgPack(id));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[id].reset(viewControl);
    connect(viewControl, &ZImgFilter::boundBoxChanged, this, &ZImgView::updateBoundBox);
    connect(viewControl, &ZImgFilter::objDeselected, this, &ZImgView::onObjDeselectedFromView);
    connect(viewControl, &ZImgFilter::objSelected, this, &ZImgView::onObjSelectedFromView);
    connect(viewControl, &ZImgFilter::objVisibleChanged, this, &ZImgView::onObjVisibleChangedFromView);
    Q_EMIT objViewReady(id);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZImgView::docImgAdded(size_t id)
{
  auto viewControl = new ZImgFilter(m_view);
  viewControl->setData(m_doc.imgPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZImgFilter::boundBoxChanged, this, &ZImgView::updateBoundBox);
  connect(viewControl, &ZImgFilter::objDeselected, this, &ZImgView::onObjDeselectedFromView);
  connect(viewControl, &ZImgFilter::objSelected, this, &ZImgView::onObjSelectedFromView);
  connect(viewControl, &ZImgFilter::objVisibleChanged, this, &ZImgView::onObjVisibleChangedFromView);
  Q_EMIT objViewReady(id);
}

void ZImgView::docImgChanged(size_t id)
{
  m_idToFilter.at(id)->setData(m_doc.imgPack(id));
}

} // namespace nim
