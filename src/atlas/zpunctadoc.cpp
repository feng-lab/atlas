#include "zpunctadoc.h"

#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zpunctadetectiondialog.h"
#include "zimgdoc.h"
#include "zanalysisworklistdialog.h"
#include "ztheme.h"
#include "zpunctawidget.h"
#include "zmessageboxhelpers.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>

namespace nim {

namespace {

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

[[nodiscard]] std::optional<double> jsonNumberAsDouble(const json::value& v)
{
  if (v.is_double()) {
    return v.as_double();
  }
  if (v.is_int64()) {
    return static_cast<double>(v.as_int64());
  }
  if (v.is_uint64()) {
    return static_cast<double>(v.as_uint64());
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<glm::dvec3> jsonArray3AsDvec3(const json::value& v)
{
  if (!v.is_array()) {
    return std::nullopt;
  }
  const auto& a = v.as_array();
  if (a.size() != 3) {
    return std::nullopt;
  }
  const auto x = jsonNumberAsDouble(a[0]);
  const auto y = jsonNumberAsDouble(a[1]);
  const auto z = jsonNumberAsDouble(a[2]);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::dvec3{*x, *y, *z};
}

} // namespace

ZPunctaDoc::ZPunctaDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZPunctaDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToPunctaPacks.at(id);
  if (ZPuncta::canWriteFile(pack->path())) {
    QString err;
    if (savePuncta(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save puncta %1").arg(pack->path()), err);
    return false;
  }
  return saveAs(id);
}

bool ZPunctaDoc::saveAs(size_t id)
{
  QStringList filters;
  QStringList formats;
  ZPuncta::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Puncta %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToPunctaPacks.at(id);
    const QString targetPath = dialog.selectedFiles().at(0);
    if (auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
        savePuncta(pack.get(), targetPath, err, formats[fmtIdx])) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save puncta %1").arg(targetPath), err);
  }
  return false;
}

bool ZPunctaDoc::canReadFile(const QString& fileName) const
{
  return ZPuncta::canReadFile(fileName);
}

size_t ZPunctaDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& [id, pack] : m_idToPunctaPacks) {
    if (pack->path() == fileName) {
      return id;
    }
  }
  try {
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZPunctaDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (jValue.is_object()) {
      constexpr std::chrono::milliseconds kDefaultTimeout{30000};

      const auto& jo = jValue.as_object();
      auto typeIt = jo.find("type");
      if (typeIt == jo.end() || !typeIt->value().is_string()) {
        errorMsg = QString("Invalid puncta JSON: missing string field 'type'");
        return 0;
      }
      const QString type = json::value_to<QString>(typeIt->value()).trimmed();
      if (type != "neuroglancer_precomputed_annotations" && type != "neuroglancer_precomputed_annotations_spatial") {
        errorMsg = QString("Unsupported puncta JSON type '%1'").arg(type);
        return 0;
      }

      auto rootIt = jo.find("segmentation_root_url");
      if (rootIt == jo.end() || !rootIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'segmentation_root_url'");
        return 0;
      }
      const QString normalizedSegRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(rootIt->value()));

      auto annIt = jo.find("annotation_root_url");
      if (annIt == jo.end() || !annIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'annotation_root_url'");
        return 0;
      }
      const QString normalizedAnnRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(annIt->value()));

      if (type == "neuroglancer_precomputed_annotations") {
        auto relIt = jo.find("relationship_id");
        if (relIt == jo.end() || !relIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'relationship_id'");
          return 0;
        }
        const QString relationshipId = json::value_to<QString>(relIt->value()).trimmed();
        if (relationshipId.isEmpty()) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: relationship_id must be non-empty");
          return 0;
        }

        auto objIt = jo.find("object_id");
        if (objIt == jo.end() || !objIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'object_id'");
          return 0;
        }
        const QString objStr = json::value_to<QString>(objIt->value()).trimmed();
        bool ok = false;
        const qulonglong objIdQt = objStr.toULongLong(&ok, 10);
        if (!ok) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: object_id must be base-10 uint64");
          return 0;
        }
        const uint64_t objectId = static_cast<uint64_t>(objIdQt);

        // Normalize persisted JSON to keep it stable across save/load cycles.
        json::object normalized;
        normalized["type"] = "neuroglancer_precomputed_annotations";
        normalized["segmentation_root_url"] = json::value_from(normalizedSegRootUrl);
        normalized["annotation_root_url"] = json::value_from(normalizedAnnRootUrl);
        normalized["relationship_id"] = json::value_from(relationshipId);
        normalized["object_id"] = json::value_from(QString::number(static_cast<qulonglong>(objectId)));
        const json::value sourceJson = normalized;

        if (const auto existing = findPunctaByExternalSource(sourceJson)) {
          return *existing;
        }

        std::shared_ptr<ZNeuroglancerPrecomputedVolume> segVol =
          ZNeuroglancerPrecomputedVolume::open(normalizedSegRootUrl, kDefaultTimeout);
        CHECK(segVol);

        std::array<double, 3> baseResNm{segVol->baseImgInfo().voxelSizeX,
                                        segVol->baseImgInfo().voxelSizeY,
                                        segVol->baseImgInfo().voxelSizeZ};
        auto source = ZNeuroglancerPrecomputedAnnotationsSource::open(
          QUrl(normalizedAnnRootUrl), baseResNm, segVol->baseVoxelOffset(), kDefaultTimeout);
        CHECK(source);

        if (source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point &&
            source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid) {
          errorMsg = QString("Unsupported neuroglancer annotations type for puncta: expected POINT or ELLIPSOID");
          return 0;
        }

        const auto anns = source->loadAnnotationsForRelatedObjectBlocking(relationshipId, objectId);
        if (anns.empty()) {
          errorMsg = QString("No annotations found for object %1 (relationship '%2')")
                       .arg(static_cast<qulonglong>(objectId))
                       .arg(relationshipId);
          return 0;
        }

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
          errorMsg = QString("No POINT/ELLIPSOID annotations decoded for object %1 (relationship '%2')")
                       .arg(static_cast<qulonglong>(objectId))
                       .arg(relationshipId);
          return 0;
        }

        const QString displayName =
          QString("NG Annotations %1 (%2)").arg(static_cast<qulonglong>(objectId)).arg(relationshipId);
        const QString tooltip =
          QString(
            "Neuroglancer precomputed annotations\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nCount: %5")
            .arg(normalizedSegRootUrl)
            .arg(normalizedAnnRootUrl)
            .arg(relationshipId)
            .arg(static_cast<qulonglong>(objectId))
            .arg(static_cast<qulonglong>(pts.size()));

        ZPuncta puncta;
        puncta.data = std::move(pts);
        return addPunctaFromExternalSource(std::move(puncta), displayName, tooltip, sourceJson);
      }

      CHECK(type == QStringLiteral("neuroglancer_precomputed_annotations_spatial"));

      auto minIt = jo.find("voxel_box_min");
      auto maxIt = jo.find("voxel_box_max");
      if (minIt == jo.end() || maxIt == jo.end()) {
        errorMsg = QString("Invalid neuroglancer annotations spatial JSON: missing 'voxel_box_min'/'voxel_box_max'");
        return 0;
      }
      const auto minOpt = jsonArray3AsDvec3(minIt->value());
      const auto maxOpt = jsonArray3AsDvec3(maxIt->value());
      if (!minOpt || !maxOpt) {
        errorMsg = QString("Invalid neuroglancer annotations spatial JSON: voxel_box_min/max must be numeric arrays of length 3");
        return 0;
      }
      const glm::dvec3 qMin{std::min(minOpt->x, maxOpt->x),
                            std::min(minOpt->y, maxOpt->y),
                            std::min(minOpt->z, maxOpt->z)};
      const glm::dvec3 qMax{std::max(minOpt->x, maxOpt->x),
                            std::max(minOpt->y, maxOpt->y),
                            std::max(minOpt->z, maxOpt->z)};
      for (int d = 0; d < 3; ++d) {
        const double lo = (&qMin.x)[d];
        const double hi = (&qMax.x)[d];
        if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi >= lo)) {
          errorMsg = QString("Invalid neuroglancer annotations spatial JSON: voxel_box_min/max must be finite and ordered");
          return 0;
        }
      }

      // Normalize persisted JSON to keep it stable across save/load cycles.
      json::object normalized;
      normalized["type"] = "neuroglancer_precomputed_annotations_spatial";
      normalized["segmentation_root_url"] = json::value_from(normalizedSegRootUrl);
      normalized["annotation_root_url"] = json::value_from(normalizedAnnRootUrl);
      {
        json::array a;
        a.push_back(qMin.x);
        a.push_back(qMin.y);
        a.push_back(qMin.z);
        normalized["voxel_box_min"] = std::move(a);
      }
      {
        json::array a;
        a.push_back(qMax.x);
        a.push_back(qMax.y);
        a.push_back(qMax.z);
        normalized["voxel_box_max"] = std::move(a);
      }
      const json::value sourceJson = normalized;

      if (const auto existing = findPunctaByExternalSource(sourceJson)) {
        return *existing;
      }

      std::shared_ptr<ZNeuroglancerPrecomputedVolume> segVol =
        ZNeuroglancerPrecomputedVolume::open(normalizedSegRootUrl, kDefaultTimeout);
      CHECK(segVol);

      std::array<double, 3> baseResNm{segVol->baseImgInfo().voxelSizeX,
                                      segVol->baseImgInfo().voxelSizeY,
                                      segVol->baseImgInfo().voxelSizeZ};
      auto source = ZNeuroglancerPrecomputedAnnotationsSource::open(
        QUrl(normalizedAnnRootUrl), baseResNm, segVol->baseVoxelOffset(), kDefaultTimeout);
      CHECK(source);

      if (source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point &&
          source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid) {
        errorMsg = QString("Unsupported neuroglancer annotations type for puncta: expected POINT or ELLIPSOID");
        return 0;
      }

      const auto anns = source->loadAnnotationsIntersectingVoxelBoxBlocking(qMin, qMax);
      if (anns.empty()) {
        errorMsg = QString("No annotations found intersecting the saved spatial query box.");
        return 0;
      }

      std::list<ZPunctum> pts;
      for (const auto& a : anns) {
        if (a.points.size() != 1) {
          continue;
        }
        const auto& p = a.points[0];
        if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid && a.ellipsoidRadiiVoxel) {
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
        errorMsg = QString("No POINT/ELLIPSOID annotations decoded for the saved spatial query box.");
        return 0;
      }

      const QString displayName = QString("NG Annotations (View z≈%1)").arg(qMin.z, 0, 'g', 6);
      const QString tooltip = QString("Neuroglancer precomputed annotations (spatial)\n"
                                      "Segmentation: %1\n"
                                      "Annotations: %2\n"
                                      "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                      "Count: %9")
                                .arg(normalizedSegRootUrl)
                                .arg(normalizedAnnRootUrl)
                                .arg(qMin.x, 0, 'g', 8)
                                .arg(qMin.y, 0, 'g', 8)
                                .arg(qMin.z, 0, 'g', 8)
                                .arg(qMax.x, 0, 'g', 8)
                                .arg(qMax.y, 0, 'g', 8)
                                .arg(qMax.z, 0, 'g', 8)
                                .arg(static_cast<qulonglong>(pts.size()));

      ZPuncta puncta;
      puncta.data = std::move(pts);
      return addPunctaFromExternalSource(std::move(puncta), displayName, tooltip, sourceJson);
    }

    if (!jValue.is_string() || asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }

    for (const auto& [id, pack] : m_idToPunctaPacks) {
      if (isSameObj(jValue, jsonValue(id))) {
        return id;
      }
    }

    const QString fileName = asQString(jValue);
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

bool ZPunctaDoc::canPrepareLoadAsync(const json::value& jValue) const
{
  // Only async-prepare local file loads. External-source puncta (e.g. Neuroglancer)
  // may involve network IO and doc cross-references, and are kept synchronous.
  if (!jValue.is_string()) {
    return false;
  }
  return !asQString(jValue).trimmed().isEmpty();
}

folly::coro::Task<ZObjDoc::PreparedLoadResult> ZPunctaDoc::prepareLoadAsync(const json::value& jValue,
                                                                            const ZObjDoc::AsyncLoadContext&) const
{
  PreparedLoadResult out;
  const QString fileName = asQString(jValue);
  if (fileName.trimmed().isEmpty()) {
    out.errorMsg = QString("File path is not string or is empty");
    co_return out;
  }

  try {
    ZPuncta puncta(fileName);
    ZPunctaDoc* self = const_cast<ZPunctaDoc*>(this);
    out.commit = [self, this, fileName, puncta = std::move(puncta)](QString& errorMsg) mutable -> size_t {
      try {
        const size_t id = self->addPuncta(std::move(puncta), fileName);
        ZSystemInfo::instance().addFileToRecentFileList(fileName);
        setLastOpenedObjPath(fileName);
        return id;
      }
      catch (const ZException& e) {
        errorMsg = e.what();
        return 0;
      }
      catch (const std::exception& e) {
        errorMsg = e.what();
        return 0;
      }
    };
  }
  catch (const ZException& e) {
    out.errorMsg = e.what();
  }
  catch (const std::exception& e) {
    out.errorMsg = e.what();
  }

  co_return out;
}

std::vector<QAction*> ZPunctaDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadPunctaAction);
  return res;
}

QMenu* ZPunctaDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_detectPunctaAction);
  res->addAction(m_generateAnalysisTextFilesAction);
  return res;
}

void ZPunctaDoc::removeObj(size_t id)
{
  auto it = m_idToPunctaPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToPunctaPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZPunctaDoc::objName(size_t id) const
{
  return m_idToPunctaPacks.at(id)->name();
}

QString ZPunctaDoc::objPath(size_t id) const
{
  return m_idToPunctaPacks.at(id)->path();
}

bool ZPunctaDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToPunctaPacks.at(id)->hasUnsavedChange;
}

QString ZPunctaDoc::objInfo(size_t id) const
{
  return m_idToPunctaPacks.at(id)->info();
}

QString ZPunctaDoc::objTooltip(size_t id) const
{
  return m_idToPunctaPacks.at(id)->tooltip();
}

const QUndoStack* ZPunctaDoc::objUndoStack(size_t id) const
{
  return m_idToPunctaPacks.at(id)->undoStack();
}

json::value ZPunctaDoc::jsonValue(size_t id) const
{
  const auto& pack = m_idToPunctaPacks.at(id);
  if (!pack->sourceJson.is_null()) {
    return pack->sourceJson;
  }
  return json::value_from(pack->path());
}

bool ZPunctaDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
  if (v1.is_object() && v2.is_object()) {
    const auto& o1 = v1.as_object();
    const auto& o2 = v2.as_object();

    auto keyFor = [](const json::object& o) -> std::optional<QString> {
      auto itType = o.find("type");
      if (itType == o.end() || !itType->value().is_string()) {
        return std::nullopt;
      }
      const QString type = json::value_to<QString>(itType->value()).trimmed();

      auto itSegRoot = o.find("segmentation_root_url");
      auto itAnnRoot = o.find("annotation_root_url");
      if (itSegRoot == o.end() || !itSegRoot->value().is_string() ||
          itAnnRoot == o.end() || !itAnnRoot->value().is_string()) {
        return std::nullopt;
      }

      QString segRoot = json::value_to<QString>(itSegRoot->value());
      QString annRoot = json::value_to<QString>(itAnnRoot->value());
      try {
        segRoot = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(segRoot));
        annRoot = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(annRoot));
      }
      catch (...) {
        return std::nullopt;
      }

      if (type == "neuroglancer_precomputed_annotations") {
        auto itRel = o.find("relationship_id");
        auto itObj = o.find("object_id");
        if (itRel == o.end() || !itRel->value().is_string() || itObj == o.end() || !itObj->value().is_string()) {
          return std::nullopt;
        }
        const QString rel = json::value_to<QString>(itRel->value()).trimmed();
        const QString obj = json::value_to<QString>(itObj->value()).trimmed();
        return QString("%1|%2|%3|%4|%5").arg(type, segRoot, annRoot, rel, obj);
      }

      if (type == "neuroglancer_precomputed_annotations_spatial") {
        auto itMin = o.find("voxel_box_min");
        auto itMax = o.find("voxel_box_max");
        if (itMin == o.end() || itMax == o.end()) {
          return std::nullopt;
        }
        const auto minOpt = jsonArray3AsDvec3(itMin->value());
        const auto maxOpt = jsonArray3AsDvec3(itMax->value());
        if (!minOpt || !maxOpt) {
          return std::nullopt;
        }

        const glm::dvec3 qMin{std::min(minOpt->x, maxOpt->x),
                              std::min(minOpt->y, maxOpt->y),
                              std::min(minOpt->z, maxOpt->z)};
        const glm::dvec3 qMax{std::max(minOpt->x, maxOpt->x),
                              std::max(minOpt->y, maxOpt->y),
                              std::max(minOpt->z, maxOpt->z)};

        auto num = [](double v) {
          return QString::number(v, 'g', 17);
        };
        return QString("%1|%2|%3|%4|%5|%6|%7|%8|%9")
          .arg(type,
               segRoot,
               annRoot,
               num(qMin.x),
               num(qMin.y),
               num(qMin.z),
               num(qMax.x),
               num(qMax.y),
               num(qMax.z));
      }

      return std::nullopt;
    };

    const auto k1 = keyFor(o1);
    const auto k2 = keyFor(o2);
    return k1 && k2 && *k1 == *k2;
  }

  if (v1.is_string() && v2.is_string()) {
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

  return false;
}

size_t ZPunctaDoc::makeAlias(size_t id)
{
  CHECK(m_idToPunctaPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToPunctaPacks[aliasId] = m_idToPunctaPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZPunctaDoc::isAlias(size_t id) const
{
  CHECK(m_idToPunctaPacks.contains(id));

  return std::ranges::any_of(m_idToPunctaPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToPunctaPacks.at(id);
  });
}

QWidget* ZPunctaDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToPunctaPacks.contains(id));

  return new ZPunctaWidget(punctaPack(id), m_doc);
}

void ZPunctaDoc::loadPuncta()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZPuncta::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Puncta File");
  if (dialog.exec()) {
    QString errorMsg;
    // auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadFile(filePath, errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load puncta %1").arg(filePath), errorMsg);
      }
    }
  }
}

void ZPunctaDoc::detectPuncta()
{
  ZPunctaDetectionDialog dlg(QApplication::activeWindow());
  dlg.exec();
}

void ZPunctaDoc::generateAnalysisTextFiles()
{
  ZAnalysisWorklistDialog dia(QApplication::activeWindow());
  dia.exec();
}

size_t ZPunctaDoc::addPuncta(ZPuncta puncta, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<ZPunctaPack>(puncta, path, id, *this);
  m_idToPunctaPacks[id]->hasUnsavedChange = false;
  m_doc.registerNewObj(m_idToPunctaPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(m_idToPunctaPacks[id].get(), &ZPunctaPack::undoStackCleanChanged, this, &ZPunctaDoc::setModified);
  return id;
}

size_t ZPunctaDoc::addPunctaFromExternalSource(ZPuncta puncta, QString displayName, QString tooltip, json::value sourceJson)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<ZPunctaPack>(std::move(puncta), /*path=*/QString{}, id, *this);
  auto& pack = *m_idToPunctaPacks.at(id);
  pack.sourceJson = std::move(sourceJson);
  pack.displayNameOverride = std::move(displayName);
  pack.tooltipOverride = std::move(tooltip);
  pack.updateDerivedData();
  pack.hasUnsavedChange = false;

  m_doc.registerNewObj(m_idToPunctaPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(m_idToPunctaPacks[id].get(), &ZPunctaPack::undoStackCleanChanged, this, &ZPunctaDoc::setModified);
  return id;
}

std::optional<size_t> ZPunctaDoc::findPunctaByExternalSource(const json::value& sourceJson) const
{
  for (const auto& [id, pack] : m_idToPunctaPacks) {
    if (pack && isSameObj(pack->sourceJson, sourceJson)) {
      return id;
    }
  }
  return std::nullopt;
}

void ZPunctaDoc::updateExternalPunctaMetadata(size_t id, QString displayName, QString tooltip)
{
  CHECK(m_idToPunctaPacks.contains(id));
  auto& pack = m_idToPunctaPacks.at(id);
  CHECK(!pack->sourceJson.is_null()) << "updateExternalPunctaMetadata is only valid for external-source puncta";

  pack->displayNameOverride = std::move(displayName);
  pack->tooltipOverride = std::move(tooltip);
  pack->updateDerivedData();
  packInfoUpdated(pack.get());
}

void ZPunctaDoc::appendExternalPunctaNoUndo(size_t id, std::list<ZPunctum> addedPuncta)
{
  CHECK(m_idToPunctaPacks.contains(id));
  auto& pack = *m_idToPunctaPacks.at(id);
  CHECK(!pack.sourceJson.is_null()) << "appendExternalPunctaNoUndo is only valid for external-source puncta";

  pack.appendPunctaNoUndo(std::move(addedPuncta));
  packInfoUpdated(&pack);
}

void ZPunctaDoc::setModified(bool)
{
  if (auto ra = qobject_cast<ZPunctaPack*>(sender())) {
    for (const auto& [id, pack] : m_idToPunctaPacks) {
      if (pack.get() == ra) {
        pack->updateDerivedData();
        pack->hasUnsavedChange = !pack->undoStack()->isClean();
        m_doc.updateObjInfo(id);
        return;
      }
    }
  }
}

void ZPunctaDoc::createActions()
{
  m_loadPunctaAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Puncta..."), this);
  m_loadPunctaAction->setStatusTip(tr("Load one or more existing puncta files"));
  connect(m_loadPunctaAction, &QAction::triggered, this, &ZPunctaDoc::loadPuncta);

  m_detectPunctaAction = new QAction(tr("&Detect Puncta..."), this);
  m_detectPunctaAction->setStatusTip(tr("Auto Detect Puncta"));
  connect(m_detectPunctaAction, &QAction::triggered, this, &ZPunctaDoc::detectPuncta);

  m_generateAnalysisTextFilesAction = new QAction(tr("&Generate Analysis Text Files..."), this);
  m_generateAnalysisTextFilesAction->setStatusTip(tr("Generate Analysis Text Files from input list"));
  connect(m_generateAnalysisTextFilesAction, &QAction::triggered, this, &ZPunctaDoc::generateAnalysisTextFiles);
}

bool ZPunctaDoc::savePuncta(ZPunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format)
{
  try {
    pack->save(fileName, format);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZPunctaDoc::packInfoUpdated(ZPunctaPack* pack)
{
  for (const auto& [id, ppack] : m_idToPunctaPacks) {
    if (ppack.get() == pack) {
      m_doc.updateObjInfo(id);
    }
  }
}

} // namespace nim
