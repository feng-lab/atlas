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
  // These fields describe geometry-space interpretation only. They intentionally do not serialize a transport
  // backend, auth/session state, or remote-store identity. Same-session runtime LOD reopen can reuse a live
  // remote context supplied by ZMeshDoc/Z3DMeshView sidecar state, but deserializing this key alone still does not
  // reconstruct any custom remote backend/session.
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

[[nodiscard]] QString neuroglancerMeshKeyString(const QString& rootUrl,
                                                const QString& meshSourceDirUrl,
                                                uint64_t segmentId,
                                                std::optional<std::array<double, 3>> baseResolutionNm = std::nullopt,
                                                std::optional<std::array<int64_t, 3>> baseVoxelOffset = std::nullopt);

// Backward-compatible equality for external mesh source identity.
//
// Canonical normalized mesh JSON should include base geometry because Atlas uses
// it to interpret the decoded mesh vertices. Older scene files omitted those
// fields, though, so dedup/restore code must still treat a missing-geometry key
// as the same object when the root URL, mesh source URL, and segment ID match.
// Once both sides provide base geometry, the comparison becomes strict again.
[[nodiscard]] bool sameNeuroglancerMeshSourceCompat(const ZNeuroglancerMeshExternalSourceKey& a,
                                                    const ZNeuroglancerMeshExternalSourceKey& b);

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
