#include "zneuroglancerprecomputedmesh.h"

#include "zneuroglancerremotecontext.h"
#include "zneuroglancershardedreader.h"
#include "zneuroglanceruint64sharding.h"
#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"

#include "zmesh.h"

#include "zcommandlineflags.h"

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>
#include <draco/point_cloud/point_cloud.h>
#include <draco/attributes/point_attribute.h>

#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/WithCancellation.h>
#include <boost/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace nim {
namespace json = boost::json;

namespace {

constexpr size_t kMeshLodCancellationCheckStride = 4096;

void maybeCancelPeriodic(const folly::CancellationToken& cancellationToken, size_t iteration)
{
  if (iteration % kMeshLodCancellationCheckStride == 0U) {
    maybeCancel(cancellationToken);
  }
}

std::string toStdString(const QString& s)
{
  const auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

QString requireString(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_string()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer mesh info", key));
  }
  return json::value_to<QString>(it->value());
}

QString optionalString(const json::object& obj, const char* key, QString defaultValue = {})
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return defaultValue;
  }
  if (!it->value().is_string()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer mesh info (expected string)", key));
  }
  return json::value_to<QString>(it->value());
}

uint64_t requireUint64(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end()) {
    throw ZException(fmt::format("Missing '{}' in neuroglancer mesh info", key));
  }
  const auto& v = it->value();
  if (v.is_uint64()) {
    return v.as_uint64();
  }
  if (v.is_int64()) {
    const int64_t s = v.as_int64();
    if (s < 0) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer mesh info (expected >= 0)", key));
    }
    return static_cast<uint64_t>(s);
  }
  throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer mesh info", key));
}

double requireDouble(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end()) {
    throw ZException(fmt::format("Missing '{}' in neuroglancer mesh info", key));
  }
  const auto& v = it->value();
  if (v.is_double()) {
    return v.as_double();
  }
  if (v.is_int64()) {
    return static_cast<double>(v.as_int64());
  }
  if (v.is_uint64()) {
    return static_cast<double>(v.as_uint64());
  }
  throw ZException(fmt::format("Invalid '{}' in neuroglancer mesh info (expected number)", key));
}

glm::mat4 parseTransform(const json::object& obj)
{
  auto it = obj.find("transform");
  if (it == obj.end() || it->value().is_null()) {
    return glm::mat4(1.0F);
  }
  if (!it->value().is_array()) {
    throw ZException("Invalid 'transform' in neuroglancer mesh info (expected array)");
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 12) {
    throw ZException("Invalid 'transform' in neuroglancer mesh info (expected length 12 array)");
  }

  glm::mat4 m(1.0F);
  float* out = glm::value_ptr(m);
  for (size_t i = 0; i < 12; ++i) {
    const auto& v = arr[i];
    if (v.is_double()) {
      out[i] = static_cast<float>(v.as_double());
    } else if (v.is_int64()) {
      out[i] = static_cast<float>(v.as_int64());
    } else if (v.is_uint64()) {
      out[i] = static_cast<float>(v.as_uint64());
    } else {
      throw ZException("Invalid 'transform' in neuroglancer mesh info (expected numbers)");
    }
  }

  // Neuroglancer TS fills the first 12 elements and then transposes (gl-matrix uses column-major).
  return glm::transpose(m);
}

ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding parseShardedEncoding(QString s, const char* field)
{
  s = s.trimmed().toLower();
  if (s.isEmpty() || s == "raw") {
    return ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Raw;
  }
  if (s == "gzip") {
    return ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Gzip;
  }
  throw ZException(fmt::format("Unsupported '{}' in neuroglancer sharding: '{}'", field, toStdString(s)));
}

ZNeuroglancerPrecomputedVolume::Scale::Sharding parseShardingSpec(const json::object& shObj)
{
  using Sharding = ZNeuroglancerPrecomputedVolume::Scale::Sharding;

  Sharding sharding{};
  const QString type = requireString(shObj, "@type");
  if (type != "neuroglancer_uint64_sharded_v1") {
    throw ZException(fmt::format("Unsupported neuroglancer sharding '@type': '{}'", toStdString(type)));
  }

  sharding.preshiftBits = static_cast<size_t>(requireUint64(shObj, "preshift_bits"));
  if (sharding.preshiftBits > 63) {
    throw ZException("Invalid neuroglancer sharding: preshift_bits must be <= 63");
  }

  const QString hashStr = requireString(shObj, "hash").trimmed().toLower();
  if (hashStr == "identity") {
    sharding.hash = Sharding::Hash::Identity;
  } else if (hashStr == "murmurhash3_x86_128") {
    sharding.hash = Sharding::Hash::MurmurHash3X86_128;
  } else {
    throw ZException(fmt::format("Unsupported neuroglancer sharding hash '{}'", toStdString(hashStr)));
  }

  sharding.minishardBits = static_cast<size_t>(requireUint64(shObj, "minishard_bits"));
  sharding.shardBits = static_cast<size_t>(requireUint64(shObj, "shard_bits"));
  if (sharding.shardBits >= 64) {
    throw ZException("Invalid neuroglancer sharding: shard_bits must be <= 63");
  }
  if (sharding.minishardBits + sharding.shardBits > 64) {
    throw ZException("Invalid neuroglancer sharding: minishard_bits + shard_bits must be <= 64");
  }
  if (sharding.minishardBits >= 60) {
    throw ZException("Invalid neuroglancer sharding: minishard_bits too large");
  }

  const QString minishardEncStr = optionalString(shObj, "minishard_index_encoding", "raw");
  sharding.minishardIndexEncoding = parseShardedEncoding(minishardEncStr, "minishard_index_encoding");

  const QString dataEncStr = optionalString(shObj, "data_encoding", "raw");
  sharding.dataEncoding = parseShardedEncoding(dataEncStr, "data_encoding");

  sharding.shardIndexSize = 16ULL << sharding.minishardBits;
  CHECK((sharding.shardIndexSize % 16ULL) == 0);

  sharding.minishardMask = sharding.minishardBits == 0 ? 0 : ((1ULL << sharding.minishardBits) - 1ULL);
  sharding.shardMask = sharding.shardBits == 0 ? 0 : ((1ULL << sharding.shardBits) - 1ULL);
  const size_t sumBits = sharding.minishardBits + sharding.shardBits;
  sharding.shardAndMinishardMask = sumBits == 64 ? ~0ULL : (sumBits == 0 ? 0 : ((1ULL << sumBits) - 1ULL));
  sharding.shardHexDigits = static_cast<int>((sharding.shardBits + 3) / 4);

  return sharding;
}

template<typename T>
T readLE(const uint8_t* p);

template<>
uint32_t readLE<uint32_t>(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

template<>
float readLE<float>(const uint8_t* p)
{
  uint32_t u = readLE<uint32_t>(p);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

ZMesh decodeLegacyNgMesh(std::span<const uint8_t> bytes)
{
  if (bytes.size() < 4) {
    throw ZException("Invalid neuroglancer legacy mesh fragment: truncated header");
  }

  size_t off = 0;
  const uint32_t numVertices = readLE<uint32_t>(bytes.data() + off);
  off += 4;

  const size_t vertexBytes = static_cast<size_t>(numVertices) * 12;
  if (off + vertexBytes > bytes.size()) {
    throw ZException("Invalid neuroglancer legacy mesh fragment: truncated vertices");
  }

  std::vector<glm::vec3> vertices;
  vertices.resize(numVertices);
  for (uint32_t i = 0; i < numVertices; ++i) {
    const float x = readLE<float>(bytes.data() + off);
    const float y = readLE<float>(bytes.data() + off + 4);
    const float z = readLE<float>(bytes.data() + off + 8);
    vertices[i] = glm::vec3(x, y, z);
    off += 12;
  }

  if (off > bytes.size()) {
    throw ZException("Invalid neuroglancer legacy mesh fragment: truncated");
  }
  const size_t remaining = bytes.size() - off;
  if ((remaining % 4) != 0) {
    throw ZException("Invalid neuroglancer legacy mesh fragment: index section is not uint32-aligned");
  }
  if ((remaining % 12) != 0) {
    throw ZException("Invalid neuroglancer legacy mesh fragment: index section is not a multiple of 3 indices");
  }

  const size_t numIndices = remaining / 4;
  std::vector<uint32_t> indices;
  indices.resize(numIndices);
  for (size_t i = 0; i < numIndices; ++i) {
    indices[i] = readLE<uint32_t>(bytes.data() + off);
    off += 4;
  }

  ZMesh mesh(ZMesh::Type::TRIANGLES);
  mesh.setVertices(vertices);
  mesh.setIndices(indices);
  return mesh;
}

constexpr std::uint8_t kFirstBitLookupTable[256] = {
  0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5, 0, 1, 0, 2,
  0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6, 0, 1, 0, 2, 0, 1, 0, 3, 0,
  1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1,
  0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0,
  2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3,
  0, 1, 0, 2, 0, 1, 0, 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0,
  1, 0, 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};

struct DecodedPartitionedDracoMesh
{
  std::vector<glm::vec3> vertices;
  std::optional<std::vector<glm::vec3>> normals;
  std::vector<std::vector<uint32_t>> partitionIndices;
};

[[nodiscard]] bool lessMsb(uint32_t a, uint32_t b)
{
  return a < b && a < (a ^ b);
}

[[nodiscard]] bool zorder3LessThan(uint32_t x0, uint32_t y0, uint32_t z0, uint32_t x1, uint32_t y1, uint32_t z1)
{
  uint32_t mostSignificant0 = z0;
  uint32_t mostSignificant1 = z1;

  if (lessMsb(mostSignificant0 ^ mostSignificant1, y0 ^ y1)) {
    mostSignificant0 = y0;
    mostSignificant1 = y1;
  }
  if (lessMsb(mostSignificant0 ^ mostSignificant1, x0 ^ x1)) {
    mostSignificant0 = x0;
    mostSignificant1 = x1;
  }

  return mostSignificant0 < mostSignificant1;
}

[[nodiscard]] uint32_t getOctreeChildIndex(uint32_t x, uint32_t y, uint32_t z)
{
  return (x & 1U) | ((y << 1U) & 2U) | ((z << 2U) & 4U);
}

void appendMeshGeometry(const ZMesh& frag,
                        std::vector<glm::vec3>& mergedVertices,
                        std::vector<glm::vec3>* mergedNormals,
                        std::vector<uint32_t>& mergedIndices)
{
  const size_t base = mergedVertices.size();
  CHECK(base <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  const uint32_t base32 = static_cast<uint32_t>(base);

  mergedVertices.insert(mergedVertices.end(), frag.vertices().begin(), frag.vertices().end());
  if (mergedNormals != nullptr) {
    CHECK(frag.normals().size() == frag.vertices().size());
    mergedNormals->insert(mergedNormals->end(), frag.normals().begin(), frag.normals().end());
  }
  mergedIndices.reserve(mergedIndices.size() + frag.indices().size());
  for (const uint32_t idx : frag.indices()) {
    CHECK(idx <= std::numeric_limits<uint32_t>::max() - base32);
    mergedIndices.push_back(idx + base32);
  }
}

std::shared_ptr<ZMesh> mergeMeshes(const std::vector<std::shared_ptr<ZMesh>>& meshes)
{
  std::vector<glm::vec3> mergedVertices;
  std::vector<glm::vec3> mergedNormals;
  std::vector<uint32_t> mergedIndices;

  bool mergeNormals = true;
  for (const auto& mesh : meshes) {
    if (!mesh || mesh->empty()) {
      continue;
    }
    if (mesh->normals().size() != mesh->vertices().size()) {
      mergeNormals = false;
      break;
    }
  }

  for (const auto& mesh : meshes) {
    if (!mesh || mesh->empty()) {
      continue;
    }
    appendMeshGeometry(*mesh, mergedVertices, mergeNormals ? &mergedNormals : nullptr, mergedIndices);
  }

  auto out = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  out->setVertices(mergedVertices);
  out->setIndices(mergedIndices);
  if (mergeNormals) {
    out->setNormals(mergedNormals);
  }
  if (!out->empty()) {
    if (!mergeNormals) {
      out->generateNormals();
    }
  }
  return out;
}

std::shared_ptr<ZMesh> buildCompactMesh(const std::vector<glm::vec3>& vertices,
                                        const std::vector<uint32_t>& indices,
                                        const std::vector<glm::vec3>* normals,
                                        const folly::CancellationToken& cancellationToken)
{
  if (indices.empty()) {
    return nullptr;
  }

  std::vector<uint32_t> remap(vertices.size(), std::numeric_limits<uint32_t>::max());
  std::vector<glm::vec3> compactVertices;
  compactVertices.reserve(std::min(vertices.size(), indices.size()));

  const bool useNormals = (normals != nullptr && normals->size() == vertices.size());
  std::vector<glm::vec3> compactNormals;
  if (useNormals) {
    compactNormals.reserve(compactVertices.capacity());
  }

  std::vector<uint32_t> compactIndices;
  compactIndices.reserve(indices.size());

  for (size_t i = 0; i < indices.size(); ++i) {
    maybeCancelPeriodic(cancellationToken, i);
    const uint32_t idx = indices[i];
    CHECK(idx < vertices.size());
    uint32_t mapped = remap[idx];
    if (mapped == std::numeric_limits<uint32_t>::max()) {
      mapped = static_cast<uint32_t>(compactVertices.size());
      remap[idx] = mapped;
      compactVertices.push_back(vertices[idx]);
      if (useNormals) {
        compactNormals.push_back((*normals)[idx]);
      }
    }
    compactIndices.push_back(mapped);
  }

  auto mesh = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  mesh->setVertices(compactVertices);
  mesh->setIndices(compactIndices);
  if (useNormals) {
    mesh->setNormals(compactNormals);
  } else {
    mesh->generateNormals();
  }
  return mesh;
}

DecodedPartitionedDracoMesh
decodePartitionedDracoMesh(std::span<const uint8_t> bytes,
                           int vertexQuantizationBits,
                           bool partition,
                           const folly::CancellationToken& cancellationToken)
{
  if (bytes.empty()) {
    throw ZException("Invalid draco fragment: empty");
  }

  draco::DecoderBuffer buffer;
  buffer.Init(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  auto typeStatus = draco::Decoder::GetEncodedGeometryType(&buffer);
  if (!typeStatus.ok()) {
    throw ZException(fmt::format("Failed to decode draco fragment: {}", typeStatus.status().error_msg()));
  }
  if (typeStatus.value() != draco::TRIANGULAR_MESH) {
    throw ZException("Unsupported draco fragment geometry type (expected triangular mesh)");
  }

  draco::Decoder decoder;
  decoder.SetSkipAttributeTransform(draco::GeometryAttribute::POSITION);
  auto meshStatus = decoder.DecodeMeshFromBuffer(&buffer);
  if (!meshStatus.ok()) {
    throw ZException(fmt::format("Failed to decode draco mesh: {}", meshStatus.status().error_msg()));
  }

  std::unique_ptr<draco::Mesh> dmesh = std::move(meshStatus).value();
  CHECK(dmesh);
  maybeCancel(cancellationToken);

  const draco::PointAttribute* const pos = dmesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
  if (pos == nullptr || pos->size() == 0) {
    throw ZException("Failed to decode draco mesh: missing POSITION attribute");
  }
  if (pos->num_components() != 3) {
    throw ZException("Failed to decode draco mesh: POSITION must have 3 components");
  }
  if (pos->data_type() != draco::DT_INT32 && pos->data_type() != draco::DT_UINT32 &&
      pos->data_type() != draco::DT_FLOAT32) {
    throw ZException("Failed to decode draco mesh: unsupported POSITION data type");
  }
  if (dmesh->GetAttributeElementType(pos->unique_id()) != draco::MESH_CORNER_ATTRIBUTE) {
    throw ZException("Failed to decode draco mesh: POSITION must use corner attributes");
  }
  if (static_cast<size_t>(pos->size()) != static_cast<size_t>(dmesh->num_points())) {
    throw ZException("Failed to decode draco mesh: POSITION count mismatch");
  }

  const draco::PointAttribute* const normal = dmesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
  bool hasValidNormals = false;
  if (normal != nullptr && normal->size() != 0U) {
    hasValidNormals = true;
    if (normal->num_components() != 3) {
      hasValidNormals = false;
    }
    if (hasValidNormals && dmesh->GetAttributeElementType(normal->unique_id()) != draco::MESH_CORNER_ATTRIBUTE) {
      hasValidNormals = false;
    }
    // We index vertices by AttributeValueIndex (same as Neuroglancer), so normals must match.
    if (hasValidNormals && static_cast<size_t>(normal->size()) != static_cast<size_t>(pos->size())) {
      hasValidNormals = false;
    }
    if (hasValidNormals) {
      for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
        if (pos->mapped_index(i).value() != normal->mapped_index(i).value()) {
          hasValidNormals = false;
          break;
        }
      }
    }
  }

  std::vector<uint32_t> indices(static_cast<size_t>(dmesh->num_faces()) * 3U);
  for (draco::FaceIndex fi(0); fi < dmesh->num_faces(); ++fi) {
    maybeCancelPeriodic(cancellationToken, static_cast<size_t>(fi.value()));
    const draco::Mesh::Face& face = dmesh->face(fi);
    indices[fi.value() * 3U + 0U] = face[0].value();
    indices[fi.value() * 3U + 1U] = face[1].value();
    indices[fi.value() * 3U + 2U] = face[2].value();
  }

  if (!pos->is_mapping_identity()) {
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = pos->mapped_index(draco::PointIndex(indices[i])).value();
    }
  }

  DecodedPartitionedDracoMesh out;
  out.vertices.resize(dmesh->num_points());
  if (hasValidNormals) {
    out.normals.emplace();
    out.normals->resize(dmesh->num_points());
  }

  // Draco attributes are not guaranteed to be a tightly packed `T[3]` array with
  // `byte_stride == 3 * sizeof(T)`. The runtime mesh LOD path added on 2026-03-12
  // read POSITION data by raw pointer arithmetic from `GetAddress(0)`, which is
  // only valid for one specific storage layout. Decode through Draco's attribute
  // conversion API so POSITION values are read correctly regardless of stride,
  // offset, or decoder-internal layout differences across platforms/toolchains.
  switch (pos->data_type()) {
    case draco::DT_INT32: {
      for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
        maybeCancelPeriodic(cancellationToken, static_cast<size_t>(i.value()));
        int32_t tmp[3] = {0, 0, 0};
        if (!pos->ConvertValue<int32_t>(draco::AttributeValueIndex(i.value()), 3, tmp)) {
          throw ZException("Failed to decode draco mesh: could not read INT32 POSITION value");
        }
        out.vertices[i.value()] =
          glm::vec3(static_cast<float>(tmp[0]), static_cast<float>(tmp[1]), static_cast<float>(tmp[2]));
      }
      break;
    }
    case draco::DT_UINT32: {
      for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
        maybeCancelPeriodic(cancellationToken, static_cast<size_t>(i.value()));
        uint32_t tmp[3] = {0U, 0U, 0U};
        if (!pos->ConvertValue<uint32_t>(draco::AttributeValueIndex(i.value()), 3, tmp)) {
          throw ZException("Failed to decode draco mesh: could not read UINT32 POSITION value");
        }
        out.vertices[i.value()] =
          glm::vec3(static_cast<float>(tmp[0]), static_cast<float>(tmp[1]), static_cast<float>(tmp[2]));
      }
      break;
    }
    case draco::DT_FLOAT32: {
      for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
        maybeCancelPeriodic(cancellationToken, static_cast<size_t>(i.value()));
        float tmp[3] = {0.0F, 0.0F, 0.0F};
        if (!pos->ConvertValue<float>(draco::AttributeValueIndex(i.value()), 3, tmp)) {
          throw ZException("Failed to decode draco mesh: could not read FLOAT32 POSITION value");
        }
        out.vertices[i.value()] = glm::vec3(tmp[0], tmp[1], tmp[2]);
      }
      break;
    }
    default:
      CHECK(false) << "Unexpected Draco POSITION data type";
  }

  if (hasValidNormals) {
    // Decode in AttributeValueIndex order (same indexing as vertices/indices).
    // If the normals turn out to be malformed (non-finite or degenerate), drop them and fall
    // back to CPU normal generation later.
    bool ok = true;
    for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
      maybeCancelPeriodic(cancellationToken, static_cast<size_t>(i.value()));
      float tmp[3] = {0.0F, 0.0F, 0.0F};
      if (!normal->ConvertValue<float>(draco::AttributeValueIndex(i.value()), 3, tmp)) {
        ok = false;
        break;
      }
      if (!std::isfinite(tmp[0]) || !std::isfinite(tmp[1]) || !std::isfinite(tmp[2])) {
        ok = false;
        break;
      }
      const glm::vec3 n(tmp[0], tmp[1], tmp[2]);
      const float len2 = glm::dot(n, n);
      if (!(len2 > 0.0F) || !std::isfinite(len2)) {
        ok = false;
        break;
      }
      (*out.normals)[i.value()] = n * (1.0F / std::sqrt(len2));
    }
    if (!ok) {
      out.normals.reset();
    }
  }

  out.partitionIndices.resize(partition ? 8U : 1U);
  if (!partition) {
    out.partitionIndices[0] = std::move(indices);
    return out;
  }

  CHECK(vertexQuantizationBits == 10 || vertexQuantizationBits == 16);
  const uint32_t partitionPoint = ((std::numeric_limits<uint32_t>::max() >> (32 - vertexQuantizationBits)) / 2U) + 1U;

  auto vertexMask = [&](const glm::vec3& v) -> uint32_t {
    uint32_t mask = 0xFFU;
    if (v.x < static_cast<float>(partitionPoint)) {
      mask &= 0b01010101U;
    } else if (v.x > static_cast<float>(partitionPoint)) {
      mask &= 0b10101010U;
    }
    if (v.y < static_cast<float>(partitionPoint)) {
      mask &= 0b00110011U;
    } else if (v.y > static_cast<float>(partitionPoint)) {
      mask &= 0b11001100U;
    }
    if (v.z < static_cast<float>(partitionPoint)) {
      mask &= 0b00001111U;
    } else if (v.z > static_cast<float>(partitionPoint)) {
      mask &= 0b11110000U;
    }
    return mask;
  };

  for (size_t i = 0; i < indices.size(); i += 3U) {
    maybeCancelPeriodic(cancellationToken, i / 3U);
    uint32_t mask = 0xFFU;
    for (size_t j = 0; j < 3U; ++j) {
      mask &= vertexMask(out.vertices[indices[i + j]]);
    }
    const uint32_t partitionIndex = kFirstBitLookupTable[mask];
    out.partitionIndices[partitionIndex].push_back(indices[i + 0U]);
    out.partitionIndices[partitionIndex].push_back(indices[i + 1U]);
    out.partitionIndices[partitionIndex].push_back(indices[i + 2U]);
  }

  return out;
}

void computeOctreeChildOffsets(std::vector<ZNeuroglancerPrecomputedMeshSource::MultiLodOctreeNode>& octree,
                               uint32_t childStart,
                               uint32_t childEnd,
                               uint32_t parentEnd)
{
  uint32_t childNode = childStart;
  for (uint32_t parentNode = childEnd; parentNode < parentEnd; ++parentNode) {
    const uint32_t parentX = octree[parentNode].gridPosition.x;
    const uint32_t parentY = octree[parentNode].gridPosition.y;
    const uint32_t parentZ = octree[parentNode].gridPosition.z;
    while (childNode < childEnd) {
      const uint32_t childX = octree[childNode].gridPosition.x >> 1U;
      const uint32_t childY = octree[childNode].gridPosition.y >> 1U;
      const uint32_t childZ = octree[childNode].gridPosition.z >> 1U;
      if (!zorder3LessThan(childX, childY, childZ, parentX, parentY, parentZ)) {
        break;
      }
      ++childNode;
    }
    octree[parentNode].childBegin = childNode;
    while (childNode < childEnd) {
      const uint32_t childX = octree[childNode].gridPosition.x >> 1U;
      const uint32_t childY = octree[childNode].gridPosition.y >> 1U;
      const uint32_t childZ = octree[childNode].gridPosition.z >> 1U;
      if (childX != parentX || childY != parentY || childZ != parentZ) {
        break;
      }
      ++childNode;
    }
    octree[parentNode].childEndAndEmpty += childNode;
  }
}

uint32_t generateHigherOctreeLevel(std::vector<ZNeuroglancerPrecomputedMeshSource::MultiLodOctreeNode>& octree,
                                   uint32_t priorStart,
                                   uint32_t priorEnd)
{
  uint32_t curEnd = priorEnd;
  octree[curEnd].gridPosition = glm::uvec3(octree[priorStart].gridPosition.x >> 1U,
                                           octree[priorStart].gridPosition.y >> 1U,
                                           octree[priorStart].gridPosition.z >> 1U);
  octree[curEnd].childBegin = priorStart;
  for (uint32_t i = priorStart + 1U; i < priorEnd; ++i) {
    const glm::uvec3 parentPos(octree[i].gridPosition.x >> 1U,
                               octree[i].gridPosition.y >> 1U,
                               octree[i].gridPosition.z >> 1U);
    if (parentPos != octree[curEnd].gridPosition) {
      octree[curEnd].childEndAndEmpty = i;
      ++curEnd;
      octree[curEnd].gridPosition = parentPos;
      octree[curEnd].childBegin = i;
      octree[curEnd].childEndAndEmpty = 0;
    }
  }
  octree[curEnd].childEndAndEmpty = priorEnd;
  return curEnd + 1U;
}

[[nodiscard]] QString meshSourceCacheKey(const QUrl& meshDirUrl,
                                         const std::array<double, 3>& baseResolutionNm,
                                         const std::array<int64_t, 3>& baseVoxelOffset,
                                         const ZRemoteObjectStore& objectStore)
{
  return QStringLiteral("%1|%2,%3,%4|%5,%6,%7|scope:%8")
    .arg(meshDirUrl.toString())
    .arg(QString::number(baseResolutionNm[0], 'g', 17))
    .arg(QString::number(baseResolutionNm[1], 'g', 17))
    .arg(QString::number(baseResolutionNm[2], 'g', 17))
    .arg(baseVoxelOffset[0])
    .arg(baseVoxelOffset[1])
    .arg(baseVoxelOffset[2])
    .arg(
      QString::number(static_cast<qulonglong>(reinterpret_cast<uintptr_t>(objectStore.contentCacheScopeToken())), 16));
}

std::mutex& openedMeshSourcesMutex()
{
  static std::mutex mutex;
  return mutex;
}

} // namespace

std::optional<uint32_t> ZNeuroglancerPrecomputedMeshSource::MultiLodManifest::coarsestStoredLod() const
{
  for (size_t i = lodScales.size(); i > 0; --i) {
    if (lodScales[i - 1] != 0.0F) {
      return static_cast<uint32_t>(i - 1);
    }
  }
  return std::nullopt;
}

uint64_t ZNeuroglancerPrecomputedMeshSource::MultiLodManifest::totalFragmentBytes() const
{
  return offsets.empty() ? 0ULL : offsets.back();
}

bool ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh::empty() const
{
  return std::ranges::none_of(subMeshes, [](const auto& mesh) {
    return mesh && !mesh->empty();
  });
}

std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>
ZNeuroglancerPrecomputedMeshSource::open(const QUrl& meshDirUrl,
                                         std::array<double, 3> baseResolutionNm,
                                         std::array<int64_t, 3> baseVoxelOffset,
                                         std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext,
                                         const folly::CancellationToken& cancellationToken)
{
  try {
    auto task = folly::coro::co_withCancellation(
      cancellationToken,
      openAsync(meshDirUrl, baseResolutionNm, baseVoxelOffset, std::move(remoteContext)));
    return folly::coro::blockingWait(std::move(task));
  }
  catch (const folly::OperationCancelled&) {
    throw ZCancellationException();
  }
}

folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>>
ZNeuroglancerPrecomputedMeshSource::openAsync(const QUrl& meshDirUrl,
                                              std::array<double, 3> baseResolutionNm,
                                              std::array<int64_t, 3> baseVoxelOffset,
                                              std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext)
{
  CHECK(remoteContext);
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);
  using SharedMeshInfoCache = std::unordered_map<QString, std::weak_ptr<const SharedMeshInfo>>;
  static SharedMeshInfoCache sharedMeshInfoCache;

  QUrl normalizedMeshDirUrl = meshDirUrl;
  if (!normalizedMeshDirUrl.toString().endsWith('/')) {
    normalizedMeshDirUrl = QUrl(normalizedMeshDirUrl.toString() + "/");
  }

  const QString cacheKey =
    meshSourceCacheKey(normalizedMeshDirUrl, baseResolutionNm, baseVoxelOffset, remoteContext->objectStore());
  std::shared_ptr<const SharedMeshInfo> sharedMeshInfo;
  {
    std::scoped_lock lock(openedMeshSourcesMutex());
    if (auto it = sharedMeshInfoCache.find(cacheKey); it != sharedMeshInfoCache.end()) {
      if (auto existing = it->second.lock()) {
        sharedMeshInfo = std::move(existing);
      } else {
        sharedMeshInfoCache.erase(it);
      }
    }
  }

  if (!sharedMeshInfo) {
    auto parsedMeshInfo = std::make_shared<SharedMeshInfo>();
    parsedMeshInfo->meshDirUrl = normalizedMeshDirUrl;
    parsedMeshInfo->baseResolutionNm = baseResolutionNm;
    parsedMeshInfo->baseVoxelOffset = baseVoxelOffset;

    {
      const glm::vec3 invRes(static_cast<float>(1.0 / parsedMeshInfo->baseResolutionNm[0]),
                             static_cast<float>(1.0 / parsedMeshInfo->baseResolutionNm[1]),
                             static_cast<float>(1.0 / parsedMeshInfo->baseResolutionNm[2]));
      const glm::vec3 voxelOffset(static_cast<float>(parsedMeshInfo->baseVoxelOffset[0]),
                                  static_cast<float>(parsedMeshInfo->baseVoxelOffset[1]),
                                  static_cast<float>(parsedMeshInfo->baseVoxelOffset[2]));

      // voxel = nm / baseResolutionNm - baseVoxelOffset
      glm::mat4 voxelFromModel(1.0F);
      voxelFromModel = glm::translate(voxelFromModel, -voxelOffset);
      voxelFromModel = glm::scale(voxelFromModel, invRes);
      parsedMeshInfo->voxelFromModel = voxelFromModel;
    }

    const QUrl infoUrl = parsedMeshInfo->meshDirUrl.resolved(QUrl("info"));
    const std::string infoUrlStr = toStdString(infoUrl.toString());
    const auto resOpt = co_await remoteContext->getResponseAsync(infoUrlStr);
    maybeCancel(cancellationToken);
    if (resOpt) {
      if (resOpt->status != 200) {
        throw ZException(
          fmt::format("Failed to fetch neuroglancer mesh info from '{}' (HTTP {})", infoUrlStr, resOpt->status));
      }

      const std::string infoText(reinterpret_cast<const char*>(resOpt->body.data()), resOpt->body.size());
      json::value jv = json::parse(infoText);
      if (!jv.is_object()) {
        throw ZException("Neuroglancer mesh info is not a JSON object");
      }
      const auto& root = jv.as_object();

      const QString type = optionalString(root, "@type", "neuroglancer_legacy_mesh").trimmed();
      if (type == "neuroglancer_legacy_mesh") {
        parsedMeshInfo->meshType = MeshType::Legacy;
      } else {
        if (type != "neuroglancer_multilod_draco") {
          throw ZException(fmt::format("Unsupported neuroglancer mesh '@type': '{}'", toStdString(type)));
        }

        MultiLodInfo info{};
        info.lodScaleMultiplier = requireDouble(root, "lod_scale_multiplier");
        info.vertexQuantizationBits = static_cast<int>(requireUint64(root, "vertex_quantization_bits"));
        if (info.vertexQuantizationBits != 10 && info.vertexQuantizationBits != 16) {
          throw ZException("Neuroglancer multi-LOD draco mesh requires vertex_quantization_bits to be 10 or 16");
        }
        info.transform = parseTransform(root);
        info.voxelFromStored = parsedMeshInfo->voxelFromModel * info.transform;

        auto shardingIt = root.find("sharding");
        if (shardingIt != root.end() && !shardingIt->value().is_null()) {
          if (!shardingIt->value().is_object()) {
            throw ZException("Invalid neuroglancer mesh sharding: expected object");
          }
          info.sharding = parseShardingSpec(shardingIt->value().as_object());
        }

        parsedMeshInfo->meshType = MeshType::MultiLodDraco;
        parsedMeshInfo->multiLodInfo = std::move(info);
      }
    }
    maybeCancel(cancellationToken);

    {
      std::scoped_lock lock(openedMeshSourcesMutex());
      if (auto it = sharedMeshInfoCache.find(cacheKey); it != sharedMeshInfoCache.end()) {
        if (auto existing = it->second.lock()) {
          sharedMeshInfo = std::move(existing);
        } else {
          sharedMeshInfoCache.erase(it);
        }
      }
      if (!sharedMeshInfo) {
        sharedMeshInfo = std::move(parsedMeshInfo);
        sharedMeshInfoCache[cacheKey] = sharedMeshInfo;
      }
    }
  }
  maybeCancel(cancellationToken);

  auto out = std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>(new ZNeuroglancerPrecomputedMeshSource());
  out->m_sharedMeshInfo = std::move(sharedMeshInfo);
  out->m_remoteContext = std::move(remoteContext);
  co_return out;
}

std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>
ZNeuroglancerPrecomputedMeshSource::open(const QUrl& meshDirUrl,
                                         std::array<double, 3> baseResolutionNm,
                                         std::array<int64_t, 3> baseVoxelOffset,
                                         std::chrono::milliseconds timeout,
                                         std::shared_ptr<const ZRemoteObjectStore> objectStore,
                                         const folly::CancellationToken& cancellationToken)
{
  try {
    auto task = folly::coro::co_withCancellation(
      cancellationToken,
      openAsync(meshDirUrl, baseResolutionNm, baseVoxelOffset, timeout, std::move(objectStore)));
    return folly::coro::blockingWait(std::move(task));
  }
  catch (const folly::OperationCancelled&) {
    throw ZCancellationException();
  }
}

folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>>
ZNeuroglancerPrecomputedMeshSource::openAsync(const QUrl& meshDirUrl,
                                              std::array<double, 3> baseResolutionNm,
                                              std::array<int64_t, 3> baseVoxelOffset,
                                              std::chrono::milliseconds timeout,
                                              std::shared_ptr<const ZRemoteObjectStore> objectStore)
{
  co_return co_await openAsync(meshDirUrl,
                               baseResolutionNm,
                               baseVoxelOffset,
                               ZNeuroglancerRemoteContext::create(timeout, std::move(objectStore)));
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest>>
ZNeuroglancerPrecomputedMeshSource::loadManifestAsync(uint64_t segmentId) const
{
  CHECK(supportsRuntimeLod());
  const std::shared_ptr<const CachedMultiLodManifest> cached = co_await loadCachedManifestAsync(segmentId);
  CHECK(cached);
  CHECK(cached->manifest);
  co_return cached->manifest;
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest>
ZNeuroglancerPrecomputedMeshSource::loadManifestBlocking(uint64_t segmentId,
                                                         const folly::CancellationToken& cancellationToken) const
{
  try {
    auto task = folly::coro::co_withCancellation(cancellationToken, loadManifestAsync(segmentId));
    return folly::coro::blockingWait(std::move(task));
  }
  catch (const folly::OperationCancelled&) {
    // co_withCancellation reports cancellation via folly::OperationCancelled.
    // Translate to Atlas' cancellation type so callers can unwind consistently.
    throw ZCancellationException();
  }
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest>
ZNeuroglancerPrecomputedMeshSource::loadManifestBlocking(uint64_t segmentId) const
{
  return folly::coro::blockingWait(loadManifestAsync(segmentId));
}

ZBBox<glm::dvec3>
ZNeuroglancerPrecomputedMeshSource::multiLodClipBoundsLocalVoxel(const MultiLodManifest& manifest) const
{
  CHECK(supportsRuntimeLod());
  CHECK(sharedMeshInfo().multiLodInfo) << "Runtime multi-LOD clip bounds require multiLodInfo";

  // Manifest clip bounds are expressed in the mesh's stored coordinate space (before voxelFromStored).
  // The runtime vertex transform uses:
  //   stored = gridOrigin + vertexOffsets[lod] + chunkShape * (2^lod) * gridPos + quantizedVertex *
  //   (chunkShape*2^lod/maxQuant)
  //
  // clipLower/clipUpper are derived from the LOD0 gridPos coverage (clipUpper is exclusive, +1 applied).
  // Include the lod0 vertex offset so the bound encloses all chunks.
  glm::dvec3 lowerStored(manifest.clipLowerBound);
  glm::dvec3 upperStored(manifest.clipUpperBound);
  if (!manifest.vertexOffsets.empty()) {
    const glm::dvec3 lod0Offset(manifest.vertexOffsets[0]);
    lowerStored += lod0Offset;
    upperStored += lod0Offset;
  }

  ZBBox<glm::dvec3> out;
  if (lowerStored.x > upperStored.x || lowerStored.y > upperStored.y || lowerStored.z > upperStored.z) {
    return out;
  }

  const glm::dmat4 voxelFromStored(sharedMeshInfo().multiLodInfo->voxelFromStored);
  auto expandCorner = [&](double x, double y, double z) {
    const glm::dvec4 p = voxelFromStored * glm::dvec4(x, y, z, 1.0);
    out.expand(glm::dvec3(p));
  };

  const glm::dvec3 lo = lowerStored;
  const glm::dvec3 hi = upperStored;
  expandCorner(lo.x, lo.y, lo.z);
  expandCorner(lo.x, lo.y, hi.z);
  expandCorner(lo.x, hi.y, lo.z);
  expandCorner(lo.x, hi.y, hi.z);
  expandCorner(hi.x, lo.y, lo.z);
  expandCorner(hi.x, lo.y, hi.z);
  expandCorner(hi.x, hi.y, lo.z);
  expandCorner(hi.x, hi.y, hi.z);
  return out;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshAsync(uint64_t segmentId, uint32_t row) const
{
  co_return co_await loadChunkMeshAsync(segmentId, row, ChunkVertexSpace::LocalVoxel);
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshAsync(uint64_t segmentId,
                                                       uint32_t row,
                                                       ChunkVertexSpace vertexSpace) const
{
  CHECK(supportsRuntimeLod());
  co_return co_await loadMultiLodChunkMeshAsync(segmentId, row, vertexSpace);
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshBlocking(uint64_t segmentId,
                                                          uint32_t row,
                                                          const folly::CancellationToken& cancellationToken) const
{
  return loadChunkMeshBlocking(segmentId, row, ChunkVertexSpace::LocalVoxel, cancellationToken);
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshBlocking(uint64_t segmentId, uint32_t row) const
{
  return loadChunkMeshBlocking(segmentId, row, ChunkVertexSpace::LocalVoxel);
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshBlocking(uint64_t segmentId,
                                                          uint32_t row,
                                                          ChunkVertexSpace vertexSpace,
                                                          const folly::CancellationToken& cancellationToken) const
{
  try {
    auto task = folly::coro::co_withCancellation(cancellationToken, loadChunkMeshAsync(segmentId, row, vertexSpace));
    return folly::coro::blockingWait(std::move(task));
  }
  catch (const folly::OperationCancelled&) {
    // co_withCancellation reports cancellation via folly::OperationCancelled.
    // Translate to Atlas' cancellation type so callers can unwind consistently.
    throw ZCancellationException();
  }
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshBlocking(uint64_t segmentId,
                                                          uint32_t row,
                                                          ChunkVertexSpace vertexSpace) const
{
  return folly::coro::blockingWait(loadChunkMeshAsync(segmentId, row, vertexSpace));
}

std::vector<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshesBlocking(uint64_t segmentId, const std::vector<uint32_t>& rows) const
{
  std::vector<std::shared_ptr<const MultiLodChunkMesh>> out;
  out.reserve(rows.size());
  for (const uint32_t row : rows) {
    out.push_back(loadChunkMeshBlocking(segmentId, row));
  }
  return out;
}

folly::coro::Task<std::shared_ptr<ZMesh>> ZNeuroglancerPrecomputedMeshSource::loadMeshAsync(uint64_t segmentId,
                                                                                            LodPolicy lodPolicy) const
{
  switch (meshType()) {
    case MeshType::Legacy:
      co_return co_await loadLegacyMeshAsync(segmentId);
    case MeshType::MultiLodDraco:
      co_return co_await loadMultiLodMeshAsync(segmentId, lodPolicy);
  }
  throw ZException("Invalid mesh type");
}

std::shared_ptr<ZMesh> ZNeuroglancerPrecomputedMeshSource::loadMeshBlocking(uint64_t segmentId,
                                                                            LodPolicy lodPolicy) const
{
  return folly::coro::blockingWait(loadMeshAsync(segmentId, lodPolicy));
}

folly::coro::Task<std::optional<std::vector<uint8_t>>> ZNeuroglancerPrecomputedMeshSource::getHttpBytesAsync(const std::string& url) const
{
  co_return co_await m_remoteContext->getBytesAsync(url);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>> ZNeuroglancerPrecomputedMeshSource::getHttpRangeBytesAsync(
  const std::string& url,
  uint64_t offset,
  uint64_t length) const
{
  co_return co_await m_remoteContext->getRangeBytesAsync(url,
                                                         offset,
                                                         length,
                                                         ZRemoteRangeReadPolicy::AllowFullResponseSlice);
}

folly::coro::Task<std::shared_ptr<ZMesh>> ZNeuroglancerPrecomputedMeshSource::loadLegacyMeshAsync(uint64_t segmentId) const
{
  CHECK(meshType() == MeshType::Legacy);

  const QString base = meshDirUrl().toString();
  const QString metaRel = QString::number(segmentId) + ":0";
  const std::string metaUrl = toStdString(base + metaRel);

  auto metaBytesOpt = co_await getHttpBytesAsync(metaUrl);
  if (!metaBytesOpt) {
    throw ZNotFoundException(
      fmt::format("Neuroglancer legacy mesh metadata not found for segment {} (expected '{}')", segmentId, metaUrl));
  }

  const std::string metaText(reinterpret_cast<const char*>(metaBytesOpt->data()), metaBytesOpt->size());
  json::value jv = json::parse(metaText);
  if (!jv.is_object()) {
    throw ZException("Invalid neuroglancer legacy mesh metadata: expected JSON object");
  }
  const auto& jo = jv.as_object();

  auto fragIt = jo.find("fragments");
  if (fragIt == jo.end() || !fragIt->value().is_array()) {
    throw ZException("Invalid neuroglancer legacy mesh metadata: missing or invalid 'fragments' array");
  }

  const auto& fragArr = fragIt->value().as_array();
  if (fragArr.empty()) {
    throw ZException("Neuroglancer legacy mesh has no fragments");
  }

  std::vector<glm::vec3> mergedVertices;
  std::vector<uint32_t> mergedIndices;

  auto appendFragment = [&](const ZMesh& frag) {
    const size_t base = mergedVertices.size();
    CHECK(base <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
    const uint32_t base32 = static_cast<uint32_t>(base);

    mergedVertices.insert(mergedVertices.end(), frag.vertices().begin(), frag.vertices().end());
    mergedIndices.reserve(mergedIndices.size() + frag.indices().size());
    for (const uint32_t idx : frag.indices()) {
      CHECK(idx <= std::numeric_limits<uint32_t>::max() - base32);
      mergedIndices.push_back(idx + base32);
    }
  };

  for (const auto& v : fragArr) {
    if (!v.is_string()) {
      throw ZException("Invalid neuroglancer legacy mesh metadata: fragments must be strings");
    }
    const QString fragName = json::value_to<QString>(v);
    const QUrl fragUrl = meshDirUrl().resolved(QUrl(fragName));
    const std::string fragUrlStr = toStdString(fragUrl.toString());

    auto fragBytesOpt = co_await getHttpBytesAsync(fragUrlStr);
    if (!fragBytesOpt) {
      throw ZException(fmt::format("Neuroglancer legacy mesh fragment not found: '{}'", fragUrlStr));
    }

    ZMesh fragMesh = decodeLegacyNgMesh(std::span<const uint8_t>(fragBytesOpt->data(), fragBytesOpt->size()));
    convertLegacyMeshVerticesNmToLocalVoxel(fragMesh);
    appendFragment(fragMesh);
  }

  auto out = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  out->setVertices(mergedVertices);
  out->setIndices(mergedIndices);
  out->generateNormals();
  co_return out;
}

folly::coro::Task<std::optional<ZNeuroglancerPrecomputedMeshSource::ShardedManifestBytes>>
ZNeuroglancerPrecomputedMeshSource::getShardedManifestBytesAsync(
  uint64_t segmentId,
  const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding) const
{
  // Chunk identifier for meshes is simply the segment ID.
  const uint64_t chunkId = segmentId;
  const uint64_t shiftedChunkId = chunkId >> sharding.preshiftBits;
  const uint64_t hashCode = (sharding.hash == ZNeuroglancerPrecomputedVolume::Scale::Sharding::Hash::Identity)
                              ? shiftedChunkId
                              : ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(shiftedChunkId, /*seed=*/0);
  const uint64_t shardAndMinishard = hashCode & sharding.shardAndMinishardMask;
  const uint64_t minishard = shardAndMinishard & sharding.minishardMask;
  const uint64_t shard = (shardAndMinishard >> sharding.minishardBits) & sharding.shardMask;

  auto entryOpt = co_await getNeuroglancerShardIndexEntryAsync(*m_remoteContext,
                                                               meshDirUrl(),
                                                               sharding,
                                                               shard,
                                                               minishard,
                                                               ZRemoteRangeReadPolicy::AllowFullResponseSlice);
  if (!entryOpt) {
    co_return std::nullopt;
  }

  auto decodedOpt = co_await getNeuroglancerDecodedMinishardIndexAsync(*m_remoteContext,
                                                                       *entryOpt,
                                                                       sharding,
                                                                       ZRemoteRangeReadPolicy::AllowFullResponseSlice);
  if (!decodedOpt) {
    co_return std::nullopt;
  }

  const auto& decoded = *decodedOpt;
  if (decoded.keys.empty()) {
    co_return std::nullopt;
  }

  auto locationOpt = findNeuroglancerShardedPayloadLocation(decoded, chunkId);
  if (!locationOpt) {
    co_return std::nullopt;
  }

  auto payloadOpt =
    co_await getNeuroglancerDecodedShardedPayloadBytesAsync(*m_remoteContext,
                                                            entryOpt->dataUrl,
                                                            *locationOpt,
                                                            sharding,
                                                            ZRemoteRangeReadPolicy::AllowFullResponseSlice);
  if (!payloadOpt) {
    co_return std::nullopt;
  }

  ShardedManifestBytes out;
  out.manifestBytes = std::move(payloadOpt->bytes);
  out.manifestStart = payloadOpt->start;
  out.dataUrl = entryOpt->dataUrl;
  co_return out;
}

ZNeuroglancerPrecomputedMeshSource::MultiLodManifest ZNeuroglancerPrecomputedMeshSource::parseMultiLodManifest(
  std::span<const uint8_t> bytes,
  const MultiLodInfo& info) const
{
  auto ensure = [&](size_t offset, size_t need) {
    if (offset + need > bytes.size()) {
      throw ZException("Invalid neuroglancer multi-LOD mesh manifest: truncated");
    }
  };

  size_t off = 0;
  auto readF = [&]() -> float {
    ensure(off, 4);
    float v = readLE<float>(bytes.data() + off);
    off += 4;
    return v;
  };
  auto readU32 = [&]() -> uint32_t {
    ensure(off, 4);
    uint32_t v = readLE<uint32_t>(bytes.data() + off);
    off += 4;
    return v;
  };
  auto readVec3 = [&]() -> glm::vec3 {
    const float x = readF();
    const float y = readF();
    const float z = readF();
    return glm::vec3(x, y, z);
  };

  MultiLodManifest manifest{};
  // Do not pass readF() directly as constructor arguments here. Argument evaluation
  // order is unspecified, and readF() advances the byte offset on every call.
  manifest.chunkShape = readVec3();
  manifest.gridOrigin = readVec3();
  const uint32_t numStoredLods = readU32();
  if (numStoredLods == 0) {
    throw ZException("Invalid neuroglancer multi-LOD mesh manifest: num_lods must be > 0");
  }

  std::vector<float> storedLodScales(numStoredLods);
  for (uint32_t i = 0; i < numStoredLods; ++i) {
    storedLodScales[i] = static_cast<float>(static_cast<double>(readF()) * info.lodScaleMultiplier);
  }

  manifest.vertexOffsets.resize(numStoredLods);
  for (uint32_t i = 0; i < numStoredLods; ++i) {
    manifest.vertexOffsets[i] = readVec3();
  }

  std::vector<uint32_t> numFragmentsPerLod(numStoredLods);
  for (uint32_t i = 0; i < numStoredLods; ++i) {
    numFragmentsPerLod[i] = readU32();
  }

  uint64_t totalFragments = 0;
  for (const uint32_t value : numFragmentsPerLod) {
    totalFragments += value;
  }
  if (bytes.size() != off + totalFragments * 16ULL) {
    throw ZException(
      fmt::format("Invalid neuroglancer multi-LOD mesh manifest size: {} bytes for {} lods and {} fragments",
                  bytes.size(),
                  numStoredLods,
                  totalFragments));
  }

  std::vector<uint32_t> fragmentInfo(totalFragments * 4ULL);
  for (uint64_t i = 0; i < fragmentInfo.size(); ++i) {
    fragmentInfo[i] = readU32();
  }

  glm::vec3 clipLower(std::numeric_limits<float>::infinity());
  glm::vec3 clipUpper(-std::numeric_limits<float>::infinity());
  uint32_t numLods = std::max(1U, numStoredLods);
  {
    size_t fragmentBase = 0;
    for (uint32_t lodIndex = 0; lodIndex < numStoredLods; ++lodIndex) {
      const uint32_t numFragments = numFragmentsPerLod[lodIndex];
      for (size_t axis = 0; axis < 3; ++axis) {
        uint32_t lowerBoundValue = std::numeric_limits<uint32_t>::max();
        uint32_t upperBoundValue = 0;
        const size_t base = fragmentBase + static_cast<size_t>(numFragments) * axis;
        for (uint32_t j = 0; j < numFragments; ++j) {
          const uint32_t value = fragmentInfo[base + j];
          lowerBoundValue = std::min(lowerBoundValue, value);
          upperBoundValue = std::max(upperBoundValue, value);
        }
        if (numFragments != 0) {
          while ((upperBoundValue >> (numLods - lodIndex - 1U)) != (lowerBoundValue >> (numLods - lodIndex - 1U))) {
            ++numLods;
          }
          if (lodIndex == 0) {
            clipLower[axis] = std::min(clipLower[axis], static_cast<float>(lowerBoundValue));
            clipUpper[axis] = std::max(clipUpper[axis], static_cast<float>(upperBoundValue + 1U));
          }
        }
      }
      fragmentBase += static_cast<size_t>(numFragments) * 4ULL;
    }
  }

  if (!std::isfinite(clipLower.x) || !std::isfinite(clipLower.y) || !std::isfinite(clipLower.z) ||
      !std::isfinite(clipUpper.x) || !std::isfinite(clipUpper.y) || !std::isfinite(clipUpper.z)) {
    clipLower = glm::vec3(0.0F);
    clipUpper = glm::vec3(0.0F);
  }

  uint32_t maxFragments = 0;
  {
    uint32_t prevNumFragments = 0;
    uint32_t prevLodIndex = 0;
    for (uint32_t lodIndex = 0; lodIndex < numStoredLods; ++lodIndex) {
      const uint32_t numFragments = numFragmentsPerLod[lodIndex];
      maxFragments += prevNumFragments * (lodIndex - prevLodIndex);
      prevLodIndex = lodIndex;
      prevNumFragments = numFragments;
      maxFragments += numFragments;
    }
    maxFragments += (numLods - 1U - prevLodIndex) * prevNumFragments;
  }

  std::vector<MultiLodOctreeNode> octreeTemp(static_cast<size_t>(std::max<uint32_t>(maxFragments, 1U)));
  std::vector<uint64_t> offsetsTemp(static_cast<size_t>(std::max<uint32_t>(maxFragments + 1U, 1U)), 0ULL);
  std::vector<uint32_t> rowLodsTemp(static_cast<size_t>(std::max<uint32_t>(maxFragments, 1U)), 0U);

  {
    uint32_t priorStart = 0;
    uint32_t baseRow = 0;
    uint64_t dataOffset = 0;
    size_t fragmentBase = 0;
    for (uint32_t lodIndex = 0; lodIndex < numStoredLods; ++lodIndex) {
      const uint32_t numFragments = numFragmentsPerLod[lodIndex];
      for (uint32_t j = 0; j < numFragments; ++j) {
        const size_t row = static_cast<size_t>(baseRow + j);
        octreeTemp[row].gridPosition = glm::uvec3(fragmentInfo[fragmentBase + j],
                                                  fragmentInfo[fragmentBase + numFragments + j],
                                                  fragmentInfo[fragmentBase + numFragments * 2ULL + j]);
        const uint32_t dataSize = fragmentInfo[fragmentBase + numFragments * 3ULL + j];
        dataOffset += dataSize;
        offsetsTemp[row + 1ULL] = dataOffset;
        octreeTemp[row].childBegin = 0;
        octreeTemp[row].childEndAndEmpty = (dataSize == 0U) ? 0x80000000U : 0U;
        rowLodsTemp[row] = lodIndex;
      }

      fragmentBase += static_cast<size_t>(numFragments) * 4ULL;

      if (lodIndex != 0U) {
        computeOctreeChildOffsets(octreeTemp, priorStart, baseRow, baseRow + numFragments);
      }

      priorStart = baseRow;
      baseRow += numFragments;
      while (lodIndex + 1U < numLods &&
             (lodIndex + 1U >= storedLodScales.size() || storedLodScales[lodIndex + 1U] == 0.0F)) {
        const uint32_t curEnd = generateHigherOctreeLevel(octreeTemp, priorStart, baseRow);
        std::fill(offsetsTemp.begin() + static_cast<ptrdiff_t>(baseRow) + 1,
                  offsetsTemp.begin() + static_cast<ptrdiff_t>(curEnd) + 1,
                  dataOffset);
        std::fill(rowLodsTemp.begin() + static_cast<ptrdiff_t>(baseRow),
                  rowLodsTemp.begin() + static_cast<ptrdiff_t>(curEnd),
                  lodIndex + 1U);
        priorStart = baseRow;
        baseRow = curEnd;
        ++lodIndex;
      }
    }
    manifest.octreeNodes.assign(octreeTemp.begin(), octreeTemp.begin() + static_cast<ptrdiff_t>(baseRow));
    manifest.offsets.assign(offsetsTemp.begin(), offsetsTemp.begin() + static_cast<ptrdiff_t>(baseRow) + 1);
    manifest.rowLods.assign(rowLodsTemp.begin(), rowLodsTemp.begin() + static_cast<ptrdiff_t>(baseRow));
  }

  manifest.lodScales.assign(numLods, 0.0F);
  for (uint32_t i = 0; i < numStoredLods; ++i) {
    manifest.lodScales[i] = storedLodScales[i];
  }

  manifest.clipLowerBound = manifest.gridOrigin + clipLower * manifest.chunkShape;
  manifest.clipUpperBound = manifest.gridOrigin + clipUpper * manifest.chunkShape;

  return manifest;
}

void ZNeuroglancerPrecomputedMeshSource::convertLegacyMeshVerticesNmToLocalVoxel(ZMesh& mesh) const
{
  // Legacy stored-model vertices are in nanometer units in the global frame:
  // voxel = nm / baseResolutionNm - baseVoxelOffset
  mesh.transformVerticesByMatrix(sharedMeshInfo().voxelFromModel);
}

ZNeuroglancerPrecomputedMeshSource::VertexToLocalVoxelTransform
ZNeuroglancerPrecomputedMeshSource::computeMultiLodVertexToLocalVoxelTransform(size_t lod,
                                                                               const glm::uvec3& fragmentPos,
                                                                               const MultiLodManifest& manifest,
                                                                               const MultiLodInfo& info) const
{
  CHECK(info.vertexQuantizationBits == 10 || info.vertexQuantizationBits == 16);
  const float maxQuant = static_cast<float>((1ULL << info.vertexQuantizationBits) - 1ULL);
  CHECK(maxQuant > 0.0F);

  const float lodScale = std::ldexp(1.0F, static_cast<int>(lod));
  const glm::vec3 scaledChunkShape = manifest.chunkShape * lodScale;

  const glm::vec3 fragmentTranslation =
    manifest.gridOrigin + manifest.vertexOffsets.at(lod) + scaledChunkShape * glm::vec3(fragmentPos);
  const glm::vec3 quantScale = scaledChunkShape / maxQuant;

  // The full transform is affine and has w=1, so use the cheaper affine form
  // (avoids per-vertex vec4 multiply and divide).
  const glm::mat3 storedToVoxelLinear(info.voxelFromStored);
  const glm::vec3 storedToVoxelTranslation(info.voxelFromStored[3]);

  const glm::vec3 base = storedToVoxelLinear * fragmentTranslation + storedToVoxelTranslation;

  glm::mat3 quantToVoxelLinear = storedToVoxelLinear;
  quantToVoxelLinear[0] *= quantScale.x;
  quantToVoxelLinear[1] *= quantScale.y;
  quantToVoxelLinear[2] *= quantScale.z;

  VertexToLocalVoxelTransform out;
  // Build an affine matrix that implements: base + quantToVoxelLinear * v
  out.vertexToLocalVoxel = glm::mat4(1.0F);
  out.vertexToLocalVoxel[0] = glm::vec4(quantToVoxelLinear[0], 0.0F);
  out.vertexToLocalVoxel[1] = glm::vec4(quantToVoxelLinear[1], 0.0F);
  out.vertexToLocalVoxel[2] = glm::vec4(quantToVoxelLinear[2], 0.0F);
  out.vertexToLocalVoxel[3] = glm::vec4(base, 1.0F);

  out.vertexToLocalVoxelNormalMatrix = glm::transpose(glm::inverse(quantToVoxelLinear));
  return out;
}

bool ZNeuroglancerPrecomputedMeshSource::convertMultiLodVerticesToLocalVoxel(std::vector<glm::vec3>& vertices,
                                                                             std::vector<glm::vec3>* normals,
                                                                             size_t lod,
                                                                             const glm::uvec3& fragmentPos,
                                                                             const MultiLodManifest& manifest,
                                                                             const MultiLodInfo& info,
                                                                             const folly::CancellationToken& cancellationToken) const
{
  const VertexToLocalVoxelTransform tf = computeMultiLodVertexToLocalVoxelTransform(lod, fragmentPos, manifest, info);

  const glm::vec3 base(tf.vertexToLocalVoxel[3]);
  const glm::mat3 quantToVoxelLinear(tf.vertexToLocalVoxel);

  for (size_t i = 0; i < vertices.size(); ++i) {
    maybeCancelPeriodic(cancellationToken, i);
    vertices[i] = base + quantToVoxelLinear * vertices[i];
  }

  if (normals == nullptr) {
    return true;
  }
  if (normals->size() != vertices.size()) {
    return false;
  }

  // Transform normals with the correct normal matrix.
  const glm::mat3 normalMatrix = tf.vertexToLocalVoxelNormalMatrix;
  for (size_t i = 0; i < normals->size(); ++i) {
    maybeCancelPeriodic(cancellationToken, i);
    glm::vec3& n = (*normals)[i];
    const glm::vec3 tn = normalMatrix * n;
    const float len2 = glm::dot(tn, tn);
    if (!std::isfinite(tn.x) || !std::isfinite(tn.y) || !std::isfinite(tn.z) || !(len2 > 0.0F) ||
        !std::isfinite(len2)) {
      return false;
    }
    n = tn * (1.0F / std::sqrt(len2));
  }
  return true;
}

std::array<float, 24> ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(const glm::mat4& modelViewProjection)
{
  std::array<float, 24> out{};
  const float* m = glm::value_ptr(modelViewProjection);
  const float m00 = m[0];
  const float m10 = m[1];
  const float m20 = m[2];
  const float m30 = m[3];
  const float m01 = m[4];
  const float m11 = m[5];
  const float m21 = m[6];
  const float m31 = m[7];
  const float m02 = m[8];
  const float m12 = m[9];
  const float m22 = m[10];
  const float m32 = m[11];
  const float m03 = m[12];
  const float m13 = m[13];
  const float m23 = m[14];
  const float m33 = m[15];

  out[0] = m30 + m00;
  out[1] = m31 + m01;
  out[2] = m32 + m02;
  out[3] = m33 + m03;

  out[4] = m30 - m00;
  out[5] = m31 - m01;
  out[6] = m32 - m02;
  out[7] = m33 - m03;

  out[8] = m30 + m10;
  out[9] = m31 + m11;
  out[10] = m32 + m12;
  out[11] = m33 + m13;

  out[12] = m30 - m10;
  out[13] = m31 - m11;
  out[14] = m32 - m12;
  out[15] = m33 - m13;

  const float nearA = m30 + m20;
  const float nearB = m31 + m21;
  const float nearC = m32 + m22;
  const float nearD = m33 + m23;
  const float farA = m30 - m20;
  const float farB = m31 - m21;
  const float farC = m32 - m22;
  const float farD = m33 - m23;

  const float nearNorm = std::sqrt(nearA * nearA + nearB * nearB + nearC * nearC);
  CHECK(nearNorm > 0.0F);
  out[16] = nearA / nearNorm;
  out[17] = nearB / nearNorm;
  out[18] = nearC / nearNorm;
  out[19] = nearD / nearNorm;

  const float farNorm = std::sqrt(farA * farA + farB * farB + farC * farC);
  CHECK(farNorm > 0.0F);
  out[20] = farA / farNorm;
  out[21] = farB / farNorm;
  out[22] = farC / farNorm;
  out[23] = farD / farNorm;

  return out;
}

std::vector<ZNeuroglancerPrecomputedMeshSource::MultiLodDesiredChunk>
ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(const MultiLodManifest& manifest,
                                                         const glm::mat4& modelViewProjection,
                                                         const std::array<float, 24>& clippingPlanes,
                                                         float detailCutoff,
                                                         uint32_t viewportWidth,
                                                         uint32_t viewportHeight)
{
  if (manifest.octreeNodes.empty() || viewportWidth == 0U || viewportHeight == 0U) {
    return {};
  }

  const float* m = glm::value_ptr(modelViewProjection);
  const float m00 = m[0];
  const float m01 = m[4];
  const float m02 = m[8];
  const float m10 = m[1];
  const float m11 = m[5];
  const float m12 = m[9];
  const float m30 = m[3];
  const float m31 = m[7];
  const float m32 = m[11];
  const float m33 = m[15];

  const float minWXCoeff = m30 > 0.0F ? 0.0F : 1.0F;
  const float minWYCoeff = m31 > 0.0F ? 0.0F : 1.0F;
  const float minWZCoeff = m32 > 0.0F ? 0.0F : 1.0F;

  const float nearA = clippingPlanes[16];
  const float nearB = clippingPlanes[17];
  const float nearC = clippingPlanes[18];
  const float nearD = clippingPlanes[19];

  auto pointW = [&](float x, float y, float z) {
    return m30 * x + m31 * y + m32 * z + m33;
  };
  auto boxW = [&](float xLower, float yLower, float zLower, float xUpper, float yUpper, float zUpper) {
    return pointW(xLower + minWXCoeff * (xUpper - xLower),
                  yLower + minWYCoeff * (yUpper - yLower),
                  zLower + minWZCoeff * (zUpper - zLower));
  };
  auto aabbVisible = [&](float xLower, float yLower, float zLower, float xUpper, float yUpper, float zUpper) {
    for (size_t i = 0; i < 6; ++i) {
      const float a = clippingPlanes[i * 4U + 0U];
      const float b = clippingPlanes[i * 4U + 1U];
      const float c = clippingPlanes[i * 4U + 2U];
      const float d = clippingPlanes[i * 4U + 3U];
      const float sum =
        std::max(a * xLower, a * xUpper) + std::max(b * yLower, b * yUpper) + std::max(c * zLower, c * zUpper) + d;
      if (sum < 0.0F) {
        return false;
      }
    }
    return true;
  };

  const float minWClip = pointW(-nearD * nearA, -nearD * nearB, -nearD * nearC);

  const float xScale =
    std::sqrt((m00 * static_cast<float>(viewportWidth)) * (m00 * static_cast<float>(viewportWidth)) +
              (m10 * static_cast<float>(viewportHeight)) * (m10 * static_cast<float>(viewportHeight)));
  const float yScale =
    std::sqrt((m01 * static_cast<float>(viewportWidth)) * (m01 * static_cast<float>(viewportWidth)) +
              (m11 * static_cast<float>(viewportHeight)) * (m11 * static_cast<float>(viewportHeight)));
  const float zScale =
    std::sqrt((m02 * static_cast<float>(viewportWidth)) * (m02 * static_cast<float>(viewportWidth)) +
              (m12 * static_cast<float>(viewportHeight)) * (m12 * static_cast<float>(viewportHeight)));
  const float scaleFactor = std::max({xScale, yScale, zScale});
  if (scaleFactor <= 0.0F) {
    return {};
  }

  std::vector<MultiLodDesiredChunk> out;
  const uint32_t maxLod = manifest.rowLods.at(manifest.rootRow());

  std::function<void(uint32_t, uint32_t, float)> handleChunk;
  handleChunk = [&](uint32_t lod, uint32_t row, float priorLodScale) {
    const float size = std::ldexp(1.0F, static_cast<int>(lod));
    const auto& node = manifest.octreeNodes.at(row);
    float xLower = static_cast<float>(node.gridPosition.x) * size * manifest.chunkShape.x + manifest.gridOrigin.x;
    float yLower = static_cast<float>(node.gridPosition.y) * size * manifest.chunkShape.y + manifest.gridOrigin.y;
    float zLower = static_cast<float>(node.gridPosition.z) * size * manifest.chunkShape.z + manifest.gridOrigin.z;
    float xUpper = xLower + size * manifest.chunkShape.x;
    float yUpper = yLower + size * manifest.chunkShape.y;
    float zUpper = zLower + size * manifest.chunkShape.z;

    xLower = std::max(xLower, manifest.clipLowerBound.x);
    yLower = std::max(yLower, manifest.clipLowerBound.y);
    zLower = std::max(zLower, manifest.clipLowerBound.z);
    xUpper = std::min(xUpper, manifest.clipUpperBound.x);
    yUpper = std::min(yUpper, manifest.clipUpperBound.y);
    zUpper = std::min(zUpper, manifest.clipUpperBound.z);

    if (!aabbVisible(xLower, yLower, zLower, xUpper, yUpper, zUpper)) {
      return;
    }

    const float minW = std::max(minWClip, boxW(xLower, yLower, zLower, xUpper, yUpper, zUpper));
    const float pixelSize = minW / scaleFactor;
    if (priorLodScale != 0.0F && pixelSize * detailCutoff >= priorLodScale) {
      return;
    }

    const float lodScale = manifest.lodScales.at(lod);
    if (lodScale != 0.0F) {
      out.push_back({lod, row, lodScale / pixelSize, node.empty()});
    }

    if (lod > 0U && (lodScale == 0.0F || pixelSize * detailCutoff < lodScale)) {
      const float nextPriorLodScale = lodScale == 0.0F ? priorLodScale : lodScale;
      for (uint32_t childRow = node.childBegin; childRow < node.childEnd(); ++childRow) {
        handleChunk(lod - 1U, childRow, nextPriorLodScale);
      }
    }
  };

  handleChunk(maxLod, manifest.rootRow(), 0.0F);
  return out;
}

std::vector<ZNeuroglancerPrecomputedMeshSource::MultiLodDrawChunk>
ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
  const MultiLodManifest& manifest,
  const glm::mat4& modelViewProjection,
  const std::array<float, 24>& clippingPlanes,
  float detailCutoff,
  uint32_t viewportWidth,
  uint32_t viewportHeight,
  const std::function<bool(uint32_t lod, uint32_t row, float renderScale)>& hasChunk)
{
  CHECK(hasChunk);

  uint32_t maxLod = 0;
  while (maxLod + 1U < manifest.lodScales.size() && manifest.lodScales[maxLod + 1U] != 0.0F) {
    ++maxLod;
  }

  struct StackEntry
  {
    int32_t row = -1;
    uint32_t parentSubChunkIndex = 0;
    float renderScale = 0.0F;
  };

  std::vector<MultiLodDrawChunk> out;
  std::vector<StackEntry> stack;
  stack.reserve(maxLod + 1U);
  uint32_t priorSubChunkIndex = 0;

  auto emitChunksUpTo = [&](uint32_t targetStackIndex, uint32_t subChunkIndex) {
    while (true) {
      if (stack.empty()) {
        return;
      }

      const uint32_t stackIndex = static_cast<uint32_t>(stack.size() - 1U);
      const uint32_t entryLod = maxLod - stackIndex;
      const StackEntry entry = stack.back();
      const uint32_t numSubChunks = entryLod == 0U ? 1U : 8U;

      if (targetStackIndex == stack.size()) {
        const uint32_t endSubChunk = subChunkIndex & (numSubChunks - 1U);
        if (priorSubChunkIndex != endSubChunk && entry.row != -1) {
          out.push_back(
            {entryLod, static_cast<uint32_t>(entry.row), priorSubChunkIndex, endSubChunk, entry.renderScale});
        }
        priorSubChunkIndex = endSubChunk + 1U;
        return;
      }

      if (priorSubChunkIndex != numSubChunks && entry.row != -1) {
        out.push_back(
          {entryLod, static_cast<uint32_t>(entry.row), priorSubChunkIndex, numSubChunks, entry.renderScale});
      }
      priorSubChunkIndex = entry.parentSubChunkIndex + 1U;
      stack.pop_back();
    }
  };

  uint32_t priorMissingLod = 0;
  const auto desiredChunks =
    desiredChunksForView(manifest, modelViewProjection, clippingPlanes, detailCutoff, viewportWidth, viewportHeight);
  for (const MultiLodDesiredChunk& desired : desiredChunks) {
    if (!desired.empty && !hasChunk(desired.lod, desired.row, desired.renderScale)) {
      priorMissingLod = std::max(priorMissingLod, desired.lod);
      continue;
    }
    if (desired.lod < priorMissingLod) {
      continue;
    }
    priorMissingLod = 0;

    const auto& node = manifest.octreeNodes.at(desired.row);
    const uint32_t subChunkIndex = getOctreeChildIndex(node.gridPosition.x, node.gridPosition.y, node.gridPosition.z);
    const uint32_t stackIndex = maxLod - desired.lod;
    emitChunksUpTo(stackIndex, subChunkIndex);
    if (stack.size() <= stackIndex) {
      stack.resize(stackIndex + 1U);
    }
    stack[stackIndex] = {desired.empty ? -1 : static_cast<int32_t>(desired.row), subChunkIndex, desired.renderScale};
    priorSubChunkIndex = 0;
  }

  emitChunksUpTo(0U, 0U);
  return out;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::CachedMultiLodManifest>>
ZNeuroglancerPrecomputedMeshSource::loadCachedManifestAsync(uint64_t segmentId) const
{
  CHECK(meshType() == MeshType::MultiLodDraco);
  CHECK(sharedMeshInfo().multiLodInfo);
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  {
    std::scoped_lock lock(m_manifestCacheMutex);
    if (auto it = m_manifestCache.find(segmentId); it != m_manifestCache.end()) {
      if (auto cached = it->second.lock()) {
        co_return cached;
      }
      m_manifestCache.erase(it);
    }
  }

  const MultiLodInfo& info = *sharedMeshInfo().multiLodInfo;
  auto cached = std::make_shared<CachedMultiLodManifest>();

  std::vector<uint8_t> manifestBytes;
  uint64_t manifestStart = 0;

  if (info.sharding) {
    auto bytesOpt = co_await getShardedManifestBytesAsync(segmentId, *info.sharding);
    maybeCancel(cancellationToken);
    if (!bytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {}", segmentId));
    }
    manifestBytes = std::move(bytesOpt->manifestBytes);
    manifestStart = bytesOpt->manifestStart;
    cached->fragmentDataUrl = std::move(bytesOpt->dataUrl);
  } else {
    const QString base = meshDirUrl().toString();
    const QString segStr = QString::number(segmentId);
    const std::string indexUrl = toStdString(base + segStr + ".index");
    auto bytesOpt = co_await getHttpBytesAsync(indexUrl);
    maybeCancel(cancellationToken);
    if (!bytesOpt) {
      throw ZNotFoundException(
        fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {} (expected '{}')",
                    segmentId,
                    indexUrl));
    }
    manifestBytes = std::move(*bytesOpt);
    cached->fragmentDataUrl = toStdString(base + segStr);
  }

  maybeCancel(cancellationToken);
  cached->manifest = std::make_shared<MultiLodManifest>(
    parseMultiLodManifest(std::span<const uint8_t>(manifestBytes.data(), manifestBytes.size()), info));
  maybeCancel(cancellationToken);
  if (info.sharding) {
    if (manifestStart < cached->manifest->totalFragmentBytes()) {
      throw ZException("Invalid neuroglancer multi-LOD sharded mesh: manifestStart precedes fragment data");
    }
    cached->fragmentDataBaseOffset = manifestStart - cached->manifest->totalFragmentBytes();
  }

  {
    std::scoped_lock lock(m_manifestCacheMutex);
    if (auto it = m_manifestCache.find(segmentId); it != m_manifestCache.end()) {
      if (auto existing = it->second.lock()) {
        co_return existing;
      }
    }
    maybeCancel(cancellationToken);
    m_manifestCache[segmentId] = cached;
  }

  co_return cached;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
ZNeuroglancerPrecomputedMeshSource::loadMultiLodChunkMeshAsync(uint64_t segmentId,
                                                               uint32_t row,
                                                               ChunkVertexSpace vertexSpace) const
{
  CHECK(meshType() == MeshType::MultiLodDraco);
  CHECK(sharedMeshInfo().multiLodInfo);

  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  const ChunkCacheKey cacheKey{.segmentId = segmentId, .row = row, .vertexSpace = vertexSpace};
  {
    std::scoped_lock lock(m_chunkCacheMutex);
    if (auto it = m_chunkCache.find(cacheKey); it != m_chunkCache.end()) {
      if (auto cached = it->second.lock()) {
        co_return cached;
      }
      m_chunkCache.erase(it);
    }
  }

  const std::shared_ptr<const CachedMultiLodManifest> cachedManifest = co_await loadCachedManifestAsync(segmentId);
  CHECK(cachedManifest);
  CHECK(cachedManifest->manifest);
  const MultiLodManifest& manifest = *cachedManifest->manifest;
  CHECK(row < manifest.octreeNodes.size());
  CHECK(row + 1U < manifest.offsets.size());

  maybeCancel(cancellationToken);

  const uint32_t lod = manifest.rowLods.at(row);
  auto chunkMesh = std::make_shared<MultiLodChunkMesh>();
  chunkMesh->subMeshes.resize(lod == 0U ? 1U : 8U);
  if (vertexSpace == ChunkVertexSpace::Quantized) {
    chunkMesh->vertexToLocalVoxelTransforms.resize(chunkMesh->subMeshes.size(), glm::mat4(1.0F));
    chunkMesh->vertexToLocalVoxelNormalMatrices.resize(chunkMesh->subMeshes.size(), glm::mat3(1.0F));
  }

  if (manifest.octreeNodes[row].empty() || manifest.offsets[row + 1U] <= manifest.offsets[row]) {
    std::scoped_lock lock(m_chunkCacheMutex);
    m_chunkCache[cacheKey] = chunkMesh;
    co_return chunkMesh;
  }

  const uint64_t startOffset = cachedManifest->fragmentDataBaseOffset + manifest.offsets[row];
  const uint64_t length = manifest.offsets[row + 1U] - manifest.offsets[row];
  auto fragBytesOpt = co_await getHttpRangeBytesAsync(cachedManifest->fragmentDataUrl, startOffset, length);
  if (!fragBytesOpt) {
    throw ZNotFoundException(
      fmt::format("Neuroglancer multi-LOD mesh fragment data not found for segment {} row {}", segmentId, row));
  }

  maybeCancel(cancellationToken);

  DecodedPartitionedDracoMesh decoded =
    decodePartitionedDracoMesh(std::span<const uint8_t>(fragBytesOpt->data(), fragBytesOpt->size()),
                               sharedMeshInfo().multiLodInfo->vertexQuantizationBits,
                               lod != 0U,
                               cancellationToken);
  std::optional<VertexToLocalVoxelTransform> vertexTransform;
  if (vertexSpace == ChunkVertexSpace::Quantized) {
    vertexTransform = computeMultiLodVertexToLocalVoxelTransform(lod,
                                                                 manifest.octreeNodes[row].gridPosition,
                                                                 manifest,
                                                                 *sharedMeshInfo().multiLodInfo);
  }
  maybeCancel(cancellationToken);
  if (vertexSpace == ChunkVertexSpace::LocalVoxel) {
    const bool normalsOk = convertMultiLodVerticesToLocalVoxel(decoded.vertices,
                                                               decoded.normals ? &(*decoded.normals) : nullptr,
                                                               lod,
                                                               manifest.octreeNodes[row].gridPosition,
                                                               manifest,
                                                               *sharedMeshInfo().multiLodInfo,
                                                               cancellationToken);
    if (!normalsOk) {
      decoded.normals.reset();
    }
  } else {
    CHECK(vertexTransform);
    const VertexToLocalVoxelTransform& tf = *vertexTransform;
    for (size_t i = 0; i < chunkMesh->subMeshes.size(); ++i) {
      chunkMesh->vertexToLocalVoxelTransforms[i] = tf.vertexToLocalVoxel;
      chunkMesh->vertexToLocalVoxelNormalMatrices[i] = tf.vertexToLocalVoxelNormalMatrix;
    }
  }
  maybeCancel(cancellationToken);
  for (size_t i = 0; i < decoded.partitionIndices.size(); ++i) {
    maybeCancelPeriodic(cancellationToken, i);
    chunkMesh->subMeshes[i] = buildCompactMesh(
      decoded.vertices, decoded.partitionIndices[i], decoded.normals ? &(*decoded.normals) : nullptr, cancellationToken);
  }
  maybeCancel(cancellationToken);

  {
    std::scoped_lock lock(m_chunkCacheMutex);
    if (auto it = m_chunkCache.find(cacheKey); it != m_chunkCache.end()) {
      if (auto existing = it->second.lock()) {
        co_return existing;
      }
    }
    m_chunkCache[cacheKey] = chunkMesh;
  }

  co_return chunkMesh;
}

folly::coro::Task<std::shared_ptr<ZMesh>>
ZNeuroglancerPrecomputedMeshSource::loadMultiLodMeshAsync(uint64_t segmentId, LodPolicy lodPolicy) const
{
  CHECK(meshType() == MeshType::MultiLodDraco);
  const std::shared_ptr<const CachedMultiLodManifest> cachedManifest = co_await loadCachedManifestAsync(segmentId);
  CHECK(cachedManifest);
  CHECK(cachedManifest->manifest);
  const MultiLodManifest& manifest = *cachedManifest->manifest;

  const uint32_t lod = (lodPolicy == LodPolicy::Finest) ? 0U : manifest.coarsestStoredLod().value_or(0U);

  std::vector<std::shared_ptr<ZMesh>> meshes;
  for (uint32_t row = 0; row < manifest.octreeNodes.size(); ++row) {
    if (manifest.rowLods[row] != lod) {
      continue;
    }
    if (manifest.octreeNodes[row].empty() || manifest.offsets[row + 1U] <= manifest.offsets[row]) {
      continue;
    }
    const std::shared_ptr<const MultiLodChunkMesh> chunkMesh =
      co_await loadMultiLodChunkMeshAsync(segmentId, row, ChunkVertexSpace::LocalVoxel);
    CHECK(chunkMesh);
    for (const auto& subMesh : chunkMesh->subMeshes) {
      if (subMesh && !subMesh->empty()) {
        meshes.push_back(subMesh);
      }
    }
  }

  auto out = mergeMeshes(meshes);
  if (!out || out->empty()) {
    throw ZException(fmt::format("Neuroglancer multi-LOD mesh has no data at LOD {} for segment {}", lod, segmentId));
  }
  co_return out;
}

} // namespace nim
