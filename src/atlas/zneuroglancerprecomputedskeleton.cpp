#include "zneuroglancerprecomputedskeleton.h"

#include "zneuroglanceruint64sharding.h"
#include "zexception.h"
#include "zlog.h"
#include "zproxygenhttpclient.h"

#include <folly/coro/BlockingWait.h>
#include <folly/compression/Compression.h>

#include <boost/json.hpp>

#include <algorithm>
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
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer skeleton info", key));
  }
  return json::value_to<QString>(it->value());
}

QString optionalString(const json::object& obj, const char* key, const char* defaultValue = "")
{
  auto it = obj.find(key);
  if (it == obj.end() || it->value().is_null()) {
    return QString::fromUtf8(defaultValue);
  }
  if (!it->value().is_string()) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer skeleton info (expected string)", key));
  }
  return json::value_to<QString>(it->value());
}

uint64_t requireUint64(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end()) {
    throw ZException(fmt::format("Missing '{}' in neuroglancer skeleton info", key));
  }
  const json::value& v = it->value();
  if (v.is_uint64()) {
    return v.as_uint64();
  }
  if (v.is_int64()) {
    if (v.as_int64() < 0) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer skeleton info (expected >= 0)", key));
    }
    return static_cast<uint64_t>(v.as_int64());
  }
  throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer skeleton info", key));
}

glm::mat4 parseTransform(const json::object& obj)
{
  auto it = obj.find("transform");
  if (it == obj.end() || it->value().is_null()) {
    return glm::mat4(1.0F);
  }
  if (!it->value().is_array()) {
    throw ZException("Invalid 'transform' in neuroglancer skeleton info (expected array)");
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 12) {
    throw ZException("Invalid 'transform' in neuroglancer skeleton info (expected length 12 array)");
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
      throw ZException("Invalid 'transform' in neuroglancer skeleton info (expected numbers)");
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

folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpBytesAsync(const std::string& url,
                                                                         std::chrono::milliseconds timeout)
{
  auto resOpt = co_await ZProxygenHttpClient::instance().getBytes(url, timeout);
  if (!resOpt) {
    co_return std::nullopt;
  }
  if (resOpt->status != 200) {
    throw ZException(fmt::format("HTTP GET failed for '{}' (status {})", url, resOpt->status));
  }
  co_return std::move(resOpt->body);
}

folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpRangeBytesAsync(const std::string& url,
                                                                              std::chrono::milliseconds timeout,
                                                                              uint64_t offset,
                                                                              uint64_t length)
{
  if (length == 0) {
    co_return std::vector<uint8_t>{};
  }
  const uint64_t endInclusive = offset + length - 1;
  auto resOpt = co_await ZProxygenHttpClient::instance().getBytes(url,
                                                                  timeout,
                                                                  {{"range", fmt::format("bytes={}-{}", offset, endInclusive)}});
  if (!resOpt) {
    co_return std::nullopt;
  }
  if (resOpt->status != 206 && resOpt->status != 200) {
    throw ZException(fmt::format("HTTP range GET failed for '{}' (status {})", url, resOpt->status));
  }
  if (resOpt->body.size() != length) {
    throw ZException(fmt::format("HTTP range GET size mismatch for '{}': got {} bytes, expected {} bytes",
                                 url,
                                 resOpt->body.size(),
                                 length));
  }
  co_return std::move(resOpt->body);
}

QString shardHexString(uint64_t shard, int digits)
{
  QString s = QString::number(shard, 16);
  if (digits > 0) {
    s = s.rightJustified(digits, QChar('0'));
  }
  return s;
}

struct ShardedShardIndexEntry
{
  std::string dataUrl;
  uint64_t baseDataOffset = 0;
  uint64_t minishardIndexStart = 0; // relative to end of shard index
  uint64_t minishardIndexEnd = 0;   // relative to end of shard index
};

folly::coro::Task<std::optional<ShardedShardIndexEntry>> getShardIndexEntry(const QUrl& baseUrl,
                                                                           const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding,
                                                                           std::chrono::milliseconds timeout,
                                                                           uint64_t shard,
                                                                           uint64_t minishard)
{
  const QString shardHex = shardHexString(shard, sharding.shardHexDigits);

  const QUrl shardUrl = baseUrl.resolved(QUrl(shardHex + ".shard"));
  const QUrl indexUrl = baseUrl.resolved(QUrl(shardHex + ".index"));
  const QUrl dataUrl = baseUrl.resolved(QUrl(shardHex + ".data"));

  const uint64_t shardIndexEntryOffset = minishard << 4;

  auto readFrom = [&](const QUrl& url) -> folly::coro::Task<std::optional<std::vector<uint8_t>>> {
    co_return co_await getHttpRangeBytesAsync(toStdString(url.toString()), timeout, shardIndexEntryOffset, 16);
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

  int mode = sharding.shardFileMode.load();
  if (mode == 1) {
    auto bytesOpt = co_await readFrom(shardUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
  } else if (mode == 2) {
    auto bytesOpt = co_await readFrom(indexUrl);
    if (!bytesOpt) {
      co_return std::nullopt;
    }
    entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/true);
  } else {
    auto bytesOpt = co_await readFrom(shardUrl);
    if (bytesOpt) {
      sharding.shardFileMode.store(1);
      entryOpt = parseEntry(std::move(*bytesOpt), /*useSplit=*/false);
    } else {
      auto legacyBytesOpt = co_await readFrom(indexUrl);
      if (legacyBytesOpt) {
        sharding.shardFileMode.store(2);
        entryOpt = parseEntry(std::move(*legacyBytesOpt), /*useSplit=*/true);
      } else {
        co_return std::nullopt;
      }
    }
  }

  CHECK(entryOpt);
  co_return entryOpt;
}

} // namespace

std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource> ZNeuroglancerPrecomputedSkeletonSource::open(
  const QUrl& skeletonDirUrl,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  const QUrl infoUrl = skeletonDirUrl.resolved(QUrl("info"));
  const std::string infoUrlStr = toStdString(infoUrl.toString());

  auto bytesOpt = folly::coro::blockingWait(getHttpBytesAsync(infoUrlStr, timeout));
  if (!bytesOpt) {
    throw ZException(fmt::format("Neuroglancer skeleton info not found (HTTP 403/404) at '{}'", infoUrlStr));
  }
  const std::string infoText(reinterpret_cast<const char*>(bytesOpt->data()), bytesOpt->size());
  return parseInfoJsonText(skeletonDirUrl, infoText, baseResolutionNm, baseVoxelOffset, timeout);
}

std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource> ZNeuroglancerPrecomputedSkeletonSource::parseInfoJsonText(
  const QUrl& skeletonDirUrl,
  const std::string& infoText,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  auto parseVertexAttributeDataType = [](QString s) -> VertexAttributeDataType {
    s = s.trimmed().toLower();
    if (s == "float32") {
      return VertexAttributeDataType::Float32;
    }
    if (s == "int8") {
      return VertexAttributeDataType::Int8;
    }
    if (s == "uint8") {
      return VertexAttributeDataType::Uint8;
    }
    if (s == "int16") {
      return VertexAttributeDataType::Int16;
    }
    if (s == "uint16") {
      return VertexAttributeDataType::Uint16;
    }
    if (s == "int32") {
      return VertexAttributeDataType::Int32;
    }
    if (s == "uint32") {
      return VertexAttributeDataType::Uint32;
    }
    throw ZException(fmt::format("Unsupported neuroglancer skeleton vertex attribute data_type '{}'", toStdString(s)));
  };

  json::value jv = json::parse(infoText);
  if (!jv.is_object()) {
    throw ZException("Neuroglancer skeleton info is not a JSON object");
  }
  const auto& root = jv.as_object();

  const QString type = optionalString(root, "@type", "neuroglancer_skeletons").trimmed();
  if (type != "neuroglancer_skeletons") {
    throw ZException(fmt::format("Unsupported neuroglancer skeleton '@type': '{}'", toStdString(type)));
  }

  auto source = std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource>(new ZNeuroglancerPrecomputedSkeletonSource());
  source->m_skeletonDirUrl = skeletonDirUrl;
  source->m_transform = parseTransform(root);

  source->m_baseResolutionNm = baseResolutionNm;
  source->m_baseVoxelOffset = baseVoxelOffset;
  source->m_timeout = timeout;

  auto attrsIt = root.find("vertex_attributes");
  if (attrsIt == root.end() || attrsIt->value().is_null()) {
    // Optional, defaults to empty.
  } else if (!attrsIt->value().is_array()) {
    throw ZException("Invalid 'vertex_attributes' in neuroglancer skeleton info (expected array)");
  } else {
    const auto& arr = attrsIt->value().as_array();
    source->m_vertexAttributes.reserve(arr.size());
    for (const auto& v : arr) {
      if (!v.is_object()) {
        throw ZException("Invalid 'vertex_attributes' in neuroglancer skeleton info (expected objects)");
      }
      const auto& ao = v.as_object();

      auto idIt = ao.find("id");
      if (idIt == ao.end() || !idIt->value().is_string()) {
        throw ZException("Invalid neuroglancer skeleton vertex attribute: missing string 'id'");
      }
      const QString id = json::value_to<QString>(idIt->value()).trimmed();
      if (id.isEmpty()) {
        throw ZException("Invalid neuroglancer skeleton vertex attribute: 'id' must be non-empty");
      }

      auto dtIt = ao.find("data_type");
      if (dtIt == ao.end() || !dtIt->value().is_string()) {
        throw ZException("Invalid neuroglancer skeleton vertex attribute: missing string 'data_type'");
      }
      const QString dataTypeStr = json::value_to<QString>(dtIt->value());
      const auto dataType = parseVertexAttributeDataType(dataTypeStr);

      auto ncIt = ao.find("num_components");
      if (ncIt == ao.end() || (!ncIt->value().is_int64() && !ncIt->value().is_uint64())) {
        throw ZException("Invalid neuroglancer skeleton vertex attribute: missing integer 'num_components'");
      }
      const uint64_t numComponents = ncIt->value().is_uint64() ? ncIt->value().as_uint64() : static_cast<uint64_t>(ncIt->value().as_int64());
      if (numComponents == 0) {
        throw ZException("Invalid neuroglancer skeleton vertex attribute: num_components must be > 0");
      }

      VertexAttribute a;
      a.id = toStdString(id);
      a.dataType = dataType;
      a.numComponents = static_cast<size_t>(numComponents);
      source->m_vertexAttributes.push_back(std::move(a));

      if (id == "radius") {
        if (dataType != VertexAttributeDataType::Float32 || numComponents != 1) {
          throw ZException("Invalid neuroglancer skeleton vertex attribute: 'radius' must be float32 with num_components=1");
        }
        source->m_hasRadiusAttribute = true;
      }
    }
  }

  auto shardingIt = root.find("sharding");
  if (shardingIt != root.end() && !shardingIt->value().is_null()) {
    if (!shardingIt->value().is_object()) {
      throw ZException("Invalid neuroglancer skeleton sharding: expected object");
    }
    source->m_sharding = parseShardingSpec(shardingIt->value().as_object());
  }

  return source;
}

std::shared_ptr<ZSkeleton> ZNeuroglancerPrecomputedSkeletonSource::decodeSkeletonBytes(std::span<const uint8_t> bytes) const
{
  auto scalarBytesFor = [](VertexAttributeDataType t) -> size_t {
    switch (t) {
    case VertexAttributeDataType::Float32:
      return 4;
    case VertexAttributeDataType::Int8:
    case VertexAttributeDataType::Uint8:
      return 1;
    case VertexAttributeDataType::Int16:
    case VertexAttributeDataType::Uint16:
      return 2;
    case VertexAttributeDataType::Int32:
    case VertexAttributeDataType::Uint32:
      return 4;
    }
    throw ZException("Invalid vertex attribute data type");
  };

  auto ensure = [&](size_t offset, size_t need, const char* what) {
    if (offset + need > bytes.size()) {
      throw ZException(fmt::format("Invalid neuroglancer skeleton: truncated {}", what));
    }
  };

  size_t off = 0;
  ensure(off, 8, "header");
  const uint32_t numVertices = readLE<uint32_t>(bytes.data() + off);
  off += 4;
  const uint32_t numEdges = readLE<uint32_t>(bytes.data() + off);
  off += 4;

  const size_t vertexPosBytes = static_cast<size_t>(numVertices) * 12;
  ensure(off, vertexPosBytes, "vertex_positions");

  const glm::vec3 invRes(static_cast<float>(1.0 / m_baseResolutionNm[0]),
                         static_cast<float>(1.0 / m_baseResolutionNm[1]),
                         static_cast<float>(1.0 / m_baseResolutionNm[2]));
  const glm::vec3 voxelOffset(static_cast<float>(m_baseVoxelOffset[0]),
                              static_cast<float>(m_baseVoxelOffset[1]),
                              static_cast<float>(m_baseVoxelOffset[2]));

  glm::mat4 voxelFromModel(1.0F);
  voxelFromModel = glm::translate(voxelFromModel, -voxelOffset);
  voxelFromModel = glm::scale(voxelFromModel, invRes);

  std::vector<glm::vec3> vertices;
  vertices.resize(numVertices);
  for (uint32_t i = 0; i < numVertices; ++i) {
    const float x = readLE<float>(bytes.data() + off);
    const float y = readLE<float>(bytes.data() + off + 4);
    const float z = readLE<float>(bytes.data() + off + 8);
    off += 12;

    const glm::vec4 stored(x, y, z, 1.0F);
    const glm::vec4 model = m_transform * stored;
    const glm::vec4 voxel = voxelFromModel * model;
    vertices[i] = glm::vec3(voxel.x, voxel.y, voxel.z);
  }

  const size_t edgeBytes = static_cast<size_t>(numEdges) * 8;
  ensure(off, edgeBytes, "edges");
  std::vector<glm::uvec2> edges;
  edges.resize(numEdges);
  for (uint32_t i = 0; i < numEdges; ++i) {
    const uint32_t a = readLE<uint32_t>(bytes.data() + off);
    const uint32_t b = readLE<uint32_t>(bytes.data() + off + 4);
    off += 8;
    if (a >= numVertices || b >= numVertices) {
      throw ZException("Invalid neuroglancer skeleton: edge vertex index out of range");
    }
    edges[i] = glm::uvec2(a, b);
  }

  std::vector<float> radii;
  if (m_hasRadiusAttribute) {
    radii.resize(numVertices, 0.0F);
  }

  // Compute the uniform scale applied by m_transform for converting stored radius units to model nm units.
  const float sx = glm::length(glm::vec3(m_transform[0]));
  const float sy = glm::length(glm::vec3(m_transform[1]));
  const float sz = glm::length(glm::vec3(m_transform[2]));
  const float sMax = std::max({sx, sy, sz});
  const float sMin = std::min({sx, sy, sz});
  const float uniformScale = (sx + sy + sz) / 3.0F;

  // World units for radii follow the same convention used by Z3DImgFilter's voxel-aspect coordTransform:
  // 1 world unit corresponds to max(voxelSizeX, voxelSizeY) nanometers.
  const double xyNm = std::max(m_baseResolutionNm[0], m_baseResolutionNm[1]);
  if (!(xyNm > 0.0) || !std::isfinite(xyNm)) {
    throw ZException("Invalid neuroglancer volume base resolution (XY)");
  }
  const float invXYNm = static_cast<float>(1.0 / xyNm);

  if (m_hasRadiusAttribute && std::isfinite(uniformScale) && uniformScale > 0.0F && sMax > 0.0F && sMin > 0.0F) {
    const float rel = (sMax - sMin) / sMax;
    if (rel > 1e-4f) {
      // External input; do not crash, but make it visible for debugging.
      VLOG(1) << fmt::format("Neuroglancer skeleton transform scale is not uniform (sx={}, sy={}, sz={}); using average {} for 'radius'",
                             sx,
                             sy,
                             sz,
                             uniformScale);
    }
  }

  for (const VertexAttribute& attr : m_vertexAttributes) {
    const size_t scalarBytes = scalarBytesFor(attr.dataType);
    const size_t totalBytes = static_cast<size_t>(numVertices) * attr.numComponents * scalarBytes;
    ensure(off, totalBytes, "vertex attribute data");

    if (m_hasRadiusAttribute && attr.id == "radius") {
      // radius: float32, 1 component (validated in parseInfoJsonText)
      for (uint32_t i = 0; i < numVertices; ++i) {
        const float rStored = readLE<float>(bytes.data() + off);
        off += 4;
        const float rNm = rStored * uniformScale;
        radii[i] = rNm * invXYNm;
      }
      continue;
    }

    // Skip unhandled attributes (but consume bytes to keep parsing aligned).
    off += totalBytes;
  }

  if (off > bytes.size()) {
    throw ZException("Invalid neuroglancer skeleton: truncated attribute data");
  }
  if (off != bytes.size()) {
    VLOG(2) << fmt::format("Neuroglancer skeleton has {} trailing bytes", bytes.size() - off);
  }

  auto out = std::make_shared<ZSkeleton>();
  out->setVertices(std::move(vertices));
  out->setEdges(std::move(edges));
  if (!radii.empty()) {
    out->setRadii(std::move(radii));
  }
  return out;
}

std::shared_ptr<ZSkeleton> ZNeuroglancerPrecomputedSkeletonSource::loadSkeletonBlocking(uint64_t segmentId) const
{
  std::optional<std::vector<uint8_t>> bytesOpt;

  if (!m_sharding) {
    const QUrl url = m_skeletonDirUrl.resolved(QUrl(QString::number(segmentId)));
    const std::string urlStr = toStdString(url.toString());
    auto resOpt = folly::coro::blockingWait(ZProxygenHttpClient::instance().getBytes(urlStr, m_timeout));
    if (!resOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton not found for segment {} (HTTP 403/404)", segmentId));
    }
    if (resOpt->status != 200) {
      throw ZException(fmt::format("Failed to fetch neuroglancer skeleton from '{}' (HTTP {})", urlStr, resOpt->status));
    }
    bytesOpt = std::move(resOpt->body);
  } else {
    const auto& sharding = *m_sharding;

    // Chunk identifier for skeletons is simply the segment ID.
    const uint64_t chunkId = segmentId;
    const uint64_t shiftedChunkId = chunkId >> sharding.preshiftBits;
    const uint64_t hashCode = (sharding.hash == ZNeuroglancerPrecomputedVolume::Scale::Sharding::Hash::Identity)
                                ? shiftedChunkId
                                : ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(shiftedChunkId, /*seed=*/0);
    const uint64_t shardAndMinishard = hashCode & sharding.shardAndMinishardMask;
    const uint64_t minishard = shardAndMinishard & sharding.minishardMask;
    const uint64_t shard = (shardAndMinishard >> sharding.minishardBits) & sharding.shardMask;

    auto entryOpt = folly::coro::blockingWait(getShardIndexEntry(m_skeletonDirUrl, sharding, m_timeout, shard, minishard));
    if (!entryOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton shard index not found for segment {}", segmentId));
    }

    const ShardedShardIndexEntry& entry = *entryOpt;
    if (entry.minishardIndexEnd <= entry.minishardIndexStart) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton not found for segment {}", segmentId));
    }

    const uint64_t minishardLen = entry.minishardIndexEnd - entry.minishardIndexStart;
    auto minishardBytesOpt = folly::coro::blockingWait(
      getHttpRangeBytesAsync(entry.dataUrl, m_timeout, entry.baseDataOffset + entry.minishardIndexStart, minishardLen));
    if (!minishardBytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton minishard not found for segment {}", segmentId));
    }

    auto decodedMinishardBytes = decodeShardedBytes(std::move(*minishardBytesOpt), sharding.minishardIndexEncoding);
    auto decoded =
      ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t>(decodedMinishardBytes.data(), decodedMinishardBytes.size()),
                                                       entry.baseDataOffset);
    if (decoded.keys.empty()) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton not found for segment {}", segmentId));
    }

    auto it = std::lower_bound(decoded.keys.begin(), decoded.keys.end(), chunkId);
    if (it == decoded.keys.end() || *it != chunkId) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton not found for segment {}", segmentId));
    }
    const size_t idx = static_cast<size_t>(it - decoded.keys.begin());
    const uint64_t start = decoded.starts[idx];
    const uint64_t end = decoded.ends[idx];
    if (end <= start) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton not found for segment {}", segmentId));
    }

    auto payloadBytesOpt =
      folly::coro::blockingWait(getHttpRangeBytesAsync(entry.dataUrl, m_timeout, start, end - start));
    if (!payloadBytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer skeleton payload not found for segment {}", segmentId));
    }
    auto decodedPayloadBytes = decodeShardedBytes(std::move(*payloadBytesOpt), sharding.dataEncoding);
    bytesOpt = std::move(decodedPayloadBytes);
  }

  CHECK(bytesOpt);
  return decodeSkeletonBytes(std::span<const uint8_t>(bytesOpt->data(), bytesOpt->size()));
}

} // namespace nim
