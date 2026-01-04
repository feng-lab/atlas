#include "zimgview.h"

#include "zmeshdoc.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedsegmentproperties.h"

#include <QMessageBox>
#include <QApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

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
    connect(viewControl, &ZImgFilter::requestNeuroglancerMeshLoad, this, [this, id](uint64_t segmentId, bool finestLod) {
      if (!m_doc.hasObjWithID(id)) {
        return;
      }
      const ZImgPack& imgPack = m_doc.imgPack(id);
      if (!imgPack.isNeuroglancerPrecomputed() || !imgPack.neuroglancerVolumeShared()->isSegmentation()) {
        return;
      }

      std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
      const auto lodPolicy = finestLod ? ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest
                                       : ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest;

      auto* watcher = new QFutureWatcher<NeuroglancerMeshLoadResult>(this);
      connect(watcher, &QFutureWatcher<NeuroglancerMeshLoadResult>::finished, this, [this, watcher]() {
        const NeuroglancerMeshLoadResult result = watcher->result();
        watcher->deleteLater();

        if (!result.error.isEmpty()) {
          QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), result.error);
          return;
        }
        if (!result.mesh) {
          return;
        }
        m_doc.doc().meshDoc().addMeshFromExternalSource(*result.mesh, result.displayName, result.tooltip, result.sourceJson);
      });

      watcher->setFuture(QtConcurrent::run([vol, segmentId, lodPolicy, finestLod]() -> NeuroglancerMeshLoadResult {
        NeuroglancerMeshLoadResult out;
        try {
          std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source = vol->loadMeshSourceBlocking();
          CHECK(source);
          out.mesh = source->loadMeshBlocking(segmentId, lodPolicy);
          if (!out.mesh || out.mesh->empty()) {
            out.error = QString("Neuroglancer mesh load returned an empty mesh for segment %1").arg(static_cast<qulonglong>(segmentId));
            return out;
          }

          json::object jo;
          jo["type"] = "neuroglancer_precomputed_mesh";
          jo["segmentation_root_url"] = json::value_from(vol->rootUrl());
          jo["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
          jo["lod_policy"] = json::value_from(finestLod ? "finest" : "coarsest");
          out.sourceJson = jo;

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

          out.displayName =
            label.isEmpty() ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
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
        catch (const ZException& e) {
          out.error = QString::fromUtf8(e.what());
        }
        catch (const std::exception& e) {
          out.error = QString::fromUtf8(e.what());
        }
        return out;
      }));
    });
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
  connect(viewControl, &ZImgFilter::requestNeuroglancerMeshLoad, this, [this, id](uint64_t segmentId, bool finestLod) {
    if (!m_doc.hasObjWithID(id)) {
      return;
    }
    const ZImgPack& imgPack = m_doc.imgPack(id);
    if (!imgPack.isNeuroglancerPrecomputed() || !imgPack.neuroglancerVolumeShared()->isSegmentation()) {
      return;
    }

    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
    const auto lodPolicy = finestLod ? ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest
                                     : ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest;

    auto* watcher = new QFutureWatcher<NeuroglancerMeshLoadResult>(this);
    connect(watcher, &QFutureWatcher<NeuroglancerMeshLoadResult>::finished, this, [this, watcher]() {
      const NeuroglancerMeshLoadResult result = watcher->result();
      watcher->deleteLater();

      if (!result.error.isEmpty()) {
        QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), result.error);
        return;
      }
      if (!result.mesh) {
        return;
      }
      m_doc.doc().meshDoc().addMeshFromExternalSource(*result.mesh, result.displayName, result.tooltip, result.sourceJson);
    });

    watcher->setFuture(QtConcurrent::run([vol, segmentId, lodPolicy, finestLod]() -> NeuroglancerMeshLoadResult {
      NeuroglancerMeshLoadResult out;
      try {
        std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source = vol->loadMeshSourceBlocking();
        CHECK(source);
        out.mesh = source->loadMeshBlocking(segmentId, lodPolicy);
        if (!out.mesh || out.mesh->empty()) {
          out.error = QString("Neuroglancer mesh load returned an empty mesh for segment %1").arg(static_cast<qulonglong>(segmentId));
          return out;
        }

        json::object jo;
        jo["type"] = "neuroglancer_precomputed_mesh";
        jo["segmentation_root_url"] = json::value_from(vol->rootUrl());
        jo["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
        jo["lod_policy"] = json::value_from(finestLod ? "finest" : "coarsest");
        out.sourceJson = jo;

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

        out.displayName =
          label.isEmpty() ? QString("NG Mesh %1").arg(static_cast<qulonglong>(segmentId))
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
      catch (const ZException& e) {
        out.error = QString::fromUtf8(e.what());
      }
      catch (const std::exception& e) {
        out.error = QString::fromUtf8(e.what());
      }
      return out;
    }));
  });
  Q_EMIT objViewReady(id);
}

void ZImgView::docImgChanged(size_t id)
{
  m_idToFilter.at(id)->setData(m_doc.imgPack(id));
}

} // namespace nim
