#include "zneuroglancerexternalsource.h"

#include "zexception.h"

#include <boost/json.hpp>

namespace nim {
namespace json = boost::json;

namespace {

template<typename T>
[[nodiscard]] std::optional<std::array<T, 3>> parseArray3(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end() || it->value().is_null()) {
    return std::nullopt;
  }
  if (!it->value().is_array()) {
    return std::nullopt;
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    return std::nullopt;
  }

  std::array<T, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    const auto& v = arr[i];
    if constexpr (std::is_same_v<T, double>) {
      if (v.is_double()) {
        out[i] = v.as_double();
      } else if (v.is_int64()) {
        out[i] = static_cast<double>(v.as_int64());
      } else if (v.is_uint64()) {
        out[i] = static_cast<double>(v.as_uint64());
      } else {
        return std::nullopt;
      }
    } else {
      if (v.is_int64()) {
        out[i] = static_cast<T>(v.as_int64());
      } else if (v.is_uint64()) {
        out[i] = static_cast<T>(v.as_uint64());
      } else {
        return std::nullopt;
      }
    }
  }

  return out;
}

[[nodiscard]] QString normalizeResolvedDirUrl(const QString& rootUrl, QString dirText)
{
  dirText = dirText.trimmed();
  if (dirText.isEmpty()) {
    return {};
  }

  if (dirText.contains("://") || dirText.startsWith("gs://")) {
    dirText = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(dirText));
  } else {
    if (!dirText.endsWith('/')) {
      dirText += '/';
    }
    const QUrl dirUrl = QUrl(rootUrl).resolved(QUrl(dirText));
    dirText = dirUrl.toString(QUrl::StripTrailingSlash);
  }

  if (!dirText.endsWith('/')) {
    dirText += '/';
  }
  return dirText;
}

} // namespace

std::optional<uint64_t> parseNeuroglancerUint64Base10(const QString& s)
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

QString neuroglancerMeshKeyString(const QString& rootUrl, const QString& meshSourceDirUrl, uint64_t segmentId)
{
  return QString("%1|%2|%3").arg(rootUrl).arg(meshSourceDirUrl).arg(segmentId);
}

QString neuroglancerSkeletonKeyString(const QString& rootUrl, const QString& skeletonSourceDirUrl, uint64_t segmentId)
{
  return QString("%1|%2|%3").arg(rootUrl).arg(skeletonSourceDirUrl).arg(segmentId);
}

std::optional<ZNeuroglancerMeshExternalSourceKey> parseNeuroglancerMeshExternalSourceKey(const json::value& v)
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

  QString rootUrl = json::value_to<QString>(itRoot->value());
  try {
    rootUrl = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(rootUrl));
  }
  catch (...) {
    return std::nullopt;
  }

  const auto segmentId = parseNeuroglancerUint64Base10(json::value_to<QString>(itSeg->value()));
  if (!segmentId) {
    return std::nullopt;
  }

  QString meshSourceDirUrl;
  if (auto itUrl = o.find("mesh_source_url"); itUrl != o.end() && itUrl->value().is_string()) {
    try {
      meshSourceDirUrl = normalizeResolvedDirUrl(rootUrl, json::value_to<QString>(itUrl->value()));
    }
    catch (...) {
      return std::nullopt;
    }
  } else if (auto itMeshKey = o.find("mesh_key"); itMeshKey != o.end() && itMeshKey->value().is_string()) {
    try {
      meshSourceDirUrl = normalizeResolvedDirUrl(rootUrl, json::value_to<QString>(itMeshKey->value()));
    }
    catch (...) {
      return std::nullopt;
    }
  }
  ZNeuroglancerMeshExternalSourceKey out;
  out.rootUrl = std::move(rootUrl);
  out.meshSourceDirUrl = std::move(meshSourceDirUrl);
  out.segmentId = *segmentId;
  out.baseResolutionNm = parseArray3<double>(o, "base_resolution_nm");
  out.baseVoxelOffset = parseArray3<int64_t>(o, "base_voxel_offset");
  return out;
}

std::optional<ZNeuroglancerSkeletonExternalSourceKey> parseNeuroglancerSkeletonExternalSourceKey(const json::value& v)
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

  QString rootUrl = json::value_to<QString>(itRoot->value());
  try {
    rootUrl = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(rootUrl));
  }
  catch (...) {
    return std::nullopt;
  }

  const auto segmentId = parseNeuroglancerUint64Base10(json::value_to<QString>(itSeg->value()));
  if (!segmentId) {
    return std::nullopt;
  }

  QString skeletonSourceDirUrl;
  if (auto itUrl = o.find("skeleton_source_url"); itUrl != o.end() && itUrl->value().is_string()) {
    try {
      skeletonSourceDirUrl = normalizeResolvedDirUrl(rootUrl, json::value_to<QString>(itUrl->value()));
    }
    catch (...) {
      return std::nullopt;
    }
  } else if (auto itKey = o.find("skeleton_key"); itKey != o.end() && itKey->value().is_string()) {
    try {
      skeletonSourceDirUrl = normalizeResolvedDirUrl(rootUrl, json::value_to<QString>(itKey->value()));
    }
    catch (...) {
      return std::nullopt;
    }
  }
  ZNeuroglancerSkeletonExternalSourceKey out;
  out.rootUrl = std::move(rootUrl);
  out.skeletonSourceDirUrl = std::move(skeletonSourceDirUrl);
  out.segmentId = *segmentId;
  return out;
}

json::value makeNeuroglancerMeshExternalSourceJson(const QString& rootUrl,
                                                   const QString& meshSourceDirUrl,
                                                   uint64_t segmentId,
                                                   std::optional<std::array<double, 3>> baseResolutionNm,
                                                   std::optional<std::array<int64_t, 3>> baseVoxelOffset)
{
  json::object sourceObj;
  sourceObj["type"] = "neuroglancer_precomputed_mesh";
  sourceObj["segmentation_root_url"] = json::value_from(ZNeuroglancerPrecomputedVolume::normalizeRootUrl(rootUrl));
  sourceObj["segment_id"] = json::value_from(QString::number(segmentId));
  sourceObj["mesh_source_url"] = json::value_from(normalizeResolvedDirUrl(rootUrl, meshSourceDirUrl));
  if (baseResolutionNm) {
    json::array arr;
    arr.emplace_back((*baseResolutionNm)[0]);
    arr.emplace_back((*baseResolutionNm)[1]);
    arr.emplace_back((*baseResolutionNm)[2]);
    sourceObj["base_resolution_nm"] = std::move(arr);
  }
  if (baseVoxelOffset) {
    json::array arr;
    arr.emplace_back((*baseVoxelOffset)[0]);
    arr.emplace_back((*baseVoxelOffset)[1]);
    arr.emplace_back((*baseVoxelOffset)[2]);
    sourceObj["base_voxel_offset"] = std::move(arr);
  }
  return sourceObj;
}

} // namespace nim
