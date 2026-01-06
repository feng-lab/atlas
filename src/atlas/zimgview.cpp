#include "zimgview.h"

#include "zmeshdoc.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedsegmentproperties.h"

#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QFutureWatcher>
#include <QMenu>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <limits>

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
    if (!vol || !vol->isSegmentation() || !vol->hasMeshDirectory()) {
      continue;
    }
    candidates.push_back(Candidate{imgObjId, filter, std::move(vol)});
  }
  if (candidates.empty()) {
    return;
  }

  menu.addSeparator();

  auto* copySegIdAct = menu.addAction("Copy Neuroglancer Segment ID Under Cursor");
  connect(copySegIdAct, &QAction::triggered, this, [this, scenePos]() {
    int bestPrecedence = std::numeric_limits<int>::min();
    std::optional<uint64_t> bestSeg;
    for (const auto& idFilter : m_idToFilter) {
      ZImgFilter* filter = idFilter.second.get();
      if (!filter) {
        continue;
      }
      const std::optional<uint64_t> segOpt = filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
      if (!segOpt) {
        continue;
      }
      const int precedence = filter->viewPrecedence();
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

  auto* loadMeshAct = menu.addAction("Load Neuroglancer Mesh for Segment Under Cursor");
  connect(loadMeshAct, &QAction::triggered, this, [this, scenePos]() {
    int bestPrecedence = std::numeric_limits<int>::min();
    std::optional<uint64_t> bestSeg;
    std::optional<size_t> bestImgObjId;
    for (const auto& idFilter : m_idToFilter) {
      const size_t imgObjId = idFilter.first;
      ZImgFilter* filter = idFilter.second.get();
      if (!filter) {
        continue;
      }
      const std::optional<uint64_t> segOpt = filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
      if (!segOpt) {
        continue;
      }
      const int precedence = filter->viewPrecedence();
      if (!bestSeg || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestSeg = segOpt;
        bestImgObjId = imgObjId;
      }
    }
    if (!bestSeg || !bestImgObjId) {
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                              "Wait for the visible tiles to finish loading, then try again."));
      return;
    }

    const uint64_t segmentId = *bestSeg;
    const ZImgPack& imgPack = m_doc.imgPack(*bestImgObjId);
    CHECK(imgPack.isNeuroglancerPrecomputed());
    const std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();

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
  });

  // Bulk load (segment_properties). Potentially heavy; expose per dataset.
  std::vector<std::shared_ptr<ZNeuroglancerPrecomputedVolume>> volsWithSegProps;
  volsWithSegProps.reserve(candidates.size());
  for (const auto& c : candidates) {
    if (!c.vol || !c.vol->hasSegmentPropertiesDirectory()) {
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

      ZMeshDoc* meshDoc = &m_doc.doc().meshDoc();
      watcher->setFuture(QtConcurrent::run([vol, meshDoc]() -> QString {
        size_t loaded = 0;
        size_t missing = 0;
        size_t errors = 0;

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

          QMetaObject::invokeMethod(
            meshDoc,
            [meshDoc, coarse, displayName, tooltip, sourceJson]() mutable {
              (void)meshDoc->addMeshFromExternalSource(*coarse, displayName, tooltip, sourceJson);
            },
            Qt::QueuedConnection);

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
          QMetaObject::invokeMethod(
            meshDoc,
            [meshDoc, fine, sourceJson]() mutable {
              const auto idOpt = meshDoc->findMeshByExternalSource(sourceJson);
              if (!idOpt) {
                return;
              }
              meshDoc->replaceMeshGeometry(*idOpt, *fine);
            },
            Qt::QueuedConnection);
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
        info += ")      ";
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
