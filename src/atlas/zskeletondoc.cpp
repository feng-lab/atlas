#include "zskeletondoc.h"

#include "zexception.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancerprecomputedskeleton.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

[[nodiscard]] std::optional<uint64_t> parseUint64Base10(const QString& s)
{
  const QString trimmed = s.trimmed();
  if (trimmed.isEmpty()) {
    return std::nullopt;
  }
  bool ok = false;
  const uint64_t v = trimmed.toULongLong(&ok, 10);
  if (!ok) {
    return std::nullopt;
  }
  return v;
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

ZSkeletonDoc::ZSkeletonDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

size_t ZSkeletonDoc::addSkeletonFromExternalSource(ZSkeleton& skeleton,
                                                   QString displayName,
                                                   QString tooltip,
                                                   json::value sourceJson)
{
  for (const auto& idPack : m_idToSkeletonPacks) {
    if (isSameObj(sourceJson, jsonValue(idPack.first))) {
      return idPack.first;
    }
  }

  size_t id = m_doc.getNewObjId();
  m_idToSkeletonPacks[id] =
    std::make_shared<SkeletonPack>(skeleton, std::move(displayName), std::move(tooltip), std::move(sourceJson));
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

std::optional<size_t> ZSkeletonDoc::findSkeletonByExternalSource(const json::value& sourceJson) const
{
  for (const auto& idPack : m_idToSkeletonPacks) {
    if (isSameObj(sourceJson, jsonValue(idPack.first))) {
      return idPack.first;
    }
  }
  return std::nullopt;
}

void ZSkeletonDoc::updateExternalSkeletonMetadata(size_t id, QString displayName, QString tooltip)
{
  CHECK(m_idToSkeletonPacks.contains(id));
  auto& pack = m_idToSkeletonPacks.at(id);
  CHECK(!pack->sourceJson.is_null()) << "updateExternalSkeletonMetadata is only valid for external-source skeletons";

  pack->displayNameOverride = std::move(displayName);
  pack->tooltipOverride = std::move(tooltip);
  pack->updateDerivedData();
  packInfoUpdated(pack.get());
}

void ZSkeletonDoc::appendExternalSkeletonGeometryNoUndo(size_t id,
                                                        std::vector<glm::vec3> vertices,
                                                        std::vector<glm::uvec2> edges)
{
  CHECK(m_idToSkeletonPacks.contains(id));
  auto& pack = m_idToSkeletonPacks.at(id);
  CHECK(!pack->sourceJson.is_null()) << "appendExternalSkeletonGeometryNoUndo is only valid for external-source skeletons";

  pack->skeleton.appendGeometry(std::move(vertices), std::move(edges));
  pack->updateDerivedData();
  packInfoUpdated(pack.get());

  // Update all views, including aliases that share the same pack.
  for (const auto& [otherId, otherPack] : m_idToSkeletonPacks) {
    if (otherPack == pack) {
      Q_EMIT skeletonChanged(otherId);
    }
  }
}

bool ZSkeletonDoc::save([[maybe_unused]] size_t id)
{
  // Skeletons are currently network-backed/imported objects with no on-disk save format.
  return true;
}

bool ZSkeletonDoc::saveAs([[maybe_unused]] size_t id)
{
  // Skeletons are currently network-backed/imported objects with no on-disk save format.
  return true;
}

bool ZSkeletonDoc::canReadFile([[maybe_unused]] const QString& fileName) const
{
  return false;
}

size_t ZSkeletonDoc::loadFile([[maybe_unused]] const QString& fileName, QString& errorMsg)
{
  errorMsg = QStringLiteral("Loading skeletons from file path is not supported yet.");
  return 0;
}

size_t ZSkeletonDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (jValue.is_object()) {
      constexpr std::chrono::milliseconds kDefaultTimeout{30000};

      const auto& jo = jValue.as_object();
      auto typeIt = jo.find("type");
      if (typeIt == jo.end() || !typeIt->value().is_string()) {
        errorMsg = QString("Invalid skeleton JSON: missing string field 'type'");
        return 0;
      }
      const QString type = json::value_to<QString>(typeIt->value()).trimmed();

      if (type == "neuroglancer_precomputed_annotations") {
        auto segRootIt = jo.find("segmentation_root_url");
        if (segRootIt == jo.end() || !segRootIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'segmentation_root_url'");
          return 0;
        }
        const QString normalizedSegRootUrl =
          ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(segRootIt->value()));

        auto annRootIt = jo.find("annotation_root_url");
        if (annRootIt == jo.end() || !annRootIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'annotation_root_url'");
          return 0;
        }
        const QString normalizedAnnRootUrl =
          ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(annRootIt->value()));

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
        const auto objOpt = parseUint64Base10(objStr);
        if (!objOpt) {
          errorMsg = QString("Invalid neuroglancer annotations JSON: object_id must be base-10 uint64");
          return 0;
        }
        const uint64_t objectId = *objOpt;

        // Deduplicate before performing any network work.
        for (const auto& idPack : m_idToSkeletonPacks) {
          if (isSameObj(jValue, jsonValue(idPack.first))) {
            return idPack.first;
          }
        }

        // Try to reuse an already-opened segmentation volume.
        std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
        for (const size_t imgId : m_doc.objsOfDoc(&m_doc.imgDoc())) {
          const ZImgPack& pack = m_doc.imgDoc().imgPack(imgId);
          if (!pack.isNeuroglancerPrecomputed()) {
            continue;
          }
          if (pack.neuroglancerRootUrl() == normalizedSegRootUrl && pack.neuroglancerVolumeShared()->isSegmentation()) {
            vol = pack.neuroglancerVolumeShared();
            break;
          }
        }
        if (!vol) {
          vol = ZNeuroglancerPrecomputedVolume::open(normalizedSegRootUrl, kDefaultTimeout);
        }
        CHECK(vol);

        std::array<double, 3> baseResNm{vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ};
        auto source = ZNeuroglancerPrecomputedAnnotationsSource::open(QUrl(normalizedAnnRootUrl),
                                                                      baseResNm,
                                                                      vol->baseVoxelOffset(),
                                                                      vol->sharedRemoteContext());
        CHECK(source);

        if (source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line &&
            source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Polyline) {
          errorMsg = QString("Unsupported neuroglancer annotations type for skeleton: expected LINE or POLYLINE");
          return 0;
        }

        const auto anns = source->loadAnnotationsForRelatedObjectBlocking(relationshipId, objectId);
        if (anns.empty()) {
          errorMsg =
            QString("No annotations found for object %1 (relationship '%2')").arg(objectId).arg(relationshipId);
          return 0;
        }

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
          errorMsg = QString("No LINE/POLYLINE segments decoded for object %1 (relationship '%2')")
                       .arg(objectId)
                       .arg(relationshipId);
          return 0;
        }

        auto skel = std::make_shared<ZSkeleton>();
        skel->setVertices(std::move(vertices));
        skel->setEdges(std::move(edges));

        json::object normalized;
        normalized["type"] = "neuroglancer_precomputed_annotations";
        normalized["segmentation_root_url"] = json::value_from(normalizedSegRootUrl);
        normalized["annotation_root_url"] = json::value_from(normalizedAnnRootUrl);
        normalized["relationship_id"] = json::value_from(relationshipId);
        normalized["object_id"] = json::value_from(QString::number(objectId));

        const QString displayName = QString("NG Annotations %1 (%2)").arg(objectId).arg(relationshipId);
        const QString tooltip =
          QString(
            "Neuroglancer precomputed annotations (lines)\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nSegments: %5")
            .arg(normalizedSegRootUrl)
            .arg(normalizedAnnRootUrl)
            .arg(relationshipId)
            .arg(objectId)
            .arg(numSegments);

        return addSkeletonFromExternalSource(*skel, displayName, tooltip, normalized);
      }

      if (type == "neuroglancer_precomputed_annotations_spatial") {
        auto segRootIt = jo.find("segmentation_root_url");
        if (segRootIt == jo.end() || !segRootIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations spatial JSON: missing string field 'segmentation_root_url'");
          return 0;
        }
        const QString normalizedSegRootUrl =
          ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(segRootIt->value()));

        auto annRootIt = jo.find("annotation_root_url");
        if (annRootIt == jo.end() || !annRootIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer annotations spatial JSON: missing string field 'annotation_root_url'");
          return 0;
        }
        const QString normalizedAnnRootUrl =
          ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(annRootIt->value()));

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

        // Deduplicate before performing any network work.
        for (const auto& idPack : m_idToSkeletonPacks) {
          if (isSameObj(normalized, jsonValue(idPack.first))) {
            return idPack.first;
          }
        }

        // Try to reuse an already-opened segmentation volume.
        std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
        for (const size_t imgId : m_doc.objsOfDoc(&m_doc.imgDoc())) {
          const ZImgPack& pack = m_doc.imgDoc().imgPack(imgId);
          if (!pack.isNeuroglancerPrecomputed()) {
            continue;
          }
          if (pack.neuroglancerRootUrl() == normalizedSegRootUrl && pack.neuroglancerVolumeShared()->isSegmentation()) {
            vol = pack.neuroglancerVolumeShared();
            break;
          }
        }
        if (!vol) {
          vol = ZNeuroglancerPrecomputedVolume::open(normalizedSegRootUrl, kDefaultTimeout);
        }
        CHECK(vol);

        std::array<double, 3> baseResNm{
          vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ};
        auto source = ZNeuroglancerPrecomputedAnnotationsSource::open(QUrl(normalizedAnnRootUrl),
                                                                      baseResNm,
                                                                      vol->baseVoxelOffset(),
                                                                      vol->sharedRemoteContext());
        CHECK(source);

        if (source->spatialLevels().empty()) {
          errorMsg = QString("Neuroglancer annotations dataset has no spatial index ('spatial' missing in info)");
          return 0;
        }

        if (source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line &&
            source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Polyline) {
          errorMsg = QString("Unsupported neuroglancer annotations type for skeleton: expected LINE or POLYLINE");
          return 0;
        }

        const auto anns = source->loadAnnotationsIntersectingVoxelBoxBlocking(qMin, qMax);
        if (anns.empty()) {
          errorMsg = QString("No annotations found intersecting the saved spatial query box.");
          return 0;
        }

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
          errorMsg = QString("No LINE/POLYLINE segments decoded for the saved spatial query box.");
          return 0;
        }

        auto skel = std::make_shared<ZSkeleton>();
        skel->setVertices(std::move(vertices));
        skel->setEdges(std::move(edges));

        const QString displayName = QString("NG Annotations (View z≈%1)").arg(qMin.z, 0, 'g', 6);
        const QString tooltip = QString("Neuroglancer precomputed annotations (spatial lines)\n"
                                        "Segmentation: %1\n"
                                        "Annotations: %2\n"
                                        "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                                        "Segments: %9")
                                  .arg(normalizedSegRootUrl)
                                  .arg(normalizedAnnRootUrl)
                                  .arg(qMin.x, 0, 'g', 8)
                                  .arg(qMin.y, 0, 'g', 8)
                                  .arg(qMin.z, 0, 'g', 8)
                                  .arg(qMax.x, 0, 'g', 8)
                                  .arg(qMax.y, 0, 'g', 8)
                                  .arg(qMax.z, 0, 'g', 8)
                                  .arg(numSegments);

        return addSkeletonFromExternalSource(*skel, displayName, tooltip, normalized);
      }

      if (type != "neuroglancer_precomputed_skeleton") {
        errorMsg = QString("Unsupported skeleton JSON type '%1'").arg(type);
        return 0;
      }

      auto rootIt = jo.find("segmentation_root_url");
      if (rootIt == jo.end() || !rootIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer skeleton JSON: missing string field 'segmentation_root_url'");
        return 0;
      }
      const QString normalizedRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(rootIt->value()));

      auto segIt = jo.find("segment_id");
      if (segIt == jo.end() || !segIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer skeleton JSON: missing string field 'segment_id'");
        return 0;
      }
      const QString segStr = json::value_to<QString>(segIt->value()).trimmed();
      const auto segOpt = parseUint64Base10(segStr);
      if (!segOpt) {
        errorMsg = QString("Invalid neuroglancer skeleton JSON: segment_id must be base-10 uint64");
        return 0;
      }
      const uint64_t segId = *segOpt;

      QString skeletonSourceUserText;
      if (auto itUrl = jo.find("skeleton_source_url"); itUrl != jo.end() && itUrl->value().is_string()) {
        skeletonSourceUserText = json::value_to<QString>(itUrl->value()).trimmed();
      } else if (auto itKey = jo.find("skeleton_key"); itKey != jo.end() && itKey->value().is_string()) {
        skeletonSourceUserText = json::value_to<QString>(itKey->value()).trimmed();
      }

      // Deduplicate before performing any network work.
      for (const auto& idPack : m_idToSkeletonPacks) {
        if (isSameObj(jValue, jsonValue(idPack.first))) {
          return idPack.first;
        }
      }

      // Try to reuse an already-opened segmentation volume.
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
      for (const size_t imgId : m_doc.objsOfDoc(&m_doc.imgDoc())) {
        const ZImgPack& pack = m_doc.imgDoc().imgPack(imgId);
        if (!pack.isNeuroglancerPrecomputed()) {
          continue;
        }
        if (pack.neuroglancerRootUrl() == normalizedRootUrl && pack.neuroglancerVolumeShared()->isSegmentation()) {
          vol = pack.neuroglancerVolumeShared();
          break;
        }
      }
      if (!vol) {
        vol = ZNeuroglancerPrecomputedVolume::open(normalizedRootUrl, kDefaultTimeout);
      }

      std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> source;
      QString skeletonSourceDirUrlForJson;
      if (!skeletonSourceUserText.isEmpty()) {
        QString s = skeletonSourceUserText.trimmed();
        if (s.contains("://") || s.startsWith("gs://")) {
          try {
            s = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(s));
          }
          catch (const std::exception&) {
            errorMsg = QString("Invalid neuroglancer skeleton JSON: skeleton_source_url is not a valid URL");
            return 0;
          }
        } else {
          if (!s.endsWith('/')) {
            s += '/';
          }
          const QUrl dirUrl = QUrl(vol->rootUrl()).resolved(QUrl(s));
          s = dirUrl.toString(QUrl::StripTrailingSlash);
        }
        if (!s.endsWith('/')) {
          s += '/';
        }
        skeletonSourceDirUrlForJson = s;

        source = ZNeuroglancerPrecomputedSkeletonSource::open(
          QUrl(skeletonSourceDirUrlForJson),
          {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
          vol->baseVoxelOffset(),
          vol->sharedRemoteContext());
      } else if (vol->hasSkeletonDirectory()) {
        source = vol->loadSkeletonSourceBlocking();
        skeletonSourceDirUrlForJson = vol->skeletonDirUrl().toString(QUrl::StripTrailingSlash) + "/";
      } else {
        errorMsg = QString("Neuroglancer volume does not specify a skeletons directory");
        return 0;
      }

      CHECK(source);
      std::shared_ptr<ZSkeleton> skel = source->loadSkeletonBlocking(segId);
      if (!skel || skel->empty()) {
        errorMsg = QString("Loaded neuroglancer skeleton is empty");
        return 0;
      }

      QString label;
      QString description;
      if (const auto props = vol->segmentPropertiesShared()) {
        if (const auto l = props->labelForId(segId)) {
          label = *l;
        }
        if (const auto d = props->descriptionForId(segId)) {
          description = *d;
        }
      }

      const QString displayName =
        label.isEmpty() ? QString("NG Skeleton %1").arg(segId) : QString("NG Skeleton %1 (%2)").arg(segId).arg(label);

      QString tooltip =
        QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2").arg(normalizedRootUrl).arg(segId);
      if (!label.isEmpty()) {
        tooltip += QString("\nLabel: %1").arg(label);
      }
      if (!description.isEmpty()) {
        tooltip += QString("\nDescription: %1").arg(description);
      }

      // Normalize persisted JSON to keep it stable across save/load cycles.
      json::object normalized;
      normalized["type"] = "neuroglancer_precomputed_skeleton";
      normalized["segmentation_root_url"] = json::value_from(normalizedRootUrl);
      normalized["segment_id"] = json::value_from(QString::number(segId));
      if (!skeletonSourceDirUrlForJson.isEmpty()) {
        normalized["skeleton_source_url"] = json::value_from(skeletonSourceDirUrlForJson);
      }

      return addSkeletonFromExternalSource(*skel, displayName, tooltip, normalized);
    }

    errorMsg = QStringLiteral("Invalid skeleton JSON (expected object)");
    return 0;
  }
  catch (const ZNotFoundException& e) {
    errorMsg = QString::fromUtf8(e.what());
    return 0;
  }
  catch (const ZException& e) {
    errorMsg = QString::fromUtf8(e.what());
    return 0;
  }
  catch (const std::exception& e) {
    errorMsg = QString::fromUtf8(e.what());
    return 0;
  }
}

std::vector<QAction*> ZSkeletonDoc::loadFileActions() const
{
  return {};
}

void ZSkeletonDoc::removeObj(size_t id)
{
  auto it = m_idToSkeletonPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToSkeletonPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZSkeletonDoc::objName(size_t id) const
{
  return m_idToSkeletonPacks.at(id)->name();
}

QString ZSkeletonDoc::objPath([[maybe_unused]] size_t id) const
{
  // Network-backed/imported skeletons do not have a file path.
  return {};
}

bool ZSkeletonDoc::objHasUnsavedChange([[maybe_unused]] size_t id) const
{
  return false;
}

QString ZSkeletonDoc::objInfo(size_t id) const
{
  return m_idToSkeletonPacks.at(id)->info();
}

QString ZSkeletonDoc::objTooltip(size_t id) const
{
  return m_idToSkeletonPacks.at(id)->tooltip();
}

json::value ZSkeletonDoc::jsonValue(size_t id) const
{
  return m_idToSkeletonPacks.at(id)->sourceJson;
}

bool ZSkeletonDoc::isSameObj(const json::value& v1, const json::value& v2) const
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
      if (type == "neuroglancer_precomputed_annotations") {
        auto itSegRoot = o.find("segmentation_root_url");
        auto itAnnRoot = o.find("annotation_root_url");
        auto itRel = o.find("relationship_id");
        auto itObj = o.find("object_id");
        if (itSegRoot == o.end() || !itSegRoot->value().is_string() || itAnnRoot == o.end() || !itAnnRoot->value().is_string() ||
            itRel == o.end() || !itRel->value().is_string() || itObj == o.end() || !itObj->value().is_string()) {
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

        const QString rel = json::value_to<QString>(itRel->value()).trimmed();
        const QString obj = json::value_to<QString>(itObj->value()).trimmed();
        return QString("%1|%2|%3|%4|%5").arg(type, segRoot, annRoot, rel, obj);
      }

      if (type == "neuroglancer_precomputed_annotations_spatial") {
        auto itSegRoot = o.find("segmentation_root_url");
        auto itAnnRoot = o.find("annotation_root_url");
        auto itMin = o.find("voxel_box_min");
        auto itMax = o.find("voxel_box_max");
        if (itSegRoot == o.end() || !itSegRoot->value().is_string() ||
            itAnnRoot == o.end() || !itAnnRoot->value().is_string() ||
            itMin == o.end() || itMax == o.end()) {
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

      const QString seg = json::value_to<QString>(itSeg->value()).trimmed();

      QString skeletonSourceDirUrl;
      if (auto itUrl = o.find("skeleton_source_url"); itUrl != o.end() && itUrl->value().is_string()) {
        skeletonSourceDirUrl = json::value_to<QString>(itUrl->value()).trimmed();
      } else if (auto itKey = o.find("skeleton_key"); itKey != o.end() && itKey->value().is_string()) {
        QString skelKey = json::value_to<QString>(itKey->value()).trimmed();
        if (!skelKey.endsWith('/')) {
          skelKey += '/';
        }
        const QUrl skelDirUrl = QUrl(root).resolved(QUrl(skelKey));
        skeletonSourceDirUrl = skelDirUrl.toString(QUrl::StripTrailingSlash);
      }
      if (!skeletonSourceDirUrl.isEmpty() && !skeletonSourceDirUrl.endsWith('/')) {
        skeletonSourceDirUrl += '/';
      }

      return QString("%1|%2|%3|%4").arg(type, root, seg, skeletonSourceDirUrl);
    };

    const auto k1 = keyFor(o1);
    const auto k2 = keyFor(o2);
    return k1 && k2 && *k1 == *k2;
  }
  return false;
}

size_t ZSkeletonDoc::makeAlias(size_t id)
{
  CHECK(m_idToSkeletonPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToSkeletonPacks[aliasId] = m_idToSkeletonPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZSkeletonDoc::isAlias(size_t id) const
{
  CHECK(m_idToSkeletonPacks.contains(id));
  return std::ranges::any_of(m_idToSkeletonPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToSkeletonPacks.at(id);
  });
}

ZSkeletonDoc::SkeletonPack::SkeletonPack(ZSkeleton& iskel, QString displayName, QString tooltip, json::value sourceJson_)
  : sourceJson(std::move(sourceJson_))
  , displayNameOverride(std::move(displayName))
  , tooltipOverride(std::move(tooltip))
{
  skeleton.swap(iskel);
  updateDerivedData();
}

void ZSkeletonDoc::SkeletonPack::updateDerivedData()
{
  m_info.clear();

  if (!displayNameOverride.isEmpty()) {
    m_name = displayNameOverride;
  } else {
    m_name = QStringLiteral("Skeleton");
  }

  if (!tooltipOverride.isEmpty()) {
    m_tooltip = tooltipOverride;
  } else {
    m_tooltip.clear();
  }
}

const QString& ZSkeletonDoc::SkeletonPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 vertices, %2 edges").arg(skeleton.numVertices()).arg(skeleton.numEdges());
  }
  return m_info;
}

void ZSkeletonDoc::createActions() {}

void ZSkeletonDoc::packInfoUpdated([[maybe_unused]] SkeletonPack* pack)
{
  for (const auto& idPack : m_idToSkeletonPacks) {
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
