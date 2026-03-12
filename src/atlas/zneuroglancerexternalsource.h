#pragma once

#include "zneuroglancerprecomputed.h"
#include "zjson.h"

#include <optional>

namespace nim {

struct ZNeuroglancerMeshExternalSourceKey
{
  QString rootUrl;
  QString meshSourceDirUrl;
  uint64_t segmentId = 0;
  std::optional<std::array<double, 3>> baseResolutionNm;
  std::optional<std::array<int64_t, 3>> baseVoxelOffset;
};

struct ZNeuroglancerSkeletonExternalSourceKey
{
  QString rootUrl;
  QString skeletonSourceDirUrl;
  uint64_t segmentId = 0;
};

[[nodiscard]] std::optional<uint64_t> parseNeuroglancerUint64Base10(const QString& s);

[[nodiscard]] QString
neuroglancerMeshKeyString(const QString& rootUrl, const QString& meshSourceDirUrl, uint64_t segmentId);

[[nodiscard]] QString
neuroglancerSkeletonKeyString(const QString& rootUrl, const QString& skeletonSourceDirUrl, uint64_t segmentId);

[[nodiscard]] std::optional<ZNeuroglancerMeshExternalSourceKey>
parseNeuroglancerMeshExternalSourceKey(const json::value& v);

[[nodiscard]] std::optional<ZNeuroglancerSkeletonExternalSourceKey>
parseNeuroglancerSkeletonExternalSourceKey(const json::value& v);

[[nodiscard]] json::value
makeNeuroglancerMeshExternalSourceJson(const QString& rootUrl,
                                       const QString& meshSourceDirUrl,
                                       uint64_t segmentId,
                                       std::optional<std::array<double, 3>> baseResolutionNm = std::nullopt,
                                       std::optional<std::array<int64_t, 3>> baseVoxelOffset = std::nullopt);

} // namespace nim
