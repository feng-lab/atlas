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
  QString s = QString::number(static_cast<qulonglong>(shard), 16);
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

ZMesh decodeDracoMesh(std::span<const uint8_t> bytes)
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
  auto meshStatus = decoder.DecodeMeshFromBuffer(&buffer);
  if (!meshStatus.ok()) {
    throw ZException(fmt::format("Failed to decode draco mesh: {}", meshStatus.status().error_msg()));
  }

  std::unique_ptr<draco::Mesh> dmesh = std::move(meshStatus).value();
  if (!dmesh) {
    throw ZException("Failed to decode draco mesh: null mesh");
  }

  const draco::PointAttribute* const pos = dmesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
  if (pos == nullptr || pos->size() == 0) {
    throw ZException("Failed to decode draco mesh: missing POSITION attribute");
  }
  if (pos->num_components() != 3) {
    throw ZException("Failed to decode draco mesh: POSITION must have 3 components");
  }

  std::vector<glm::vec3> vertices;
  vertices.resize(dmesh->num_points());
  for (draco::PointIndex i(0); i < dmesh->num_points(); ++i) {
    glm::vec3 v(0.0F);
    if (!pos->ConvertValue<float, 3>(pos->mapped_index(i), &v[0])) {
      throw ZException("Failed to decode draco mesh: ConvertValue(POSITION) failed");
    }
    vertices[i.value()] = v;
  }

  std::vector<uint32_t> indices;
  indices.resize(static_cast<size_t>(dmesh->num_faces()) * 3);
  for (draco::FaceIndex fi(0); fi < dmesh->num_faces(); ++fi) {
    const draco::Mesh::Face& face = dmesh->face(fi);
    indices[fi.value() * 3 + 0] = face[0].value();
    indices[fi.value() * 3 + 1] = face[1].value();
    indices[fi.value() * 3 + 2] = face[2].value();
  }

  ZMesh mesh(ZMesh::Type::TRIANGLES);
  mesh.setVertices(vertices);
  mesh.setIndices(indices);
  return mesh;
}

} // namespace

uint64_t ZNeuroglancerPrecomputedMeshSource::MultiLodManifest::totalFragmentBytes() const
{
  uint64_t total = 0;
  for (const uint64_t b : lodTotalSizes) {
    total += b;
  }
  return total;
}

std::shared_ptr<ZNeuroglancerPrecomputedMeshSource> ZNeuroglancerPrecomputedMeshSource::open(
  const QUrl& meshDirUrl,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  auto out = std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>(new ZNeuroglancerPrecomputedMeshSource());
  out->m_meshDirUrl = meshDirUrl;
  if (!out->m_meshDirUrl.toString().endsWith('/')) {
    out->m_meshDirUrl = QUrl(out->m_meshDirUrl.toString() + "/");
  }
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
  const QString metaRel = QString::number(static_cast<qulonglong>(segmentId)) + ":0";
  const std::string metaUrl = toStdString(base + metaRel);

  auto metaBytesOpt = co_await getHttpBytesAsync(metaUrl);
  if (!metaBytesOpt) {
    throw ZException(fmt::format("Neuroglancer legacy mesh metadata not found for segment {} (expected '{}')",
                                 segmentId,
                                 metaUrl));
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

folly::coro::Task<std::optional<std::vector<uint8_t>>> ZNeuroglancerPrecomputedMeshSource::getShardedManifestBytesAsync(
  uint64_t segmentId,
  uint64_t* manifestStart,
  const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding) const
{
  CHECK(manifestStart);
  *manifestStart = 0;

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

  *manifestStart = start;
  auto manifestBytesOpt = co_await getHttpRangeBytesAsync(entry.dataUrl, start, end - start);
  if (!manifestBytesOpt) {
    co_return std::nullopt;
  }

  auto decodedManifestBytes = decodeShardedBytes(std::move(*manifestBytesOpt), sharding.dataEncoding);
  co_return decodedManifestBytes;
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

  MultiLodManifest m{};
  m.chunkShape = glm::vec3(readF(), readF(), readF());
  m.gridOrigin = glm::vec3(readF(), readF(), readF());
  const uint32_t numLods = readU32();
  if (numLods == 0) {
    throw ZException("Invalid neuroglancer multi-LOD mesh manifest: num_lods must be > 0");
  }

  m.lodScales.resize(numLods);
  for (uint32_t i = 0; i < numLods; ++i) {
    m.lodScales[i] = static_cast<float>(static_cast<double>(readF()) * info.lodScaleMultiplier);
  }

  m.vertexOffsets.resize(numLods);
  for (uint32_t i = 0; i < numLods; ++i) {
    m.vertexOffsets[i] = glm::vec3(readF(), readF(), readF());
  }

  std::vector<uint32_t> numFragments(numLods);
  for (uint32_t i = 0; i < numLods; ++i) {
    numFragments[i] = readU32();
  }

  m.fragmentPositions.resize(numLods);
  m.fragmentSizes.resize(numLods);
  m.lodStartOffsets.resize(numLods);
  m.lodTotalSizes.resize(numLods);

  uint64_t cumulative = 0;
  for (uint32_t lod = 0; lod < numLods; ++lod) {
    const uint32_t n = numFragments[lod];
    m.lodStartOffsets[lod] = cumulative;

    std::vector<uint32_t> xs(n), ys(n), zs(n);
    for (uint32_t i = 0; i < n; ++i) {
      xs[i] = readU32();
    }
    for (uint32_t i = 0; i < n; ++i) {
      ys[i] = readU32();
    }
    for (uint32_t i = 0; i < n; ++i) {
      zs[i] = readU32();
    }

    m.fragmentPositions[lod].resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      m.fragmentPositions[lod][i] = glm::uvec3(xs[i], ys[i], zs[i]);
    }

    m.fragmentSizes[lod].resize(n);
    uint64_t lodBytes = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t sz = readU32();
      m.fragmentSizes[lod][i] = sz;
      lodBytes += sz;
    }
    m.lodTotalSizes[lod] = lodBytes;
    cumulative += lodBytes;
  }

  if (off != bytes.size()) {
    // Ignore trailing bytes to be tolerant, but warn in debug builds.
    VLOG(1) << fmt::format("Neuroglancer multi-LOD mesh manifest has {} trailing bytes", bytes.size() - off);
  }

  return m;
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

void ZNeuroglancerPrecomputedMeshSource::convertMultiLodFragmentToLocalVoxel(ZMesh& fragment,
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

  fragment.transformVerticesByMatrix(voxelFromModel * modelFromQuant);
}

folly::coro::Task<std::shared_ptr<ZMesh>> ZNeuroglancerPrecomputedMeshSource::loadMultiLodMeshAsync(uint64_t segmentId,
                                                                                                    LodPolicy lodPolicy) const
{
  CHECK(m_meshType == MeshType::MultiLodDraco);
  CHECK(m_multiLodInfo);
  const MultiLodInfo& info = *m_multiLodInfo;

  const QString base = m_meshDirUrl.toString();
  const QString segStr = QString::number(static_cast<qulonglong>(segmentId));

  uint64_t manifestStart = 0;
  std::vector<uint8_t> manifestBytes;

  if (info.sharding) {
    auto bytesOpt = co_await getShardedManifestBytesAsync(segmentId, &manifestStart, *info.sharding);
    if (!bytesOpt) {
      throw ZException(fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {}", segmentId));
    }
    manifestBytes = std::move(*bytesOpt);
  } else {
    const std::string indexUrl = toStdString(base + segStr + ".index");
    auto bytesOpt = co_await getHttpBytesAsync(indexUrl);
    if (!bytesOpt) {
      throw ZException(fmt::format("Neuroglancer multi-LOD mesh manifest not found for segment {} (expected '{}')",
                                   segmentId,
                                   indexUrl));
    }
    manifestBytes = std::move(*bytesOpt);
  }

  const MultiLodManifest manifest = parseMultiLodManifest(std::span<const uint8_t>(manifestBytes.data(), manifestBytes.size()), info);
  const size_t numLods = manifest.lodScales.size();
  CHECK(numLods > 0);

  const size_t lod = (lodPolicy == LodPolicy::Finest) ? 0 : (numLods - 1);
  const uint64_t lodOffset = manifest.lodStartOffsets.at(lod);
  const uint64_t lodBytes = manifest.lodTotalSizes.at(lod);

  std::vector<uint8_t> fragBytes;

  if (lodBytes == 0) {
    throw ZException(fmt::format("Neuroglancer multi-LOD mesh has no data at LOD {} for segment {}", lod, segmentId));
  }

  if (info.sharding) {
    const uint64_t totalFragBytes = manifest.totalFragmentBytes();
    if (manifestStart < totalFragBytes) {
      throw ZException("Invalid neuroglancer multi-LOD sharded mesh: manifestStart precedes fragment data");
    }
    const uint64_t fragDataStart = manifestStart - totalFragBytes;
    const uint64_t lodDataStart = fragDataStart + lodOffset;

    // Sharded format uses the shard file as the data source for both manifest and fragments.
    // Recompute the data URL using the same sharding mapping.
    // (We keep this simple by requesting the fragment data range via the sharded manifest helper.)
    // Note: getShardedManifestBytesAsync returns the manifest, but we also need the shard URL; recompute directly.
    const auto& sharding = *info.sharding;
    const uint64_t shiftedChunkId = segmentId >> sharding.preshiftBits;
    const uint64_t hashCode = (sharding.hash == ZNeuroglancerPrecomputedVolume::Scale::Sharding::Hash::Identity)
                                ? shiftedChunkId
                                : ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(shiftedChunkId, /*seed=*/0);
    const uint64_t shardAndMinishard = hashCode & sharding.shardAndMinishardMask;
    const uint64_t shard = (shardAndMinishard >> sharding.minishardBits) & sharding.shardMask;
    const QString shardHex = shardHexString(shard, sharding.shardHexDigits);
    const QUrl shardUrl = m_meshDirUrl.resolved(QUrl(shardHex + ".shard"));
    const QUrl dataUrl = m_meshDirUrl.resolved(QUrl(shardHex + ".data"));

    const int mode = sharding.shardFileMode.load();
    const std::string dataUrlStr =
      (mode == 2) ? toStdString(dataUrl.toString()) : toStdString(shardUrl.toString());

    auto bytesOpt = co_await getHttpRangeBytesAsync(dataUrlStr, lodDataStart, lodBytes);
    if (!bytesOpt) {
      throw ZException("Failed to read sharded mesh fragment data");
    }
    fragBytes = std::move(*bytesOpt);
  } else {
    const std::string fragUrl = toStdString(base + segStr);
    auto bytesOpt = co_await getHttpRangeBytesAsync(fragUrl, lodOffset, lodBytes);
    if (!bytesOpt) {
      throw ZException(fmt::format("Neuroglancer multi-LOD mesh fragment data not found for segment {}", segmentId));
    }
    fragBytes = std::move(*bytesOpt);
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

  size_t cursor = 0;
  const auto& sizes = manifest.fragmentSizes.at(lod);
  const auto& positions = manifest.fragmentPositions.at(lod);
  CHECK(sizes.size() == positions.size());

  for (size_t i = 0; i < sizes.size(); ++i) {
    const uint32_t sz = sizes[i];
    if (sz == 0) {
      continue;
    }
    const size_t szBytes = static_cast<size_t>(sz);
    if (cursor + szBytes > fragBytes.size()) {
      throw ZException("Invalid neuroglancer multi-LOD mesh fragment data: truncated");
    }
    const uint8_t* fragData = fragBytes.data() + cursor;
    ZMesh fragMesh = decodeDracoMesh(std::span<const uint8_t>(fragData, szBytes));
    convertMultiLodFragmentToLocalVoxel(fragMesh, lod, positions[i], manifest, info);
    appendFragment(fragMesh);
    cursor += szBytes;
  }

  auto out = std::make_shared<ZMesh>(ZMesh::Type::TRIANGLES);
  out->setVertices(mergedVertices);
  out->setIndices(mergedIndices);
  out->generateNormals();
  co_return out;
}

} // namespace nim
