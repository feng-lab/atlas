#include "zskeletondoc.h"

#include "zexception.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancerprecomputedskeleton.h"

#include <algorithm>

namespace nim {

namespace {

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

        source =
          ZNeuroglancerPrecomputedSkeletonSource::open(QUrl(skeletonSourceDirUrlForJson),
                                                      {vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ},
                                                      vol->baseVoxelOffset(),
                                                      vol->defaultTimeout());
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

      const QString displayName = label.isEmpty() ? QString("NG Skeleton %1").arg(static_cast<qulonglong>(segId))
                                                  : QString("NG Skeleton %1 (%2)").arg(static_cast<qulonglong>(segId)).arg(label);

      QString tooltip = QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2")
                          .arg(normalizedRootUrl)
                          .arg(static_cast<qulonglong>(segId));
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
      normalized["segment_id"] = json::value_from(QString::number(static_cast<qulonglong>(segId)));
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
