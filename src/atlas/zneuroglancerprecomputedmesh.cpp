#include "zneuroglancerprecomputedmesh.h"

#include "zneuroglanceruint64sharding.h"
#include "zexception.h"
#include "zlog.h"
#include "zproxygenhttpclient.h"

#include "zmesh.h"

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>
#include <draco/point_cloud/point_cloud.h>
#include <draco/attributes/point_attribute.h>

#include <folly/coro/BlockingWait.h>
#include <folly/compression/Compression.h>

#include <boost/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace nim {
namespace json = boost::json;

namespace {

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

QString shardHexString(uint64_t shard, int digits)
{
  QString s = QString::number(shard, 16);
  if (digits > 0) {
    s = s.rightJustified(digits, QChar('0'));
  }
  return s;
}

std::vector<uint8_t> decompressGzipBytes(std::vector<uint8_t> bytes)
{
  if (bytes.empty()) {
    return bytes;
  }
  auto codec = folly::compression::getCodec(folly::compression::CodecType::GZIP);
  auto uncompressed =
    codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  std::vector<uint8_t> out(uncompressed.size());
  std::memcpy(out.data(), uncompressed.data(), out.size());
  return out;
}

std::vector<uint8_t> decodeShardedBytes(std::vector<uint8_t> bytes,
                                        ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding enc)
{
  switch (enc) {
  case ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Raw:
    return bytes;
  case ZNeuroglancerPrecomputedVolume::Scale::Sharding::DataEncoding::Gzip:
    return decompressGzipBytes(std::move(bytes));
  }
  throw ZException("Invalid sharded data encoding");
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

void appendMeshGeometry(const ZMesh& frag, std::vector<glm::vec3>& mergedVertices, std::vector<uint32_t>& mergedIndices)
{
  const size_t base = mergedVertices.size();
  CHECK(base <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  const uint32_t base32 = static_cast<uint32_t>(base);

  mergedVertices.insert(mergedVertices.end(), frag.vertices().begin(), frag.vertices().end());
  mergedIndices.reserve(mergedIndices.size() + frag.indices().size());
  for (const uint32_t idx : frag.indices()) {
    CHECK(idx <= std::numeric_limits<uint32_t>::max() - base32);
    mergedIndices.push_back(idx + base32);
  }
}

std::shared_ptr<ZMesh> mergeMeshes(const std::vector<std::shared_ptr<ZMesh>>& meshes)
{
  std::vector<glm::vec3> mergedVertices;
  std::vector<uint32_t> mergedIndices;
  for (const auto& mesh : meshes) {
    if (!mesh || mesh->empty()) {
      continue;
    }
    appendMeshGeometry(*mesh, mergedVertices, mergedIndices);
  }

  auto out = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  out->setVertices(mergedVertices);
  out->setIndices(mergedIndices);
  if (!out->empty()) {
    out->generateNormals();
  }
  return out;
}

std::shared_ptr<ZMesh> buildCompactMesh(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices)
{
  if (indices.empty()) {
    return nullptr;
  }

  std::vector<uint32_t> remap(vertices.size(), std::numeric_limits<uint32_t>::max());
  std::vector<glm::vec3> compactVertices;
  compactVertices.reserve(std::min(vertices.size(), indices.size()));

  std::vector<uint32_t> compactIndices;
  compactIndices.reserve(indices.size());

  for (const uint32_t idx : indices) {
    CHECK(idx < vertices.size());
    uint32_t mapped = remap[idx];
    if (mapped == std::numeric_limits<uint32_t>::max()) {
      mapped = static_cast<uint32_t>(compactVertices.size());
      remap[idx] = mapped;
      compactVertices.push_back(vertices[idx]);
    }
    compactIndices.push_back(mapped);
  }

  auto mesh = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  mesh->setVertices(compactVertices);
  mesh->setIndices(compactIndices);
  mesh->generateNormals();
  return mesh;
}

DecodedPartitionedDracoMesh
decodePartitionedDracoMesh(std::span<const uint8_t> bytes, int vertexQuantizationBits, bool partition)
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

  const draco::PointAttribute* const pos = dmesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
  if (pos == nullptr || pos->size() == 0) {
    throw ZException("Failed to decode draco mesh: missing POSITION attribute");
  }
  if (pos->num_components() != 3) {
    throw ZException("Failed to decode draco mesh: POSITION must have 3 components");
  }
  if (pos->data_type() != draco::DT_INT32 && pos->data_type() != draco::DT_FLOAT32) {
    throw ZException("Failed to decode draco mesh: unsupported POSITION data type");
  }
  if (dmesh->GetAttributeElementType(pos->unique_id()) != draco::MESH_CORNER_ATTRIBUTE) {
    throw ZException("Failed to decode draco mesh: POSITION must use corner attributes");
  }
  if (static_cast<size_t>(pos->size()) != static_cast<size_t>(dmesh->num_points())) {
    throw ZException("Failed to decode draco mesh: POSITION count mismatch");
  }

  std::vector<uint32_t> indices(static_cast<size_t>(dmesh->num_faces()) * 3U);
  for (draco::FaceIndex fi(0); fi < dmesh->num_faces(); ++fi) {
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

  if (pos->data_type() == draco::DT_INT32) {
    const auto* raw = reinterpret_cast<const int32_t*>(pos->GetAddress(draco::AttributeValueIndex(0)));
    CHECK(raw);
    for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
      const size_t base = static_cast<size_t>(i.value()) * 3U;
      out.vertices[i.value()] = glm::vec3(static_cast<float>(raw[base + 0U]),
                                          static_cast<float>(raw[base + 1U]),
                                          static_cast<float>(raw[base + 2U]));
    }
  } else {
    const auto* raw = reinterpret_cast<const float*>(pos->GetAddress(draco::AttributeValueIndex(0)));
    CHECK(raw);
    for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
      const size_t base = static_cast<size_t>(i.value()) * 3U;
      out.vertices[i.value()] = glm::vec3(raw[base + 0U], raw[base + 1U], raw[base + 2U]);
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
                                         const std::array<int64_t, 3>& baseVoxelOffset)
{
  return QStringLiteral("%1|%2,%3,%4|%5,%6,%7")
    .arg(meshDirUrl.toString())
    .arg(QString::number(baseResolutionNm[0], 'g', 17))
    .arg(QString::number(baseResolutionNm[1], 'g', 17))
    .arg(QString::number(baseResolutionNm[2], 'g', 17))
    .arg(baseVoxelOffset[0])
    .arg(baseVoxelOffset[1])
    .arg(baseVoxelOffset[2]);
}

std::mutex& openedMeshSourcesMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<QString, std::weak_ptr<ZNeuroglancerPrecomputedMeshSource>>& openedMeshSources()
{
  static std::unordered_map<QString, std::weak_ptr<ZNeuroglancerPrecomputedMeshSource>> cache;
  return cache;
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

std::shared_ptr<ZNeuroglancerPrecomputedMeshSource> ZNeuroglancerPrecomputedMeshSource::open(
  const QUrl& meshDirUrl,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  QUrl normalizedMeshDirUrl = meshDirUrl;
  if (!normalizedMeshDirUrl.toString().endsWith('/')) {
    normalizedMeshDirUrl = QUrl(normalizedMeshDirUrl.toString() + "/");
  }

  const QString cacheKey = meshSourceCacheKey(normalizedMeshDirUrl, baseResolutionNm, baseVoxelOffset);
  {
    std::lock_guard<std::mutex> lock(openedMeshSourcesMutex());
    auto& cache = openedMeshSources();
    if (auto it = cache.find(cacheKey); it != cache.end()) {
      if (auto existing = it->second.lock()) {
        if (timeout.count() > 0 && timeout > existing->m_timeout) {
          existing->m_timeout = timeout;
        }
        return existing;
      }
      cache.erase(it);
    }
  }

  auto out = std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>(new ZNeuroglancerPrecomputedMeshSource());
  out->m_meshDirUrl = normalizedMeshDirUrl;
  out->m_baseResolutionNm = baseResolutionNm;
  out->m_baseVoxelOffset = baseVoxelOffset;
  out->m_timeout = timeout.count() > 0 ? timeout : out->m_timeout;

  const QUrl infoUrl = out->m_meshDirUrl.resolved(QUrl("info"));
  const std::string infoUrlStr = toStdString(infoUrl.toString());
  auto resOpt = folly::coro::blockingWait(ZProxygenHttpClient::instance().getBytes(infoUrlStr, out->m_timeout));
  if (!resOpt) {
    // No info file -> legacy mesh.
    out->m_meshType = MeshType::Legacy;
    return out;
  }
  if (resOpt->status != 200) {
    throw ZException(fmt::format("Failed to fetch neuroglancer mesh info from '{}' (HTTP {})", infoUrlStr, resOpt->status));
  }

  const std::string infoText(reinterpret_cast<const char*>(resOpt->body.data()), resOpt->body.size());
  json::value jv = json::parse(infoText);
  if (!jv.is_object()) {
    throw ZException("Neuroglancer mesh info is not a JSON object");
  }
  const auto& root = jv.as_object();

  const QString type = optionalString(root, "@type", "neuroglancer_legacy_mesh").trimmed();
  if (type == "neuroglancer_legacy_mesh") {
    out->m_meshType = MeshType::Legacy;
    return out;
  }
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

  auto shardingIt = root.find("sharding");
  if (shardingIt != root.end() && !shardingIt->value().is_null()) {
    if (!shardingIt->value().is_object()) {
      throw ZException("Invalid neuroglancer mesh sharding: expected object");
    }
    info.sharding = parseShardingSpec(shardingIt->value().as_object());
  }

  out->m_meshType = MeshType::MultiLodDraco;
  out->m_multiLodInfo = info;

  {
    std::lock_guard<std::mutex> lock(openedMeshSourcesMutex());
    openedMeshSources()[cacheKey] = out;
  }
  return out;
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest>
ZNeuroglancerPrecomputedMeshSource::loadManifestBlocking(uint64_t segmentId) const
{
  CHECK(supportsRuntimeLod());
  return folly::coro::blockingWait(loadCachedManifestAsync(segmentId))->manifest;
}

std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>
ZNeuroglancerPrecomputedMeshSource::loadChunkMeshBlocking(uint64_t segmentId, uint32_t row) const
{
  CHECK(supportsRuntimeLod());
  return folly::coro::blockingWait(loadMultiLodChunkMeshAsync(segmentId, row));
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

std::shared_ptr<ZMesh> ZNeuroglancerPrecomputedMeshSource::loadMeshBlocking(uint64_t segmentId, LodPolicy lodPolicy) const
{
  switch (m_meshType) {
  case MeshType::Legacy:
    return folly::coro::blockingWait(loadLegacyMeshAsync(segmentId));
  case MeshType::MultiLodDraco:
    return folly::coro::blockingWait(loadMultiLodMeshAsync(segmentId, lodPolicy));
  }
  throw ZException("Invalid mesh type");
}

folly::coro::Task<std::optional<std::vector<uint8_t>>> ZNeuroglancerPrecomputedMeshSource::getHttpBytesAsync(const std::string& url) const
{
  auto resOpt = co_await ZProxygenHttpClient::instance().getBytes(url, m_timeout);
  if (!resOpt) {
    co_return std::nullopt;
  }
  if (resOpt->status != 200) {
    throw ZException(fmt::format("HTTP GET failed for '{}' (status {})", url, resOpt->status));
  }
  co_return std::move(resOpt->body);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>> ZNeuroglancerPrecomputedMeshSource::getHttpRangeBytesAsync(
  const std::string& url,
  uint64_t offset,
  uint64_t length) const
{
  if (length == 0) {
    co_return std::vector<uint8_t>{};
  }
  const uint64_t endInclusive = offset + length - 1;
  auto resOpt = co_await ZProxygenHttpClient::instance().getBytes(
    url,
    m_timeout,
    {{"range", fmt::format("bytes={}-{}", offset, endInclusive)}});
  if (!resOpt) {
    co_return std::nullopt;
  }
  if (resOpt->status != 206 && resOpt->status != 200) {
    throw ZException(fmt::format("HTTP range GET failed for '{}' (status {})", url, resOpt->status));
  }
  if (resOpt->body.size() == length) {
    co_return std::move(resOpt->body);
  }
  // Some servers ignore range and return the full file with 200; accept if we can slice it.
  if (resOpt->status == 200 && resOpt->body.size() >= offset + length) {
    std::vector<uint8_t> out(length);
    std::memcpy(out.data(), resOpt->body.data() + offset, length);
    co_return out;
  }
  throw ZException(fmt::format("HTTP range GET size mismatch for '{}': got {} bytes, expected {} bytes",
                               url,
                               resOpt->body.size(),
                               length));
}

folly::coro::Task<std::shared_ptr<ZMesh>> ZNeuroglancerPrecomputedMeshSource::loadLegacyMeshAsync(uint64_t segmentId) const
{
  CHECK(m_meshType == MeshType::Legacy);

  const QString base = m_meshDirUrl.toString();
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
    const QUrl fragUrl = m_meshDirUrl.resolved(QUrl(fragName));
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

  const QString shardHex = shardHexString(shard, sharding.shardHexDigits);
  const QUrl shardUrl = m_meshDirUrl.resolved(QUrl(shardHex + ".shard"));
  const QUrl indexUrl = m_meshDirUrl.resolved(QUrl(shardHex + ".index"));
  const QUrl dataUrl = m_meshDirUrl.resolved(QUrl(shardHex + ".data"));

  const uint64_t shardIndexEntryOffset = minishard << 4;

  struct ShardedShardIndexEntry
  {
    std::string dataUrl;
    uint64_t baseDataOffset = 0;
    uint64_t minishardIndexStart = 0; // relative to end of shard index
    uint64_t minishardIndexEnd = 0;   // relative to end of shard index
  };

  auto readEntryFrom = [&](const QUrl& url) -> folly::coro::Task<std::optional<std::vector<uint8_t>>> {
    co_return co_await getHttpRangeBytesAsync(toStdString(url.toString()), shardIndexEntryOffset, 16);
  };

  auto parseEntry = [&](std::vector<uint8_t> bytes, bool useSplit) -> ShardedShardIndexEntry {
    CHECK(bytes.size() == 16);
    ShardedShardIndexEntry out{};
    out.minishardIndexStart = ZNeuroglancerUint64Sharding::readU64LE(bytes.data());
    out.minishardIndexEnd = ZNeuroglancerUint64Sharding::readU64LE(bytes.data() + 8);
    if (useSplit) {
      out.dataUrl = toStdString(dataUrl.toString());
      out.baseDataOffset = 0;
    } else {
      out.dataUrl = toStdString(shardUrl.toString());
      out.baseDataOffset = sharding.shardIndexSize;
    }
    return out;
  };

  std::optional<ShardedShardIndexEntry> entryOpt;

  const int mode = sharding.shardFileMode.load();
  if (mode == 1) {
    auto bytesOpt = co_await readEntryFrom(shardUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
  } else if (mode == 2) {
    auto bytesOpt = co_await readEntryFrom(indexUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/true);
  } else {
    auto bytesOpt = co_await readEntryFrom(shardUrl);
    if (bytesOpt) {
      sharding.shardFileMode.store(1);
      entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
    } else {
      auto legacyBytesOpt = co_await readEntryFrom(indexUrl);
      if (legacyBytesOpt) {
        sharding.shardFileMode.store(2);
        entryOpt = parseEntry(std::move(*legacyBytesOpt), /*useSplit=*/true);
      } else {
        co_return std::nullopt;
      }
    }
  }

  CHECK(entryOpt);
  const ShardedShardIndexEntry& entry = *entryOpt;
  if (entry.minishardIndexEnd <= entry.minishardIndexStart) {
    co_return std::nullopt;
  }

  const uint64_t minishardLen = entry.minishardIndexEnd - entry.minishardIndexStart;
  auto minishardBytesOpt = co_await getHttpRangeBytesAsync(entry.dataUrl, entry.baseDataOffset + entry.minishardIndexStart, minishardLen);
  if (!minishardBytesOpt) {
    co_return std::nullopt;
  }

  auto decodedMinishardBytes = decodeShardedBytes(std::move(*minishardBytesOpt), sharding.minishardIndexEncoding);
  auto decoded =
    ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t>(decodedMinishardBytes.data(), decodedMinishardBytes.size()),
                                                     entry.baseDataOffset);
  if (decoded.keys.empty()) {
    co_return std::nullopt;
  }

  auto it = std::lower_bound(decoded.keys.begin(), decoded.keys.end(), chunkId);
  if (it == decoded.keys.end() || *it != chunkId) {
    co_return std::nullopt;
  }
  const size_t idx = static_cast<size_t>(it - decoded.keys.begin());
  const uint64_t start = decoded.starts[idx];
  const uint64_t end = decoded.ends[idx];
  if (end <= start) {
    co_return std::nullopt;
  }

  auto manifestBytesOpt = co_await getHttpRangeBytesAsync(entry.dataUrl, start, end - start);
  if (!manifestBytesOpt) {
    co_return std::nullopt;
  }

  ShardedManifestBytes out;
  out.manifestBytes = decodeShardedBytes(std::move(*manifestBytesOpt), sharding.dataEncoding);
  out.manifestStart = start;
  out.dataUrl = entry.dataUrl;
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

  MultiLodManifest manifest{};
  manifest.chunkShape = glm::vec3(readF(), readF(), readF());
  manifest.gridOrigin = glm::vec3(readF(), readF(), readF());
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
    manifest.vertexOffsets[i] = glm::vec3(readF(), readF(), readF());
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
  const glm::vec3 invRes(static_cast<float>(1.0 / m_baseResolutionNm[0]),
                         static_cast<float>(1.0 / m_baseResolutionNm[1]),
                         static_cast<float>(1.0 / m_baseResolutionNm[2]));
  const glm::vec3 voxelOffset(static_cast<float>(m_baseVoxelOffset[0]),
                              static_cast<float>(m_baseVoxelOffset[1]),
                              static_cast<float>(m_baseVoxelOffset[2]));

  // stored-model vertices are in nanometer units in the global frame:
  // voxel = nm / baseResolutionNm - baseVoxelOffset
  glm::mat4 voxelFromNm(1.0F);
  voxelFromNm = glm::translate(voxelFromNm, -voxelOffset);
  voxelFromNm = glm::scale(voxelFromNm, invRes);
  mesh.transformVerticesByMatrix(voxelFromNm);
}

void ZNeuroglancerPrecomputedMeshSource::convertMultiLodVerticesToLocalVoxel(std::vector<glm::vec3>& vertices,
                                                                             size_t lod,
                                                                             const glm::uvec3& fragmentPos,
                                                                             const MultiLodManifest& manifest,
                                                                             const MultiLodInfo& info) const
{
  CHECK(info.vertexQuantizationBits == 10 || info.vertexQuantizationBits == 16);
  const float maxQuant = static_cast<float>((1ULL << info.vertexQuantizationBits) - 1ULL);
  CHECK(maxQuant > 0.0F);

  const glm::vec3 invRes(static_cast<float>(1.0 / m_baseResolutionNm[0]),
                         static_cast<float>(1.0 / m_baseResolutionNm[1]),
                         static_cast<float>(1.0 / m_baseResolutionNm[2]));
  const glm::vec3 voxelOffset(static_cast<float>(m_baseVoxelOffset[0]),
                              static_cast<float>(m_baseVoxelOffset[1]),
                              static_cast<float>(m_baseVoxelOffset[2]));

  const float lodScale = std::ldexp(1.0F, static_cast<int>(lod));
  const glm::vec3 scaledChunkShape = manifest.chunkShape * lodScale;

  const glm::vec3 fragmentTranslation =
    manifest.gridOrigin + manifest.vertexOffsets.at(lod) + scaledChunkShape * glm::vec3(fragmentPos);
  const glm::vec3 quantScale = scaledChunkShape / maxQuant;

  glm::mat4 localFromQuant(1.0F);
  localFromQuant = glm::translate(localFromQuant, fragmentTranslation);
  localFromQuant = glm::scale(localFromQuant, quantScale);

  glm::mat4 modelFromQuant = info.transform * localFromQuant;

  glm::mat4 voxelFromModel(1.0F);
  voxelFromModel = glm::translate(voxelFromModel, -voxelOffset);
  voxelFromModel = glm::scale(voxelFromModel, invRes);

  const glm::mat4 transform = voxelFromModel * modelFromQuant;
  for (glm::vec3& vertex : vertices) {
    vertex = glm::applyMatrix(transform, vertex);
  }
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
  CHECK(m_meshType == MeshType::MultiLodDraco);
  CHECK(m_multiLodInfo);

  {
    std::lock_guard<std::mutex> lock(m_manifestCacheMutex);
    if (auto it = m_manifestCache.find(segmentId); it != m_manifestCache.end()) {
      if (auto cached = it->second.lock()) {
        co_return cached;
      }
      m_manifestCache.erase(it);
    }
  }

  const MultiLodInfo& info = *m_multiLodInfo;
  auto cached = std::make_shared<CachedMultiLodManifest>();

  std::vector<uint8_t> manifestBytes;
  uint64_t manifestStart = 0;

  if (info.sharding) {
    auto bytesOpt = co_await getShardedManifestBytesAsync(segmentId, *info.sharding);
    if (!bytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {}", segmentId));
    }
    manifestBytes = std::move(bytesOpt->manifestBytes);
    manifestStart = bytesOpt->manifestStart;
    cached->fragmentDataUrl = std::move(bytesOpt->dataUrl);
  } else {
    const QString base = m_meshDirUrl.toString();
    const QString segStr = QString::number(segmentId);
    const std::string indexUrl = toStdString(base + segStr + ".index");
    auto bytesOpt = co_await getHttpBytesAsync(indexUrl);
    if (!bytesOpt) {
      throw ZNotFoundException(
        fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {} (expected '{}')",
                    segmentId,
                    indexUrl));
    }
    manifestBytes = std::move(*bytesOpt);
    cached->fragmentDataUrl = toStdString(base + segStr);
  }

  cached->manifest = std::make_shared<MultiLodManifest>(
    parseMultiLodManifest(std::span<const uint8_t>(manifestBytes.data(), manifestBytes.size()), info));
  if (info.sharding) {
    if (manifestStart < cached->manifest->totalFragmentBytes()) {
      throw ZException("Invalid neuroglancer multi-LOD sharded mesh: manifestStart precedes fragment data");
    }
    cached->fragmentDataBaseOffset = manifestStart - cached->manifest->totalFragmentBytes();
  }

  {
    std::lock_guard<std::mutex> lock(m_manifestCacheMutex);
    if (auto it = m_manifestCache.find(segmentId); it != m_manifestCache.end()) {
      if (auto existing = it->second.lock()) {
        co_return existing;
      }
    }
    m_manifestCache[segmentId] = cached;
  }

  co_return cached;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
ZNeuroglancerPrecomputedMeshSource::loadMultiLodChunkMeshAsync(uint64_t segmentId, uint32_t row) const
{
  CHECK(m_meshType == MeshType::MultiLodDraco);
  CHECK(m_multiLodInfo);

  const ChunkCacheKey cacheKey{segmentId, row};
  {
    std::lock_guard<std::mutex> lock(m_chunkCacheMutex);
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

  const uint32_t lod = manifest.rowLods.at(row);
  auto chunkMesh = std::make_shared<MultiLodChunkMesh>();
  chunkMesh->subMeshes.resize(lod == 0U ? 1U : 8U);

  if (manifest.octreeNodes[row].empty() || manifest.offsets[row + 1U] <= manifest.offsets[row]) {
    std::lock_guard<std::mutex> lock(m_chunkCacheMutex);
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

  DecodedPartitionedDracoMesh decoded =
    decodePartitionedDracoMesh(std::span<const uint8_t>(fragBytesOpt->data(), fragBytesOpt->size()),
                               m_multiLodInfo->vertexQuantizationBits,
                               lod != 0U);
  convertMultiLodVerticesToLocalVoxel(decoded.vertices,
                                      lod,
                                      manifest.octreeNodes[row].gridPosition,
                                      manifest,
                                      *m_multiLodInfo);
  for (size_t i = 0; i < decoded.partitionIndices.size(); ++i) {
    chunkMesh->subMeshes[i] = buildCompactMesh(decoded.vertices, decoded.partitionIndices[i]);
  }

  {
    std::lock_guard<std::mutex> lock(m_chunkCacheMutex);
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
  CHECK(m_meshType == MeshType::MultiLodDraco);
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
    const std::shared_ptr<const MultiLodChunkMesh> chunkMesh = co_await loadMultiLodChunkMeshAsync(segmentId, row);
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
