#include "zimgview.h"

#include "zmeshdoc.h"
#include "zpunctadoc.h"
#include "zskeletondoc.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancersegmentpropertiesdialog.h"

#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QFuture>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <type_traits>
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

struct NeuroglancerSkeletonLoadResult
{
  QString error;
  std::shared_ptr<ZSkeleton> skeleton;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
};

struct NeuroglancerAnnotationsSourceOpenResult
{
  QString error;
  std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> source;
  QStringList relationshipIds;
};

struct NeuroglancerAnnotationsLoadResult
{
  QString error;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
  std::shared_ptr<ZPuncta> puncta;
  std::shared_ptr<ZSkeleton> skeleton;
};

struct NeuroglancerMeshSourceKey
{
  QString rootUrl;
  QString meshSourceDirUrl;
  uint64_t segmentId = 0;
};

struct NeuroglancerSkeletonSourceKey
{
  QString rootUrl;
  QString skeletonSourceDirUrl;
  uint64_t segmentId = 0;
};

[[nodiscard]] QString neuroglancerMeshKeyString(const QString& rootUrl, const QString& meshSourceDirUrl, uint64_t segmentId)
{
  return QString("%1|%2|%3").arg(rootUrl).arg(meshSourceDirUrl).arg(static_cast<qulonglong>(segmentId));
}

[[nodiscard]] QString neuroglancerSkeletonKeyString(const QString& rootUrl, const QString& skeletonSourceDirUrl, uint64_t segmentId)
{
  return QString("%1|%2|%3").arg(rootUrl).arg(skeletonSourceDirUrl).arg(static_cast<qulonglong>(segmentId));
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

[[nodiscard]] QString formatAnnotationPropertyValue(
  const ZNeuroglancerPrecomputedAnnotationsSource::Annotation::PropertyValue& v)
{
  return std::visit(
    [](auto&& value) -> QString {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::array<uint8_t, 3>>) {
        return QString("#%1%2%3")
          .arg(value[0], 2, 16, QChar('0'))
          .arg(value[1], 2, 16, QChar('0'))
          .arg(value[2], 2, 16, QChar('0'))
          .toUpper();
      } else if constexpr (std::is_same_v<T, std::array<uint8_t, 4>>) {
        return QString("rgba(%1,%2,%3,%4)").arg(value[0]).arg(value[1]).arg(value[2]).arg(value[3]);
      } else if constexpr (std::is_same_v<T, float>) {
        return QString::number(static_cast<double>(value), 'g', 8);
      } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return QString::number(static_cast<qlonglong>(value));
      } else if constexpr (std::is_integral_v<T> && !std::is_signed_v<T>) {
        return QString::number(static_cast<qulonglong>(value));
      } else {
        static_assert(std::is_same_v<T, void>, "Unhandled annotation property value type");
        return {};
      }
    },
    v);
}

[[nodiscard]] std::optional<double> numericPropertyAsDouble(
  const ZNeuroglancerPrecomputedAnnotationsSource::Annotation::PropertyValue& v)
{
  return std::visit(
    [](auto&& value) -> std::optional<double> {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::array<uint8_t, 3>> || std::is_same_v<T, std::array<uint8_t, 4>>) {
        return std::nullopt;
      } else {
        return static_cast<double>(value);
      }
    },
    v);
}

void applyAnnotationPropertiesToPunctum(const ZNeuroglancerPrecomputedAnnotationsSource& source,
                                       const ZNeuroglancerPrecomputedAnnotationsSource::Annotation& ann,
                                       ZPunctum& punctum)
{
  const auto& specs = source.properties();
  if (specs.empty()) {
    return;
  }
  if (ann.propertyValues.size() != specs.size()) {
    return;
  }

  QStringList props;
  props.reserve(static_cast<int>(specs.size()));
  for (size_t i = 0; i < specs.size(); ++i) {
    const QString k = specs[i].id.trimmed();
    const QString v = formatAnnotationPropertyValue(ann.propertyValues[i]);
    props.push_back(QString("%1=%2").arg(k, v));

    if (k.compare(QStringLiteral("score"), Qt::CaseInsensitive) == 0) {
      if (const auto d = numericPropertyAsDouble(ann.propertyValues[i])) {
        punctum.setScore(*d);
      }
    }
  }

  if (!props.isEmpty()) {
    punctum.comment = props.join("; ").toStdString();
    punctum.property1 = props.value(0).toStdString();
    punctum.property2 = props.value(1).toStdString();
    punctum.property3 = props.value(2).toStdString();
  }
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

  QString meshSourceDirUrl;
  if (auto itUrl = o.find("mesh_source_url"); itUrl != o.end() && itUrl->value().is_string()) {
    meshSourceDirUrl = json::value_to<QString>(itUrl->value()).trimmed();
  } else if (auto itMeshKey = o.find("mesh_key"); itMeshKey != o.end() && itMeshKey->value().is_string()) {
    QString meshKey = json::value_to<QString>(itMeshKey->value()).trimmed();
    if (!meshKey.endsWith('/')) {
      meshKey += '/';
    }
    const QUrl meshDirUrl = QUrl(root).resolved(QUrl(meshKey));
    meshSourceDirUrl = meshDirUrl.toString(QUrl::StripTrailingSlash);
  }
  if (!meshSourceDirUrl.isEmpty() && !meshSourceDirUrl.endsWith('/')) {
    meshSourceDirUrl += '/';
  }
  if (meshSourceDirUrl.isEmpty()) {
    return std::nullopt;
  }

  NeuroglancerMeshSourceKey out;
  out.rootUrl = std::move(root);
  out.meshSourceDirUrl = std::move(meshSourceDirUrl);
  out.segmentId = *segOpt;
  return out;
}

[[nodiscard]] std::optional<NeuroglancerSkeletonSourceKey> parseNeuroglancerSkeletonSourceKey(const json::value& v)
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
  if (type != "neuroglancer_precomputed_skeleton") {
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

  QString skeletonSourceDirUrl;
  if (auto itUrl = o.find("skeleton_source_url"); itUrl != o.end() && itUrl->value().is_string()) {
    skeletonSourceDirUrl = json::value_to<QString>(itUrl->value()).trimmed();
  } else if (auto itKey = o.find("skeleton_key"); itKey != o.end() && itKey->value().is_string()) {
    QString skeletonKey = json::value_to<QString>(itKey->value()).trimmed();
    if (!skeletonKey.endsWith('/')) {
      skeletonKey += '/';
    }
    const QUrl skelDirUrl = QUrl(root).resolved(QUrl(skeletonKey));
    skeletonSourceDirUrl = skelDirUrl.toString(QUrl::StripTrailingSlash);
  }
  if (!skeletonSourceDirUrl.isEmpty() && !skeletonSourceDirUrl.endsWith('/')) {
    skeletonSourceDirUrl += '/';
  }
  if (skeletonSourceDirUrl.isEmpty()) {
    return std::nullopt;
  }

  NeuroglancerSkeletonSourceKey out;
  out.rootUrl = std::move(root);
  out.skeletonSourceDirUrl = std::move(skeletonSourceDirUrl);
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

ZImgView::~ZImgView()
{
  cancelNeuroglancerAnnotationsSpatialLoad(/*markCancelledTooltip=*/false);
}

void ZImgView::cancelNeuroglancerAnnotationsSpatialLoad(bool markCancelledTooltip)
{
  if (!m_ngAnnotationsSpatialCancel) {
    return;
  }

  // Mark cancelled first so worker threads stop scheduling any further updates.
  m_ngAnnotationsSpatialCancel->store(true, std::memory_order_relaxed);

  if (!markCancelledTooltip) {
    return;
  }
  if (m_ngAnnotationsSpatialCompleted) {
    return;
  }

  const QString status = QStringLiteral("cancelled (superseded by a newer request)");

  // Best-effort: if the placeholder object exists, update its tooltip to reflect cancellation.
  if (m_ngAnnotationsSpatialPunctaId && m_doc.doc().punctaDoc().hasObjWithID(*m_ngAnnotationsSpatialPunctaId)) {
    const size_t id = *m_ngAnnotationsSpatialPunctaId;
    const uint64_t count = static_cast<uint64_t>(m_doc.doc().punctaDoc().punctaPack(id).puncta().data.size());
    const auto& qMin = m_ngAnnotationsSpatialQMin;
    const auto& qMax = m_ngAnnotationsSpatialQMax;
    const QString tooltip = QString(
                              "Neuroglancer precomputed annotations (spatial)\n"
                              "Segmentation: %1\n"
                              "Annotations: %2\n"
                              "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                              "Status: %9\n"
                              "Count: %10")
                            .arg(m_ngAnnotationsSpatialSegRootUrl)
                            .arg(m_ngAnnotationsSpatialAnnRootUrl)
                            .arg(qMin[0], 0, 'g', 8)
                            .arg(qMin[1], 0, 'g', 8)
                            .arg(qMin[2], 0, 'g', 8)
                            .arg(qMax[0], 0, 'g', 8)
                            .arg(qMax[1], 0, 'g', 8)
                            .arg(qMax[2], 0, 'g', 8)
                            .arg(status)
                            .arg(static_cast<qulonglong>(count));
    m_doc.doc().punctaDoc().updateExternalPunctaMetadata(id, m_ngAnnotationsSpatialDisplayName, tooltip);
  }

  if (m_ngAnnotationsSpatialSkeletonId && m_doc.doc().skeletonDoc().hasObjWithID(*m_ngAnnotationsSpatialSkeletonId)) {
    const size_t id = *m_ngAnnotationsSpatialSkeletonId;
    const uint64_t segCount = static_cast<uint64_t>(m_doc.doc().skeletonDoc().skeleton(id).numEdges());
    const auto& qMin = m_ngAnnotationsSpatialQMin;
    const auto& qMax = m_ngAnnotationsSpatialQMax;
    const QString tooltip = QString(
                              "Neuroglancer precomputed annotations (spatial lines)\n"
                              "Segmentation: %1\n"
                              "Annotations: %2\n"
                              "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                              "Status: %9\n"
                              "Segments: %10")
                            .arg(m_ngAnnotationsSpatialSegRootUrl)
                            .arg(m_ngAnnotationsSpatialAnnRootUrl)
                            .arg(qMin[0], 0, 'g', 8)
                            .arg(qMin[1], 0, 'g', 8)
                            .arg(qMin[2], 0, 'g', 8)
                            .arg(qMax[0], 0, 'g', 8)
                            .arg(qMax[1], 0, 'g', 8)
                            .arg(qMax[2], 0, 'g', 8)
                            .arg(status)
                            .arg(static_cast<qulonglong>(segCount));
    m_doc.doc().skeletonDoc().updateExternalSkeletonMetadata(id, m_ngAnnotationsSpatialDisplayName, tooltip);
  }
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
    bool hasSkeletonDirectory = false;
    bool hasAnnotationsSource = false;
    bool hasSegmentPropertiesDirectory = false;
    QString meshSourceDirUrl;
    QString skeletonSourceDirUrl;
    QString annotationsSourceRootUrl;
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
    c.hasSegmentPropertiesDirectory = c.vol && c.vol->hasSegmentPropertiesDirectory();

    // External sources can either be declared in the dataset info (mesh/skeletons keys),
    // or explicitly registered by the user on the dataset object (override URL).
    c.meshSourceDirUrl = imgPack.neuroglancerMeshSourceOverrideUrl();
    if (c.meshSourceDirUrl.isEmpty() && c.vol && c.vol->hasMeshDirectory()) {
      c.meshSourceDirUrl = c.vol->meshDirUrl().toString(QUrl::StripTrailingSlash) + "/";
    }
    c.skeletonSourceDirUrl = imgPack.neuroglancerSkeletonSourceOverrideUrl();
    if (c.skeletonSourceDirUrl.isEmpty() && c.vol && c.vol->hasSkeletonDirectory()) {
      c.skeletonSourceDirUrl = c.vol->skeletonDirUrl().toString(QUrl::StripTrailingSlash) + "/";
    }

    c.hasMeshDirectory = !c.meshSourceDirUrl.isEmpty();
    c.hasSkeletonDirectory = !c.skeletonSourceDirUrl.isEmpty();

    // Annotations are stored in a separate precomputed dataset, so there is no volume-info fallback.
    c.annotationsSourceRootUrl = imgPack.neuroglancerAnnotationsSourceOverrideUrl();
    c.hasAnnotationsSource = !c.annotationsSourceRootUrl.trimmed().isEmpty();
    candidates.push_back(std::move(c));
  }
  if (candidates.empty()) {
    return;
  }

  const bool hasAnyMesh = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasMeshDirectory;
  });

  const bool hasAnySkeleton = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasSkeletonDirectory;
  });

  const bool hasAnyAnnotations = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasAnnotationsSource;
  });

  auto startAnnotationsLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString annRootUrl, uint64_t segmentId) {
    CHECK(vol);
    annRootUrl = annRootUrl.trimmed();
    CHECK(!annRootUrl.isEmpty());

    auto* openWatcher = new QFutureWatcher<NeuroglancerAnnotationsSourceOpenResult>(this);
    connect(openWatcher,
            &QFutureWatcher<NeuroglancerAnnotationsSourceOpenResult>::finished,
            this,
            [this, openWatcher, vol, segmentId, annRootUrl]() {
              const NeuroglancerAnnotationsSourceOpenResult r = openWatcher->result();
              openWatcher->deleteLater();

              if (!r.error.isEmpty()) {
                QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
                return;
              }
              if (!r.source) {
                QMessageBox::information(QApplication::activeWindow(),
                                         QApplication::applicationName(),
                                         QStringLiteral("Failed to open Neuroglancer annotations dataset."));
                return;
              }
              if (r.relationshipIds.isEmpty()) {
                QMessageBox::information(
                  QApplication::activeWindow(),
                  QApplication::applicationName(),
                  QStringLiteral("This Neuroglancer annotations dataset declares no relationships.\n\n"
                                 "Atlas currently supports relationship-index based loads (by segment/object ID)."));
                return;
              }

              QString relationshipId;
              if (r.relationshipIds.size() == 1) {
                relationshipId = r.relationshipIds.at(0);
              } else {
                bool ok = false;
                relationshipId = QInputDialog::getItem(QApplication::activeWindow(),
                                                       QApplication::applicationName(),
                                                       QStringLiteral("Select relationship id (used to map object IDs to annotations):"),
                                                       r.relationshipIds,
                                                       /*current=*/0,
                                                       /*editable=*/false,
                                                       &ok)
                                  .trimmed();
                if (!ok || relationshipId.isEmpty()) {
                  return;
                }
              }

              json::object sourceObj;
              sourceObj["type"] = "neuroglancer_precomputed_annotations";
              sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
              sourceObj["annotation_root_url"] = json::value_from(annRootUrl);
              sourceObj["relationship_id"] = json::value_from(relationshipId);
              sourceObj["object_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
              const json::value sourceJson = sourceObj;

              if (auto existing = m_doc.doc().punctaDoc().findPunctaByExternalSource(sourceJson)) {
                (void)existing;
                return;
              }
              if (auto existing = m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
                (void)existing;
                return;
              }

              auto* loadWatcher = new QFutureWatcher<NeuroglancerAnnotationsLoadResult>(this);
              connect(loadWatcher,
                      &QFutureWatcher<NeuroglancerAnnotationsLoadResult>::finished,
                      this,
                      [this, loadWatcher]() {
                        NeuroglancerAnnotationsLoadResult out = loadWatcher->result();
                        loadWatcher->deleteLater();

                        if (!out.error.isEmpty()) {
                          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), out.error);
                          return;
                        }

                        if (out.puncta) {
                          if (m_doc.doc().punctaDoc().findPunctaByExternalSource(out.sourceJson)) {
                            return;
                          }
                          (void)m_doc.doc().punctaDoc().addPunctaFromExternalSource(std::move(*out.puncta),
                                                                                    out.displayName,
                                                                                    out.tooltip,
                                                                                    out.sourceJson);
                          return;
                        }
                        if (out.skeleton) {
                          if (m_doc.doc().skeletonDoc().findSkeletonByExternalSource(out.sourceJson)) {
                            return;
                          }
                          (void)m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(*out.skeleton,
                                                                                        out.displayName,
                                                                                        out.tooltip,
                                                                                        out.sourceJson);
                          return;
                        }

                        QMessageBox::information(QApplication::activeWindow(),
                                                 QApplication::applicationName(),
                                                 QStringLiteral("No renderable annotation geometry was produced."));
                      });

              loadWatcher->setFuture(QtConcurrent::run([vol, segmentId, source = r.source, relationshipId, annRootUrl, sourceJson]() {
                NeuroglancerAnnotationsLoadResult out;
                out.sourceJson = sourceJson;
                try {
                  CHECK(vol);
                  CHECK(source);
                  const auto anns = source->loadAnnotationsForRelatedObjectBlocking(relationshipId, segmentId);
                  if (anns.empty()) {
                    out.error = QString("No annotations found for segment %1 (relationship '%2')")
                                  .arg(static_cast<qulonglong>(segmentId))
                                  .arg(relationshipId);
                    return out;
                  }

                  if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point ||
                      source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid) {
                    std::list<ZPunctum> pts;
                    for (const auto& a : anns) {
                      if (a.points.size() != 1) {
                        continue;
                      }
                      const auto& p = a.points[0];
                      if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid &&
                          a.ellipsoidRadiiVoxel) {
                        const glm::vec3 r = *a.ellipsoidRadiiVoxel;
                        const double rx = static_cast<double>(r.x);
                        const double ry = static_cast<double>(r.y);
                        const double rz = static_cast<double>(r.z);
                        const double rMax = std::max({rx, ry, rz});
                        ZPunctum punctum(p.x, p.y, p.z, /*r=*/rMax);
                        punctum.setRadii(rx, ry, rz);
                        punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
                        punctum.setMaxIntensity(255.0);
                        punctum.setMeanIntensity(255.0);
                        applyAnnotationPropertiesToPunctum(*source, a, punctum);
                        if (a.rgba8) {
                          const auto& c = *a.rgba8;
                          punctum.setColor(col4(c[0], c[1], c[2], c[3]));
                        }
                        pts.push_back(std::move(punctum));
                      } else {
                        ZPunctum punctum(p.x, p.y, p.z, /*r=*/2.0);
                      punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
                      punctum.setMaxIntensity(255.0);
                      punctum.setMeanIntensity(255.0);
                        applyAnnotationPropertiesToPunctum(*source, a, punctum);
                      if (a.rgba8) {
                        const auto& c = *a.rgba8;
                        punctum.setColor(col4(c[0], c[1], c[2], c[3]));
                      }
                      pts.push_back(std::move(punctum));
                      }
                    }
                    if (pts.empty()) {
                      out.error = QString("No POINT/ELLIPSOID annotations decoded for segment %1 (relationship '%2')")
                                    .arg(static_cast<qulonglong>(segmentId))
                                    .arg(relationshipId);
                      return out;
                    }
                    {
                      auto p = std::make_shared<ZPuncta>();
                      p->data = std::move(pts);
                      out.puncta = std::move(p);
                    }
                    out.displayName =
                      QString("NG Annotations %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(relationshipId);
                    out.tooltip =
                      QString("Neuroglancer precomputed annotations\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nCount: %5")
                        .arg(vol->rootUrl())
                        .arg(annRootUrl)
                        .arg(relationshipId)
                        .arg(static_cast<qulonglong>(segmentId))
                        .arg(static_cast<qulonglong>(out.puncta->data.size()));
                    return out;
                  }

                  if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line ||
                      source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Polyline) {
                    std::vector<glm::vec3> vertices;
                    std::vector<glm::uvec2> edges;
                    size_t numSegments = 0;
                    for (const auto& a : anns) {
                      if (a.points.size() < 2) {
                        continue;
                      }
                      const size_t base = vertices.size();
                      vertices.insert(vertices.end(), a.points.begin(), a.points.end());
                      for (size_t i = 0; i + 1 < a.points.size(); ++i) {
                        edges.emplace_back(static_cast<uint32_t>(base + i), static_cast<uint32_t>(base + i + 1));
                        ++numSegments;
                      }
                    }
                    if (vertices.empty() || edges.empty()) {
                      out.error = QString("No LINE/POLYLINE annotations decoded for segment %1 (relationship '%2')")
                                    .arg(static_cast<qulonglong>(segmentId))
                                    .arg(relationshipId);
                      return out;
                    }
                    auto skel = std::make_shared<ZSkeleton>();
                    skel->setVertices(std::move(vertices));
                    skel->setEdges(std::move(edges));
                    out.skeleton = std::move(skel);
                    out.displayName =
                      QString("NG Annotations %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(relationshipId);
                    out.tooltip =
                      QString("Neuroglancer precomputed annotations (lines)\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nSegments: %5")
                        .arg(vol->rootUrl())
                        .arg(annRootUrl)
                        .arg(relationshipId)
                        .arg(static_cast<qulonglong>(segmentId))
                        .arg(static_cast<qulonglong>(numSegments));
                    return out;
                  }

                  out.error = QString("Unsupported annotation_type for rendering: only POINT/LINE/POLYLINE are supported.");
                  return out;
                }
                catch (const ZNotFoundException&) {
                  out.error = QString("No annotations found for segment %1 (relationship '%2')")
                                .arg(static_cast<qulonglong>(segmentId))
                                .arg(relationshipId);
                  return out;
                }
                catch (const std::exception& e) {
                  out.error = QString("Failed to load neuroglancer annotations:\n%1").arg(QString::fromUtf8(e.what()));
                  return out;
                }
              }));
            });

    openWatcher->setFuture(QtConcurrent::run([vol, annRootUrl]() -> NeuroglancerAnnotationsSourceOpenResult {
      NeuroglancerAnnotationsSourceOpenResult out;
      try {
        CHECK(vol);
        std::array<double, 3> baseResNm{vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ};
        out.source = ZNeuroglancerPrecomputedAnnotationsSource::open(QUrl(annRootUrl), baseResNm, vol->baseVoxelOffset(), vol->defaultTimeout());
        CHECK(out.source);
        for (const auto& rel : out.source->relationships()) {
          if (!rel.id.trimmed().isEmpty()) {
            out.relationshipIds << rel.id;
          }
        }
      }
      catch (const std::exception& e) {
        out.error = QString("Failed to open neuroglancer annotations dataset:\n%1").arg(QString::fromUtf8(e.what()));
      }
      return out;
    }));
  };

  auto startAnnotationsSpatialLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol,
           QString annRootUrl,
           glm::dvec3 voxelMin,
           glm::dvec3 voxelMax) {
      CHECK(vol);
      annRootUrl = annRootUrl.trimmed();
      CHECK(!annRootUrl.isEmpty());

      const glm::dvec3 qMin{std::min(voxelMin.x, voxelMax.x),
                            std::min(voxelMin.y, voxelMax.y),
                            std::min(voxelMin.z, voxelMax.z)};
      const glm::dvec3 qMax{std::max(voxelMin.x, voxelMax.x),
                            std::max(voxelMin.y, voxelMax.y),
                            std::max(voxelMin.z, voxelMax.z)};

      json::object sourceObj;
      sourceObj["type"] = "neuroglancer_precomputed_annotations_spatial";
      sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
      sourceObj["annotation_root_url"] = json::value_from(annRootUrl);
      {
        json::array a;
        a.push_back(qMin.x);
        a.push_back(qMin.y);
        a.push_back(qMin.z);
        sourceObj["voxel_box_min"] = std::move(a);
      }
      {
        json::array a;
        a.push_back(qMax.x);
        a.push_back(qMax.y);
        a.push_back(qMax.z);
        sourceObj["voxel_box_max"] = std::move(a);
      }
      const json::value sourceJson = sourceObj;

      // If already loaded, do not re-fetch.
      if (m_doc.doc().punctaDoc().findPunctaByExternalSource(sourceJson) ||
          m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
        return;
      }

      // Cancel any in-flight spatial annotation load started from this view. If the prior request already
      // produced partial results, mark it as cancelled so users understand why it stopped updating.
      cancelNeuroglancerAnnotationsSpatialLoad(/*markCancelledTooltip=*/true);
      m_ngAnnotationsSpatialCancel = std::make_shared<std::atomic_bool>(false);
      const std::shared_ptr<std::atomic_bool> cancelFlag = m_ngAnnotationsSpatialCancel;
      QPointer<ZImgView> viewPtr(this);

      // Reset per-request UI bookkeeping.
      m_ngAnnotationsSpatialCompleted = false;
      m_ngAnnotationsSpatialPunctaId.reset();
      m_ngAnnotationsSpatialSkeletonId.reset();
      m_ngAnnotationsSpatialDisplayName.clear();
      m_ngAnnotationsSpatialSegRootUrl = vol->rootUrl();
      m_ngAnnotationsSpatialAnnRootUrl = annRootUrl;
      m_ngAnnotationsSpatialQMin = {qMin.x, qMin.y, qMin.z};
      m_ngAnnotationsSpatialQMax = {qMax.x, qMax.y, qMax.z};

      auto* openWatcher = new QFutureWatcher<NeuroglancerAnnotationsSourceOpenResult>(this);
      connect(openWatcher,
              &QFutureWatcher<NeuroglancerAnnotationsSourceOpenResult>::finished,
              this,
              [viewPtr, cancelFlag, openWatcher, vol, annRootUrl, qMin, qMax, sourceJson]() {
                const NeuroglancerAnnotationsSourceOpenResult r = openWatcher->result();
                openWatcher->deleteLater();

                if (!viewPtr || QCoreApplication::closingDown()) {
                  return;
                }
                if (!cancelFlag || cancelFlag != viewPtr->m_ngAnnotationsSpatialCancel ||
                    cancelFlag->load(std::memory_order_relaxed)) {
                  return;
                }

                if (!r.error.isEmpty()) {
                  QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
                  return;
                }
                if (!r.source) {
                  QMessageBox::information(QApplication::activeWindow(),
                                           QApplication::applicationName(),
                                           QStringLiteral("Failed to open neuroglancer annotations dataset."));
                  return;
                }
                if (r.source->spatialLevels().empty()) {
                  QMessageBox::information(
                    QApplication::activeWindow(),
                    QApplication::applicationName(),
                    QStringLiteral("This neuroglancer annotations dataset has no spatial index ('spatial' missing in info).\n\n"
                                   "Use the segment-based annotations load actions instead (relationship index)."));
                  return;
                }

                using AT = ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType;
                const AT type = r.source->annotationType();
                const QString displayName = QString("NG Annotations (View z≈%1)").arg(qMin.z, 0, 'g', 6);

                std::optional<size_t> punctaObjId;
                std::optional<size_t> skeletonObjId;

                if (type == AT::Point || type == AT::Ellipsoid) {
                  ZPuncta empty;
                  const QString tooltip = QString(
                                            "Neuroglancer precomputed annotations (spatial)\n"
                                            "Segmentation: %1\n"
                                            "Annotations: %2\n"
                                            "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                            "Status: loading…\n"
                                            "Count: 0")
                                          .arg(vol->rootUrl())
                                          .arg(annRootUrl)
                                          .arg(qMin.x, 0, 'g', 8)
                                          .arg(qMin.y, 0, 'g', 8)
                                          .arg(qMin.z, 0, 'g', 8)
                                          .arg(qMax.x, 0, 'g', 8)
                                          .arg(qMax.y, 0, 'g', 8)
                                          .arg(qMax.z, 0, 'g', 8);
                  punctaObjId = viewPtr->m_doc.doc().punctaDoc().addPunctaFromExternalSource(
                    std::move(empty), displayName, tooltip, sourceJson);
                } else if (type == AT::Line || type == AT::Polyline) {
                  ZSkeleton empty;
                  const QString tooltip = QString(
                                            "Neuroglancer precomputed annotations (spatial lines)\n"
                                            "Segmentation: %1\n"
                                            "Annotations: %2\n"
                                            "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                            "Status: loading…\n"
                                            "Segments: 0")
                                          .arg(vol->rootUrl())
                                          .arg(annRootUrl)
                                          .arg(qMin.x, 0, 'g', 8)
                                          .arg(qMin.y, 0, 'g', 8)
                                          .arg(qMin.z, 0, 'g', 8)
                                          .arg(qMax.x, 0, 'g', 8)
                                          .arg(qMax.y, 0, 'g', 8)
                                          .arg(qMax.z, 0, 'g', 8);
                  skeletonObjId = viewPtr->m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(
                    empty, displayName, tooltip, sourceJson);
                } else {
                  QMessageBox::information(
                    QApplication::activeWindow(),
                    QApplication::applicationName(),
                    QStringLiteral("Unsupported annotation_type for rendering: only POINT/ELLIPSOID or LINE/POLYLINE are supported."));
                  return;
                }

                if (!punctaObjId && !skeletonObjId) {
                  return;
                }

                viewPtr->m_ngAnnotationsSpatialDisplayName = displayName;
                viewPtr->m_ngAnnotationsSpatialPunctaId = punctaObjId;
                viewPtr->m_ngAnnotationsSpatialSkeletonId = skeletonObjId;

                // Stream spatial index results and apply incrementally as each batch becomes available.
                // This preserves progressive feedback (coarse-first due to spatial level ordering) and
                // avoids launching new requests after cancellation.
                [[maybe_unused]] const QFuture<void> bg = QtConcurrent::run([viewPtr,
                                                                            cancelFlag,
                                                                            source = r.source,
                                                                            vol,
                                                                            annRootUrl,
                                                                            qMin,
                                                                            qMax,
                                                                            displayName,
                                                                            punctaObjId,
                                                                            skeletonObjId,
                                                                            type]() {
                  try {
                    CHECK(source);
                    CHECK(vol);

                    source->streamAnnotationsIntersectingVoxelBoxBlocking(
                      qMin,
                      qMax,
                      [viewPtr,
                       cancelFlag,
                       vol,
                       annRootUrl,
                       qMin,
                       qMax,
                       displayName,
                       punctaObjId,
                       skeletonObjId,
                       type,
                       source](ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadUpdate update) {
                        if (!viewPtr) {
                          return;
                        }
                        if (!cancelFlag || cancelFlag->load(std::memory_order_relaxed)) {
                          return;
                        }

                        const auto progress = update.progress;

                        if (punctaObjId) {
                          auto pts = std::make_shared<std::list<ZPunctum>>();
                          if (!update.newAnnotations.empty()) {
                            for (const auto& a : update.newAnnotations) {
                              if (a.points.size() != 1) {
                                continue;
                              }
                              const auto& p = a.points[0];
                              if (type == AT::Ellipsoid && a.ellipsoidRadiiVoxel) {
                                const glm::vec3 r = *a.ellipsoidRadiiVoxel;
                                const double rx = static_cast<double>(r.x);
                                const double ry = static_cast<double>(r.y);
                                const double rz = static_cast<double>(r.z);
                                const double rMax = std::max({rx, ry, rz});
                                ZPunctum punctum(p.x, p.y, p.z, /*r=*/rMax);
                                punctum.setRadii(rx, ry, rz);
                                punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
                                punctum.setMaxIntensity(255.0);
                                punctum.setMeanIntensity(255.0);
                                applyAnnotationPropertiesToPunctum(*source, a, punctum);
                                if (a.rgba8) {
                                  const auto& c = *a.rgba8;
                                  punctum.setColor(col4(c[0], c[1], c[2], c[3]));
                                }
                                pts->push_back(std::move(punctum));
                              } else {
                                ZPunctum punctum(p.x, p.y, p.z, /*r=*/2.0);
                                punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
                                punctum.setMaxIntensity(255.0);
                                punctum.setMeanIntensity(255.0);
                                applyAnnotationPropertiesToPunctum(*source, a, punctum);
                                if (a.rgba8) {
                                  const auto& c = *a.rgba8;
                                  punctum.setColor(col4(c[0], c[1], c[2], c[3]));
                                }
                                pts->push_back(std::move(punctum));
                              }
                            }
                          }

                          QMetaObject::invokeMethod(
                            viewPtr,
                            [viewPtr, cancelFlag, displayName, vol, annRootUrl, qMin, qMax, progress, punctaObjId, pts]() {
                              if (!viewPtr || QCoreApplication::closingDown()) {
                                return;
                              }
                              if (!cancelFlag || cancelFlag != viewPtr->m_ngAnnotationsSpatialCancel ||
                                  cancelFlag->load(std::memory_order_relaxed)) {
                                return;
                              }
                              CHECK(punctaObjId);
                              const size_t id = *punctaObjId;
                              auto& doc = viewPtr->m_doc.doc().punctaDoc();
                              if (!doc.hasObjWithID(id)) {
                                cancelFlag->store(true, std::memory_order_relaxed);
                                return;
                              }

                              if (pts && !pts->empty()) {
                                doc.appendExternalPunctaNoUndo(id, std::move(*pts));
                              }

                              const uint64_t count = static_cast<uint64_t>(doc.punctaPack(id).puncta().data.size());
                              const bool done = (progress.levelsTotal > 0) &&
                                                (progress.levelIndex + 1 == progress.levelsTotal) &&
                                                (progress.visitedCells >= progress.totalCells);
                              viewPtr->m_ngAnnotationsSpatialCompleted = done;
                              const QString status = done ? QStringLiteral("done")
                                                          : QString("loading… %1/%2 cells (level %3/%4)")
                                                              .arg(static_cast<qulonglong>(progress.visitedCells))
                                                              .arg(static_cast<qulonglong>(progress.totalCells))
                                                              .arg(static_cast<qulonglong>(progress.levelIndex + 1))
                                                              .arg(static_cast<qulonglong>(progress.levelsTotal));
                              const QString tooltip = QString(
                                                        "Neuroglancer precomputed annotations (spatial)\n"
                                                        "Segmentation: %1\n"
                                                        "Annotations: %2\n"
                                                        "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                                        "Status: %9\n"
                                                        "Count: %10")
                                                      .arg(vol->rootUrl())
                                                      .arg(annRootUrl)
                                                      .arg(qMin.x, 0, 'g', 8)
                                                      .arg(qMin.y, 0, 'g', 8)
                                                      .arg(qMin.z, 0, 'g', 8)
                                                      .arg(qMax.x, 0, 'g', 8)
                                                      .arg(qMax.y, 0, 'g', 8)
                                                      .arg(qMax.z, 0, 'g', 8)
                                                      .arg(status)
                                                      .arg(static_cast<qulonglong>(count));

                              doc.updateExternalPunctaMetadata(id, displayName, tooltip);

                              if (done && count == 0) {
                                viewPtr->m_doc.doc().removeObj(id);
                                QMessageBox::information(
                                  QApplication::activeWindow(),
                                  QApplication::applicationName(),
                                  QStringLiteral("No annotations found intersecting the current view region."));
                              }
                            },
                            Qt::QueuedConnection);
                        } else if (skeletonObjId) {
                          auto geom = std::make_shared<std::pair<std::vector<glm::vec3>, std::vector<glm::uvec2>>>();
                          auto& verts = geom->first;
                          auto& edges = geom->second;

                          if (!update.newAnnotations.empty()) {
                            size_t base = 0;
                            for (const auto& a : update.newAnnotations) {
                              if (a.points.size() < 2) {
                                continue;
                              }
                              base = verts.size();
                              verts.insert(verts.end(), a.points.begin(), a.points.end());
                              for (size_t i = 0; i + 1 < a.points.size(); ++i) {
                                edges.emplace_back(static_cast<uint32_t>(base + i),
                                                   static_cast<uint32_t>(base + i + 1));
                              }
                            }
                          }

                          QMetaObject::invokeMethod(
                            viewPtr,
                            [viewPtr, cancelFlag, displayName, vol, annRootUrl, qMin, qMax, progress, skeletonObjId, geom]() {
                              if (!viewPtr || QCoreApplication::closingDown()) {
                                return;
                              }
                              if (!cancelFlag || cancelFlag != viewPtr->m_ngAnnotationsSpatialCancel ||
                                  cancelFlag->load(std::memory_order_relaxed)) {
                                return;
                              }
                              CHECK(skeletonObjId);
                              const size_t id = *skeletonObjId;
                              auto& doc = viewPtr->m_doc.doc().skeletonDoc();
                              if (!doc.hasObjWithID(id)) {
                                cancelFlag->store(true, std::memory_order_relaxed);
                                return;
                              }

                              if (geom && !geom->first.empty() && !geom->second.empty()) {
                                doc.appendExternalSkeletonGeometryNoUndo(id, std::move(geom->first), std::move(geom->second));
                              }

                              const uint64_t segCount = static_cast<uint64_t>(doc.skeleton(id).numEdges());
                              const bool done = (progress.levelsTotal > 0) &&
                                                (progress.levelIndex + 1 == progress.levelsTotal) &&
                                                (progress.visitedCells >= progress.totalCells);
                              viewPtr->m_ngAnnotationsSpatialCompleted = done;
                              const QString status = done ? QStringLiteral("done")
                                                          : QString("loading… %1/%2 cells (level %3/%4)")
                                                              .arg(static_cast<qulonglong>(progress.visitedCells))
                                                              .arg(static_cast<qulonglong>(progress.totalCells))
                                                              .arg(static_cast<qulonglong>(progress.levelIndex + 1))
                                                              .arg(static_cast<qulonglong>(progress.levelsTotal));
                              const QString tooltip = QString(
                                                        "Neuroglancer precomputed annotations (spatial lines)\n"
                                                        "Segmentation: %1\n"
                                                        "Annotations: %2\n"
                                                        "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                                        "Status: %9\n"
                                                        "Segments: %10")
                                                      .arg(vol->rootUrl())
                                                      .arg(annRootUrl)
                                                      .arg(qMin.x, 0, 'g', 8)
                                                      .arg(qMin.y, 0, 'g', 8)
                                                      .arg(qMin.z, 0, 'g', 8)
                                                      .arg(qMax.x, 0, 'g', 8)
                                                      .arg(qMax.y, 0, 'g', 8)
                                                      .arg(qMax.z, 0, 'g', 8)
                                                      .arg(status)
                                                      .arg(static_cast<qulonglong>(segCount));

                              doc.updateExternalSkeletonMetadata(id, displayName, tooltip);

                              if (done && segCount == 0) {
                                viewPtr->m_doc.doc().removeObj(id);
                                QMessageBox::information(
                                  QApplication::activeWindow(),
                                  QApplication::applicationName(),
                                  QStringLiteral("No annotations found intersecting the current view region."));
                              }
                            },
                            Qt::QueuedConnection);
                        }
                      },
                      cancelFlag.get());
                  }
                  catch (const std::exception& e) {
                    if (!cancelFlag || cancelFlag->load(std::memory_order_relaxed)) {
                      return;
                    }
                    const QString err = QString::fromUtf8(e.what());
                    QMetaObject::invokeMethod(
                      viewPtr,
                      [viewPtr, cancelFlag, displayName, punctaObjId, skeletonObjId, err]() {
                        if (!viewPtr || QCoreApplication::closingDown()) {
                          return;
                        }
                        if (!cancelFlag || cancelFlag != viewPtr->m_ngAnnotationsSpatialCancel ||
                            cancelFlag->load(std::memory_order_relaxed)) {
                          return;
                        }

                        // Best-effort cleanup of the placeholder object if it is still empty.
                        if (punctaObjId) {
                          auto& doc = viewPtr->m_doc.doc().punctaDoc();
                          if (doc.hasObjWithID(*punctaObjId)) {
                            if (doc.punctaPack(*punctaObjId).puncta().data.empty()) {
                              viewPtr->m_doc.doc().removeObj(*punctaObjId);
                            } else {
                              doc.updateExternalPunctaMetadata(
                                *punctaObjId, displayName, QStringLiteral("Failed to load annotations."));
                            }
                          }
                        }
                        if (skeletonObjId) {
                          auto& doc = viewPtr->m_doc.doc().skeletonDoc();
                          if (doc.hasObjWithID(*skeletonObjId)) {
                            if (doc.skeleton(*skeletonObjId).empty()) {
                              viewPtr->m_doc.doc().removeObj(*skeletonObjId);
                            } else {
                              doc.updateExternalSkeletonMetadata(
                                *skeletonObjId, displayName, QStringLiteral("Failed to load annotations."));
                            }
                          }
                        }

                        QMessageBox::information(
                          QApplication::activeWindow(),
                          QApplication::applicationName(),
                          QString("Failed to load neuroglancer annotations (spatial):\n%1").arg(err));
                      },
                      Qt::QueuedConnection);
                  }
                });
              });

      openWatcher->setFuture(QtConcurrent::run([vol, annRootUrl]() -> NeuroglancerAnnotationsSourceOpenResult {
        NeuroglancerAnnotationsSourceOpenResult out;
        try {
          CHECK(vol);
          std::array<double, 3> baseResNm{
            vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ};
          out.source = ZNeuroglancerPrecomputedAnnotationsSource::open(
            QUrl(annRootUrl), baseResNm, vol->baseVoxelOffset(), vol->defaultTimeout());
          CHECK(out.source);
          for (const auto& rel : out.source->relationships()) {
            if (!rel.id.trimmed().isEmpty()) {
              out.relationshipIds << rel.id;
            }
          }
        }
        catch (const std::exception& e) {
          out.error = QString("Failed to open neuroglancer annotations dataset:\n%1").arg(QString::fromUtf8(e.what()));
        }
        return out;
      }));
    };

  auto startMeshLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString meshSourceDirUrl, uint64_t segmentId) {
    CHECK(vol);
    meshSourceDirUrl = meshSourceDirUrl.trimmed();
    CHECK(!meshSourceDirUrl.isEmpty());

    json::object sourceObj;
    sourceObj["type"] = "neuroglancer_precomputed_mesh";
    sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
    sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
    sourceObj["mesh_source_url"] = json::value_from(meshSourceDirUrl);
    const json::value sourceJson = sourceObj;

    // If already loaded, do not re-fetch.
    if (m_doc.doc().meshDoc().findMeshByExternalSource(sourceJson)) {
      return;
    }

    auto* coarseWatcher = new QFutureWatcher<NeuroglancerMeshLoadResult>(this);
    connect(coarseWatcher,
            &QFutureWatcher<NeuroglancerMeshLoadResult>::finished,
            this,
            [this, coarseWatcher, vol, segmentId, meshSourceDirUrl]() {
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
                          VLOG(1) << fmt::format("Neuroglancer mesh refinement failed: {}", fine.error);
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

              fineWatcher->setFuture(
                QtConcurrent::run([vol, segmentId, coarse, meshSourceDirUrl]() -> NeuroglancerMeshLoadResult {
                NeuroglancerMeshLoadResult out;
                // Reuse metadata (sourceJson/displayName/tooltip) from coarse for consistency.
                out.sourceJson = coarse.sourceJson;
                out.displayName = coarse.displayName;
                out.tooltip = coarse.tooltip;
                try {
                  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source =
                    ZNeuroglancerPrecomputedMeshSource::open(QUrl(meshSourceDirUrl),
                                                            {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                            vol->baseVoxelOffset(),
                                                            vol->defaultTimeout());
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

    coarseWatcher->setFuture(
      QtConcurrent::run([vol, segmentId, sourceJson, meshSourceDirUrl]() -> NeuroglancerMeshLoadResult {
      NeuroglancerMeshLoadResult out;
      out.sourceJson = sourceJson;
      try {
        std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source =
          ZNeuroglancerPrecomputedMeshSource::open(QUrl(meshSourceDirUrl),
                                                  {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                  vol->baseVoxelOffset(),
                                                  vol->defaultTimeout());
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

  auto startMeshBatchLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString meshSourceDirUrl, std::vector<uint64_t> ids) {
    CHECK(vol);
    meshSourceDirUrl = meshSourceDirUrl.trimmed();
    CHECK(!meshSourceDirUrl.isEmpty());

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
      existingKeys.insert(
        neuroglancerMeshKeyString(keyOpt->rootUrl, keyOpt->meshSourceDirUrl, keyOpt->segmentId));
    }

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [watcher]() {
      const QString msg = watcher->result();
      watcher->deleteLater();
      if (!msg.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), msg);
      }
    });

    watcher->setFuture(QtConcurrent::run(
      [vol, meshDocPtr, ids = std::move(ids), existingKeys = std::move(existingKeys), meshSourceDirUrl]() mutable {
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
        source = ZNeuroglancerPrecomputedMeshSource::open(QUrl(meshSourceDirUrl),
                                                          {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                          vol->baseVoxelOffset(),
                                                          vol->defaultTimeout());
      }
      catch (const std::exception& e) {
        return QString("Failed to load neuroglancer mesh source: %1").arg(QString::fromUtf8(e.what()));
      }
      CHECK(source);

      const std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props = vol->segmentPropertiesShared();

      const QString rootUrl = vol->rootUrl();
      const QString meshSourceKey = meshSourceDirUrl;

      for (const uint64_t segmentId : ids) {
        if (QCoreApplication::closingDown() || !meshDocPtr) {
          break;
        }

        const QString keyStr = neuroglancerMeshKeyString(rootUrl, meshSourceKey, segmentId);
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
        sourceObj["mesh_source_url"] = json::value_from(meshSourceDirUrl);
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

  auto startSkeletonLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString skeletonSourceDirUrl, uint64_t segmentId) {
    CHECK(vol);
    skeletonSourceDirUrl = skeletonSourceDirUrl.trimmed();
    CHECK(!skeletonSourceDirUrl.isEmpty());

    json::object sourceObj;
    sourceObj["type"] = "neuroglancer_precomputed_skeleton";
    sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
    sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
    sourceObj["skeleton_source_url"] = json::value_from(skeletonSourceDirUrl);
    const json::value sourceJson = sourceObj;

    // If already loaded, do not re-fetch.
    if (m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
      return;
    }

    auto* watcher = new QFutureWatcher<NeuroglancerSkeletonLoadResult>(this);
    connect(watcher, &QFutureWatcher<NeuroglancerSkeletonLoadResult>::finished, this, [this, watcher, segmentId]() {
      const NeuroglancerSkeletonLoadResult r = watcher->result();
      watcher->deleteLater();

      if (!r.error.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
        return;
      }
      if (!r.skeleton || r.skeleton->empty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QString("Neuroglancer skeleton load returned an empty skeleton for segment %1")
                                   .arg(static_cast<qulonglong>(segmentId)));
        return;
      }

      // Avoid duplicates if another async task already added the same external-source skeleton.
      if (m_doc.doc().skeletonDoc().findSkeletonByExternalSource(r.sourceJson)) {
        return;
      }
      (void)m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(*r.skeleton, r.displayName, r.tooltip, r.sourceJson);
    });

    watcher->setFuture(
      QtConcurrent::run([vol, segmentId, sourceJson, skeletonSourceDirUrl]() -> NeuroglancerSkeletonLoadResult {
      NeuroglancerSkeletonLoadResult out;
      out.sourceJson = sourceJson;
      try {
        std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> source =
          ZNeuroglancerPrecomputedSkeletonSource::open(QUrl(skeletonSourceDirUrl),
                                                       {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                       vol->baseVoxelOffset(),
                                                       vol->defaultTimeout());
        CHECK(source);
        out.skeleton = source->loadSkeletonBlocking(segmentId);
        if (!out.skeleton || out.skeleton->empty()) {
          out.error = QString("Neuroglancer skeleton load returned an empty skeleton for segment %1")
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

        out.displayName = label.isEmpty() ? QString("NG Skeleton %1").arg(static_cast<qulonglong>(segmentId))
                                          : QString("NG Skeleton %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

        out.tooltip = QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2")
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
        out.error = QString("No neuroglancer skeleton found for segment %1").arg(static_cast<qulonglong>(segmentId));
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

  auto startSkeletonBatchLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString skeletonSourceDirUrl, std::vector<uint64_t> ids) {
    CHECK(vol);
    skeletonSourceDirUrl = skeletonSourceDirUrl.trimmed();
    CHECK(!skeletonSourceDirUrl.isEmpty());

    if (ids.empty()) {
      return;
    }

    // Dedup early and strip background ID 0 (it is typically "unlabeled" and rarely has a skeleton).
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
    if (ids.empty()) {
      return;
    }

    QPointer<ZSkeletonDoc> skeletonDocPtr = &m_doc.doc().skeletonDoc();
    CHECK(skeletonDocPtr);
    ZSkeletonDoc* skeletonDoc = skeletonDocPtr.data();

    std::set<QString> existingKeys;
    for (const size_t skelId : skeletonDoc->objs()) {
      const auto keyOpt = parseNeuroglancerSkeletonSourceKey(skeletonDoc->jsonValue(skelId));
      if (!keyOpt) {
        continue;
      }
      existingKeys.insert(
        neuroglancerSkeletonKeyString(keyOpt->rootUrl, keyOpt->skeletonSourceDirUrl, keyOpt->segmentId));
    }

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [watcher]() {
      const QString msg = watcher->result();
      watcher->deleteLater();
      if (!msg.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), msg);
      }
    });

    watcher->setFuture(QtConcurrent::run([vol,
                                          skeletonDocPtr,
                                          ids = std::move(ids),
                                          existingKeys = std::move(existingKeys),
                                          skeletonSourceDirUrl]() mutable {
      size_t loaded = 0;
      size_t missing = 0;
      size_t skipped = 0;
      size_t errors = 0;

      if (QCoreApplication::closingDown() || !skeletonDocPtr) {
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

      std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> source;
      try {
        source = ZNeuroglancerPrecomputedSkeletonSource::open(QUrl(skeletonSourceDirUrl),
                                                              {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                              vol->baseVoxelOffset(),
                                                              vol->defaultTimeout());
      }
      catch (const std::exception& e) {
        return QString("Failed to load neuroglancer skeleton source: %1").arg(QString::fromUtf8(e.what()));
      }
      CHECK(source);

      const std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props = vol->segmentPropertiesShared();

      const QString rootUrl = vol->rootUrl();
      const QString skeletonSourceKey = skeletonSourceDirUrl;

      for (const uint64_t segmentId : ids) {
        if (QCoreApplication::closingDown() || !skeletonDocPtr) {
          break;
        }

        const QString keyStr = neuroglancerSkeletonKeyString(rootUrl, skeletonSourceKey, segmentId);
        if (existingKeys.contains(keyStr)) {
          ++skipped;
          continue;
        }

        std::shared_ptr<ZSkeleton> skel;
        try {
          skel = source->loadSkeletonBlocking(segmentId);
        }
        catch (const ZNotFoundException&) {
          ++missing;
          continue;
        }
        catch (const std::exception& e) {
          ++errors;
          VLOG(1) << fmt::format("Failed to load neuroglancer skeleton for segment {}: {}", segmentId, e.what());
          continue;
        }

        if (!skel || skel->empty()) {
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
                                      ? QString("NG Skeleton %1").arg(static_cast<qulonglong>(segmentId))
                                      : QString("NG Skeleton %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

        QString tooltip = QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2")
                            .arg(rootUrl)
                            .arg(static_cast<qulonglong>(segmentId));
        if (!label.isEmpty()) {
          tooltip += QString("\nLabel: %1").arg(label);
        }
        if (!description.isEmpty()) {
          tooltip += QString("\nDescription: %1").arg(description);
        }

        json::object sourceObj;
        sourceObj["type"] = "neuroglancer_precomputed_skeleton";
        sourceObj["segmentation_root_url"] = json::value_from(rootUrl);
        sourceObj["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segmentId)));
        sourceObj["skeleton_source_url"] = json::value_from(skeletonSourceDirUrl);
        const json::value sourceJson = sourceObj;

        enqueueToUi([skeletonDocPtr, skel, displayName, tooltip, sourceJson]() mutable {
          if (QCoreApplication::closingDown() || !skeletonDocPtr) {
            return;
          }
          if (skeletonDocPtr->findSkeletonByExternalSource(sourceJson)) {
            return;
          }
          (void)skeletonDocPtr->addSkeletonFromExternalSource(*skel, displayName, tooltip, sourceJson);
        });

        existingKeys.insert(keyStr);
        ++loaded;
      }

      if (loaded == 0 && missing == 0 && skipped == 0 && errors == 0) {
        return QString{};
      }
      return QString("Neuroglancer skeleton batch load finished.\n\nLoaded: %1\nMissing skeleton: %2\nSkipped (already loaded): %3\nErrors: %4")
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

  if (!hasAnySkeleton) {
    auto* explainSkeletonAct =
      menu.addAction("Neuroglancer Skeletons: configure skeleton source in Object View Setting…");
    connect(explainSkeletonAct, &QAction::triggered, this, [candidates]() {
      std::set<QString> roots;
      for (const auto& c : candidates) {
        if (c.vol) {
          roots.insert(c.vol->rootUrl());
        }
      }

      QString rootList;
      for (const auto& r : roots) {
        rootList += QString("  %1\n").arg(r);
      }

      QMessageBox::information(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QStringLiteral("No Neuroglancer skeleton source is configured for the currently visible segmentation dataset(s).\n\n"
                       "To enable skeleton loading:\n"
                       "  1) Select the Neuroglancer segmentation object.\n"
                       "  2) Open Object View Setting → Neuroglancer Sources.\n"
                       "  3) Set \"Skeleton Source Override\" to a skeletons directory URL/path (absolute or relative).\n\n"
                       "Visible segmentation dataset(s):\n%1")
          .arg(rootList.trimmed()));
    });
  }

  if (!hasAnyAnnotations) {
    auto* explainAnnotationsAct =
      menu.addAction("Neuroglancer Annotations: configure annotations source in Object View Setting…");
    connect(explainAnnotationsAct, &QAction::triggered, this, [candidates]() {
      std::set<QString> roots;
      for (const auto& c : candidates) {
        if (c.vol) {
          roots.insert(c.vol->rootUrl());
        }
      }

      QString rootList;
      for (const auto& r : roots) {
        rootList += QString("  %1\n").arg(r);
      }

      QMessageBox::information(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QStringLiteral("No Neuroglancer annotations source is configured for the currently visible segmentation dataset(s).\n\n"
                       "To enable annotation loading:\n"
                       "  1) Select the Neuroglancer segmentation object.\n"
                       "  2) Open Object View Setting → Neuroglancer Sources.\n"
                       "  3) Set \"Annotations Source Override\" to an annotations dataset root URL/path (absolute or relative).\n\n"
                       "Visible segmentation dataset(s):\n%1")
          .arg(rootList.trimmed()));
    });
  }

  if (hasAnyMesh) {
    auto* loadMeshAct = menu.addAction("Load Neuroglancer Mesh for Segment Under Cursor");
    connect(loadMeshAct, &QAction::triggered, this, [scenePos, startMeshLoad, candidates]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestMeshSourceDirUrl;
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
          bestMeshSourceDirUrl = c.meshSourceDirUrl;
        }
      }
      if (!bestSeg || !bestVol || bestMeshSourceDirUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startMeshLoad(bestVol, bestMeshSourceDirUrl, *bestSeg);
    });
  }

  if (hasAnySkeleton) {
    auto* loadSkeletonAct = menu.addAction("Load Neuroglancer Skeleton for Segment Under Cursor");
    connect(loadSkeletonAct, &QAction::triggered, this, [scenePos, startSkeletonLoad, candidates]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestSkeletonSourceDirUrl;
      for (const auto& c : candidates) {
        if (!c.hasSkeletonDirectory) {
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
          bestSkeletonSourceDirUrl = c.skeletonSourceDirUrl;
        }
      }
      if (!bestSeg || !bestVol || bestSkeletonSourceDirUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startSkeletonLoad(bestVol, bestSkeletonSourceDirUrl, *bestSeg);
    });
  }

  if (hasAnyAnnotations) {
    auto* loadAnnUnderCursorAct = menu.addAction("Load Neuroglancer Annotations for Segment Under Cursor");
    connect(loadAnnUnderCursorAct, &QAction::triggered, this, [scenePos, candidates, startAnnotationsLoad]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestAnnotationsRootUrl;
      for (const auto& c : candidates) {
        if (!c.hasAnnotationsSource) {
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
          bestAnnotationsRootUrl = c.annotationsSourceRootUrl;
        }
      }
      if (!bestSeg || !bestVol || bestAnnotationsRootUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startAnnotationsLoad(bestVol, bestAnnotationsRootUrl, *bestSeg);
    });
  }

  // Choose the top-most (highest view precedence) visible segmentation layer for dataset-scoped actions.
  auto pickTopmostSegmentationCandidate = [&](auto wantsCandidate) -> const Candidate* {
    int bestPrecedence = std::numeric_limits<int>::min();
    const Candidate* best = nullptr;
    for (const auto& c : candidates) {
      if (!wantsCandidate(c)) {
        continue;
      }
      CHECK(c.filter);
      const int precedence = c.filter->viewPrecedence();
      if (!best || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        best = &c;
      }
    }
    return best;
  };

  const Candidate* topAnnotations = hasAnyAnnotations ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasAnnotationsSource;
  }) : nullptr;

  if (hasAnyAnnotations) {
    if (topAnnotations && topAnnotations->vol && !topAnnotations->annotationsSourceRootUrl.trimmed().isEmpty()) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topAnnotations->vol;
      const QString annotationsRootUrl = topAnnotations->annotationsSourceRootUrl;
      auto* loadAnnByIdAct =
        menu.addAction(QString("Load Neuroglancer Annotations for Segment ID… (%1)").arg(topVol->rootUrl()));
      connect(loadAnnByIdAct, &QAction::triggered, this, [topVol, annotationsRootUrl, startAnnotationsLoad]() {
        QString prefill;
        if (const auto clipOpt = parseUint64Base10(QApplication::clipboard()->text())) {
          prefill = QString::number(static_cast<qulonglong>(*clipOpt));
        }
        const QString s = QInputDialog::getText(QApplication::activeWindow(),
                                                QApplication::applicationName(),
                                                QStringLiteral("Neuroglancer segment/object ID (uint64, base-10):"),
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
                                   QStringLiteral("Invalid object ID. Expected a base-10 uint64 value."));
          return;
        }
        startAnnotationsLoad(topVol, annotationsRootUrl, *idOpt);
      });

      auto* loadAnnInViewAct =
        menu.addAction(QString("Load Neuroglancer Annotations in View (spatial index)… (%1)").arg(topVol->rootUrl()));
      connect(loadAnnInViewAct,
              &QAction::triggered,
              this,
              [this, topVol, annotationsRootUrl, startAnnotationsSpatialLoad]() {
                const QRectF viewport = m_view.currentViewport().normalized();
                const int z = m_view.currentSlice();

                // Query a thin slab corresponding to the current cross-section slice.
                const glm::dvec3 voxelMin{viewport.left(), viewport.top(), static_cast<double>(z)};
                const glm::dvec3 voxelMax{viewport.right(), viewport.bottom(), static_cast<double>(z + 1)};

                startAnnotationsSpatialLoad(topVol, annotationsRootUrl, voxelMin, voxelMax);
              });
    }
  }

  const Candidate* topMesh = hasAnyMesh ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasMeshDirectory;
  }) : nullptr;

  const Candidate* topSkeleton = hasAnySkeleton ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasSkeletonDirectory;
  }) : nullptr;

  if (hasAnyMesh) {
    if (topMesh && topMesh->vol) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topMesh->vol;
      const QString meshSourceDirUrl = topMesh->meshSourceDirUrl;
      auto* loadMeshByIdAct =
        menu.addAction(QString("Load Neuroglancer Mesh for Segment ID… (%1)").arg(topVol->rootUrl()));
    connect(loadMeshByIdAct, &QAction::triggered, this, [topVol, meshSourceDirUrl, startMeshLoad]() {
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
      startMeshLoad(topVol, meshSourceDirUrl, *idOpt);
    });

    auto* loadMeshesFromClipboardAct =
      menu.addAction(QString("Load Neuroglancer Meshes for Segment IDs from Clipboard… (%1)").arg(topVol->rootUrl()));
    connect(loadMeshesFromClipboardAct,
            &QAction::triggered,
            this,
            [topVol, meshSourceDirUrl, startMeshBatchLoad]() {
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

      startMeshBatchLoad(topVol, meshSourceDirUrl, std::vector<uint64_t>(unique.begin(), unique.end()));
    });

    auto* loadVisibleMeshesAct =
      menu.addAction(QString("Load Neuroglancer Meshes for Visible Segments (cached)… (%1)").arg(topVol->rootUrl()));
    connect(loadVisibleMeshesAct,
            &QAction::triggered,
            this,
            [this, topVol, meshSourceDirUrl, startMeshBatchLoad]() {
      CHECK(topVol);

      const QRectF viewport = m_view.currentViewport();
      const double scale = m_view.currentScale();
      const int z = m_view.currentSlice();

      auto* watcher = new QFutureWatcher<CollectVisibleSegIdsResult>(this);
      connect(watcher,
              &QFutureWatcher<CollectVisibleSegIdsResult>::finished,
              this,
              [watcher, topVol, meshSourceDirUrl, startMeshBatchLoad]() {
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

        startMeshBatchLoad(topVol, meshSourceDirUrl, r.ids);
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

  if (hasAnySkeleton) {
    if (topSkeleton && topSkeleton->vol) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topSkeleton->vol;
      const QString skeletonSourceDirUrl = topSkeleton->skeletonSourceDirUrl;
      auto* loadSkeletonByIdAct =
        menu.addAction(QString("Load Neuroglancer Skeleton for Segment ID… (%1)").arg(topVol->rootUrl()));
      connect(loadSkeletonByIdAct,
              &QAction::triggered,
              this,
              [topVol, skeletonSourceDirUrl, startSkeletonLoad]() {
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
        startSkeletonLoad(topVol, skeletonSourceDirUrl, *idOpt);
      });

      auto* loadSkeletonsFromClipboardAct = menu.addAction(
        QString("Load Neuroglancer Skeletons for Segment IDs from Clipboard… (%1)").arg(topVol->rootUrl()));
      connect(loadSkeletonsFromClipboardAct,
              &QAction::triggered,
              this,
              [topVol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
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
          QString("Load skeletons for %1 segment IDs from clipboard?\n\nThis will download skeleton data and may take time.")
            .arg(static_cast<qulonglong>(unique.size())),
          QMessageBox::Ok | QMessageBox::Cancel,
          QMessageBox::Cancel);
        if (response != QMessageBox::Ok) {
          return;
        }

        startSkeletonBatchLoad(topVol, skeletonSourceDirUrl, std::vector<uint64_t>(unique.begin(), unique.end()));
      });

      auto* loadVisibleSkeletonsAct = menu.addAction(
        QString("Load Neuroglancer Skeletons for Visible Segments (cached)… (%1)").arg(topVol->rootUrl()));
      connect(loadVisibleSkeletonsAct,
              &QAction::triggered,
              this,
              [this, topVol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
        CHECK(topVol);

        const QRectF viewport = m_view.currentViewport();
        const double scale = m_view.currentScale();
        const int z = m_view.currentSlice();

        auto* watcher = new QFutureWatcher<CollectVisibleSegIdsResult>(this);
        connect(
          watcher,
          &QFutureWatcher<CollectVisibleSegIdsResult>::finished,
          this,
          [watcher, topVol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
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
                               "This action only considers cached tiles at the target (final) LOD for the current viewport "
                               "scale and ignores coarse fallback tiles.\n\n"
                               "Wait for refinement to complete for the current zoom level, then try again."));
              return;
            }

            const int response = QMessageBox::question(
              QApplication::activeWindow(),
              QApplication::applicationName(),
              QString(
                "Load skeletons for %1 segments visible in the current viewport (cached tiles only)?\n\n"
                "This will download skeleton data and may take time.\n"
                "Note: segment ID 0 is treated as background and will be ignored.")
                .arg(static_cast<qulonglong>(r.ids.size())),
              QMessageBox::Ok | QMessageBox::Cancel,
              QMessageBox::Cancel);
            if (response != QMessageBox::Ok) {
              return;
            }

            startSkeletonBatchLoad(topVol, skeletonSourceDirUrl, r.ids);
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
                // Visible segment skeletons are defined as IDs from cached tiles at the "final" LOD for the
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
  auto pickTopmostSourceDirUrlForRoot = [&](const QString& rootUrl, auto getUrl) -> QString {
    int bestPrecedence = std::numeric_limits<int>::min();
    QString bestUrl;
    for (const auto& c : candidates) {
      if (!c.vol || c.vol->rootUrl() != rootUrl) {
        continue;
      }
      const QString url = getUrl(c).trimmed();
      if (url.isEmpty()) {
        continue;
      }
      CHECK(c.filter);
      const int precedence = c.filter->viewPrecedence();
      if (bestUrl.isEmpty() || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestUrl = url;
      }
    }
    return bestUrl;
  };

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
    const QString meshSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.meshSourceDirUrl; });

    auto* showPropsAct = menu.addAction(QString("Show Neuroglancer Segment Properties… (%1)").arg(vol->rootUrl()));
    connect(showPropsAct,
            &QAction::triggered,
            this,
            [meshDocPtr = QPointer<ZMeshDoc>(&m_doc.doc().meshDoc()),
             skeletonDocPtr = QPointer<ZSkeletonDoc>(&m_doc.doc().skeletonDoc()),
             vol,
             meshSourceDirUrl,
             startMeshLoad]() {
      auto* dlg = new ZNeuroglancerSegmentPropertiesDialog(vol, QApplication::activeWindow());
      dlg->setAttribute(Qt::WA_DeleteOnClose, true);
      if (!meshSourceDirUrl.isEmpty()) {
        dlg->setMeshLoadCallback([startMeshLoad, vol, meshSourceDirUrl](uint64_t id) {
          startMeshLoad(vol, meshSourceDirUrl, id);
        });
      }
      dlg->setAfterPropertiesLoadedCallback([meshDocPtr, skeletonDocPtr, propsRootUrl = vol->rootUrl()](auto props) {
        if (QCoreApplication::closingDown() || !props) {
          return;
        }

        if (meshDocPtr) {
          for (const size_t meshId : meshDocPtr->objs()) {
            const auto srcOpt = parseNeuroglancerMeshSourceKey(meshDocPtr->jsonValue(meshId));
            if (!srcOpt) {
              continue;
            }
            if (srcOpt->rootUrl != propsRootUrl) {
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
        }

        if (skeletonDocPtr) {
          for (const size_t skelId : skeletonDocPtr->objs()) {
            const auto srcOpt = parseNeuroglancerSkeletonSourceKey(skeletonDocPtr->jsonValue(skelId));
            if (!srcOpt) {
              continue;
            }
            if (srcOpt->rootUrl != propsRootUrl) {
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

            const QString displayName =
              label.isEmpty() ? QString("NG Skeleton %1").arg(static_cast<qulonglong>(segmentId))
                              : QString("NG Skeleton %1 (%2)").arg(static_cast<qulonglong>(segmentId)).arg(label);

            QString tooltip = QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2")
                                .arg(propsRootUrl)
                                .arg(static_cast<qulonglong>(segmentId));
            if (!label.isEmpty()) {
              tooltip += QString("\nLabel: %1").arg(label);
            }
            if (!description.isEmpty()) {
              tooltip += QString("\nDescription: %1").arg(description);
            }

            skeletonDocPtr->updateExternalSkeletonMetadata(skelId, displayName, tooltip);
          }
        }
      });
      dlg->open();
    });
  }

  for (const auto& vol : volsWithSegProps) {
    const QString meshSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.meshSourceDirUrl; });
    if (meshSourceDirUrl.isEmpty()) {
      continue;
    }
    auto* loadAllMeshesAct = menu.addAction(
      QString("Load Neuroglancer Meshes for All Segments (segment_properties)… (%1)").arg(vol->rootUrl()));
    connect(loadAllMeshesAct, &QAction::triggered, this, [this, vol, meshSourceDirUrl, startMeshBatchLoad]() {
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

      auto* watcher = new QFutureWatcher<CollectVisibleSegIdsResult>(this);
      connect(
        watcher,
        &QFutureWatcher<CollectVisibleSegIdsResult>::finished,
        this,
        [watcher, vol, meshSourceDirUrl, startMeshBatchLoad]() {
          const CollectVisibleSegIdsResult r = watcher->result();
          watcher->deleteLater();
          if (!r.error.isEmpty()) {
            QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
            return;
          }
          if (r.ids.empty()) {
            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("segment_properties returned an empty ID list."));
            return;
          }
          startMeshBatchLoad(vol, meshSourceDirUrl, r.ids);
        });

      watcher->setFuture(QtConcurrent::run([vol]() -> CollectVisibleSegIdsResult {
        CollectVisibleSegIdsResult out;
        try {
          const auto props = vol->loadSegmentPropertiesBlocking();
          CHECK(props);
          out.ids = props->ids();
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

  for (const auto& vol : volsWithSegProps) {
    const QString skeletonSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.skeletonSourceDirUrl; });
    if (skeletonSourceDirUrl.isEmpty()) {
      continue;
    }
    auto* loadAllSkeletonsAct = menu.addAction(
      QString("Load Neuroglancer Skeletons for All Segments (segment_properties)… (%1)").arg(vol->rootUrl()));
    connect(loadAllSkeletonsAct,
            &QAction::triggered,
            this,
            [this, vol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
      const auto response = QMessageBox::question(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QString(
          "Load skeletons for all segments listed in segment_properties?\n\nThis will download segment properties (if not cached) and may take a long time and consume significant memory."),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);
      if (response != QMessageBox::Ok) {
        return;
      }

      auto* watcher = new QFutureWatcher<CollectVisibleSegIdsResult>(this);
      connect(watcher,
              &QFutureWatcher<CollectVisibleSegIdsResult>::finished,
              this,
              [watcher, vol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
        const CollectVisibleSegIdsResult r = watcher->result();
        watcher->deleteLater();
        if (!r.error.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
          return;
        }
        if (r.ids.empty()) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("segment_properties returned an empty ID list."));
          return;
        }
        startSkeletonBatchLoad(vol, skeletonSourceDirUrl, r.ids);
      });

      watcher->setFuture(QtConcurrent::run([vol]() -> CollectVisibleSegIdsResult {
        CollectVisibleSegIdsResult out;
        try {
          const auto props = vol->loadSegmentPropertiesBlocking();
          CHECK(props);
          out.ids = props->ids();
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
    connect(viewControl,
            &ZImgFilter::neuroglancerDatasetUnsupported,
            this,
            [this, id](const QString& /*datasetUrl*/, const QString& /*error*/) {
              // Remove the object via ZDoc so the object manager stays consistent.
              // Use a queued connection to avoid deleting the sender while it is still processing callbacks.
              if (!m_doc.hasObjWithID(id)) {
                return;
              }
              m_doc.doc().removeObj(id);
            },
            Qt::QueuedConnection);
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
  connect(viewControl,
          &ZImgFilter::neuroglancerDatasetUnsupported,
          this,
          [this, id](const QString& /*datasetUrl*/, const QString& /*error*/) {
            if (!m_doc.hasObjWithID(id)) {
              return;
            }
            m_doc.doc().removeObj(id);
          },
          Qt::QueuedConnection);
  Q_EMIT objViewReady(id);
}

void ZImgView::docImgChanged(size_t id)
{
  m_idToFilter.at(id)->setData(m_doc.imgPack(id));
}

} // namespace nim
