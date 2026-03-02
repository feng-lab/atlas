#include "zneuroglancerprecomputedannotations.h"

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
#include <unordered_set>
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
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer annotations info", key));
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
    throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected string)", key));
  }
  return json::value_to<QString>(it->value());
}

uint64_t requireUint64(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end()) {
    throw ZException(fmt::format("Missing '{}' in neuroglancer annotations info", key));
  }
  const json::value& v = it->value();
  if (v.is_uint64()) {
    return v.as_uint64();
  }
  if (v.is_int64()) {
    if (v.as_int64() < 0) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected >= 0)", key));
    }
    return static_cast<uint64_t>(v.as_int64());
  }
  throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected uint64)", key));
}

double numberToDouble(const json::value& v, const char* what)
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
  throw ZException(fmt::format("Invalid {} in neuroglancer annotations info (expected number)", what));
}

uint64_t numberToUint64(const json::value& v, const char* what)
{
  if (v.is_uint64()) {
    return v.as_uint64();
  }
  if (v.is_int64()) {
    const int64_t i = v.as_int64();
    if (i < 0) {
      throw ZException(fmt::format("Invalid {} in neuroglancer annotations info (expected >= 0)", what));
    }
    return static_cast<uint64_t>(i);
  }
  throw ZException(fmt::format("Invalid {} in neuroglancer annotations info (expected integer)", what));
}

std::array<double, 3> requireNumberArray3(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer annotations info (expected array)", key));
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected length-3 array)", key));
  }
  std::array<double, 3> out{};
  for (size_t d = 0; d < 3; ++d) {
    out[d] = numberToDouble(arr[d], key);
  }
  return out;
}

std::array<uint64_t, 3> requirePositiveUintArray3(const json::object& obj, const char* key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->value().is_array()) {
    throw ZException(fmt::format("Missing or invalid '{}' in neuroglancer annotations info (expected array)", key));
  }
  const auto& arr = it->value().as_array();
  if (arr.size() != 3) {
    throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected length-3 array)", key));
  }
  std::array<uint64_t, 3> out{};
  for (size_t d = 0; d < 3; ++d) {
    out[d] = numberToUint64(arr[d], key);
    if (out[d] == 0) {
      throw ZException(fmt::format("Invalid '{}' in neuroglancer annotations info (expected > 0)", key));
    }
  }
  return out;
}

double neuroglancerUnitToMeters(QString unit)
{
  unit = unit.trimmed().toLower();
  if (unit.isEmpty()) {
    // Unitless quantity (not meaningful for spatial coordinates); treat as meters to avoid divide-by-zero.
    return 1.0;
  }
  if (unit == "m") {
    return 1.0;
  }
  if (unit == "mm") {
    return 1e-3;
  }
  if (unit == "um" || unit == "µm") {
    return 1e-6;
  }
  if (unit == "nm") {
    return 1e-9;
  }
  throw ZException(fmt::format("Unsupported neuroglancer unit '{}'", toStdString(unit)));
}

ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType parseAnnotationType(QString s)
{
  s = s.trimmed().toUpper();
  if (s == "POINT") {
    return ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point;
  }
  if (s == "LINE") {
    return ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line;
  }
  if (s == "AXIS_ALIGNED_BOUNDING_BOX") {
    return ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::AxisAlignedBoundingBox;
  }
  if (s == "ELLIPSOID") {
    return ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid;
  }
  if (s == "POLYLINE") {
    return ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Polyline;
  }
  throw ZException(fmt::format("Unsupported neuroglancer annotation_type '{}'", toStdString(s)));
}

ZNeuroglancerPrecomputedAnnotationsSource::PropertyType parsePropertyType(QString s)
{
  s = s.trimmed().toLower();
  if (s == "rgb") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Rgb;
  }
  if (s == "rgba") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Rgba;
  }
  if (s == "uint8") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Uint8;
  }
  if (s == "int8") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Int8;
  }
  if (s == "uint16") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Uint16;
  }
  if (s == "int16") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Int16;
  }
  if (s == "uint32") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Uint32;
  }
  if (s == "int32") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Int32;
  }
  if (s == "float32") {
    return ZNeuroglancerPrecomputedAnnotationsSource::PropertyType::Float32;
  }
  throw ZException(fmt::format("Unsupported neuroglancer annotations property type '{}'", toStdString(s)));
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
uint16_t readLE<uint16_t>(const uint8_t* p)
{
  return (static_cast<uint16_t>(p[0]) << 0) | (static_cast<uint16_t>(p[1]) << 8);
}

template<>
int16_t readLE<int16_t>(const uint8_t* p)
{
  const uint16_t u = readLE<uint16_t>(p);
  int16_t i = 0;
  std::memcpy(&i, &u, sizeof(int16_t));
  return i;
}

template<>
uint32_t readLE<uint32_t>(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

template<>
int32_t readLE<int32_t>(const uint8_t* p)
{
  const uint32_t u = readLE<uint32_t>(p);
  int32_t i = 0;
  std::memcpy(&i, &u, sizeof(int32_t));
  return i;
}

template<>
float readLE<float>(const uint8_t* p)
{
  const uint32_t u = readLE<uint32_t>(p);
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

std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource> ZNeuroglancerPrecomputedAnnotationsSource::open(
  const QUrl& annotationRootUrl,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  QUrl rootUrl = annotationRootUrl;
  if (!rootUrl.toString().endsWith('/')) {
    rootUrl = QUrl(rootUrl.toString() + "/");
  }

  const QUrl infoUrl = rootUrl.resolved(QUrl("info"));
  const std::string infoUrlStr = toStdString(infoUrl.toString());

  auto bytesOpt = folly::coro::blockingWait(getHttpBytesAsync(infoUrlStr, timeout));
  if (!bytesOpt) {
    throw ZException(fmt::format("Neuroglancer annotations info not found (HTTP 404) at '{}'", infoUrlStr));
  }
  const std::string infoText(reinterpret_cast<const char*>(bytesOpt->data()), bytesOpt->size());
  return parseInfoJsonText(rootUrl, infoText, baseResolutionNm, baseVoxelOffset, timeout);
}

std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource> ZNeuroglancerPrecomputedAnnotationsSource::parseInfoJsonText(
  const QUrl& annotationRootUrl,
  const std::string& infoText,
  std::array<double, 3> baseResolutionNm,
  std::array<int64_t, 3> baseVoxelOffset,
  std::chrono::milliseconds timeout)
{
  auto out = std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource>(new ZNeuroglancerPrecomputedAnnotationsSource());
  out->m_rootUrl = annotationRootUrl;
  if (!out->m_rootUrl.toString().endsWith('/')) {
    out->m_rootUrl = QUrl(out->m_rootUrl.toString() + "/");
  }

  out->m_baseResolutionNm = baseResolutionNm;
  out->m_baseVoxelOffset = baseVoxelOffset;
  out->m_timeout = timeout;

  json::value jv;
  try {
    jv = json::parse(infoText);
  }
  catch (const std::exception& e) {
    throw ZException(fmt::format("Failed to parse neuroglancer annotations info JSON: {}", e.what()));
  }

  if (!jv.is_object()) {
    throw ZException("Invalid neuroglancer annotations info: expected JSON object");
  }
  const auto& root = jv.as_object();

  const QString type = requireString(root, "@type").trimmed();
  if (type != "neuroglancer_annotations_v1") {
    throw ZException(fmt::format("Unsupported neuroglancer annotations '@type': '{}'", toStdString(type)));
  }

  const QString annotationTypeStr = requireString(root, "annotation_type");
  out->m_annotationType = parseAnnotationType(annotationTypeStr);

  auto dimsIt = root.find("dimensions");
  if (dimsIt == root.end() || !dimsIt->value().is_object()) {
    throw ZException("Invalid neuroglancer annotations info: missing object field 'dimensions'");
  }
  const auto& dimsObj = dimsIt->value().as_object();
  if (dimsObj.size() != 3) {
    throw ZException(fmt::format("Unsupported neuroglancer annotations rank {} (expected 3)", dimsObj.size()));
  }

  // Parse in JSON insertion order (matches Neuroglancer rank order).
  size_t dimIndex = 0;
  for (const auto& kv : dimsObj) {
    if (!kv.value().is_array()) {
      throw ZException("Invalid neuroglancer annotations info: dimensions value must be [scale, unit]");
    }
    const auto& arr = kv.value().as_array();
    if (arr.size() != 2) {
      throw ZException("Invalid neuroglancer annotations info: dimensions value must be length-2 array");
    }
    double scale = 0.0;
    if (arr[0].is_double()) {
      scale = arr[0].as_double();
    } else if (arr[0].is_int64()) {
      scale = static_cast<double>(arr[0].as_int64());
    } else if (arr[0].is_uint64()) {
      scale = static_cast<double>(arr[0].as_uint64());
    } else {
      throw ZException("Invalid neuroglancer annotations info: dimensions scale must be numeric");
    }
    if (!(scale > 0.0) || !std::isfinite(scale)) {
      throw ZException("Invalid neuroglancer annotations info: dimensions scale must be finite and > 0");
    }

    if (!arr[1].is_string()) {
      throw ZException("Invalid neuroglancer annotations info: dimensions unit must be a string");
    }
    const QString unit = json::value_to<QString>(arr[1]);
    const double meters = neuroglancerUnitToMeters(unit);
    const double nm = scale * meters * 1e9;
    if (!(nm > 0.0) || !std::isfinite(nm)) {
      throw ZException("Invalid neuroglancer annotations info: dimensions scale in nm must be finite and > 0");
    }
    out->m_dimScaleNm.at(dimIndex) = nm;
    ++dimIndex;
  }

  auto propsIt = root.find("properties");
  if (propsIt != root.end() && !propsIt->value().is_null()) {
    if (!propsIt->value().is_array()) {
      throw ZException("Invalid neuroglancer annotations info: properties must be an array");
    }
    const auto& arr = propsIt->value().as_array();
    out->m_properties.clear();
    out->m_properties.reserve(arr.size());
    for (const auto& pv : arr) {
      if (!pv.is_object()) {
        throw ZException("Invalid neuroglancer annotations info: properties array entries must be objects");
      }
      const auto& po = pv.as_object();
      PropertySpec p;
      p.id = requireString(po, "id").trimmed();
      p.type = parsePropertyType(requireString(po, "type"));
      p.description = optionalString(po, "description");
      out->m_properties.push_back(std::move(p));
    }
  }

  auto relationshipsIt = root.find("relationships");
  if (relationshipsIt != root.end() && !relationshipsIt->value().is_null()) {
    if (!relationshipsIt->value().is_array()) {
      throw ZException("Invalid neuroglancer annotations info: relationships must be an array");
    }
    const auto& arr = relationshipsIt->value().as_array();
    out->m_relationships.clear();
    out->m_relationships.reserve(arr.size());
    for (const auto& rv : arr) {
      if (!rv.is_object()) {
        throw ZException("Invalid neuroglancer annotations info: relationships array entries must be objects");
      }
      const auto& ro = rv.as_object();
      RelationshipSpec r;
      r.id = requireString(ro, "id").trimmed();
      QString key = requireString(ro, "key").trimmed();
      if (!key.endsWith('/')) {
        key += '/';
      }
      r.indexDirUrl = out->m_rootUrl.resolved(QUrl(key));
      if (auto shIt = ro.find("sharding"); shIt != ro.end() && !shIt->value().is_null()) {
        if (!shIt->value().is_object()) {
          throw ZException("Invalid neuroglancer annotations relationship sharding: expected object");
        }
        r.sharding = parseShardingSpec(shIt->value().as_object());
      }
      out->m_relationships.push_back(std::move(r));
    }
  }

  auto byIdIt = root.find("by_id");
  if (byIdIt != root.end() && !byIdIt->value().is_null()) {
    if (!byIdIt->value().is_object()) {
      throw ZException("Invalid neuroglancer annotations info: by_id must be an object");
    }
    const auto& bo = byIdIt->value().as_object();
    IndexSpec byId;
    QString key = requireString(bo, "key").trimmed();
    if (!key.endsWith('/')) {
      key += '/';
    }
    byId.indexDirUrl = out->m_rootUrl.resolved(QUrl(key));
    if (auto shIt = bo.find("sharding"); shIt != bo.end() && !shIt->value().is_null()) {
      if (!shIt->value().is_object()) {
        throw ZException("Invalid neuroglancer annotations by_id sharding: expected object");
      }
      byId.sharding = parseShardingSpec(shIt->value().as_object());
    }
    out->m_byId = std::move(byId);
  }

  out->m_lowerBoundCoord = requireNumberArray3(root, "lower_bound");
  out->m_upperBoundCoord = requireNumberArray3(root, "upper_bound");
  for (size_t d = 0; d < 3; ++d) {
    if (!(out->m_upperBoundCoord[d] > out->m_lowerBoundCoord[d]) || !std::isfinite(out->m_lowerBoundCoord[d]) ||
        !std::isfinite(out->m_upperBoundCoord[d])) {
      throw ZException("Invalid neuroglancer annotations bounds (expected finite upper_bound > lower_bound)");
    }
  }

  auto spatialIt = root.find("spatial");
  if (spatialIt != root.end() && !spatialIt->value().is_null()) {
    if (!spatialIt->value().is_array()) {
      throw ZException("Invalid neuroglancer annotations info: spatial must be an array");
    }
    const auto& arr = spatialIt->value().as_array();
    out->m_spatial.clear();
    out->m_spatial.reserve(arr.size());
    for (const auto& sv : arr) {
      if (!sv.is_object()) {
        throw ZException("Invalid neuroglancer annotations spatial level: expected object");
      }
      const auto& so = sv.as_object();
      SpatialLevelSpec level;
      QString key = requireString(so, "key").trimmed();
      if (!key.endsWith('/')) {
        key += '/';
      }
      level.indexDirUrl = out->m_rootUrl.resolved(QUrl(key));
      level.gridShape = requirePositiveUintArray3(so, "grid_shape");
      {
        const std::array<double, 3> chunk = requireNumberArray3(so, "chunk_size");
        for (size_t d = 0; d < 3; ++d) {
          if (!(chunk[d] > 0.0) || !std::isfinite(chunk[d])) {
            throw ZException("Invalid neuroglancer annotations chunk_size (expected finite > 0)");
          }
          level.chunkSize[d] = chunk[d];
        }
      }
      level.limit = requireUint64(so, "limit");
      if (level.limit == 0) {
        throw ZException("Invalid neuroglancer annotations spatial level: limit must be > 0");
      }
      if (auto shIt = so.find("sharding"); shIt != so.end() && !shIt->value().is_null()) {
        if (!shIt->value().is_object()) {
          throw ZException("Invalid neuroglancer annotations spatial sharding: expected object");
        }
        level.sharding = parseShardingSpec(shIt->value().as_object());
      }
      out->m_spatial.push_back(std::move(level));
    }
  }

  // Validate base resolution (external input: volume info).
  for (size_t d = 0; d < 3; ++d) {
    if (!(out->m_baseResolutionNm[d] > 0.0) || !std::isfinite(out->m_baseResolutionNm[d])) {
      throw ZException("Invalid baseResolutionNm for neuroglancer annotations");
    }
  }

  return out;
}

std::optional<ZNeuroglancerPrecomputedAnnotationsSource::RelationshipSpec>
ZNeuroglancerPrecomputedAnnotationsSource::findRelationship(const QString& id) const
{
  const QString key = id.trimmed();
  if (key.isEmpty()) {
    return std::nullopt;
  }
  for (const auto& r : m_relationships) {
    if (r.id == key) {
      return r;
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> ZNeuroglancerPrecomputedAnnotationsSource::loadIndexEntryBlocking(
  const QUrl& dirUrl,
  const std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding>& shardingOpt,
  uint64_t key) const
{
  std::optional<std::vector<uint8_t>> bytesOpt;
  if (!shardingOpt) {
    const QUrl url = dirUrl.resolved(QUrl(QString::number(key)));
    const std::string urlStr = toStdString(url.toString());
    auto resOpt = folly::coro::blockingWait(ZProxygenHttpClient::instance().getBytes(urlStr, m_timeout));
    if (!resOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations index entry not found for key {} (HTTP 404)", key));
    }
    if (resOpt->status != 200) {
      throw ZException(fmt::format("Failed to fetch neuroglancer annotations index entry from '{}' (HTTP {})", urlStr, resOpt->status));
    }
    bytesOpt = std::move(resOpt->body);
  } else {
    const auto& sharding = *shardingOpt;

    const uint64_t chunkId = key;
    const uint64_t shiftedChunkId = chunkId >> sharding.preshiftBits;
    const uint64_t hashCode = (sharding.hash == ZNeuroglancerPrecomputedVolume::Scale::Sharding::Hash::Identity)
                                ? shiftedChunkId
                                : ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(shiftedChunkId, /*seed=*/0);
    const uint64_t shardAndMinishard = hashCode & sharding.shardAndMinishardMask;
    const uint64_t minishard = shardAndMinishard & sharding.minishardMask;
    const uint64_t shard = (shardAndMinishard >> sharding.minishardBits) & sharding.shardMask;

    auto entryOpt = folly::coro::blockingWait(getShardIndexEntry(dirUrl, sharding, m_timeout, shard, minishard));
    if (!entryOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations shard index not found for key {}", key));
    }

    const ShardedShardIndexEntry& entry = *entryOpt;
    if (entry.minishardIndexEnd <= entry.minishardIndexStart) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations index entry not found for key {}", key));
    }

    const uint64_t minishardLen = entry.minishardIndexEnd - entry.minishardIndexStart;
    auto minishardBytesOpt = folly::coro::blockingWait(
      getHttpRangeBytesAsync(entry.dataUrl, m_timeout, entry.baseDataOffset + entry.minishardIndexStart, minishardLen));
    if (!minishardBytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations minishard not found for key {}", key));
    }

    auto decodedMinishardBytes = decodeShardedBytes(std::move(*minishardBytesOpt), sharding.minishardIndexEncoding);
    auto decoded =
      ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t>(decodedMinishardBytes.data(), decodedMinishardBytes.size()),
                                                       entry.baseDataOffset);
    if (decoded.keys.empty()) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations index entry not found for key {}", key));
    }

    auto it = std::lower_bound(decoded.keys.begin(), decoded.keys.end(), chunkId);
    if (it == decoded.keys.end() || *it != chunkId) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations index entry not found for key {}", key));
    }
    const size_t idx = static_cast<size_t>(it - decoded.keys.begin());
    const uint64_t start = decoded.starts[idx];
    const uint64_t end = decoded.ends[idx];
    if (end <= start) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations index entry not found for key {}", key));
    }

    auto payloadBytesOpt =
      folly::coro::blockingWait(getHttpRangeBytesAsync(entry.dataUrl, m_timeout, start, end - start));
    if (!payloadBytesOpt) {
      throw ZNotFoundException(fmt::format("Neuroglancer annotations payload not found for key {}", key));
    }
    auto decodedPayloadBytes = decodeShardedBytes(std::move(*payloadBytesOpt), sharding.dataEncoding);
    bytesOpt = std::move(decodedPayloadBytes);
  }

  CHECK(bytesOpt);
  return std::move(*bytesOpt);
}

glm::vec3 ZNeuroglancerPrecomputedAnnotationsSource::voxelFromCoordUnits(const glm::vec3& coord) const
{
  // coord is in the coordinate units defined by dimensions; convert to nm then to voxel index.
  const double nmX = static_cast<double>(coord.x) * m_dimScaleNm[0];
  const double nmY = static_cast<double>(coord.y) * m_dimScaleNm[1];
  const double nmZ = static_cast<double>(coord.z) * m_dimScaleNm[2];

  const double vx = nmX / m_baseResolutionNm[0] - static_cast<double>(m_baseVoxelOffset[0]);
  const double vy = nmY / m_baseResolutionNm[1] - static_cast<double>(m_baseVoxelOffset[1]);
  const double vz = nmZ / m_baseResolutionNm[2] - static_cast<double>(m_baseVoxelOffset[2]);

  return glm::vec3(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz));
}

glm::dvec3 ZNeuroglancerPrecomputedAnnotationsSource::coordUnitsFromVoxel(const glm::dvec3& voxel) const
{
  const double nmX = (voxel.x + static_cast<double>(m_baseVoxelOffset[0])) * m_baseResolutionNm[0];
  const double nmY = (voxel.y + static_cast<double>(m_baseVoxelOffset[1])) * m_baseResolutionNm[1];
  const double nmZ = (voxel.z + static_cast<double>(m_baseVoxelOffset[2])) * m_baseResolutionNm[2];

  const double cx = nmX / m_dimScaleNm[0];
  const double cy = nmY / m_dimScaleNm[1];
  const double cz = nmZ / m_dimScaleNm[2];

  return glm::dvec3(cx, cy, cz);
}

std::optional<std::vector<uint8_t>> ZNeuroglancerPrecomputedAnnotationsSource::loadSpatialCellEntryBlocking(
  const SpatialLevelSpec& level,
  const std::array<uint64_t, 3>& cell) const
{
  if (!level.sharding) {
    const QString filename = QString("%1_%2_%3").arg(cell[0]).arg(cell[1]).arg(cell[2]);
    const QUrl url = level.indexDirUrl.resolved(QUrl(filename));
    const std::string urlStr = toStdString(url.toString());
    auto resOpt = folly::coro::blockingWait(ZProxygenHttpClient::instance().getBytes(urlStr, m_timeout));
    if (!resOpt) {
      return std::nullopt;
    }
    if (resOpt->status != 200) {
      throw ZException(
        fmt::format("Failed to fetch neuroglancer spatial index entry from '{}' (HTTP {})", urlStr, resOpt->status));
    }
    return std::move(resOpt->body);
  }

  const uint64_t key = ZNeuroglancerUint64Sharding::compressedMortonCode(cell, level.gridShape);
  try {
    return loadIndexEntryBlocking(level.indexDirUrl, level.sharding, key);
  }
  catch (const ZNotFoundException&) {
    return std::nullopt;
  }
}

namespace {

struct VoxelBox
{
  glm::dvec3 min;
  glm::dvec3 max;
};

[[nodiscard]] VoxelBox makeVoxelBox(glm::dvec3 a, glm::dvec3 b)
{
  VoxelBox out;
  out.min = glm::min(a, b);
  out.max = glm::max(a, b);
  return out;
}

[[nodiscard]] bool intersectsAabb(const VoxelBox& box, const VoxelBox& other)
{
  if (box.max.x < other.min.x || other.max.x < box.min.x) {
    return false;
  }
  if (box.max.y < other.min.y || other.max.y < box.min.y) {
    return false;
  }
  if (box.max.z < other.min.z || other.max.z < box.min.z) {
    return false;
  }
  return true;
}

[[nodiscard]] bool segmentIntersectsAabb(const glm::dvec3& p0, const glm::dvec3& p1, const VoxelBox& box)
{
  // Slab intersection (Liang-Barsky style) for axis-aligned bounding boxes.
  constexpr double kEps = 1e-12;
  glm::dvec3 d = p1 - p0;
  double tMin = 0.0;
  double tMax = 1.0;

  auto update = [&](double p0d, double dd, double minD, double maxD) {
    if (std::abs(dd) < kEps) {
      return (p0d >= minD && p0d <= maxD);
    }
    double t1 = (minD - p0d) / dd;
    double t2 = (maxD - p0d) / dd;
    if (t1 > t2) {
      std::swap(t1, t2);
    }
    tMin = std::max(tMin, t1);
    tMax = std::min(tMax, t2);
    return tMin <= tMax;
  };

  if (!update(p0.x, d.x, box.min.x, box.max.x)) {
    return false;
  }
  if (!update(p0.y, d.y, box.min.y, box.max.y)) {
    return false;
  }
  if (!update(p0.z, d.z, box.min.z, box.max.z)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool annotationIntersectsVoxelBox(const ZNeuroglancerPrecomputedAnnotationsSource::Annotation& a,
                                                const VoxelBox& box)
{
  using AnnotationType = ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType;

  if (a.points.empty()) {
    return false;
  }

  switch (a.type) {
  case AnnotationType::Point:
  {
    const auto& p = a.points[0];
    return (static_cast<double>(p.x) >= box.min.x && static_cast<double>(p.x) <= box.max.x &&
            static_cast<double>(p.y) >= box.min.y && static_cast<double>(p.y) <= box.max.y &&
            static_cast<double>(p.z) >= box.min.z && static_cast<double>(p.z) <= box.max.z);
  }
  case AnnotationType::Ellipsoid: {
    const auto& c = a.points[0];
    if (!a.ellipsoidRadiiVoxel) {
      return (static_cast<double>(c.x) >= box.min.x && static_cast<double>(c.x) <= box.max.x &&
              static_cast<double>(c.y) >= box.min.y && static_cast<double>(c.y) <= box.max.y &&
              static_cast<double>(c.z) >= box.min.z && static_cast<double>(c.z) <= box.max.z);
    }

    const glm::vec3 r = *a.ellipsoidRadiiVoxel;
    const double rx = static_cast<double>(r.x);
    const double ry = static_cast<double>(r.y);
    const double rz = static_cast<double>(r.z);
    if (!(rx > 0.0) || !(ry > 0.0) || !(rz > 0.0)) {
      // Degenerate ellipsoid: treat as point.
      return (static_cast<double>(c.x) >= box.min.x && static_cast<double>(c.x) <= box.max.x &&
              static_cast<double>(c.y) >= box.min.y && static_cast<double>(c.y) <= box.max.y &&
              static_cast<double>(c.z) >= box.min.z && static_cast<double>(c.z) <= box.max.z);
    }

    const double px = std::clamp(static_cast<double>(c.x), box.min.x, box.max.x);
    const double py = std::clamp(static_cast<double>(c.y), box.min.y, box.max.y);
    const double pz = std::clamp(static_cast<double>(c.z), box.min.z, box.max.z);
    const double dx = (px - static_cast<double>(c.x)) / rx;
    const double dy = (py - static_cast<double>(c.y)) / ry;
    const double dz = (pz - static_cast<double>(c.z)) / rz;
    const double dist2 = dx * dx + dy * dy + dz * dz;
    return dist2 <= 1.0;
  }
  case AnnotationType::Line: {
    if (a.points.size() != 2) {
      return false;
    }
    return segmentIntersectsAabb(glm::dvec3(a.points[0]), glm::dvec3(a.points[1]), box);
  }
  case AnnotationType::Polyline: {
    if (a.points.size() < 2) {
      return false;
    }
    for (size_t i = 1; i < a.points.size(); ++i) {
      if (segmentIntersectsAabb(glm::dvec3(a.points[i - 1]), glm::dvec3(a.points[i]), box)) {
        return true;
      }
    }
    return false;
  }
  case AnnotationType::AxisAlignedBoundingBox: {
    if (a.points.size() != 2) {
      return false;
    }
    const VoxelBox ab = makeVoxelBox(glm::dvec3(a.points[0]), glm::dvec3(a.points[1]));
    return intersectsAabb(box, ab);
  }
  }

  return false;
}

} // namespace

std::vector<ZNeuroglancerPrecomputedAnnotationsSource::Annotation>
ZNeuroglancerPrecomputedAnnotationsSource::loadAnnotationsIntersectingVoxelBoxBlocking(const glm::dvec3& voxelMin,
                                                                                       const glm::dvec3& voxelMax) const
{
  if (m_spatial.empty()) {
    throw ZException("Neuroglancer annotations dataset has no spatial index ('spatial' missing in info)");
  }

  const VoxelBox qVoxel = makeVoxelBox(voxelMin, voxelMax);
  const glm::dvec3 qMinCoord = coordUnitsFromVoxel(qVoxel.min);
  const glm::dvec3 qMaxCoord = coordUnitsFromVoxel(qVoxel.max);

  std::vector<Annotation> out;
  out.reserve(1024);
  std::unordered_set<uint64_t> seen;
  seen.reserve(1024);

  for (const SpatialLevelSpec& level : m_spatial) {
    std::array<int64_t, 3> cellMin{};
    std::array<int64_t, 3> cellMax{};

    for (size_t d = 0; d < 3; ++d) {
      const double origin = m_lowerBoundCoord[d];
      const double chunk = level.chunkSize[d];
      const int64_t maxIdx = static_cast<int64_t>(level.gridShape[d]) - 1;
      CHECK(maxIdx >= 0);

      const double a = (qMinCoord[d] - origin) / chunk;
      const double b = (qMaxCoord[d] - origin) / chunk;
      int64_t lo = static_cast<int64_t>(std::floor(std::min(a, b)));
      int64_t hi = static_cast<int64_t>(std::floor(std::max(a, b)));

      lo = std::clamp<int64_t>(lo, 0, maxIdx);
      hi = std::clamp<int64_t>(hi, 0, maxIdx);
      cellMin[d] = lo;
      cellMax[d] = hi;
    }

    for (int64_t z = cellMin[2]; z <= cellMax[2]; ++z) {
      for (int64_t y = cellMin[1]; y <= cellMax[1]; ++y) {
        for (int64_t x = cellMin[0]; x <= cellMax[0]; ++x) {
          const std::array<uint64_t, 3> cell = {static_cast<uint64_t>(x), static_cast<uint64_t>(y), static_cast<uint64_t>(z)};

          auto bytesOpt = loadSpatialCellEntryBlocking(level, cell);
          if (!bytesOpt) {
            continue;
          }
          const auto anns = decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytesOpt->data(), bytesOpt->size()));
          for (const auto& a : anns) {
            if (!annotationIntersectsVoxelBox(a, qVoxel)) {
              continue;
            }
            if (!seen.insert(a.id).second) {
              continue;
            }
            out.push_back(a);
          }
        }
      }
    }
  }

  return out;
}

void ZNeuroglancerPrecomputedAnnotationsSource::streamAnnotationsIntersectingVoxelBoxBlocking(
  const glm::dvec3& voxelMin,
  const glm::dvec3& voxelMax,
  const SpatialLoadUpdateCallback& onUpdate,
  const std::atomic_bool* cancelFlag,
  std::chrono::milliseconds minUpdateInterval,
  size_t maxAnnotationsPerUpdate) const
{
  CHECK(static_cast<bool>(onUpdate)) << "streamAnnotationsIntersectingVoxelBoxBlocking requires an onUpdate callback";
  if (m_spatial.empty()) {
    throw ZException("Neuroglancer annotations dataset has no spatial index ('spatial' missing in info)");
  }

  const VoxelBox qVoxel = makeVoxelBox(voxelMin, voxelMax);
  const glm::dvec3 qMinCoord = coordUnitsFromVoxel(qVoxel.min);
  const glm::dvec3 qMaxCoord = coordUnitsFromVoxel(qVoxel.max);

  auto safeAddU64 = [](uint64_t a, uint64_t b) -> uint64_t {
    if (a > std::numeric_limits<uint64_t>::max() - b) {
      return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
  };

  auto safeMulU64 = [](uint64_t a, uint64_t b) -> uint64_t {
    if (a == 0 || b == 0) {
      return 0;
    }
    if (a > std::numeric_limits<uint64_t>::max() / b) {
      return std::numeric_limits<uint64_t>::max();
    }
    return a * b;
  };

  struct LevelCellRange
  {
    const SpatialLevelSpec* level = nullptr;
    std::array<int64_t, 3> cellMin{};
    std::array<int64_t, 3> cellMax{};
  };

  std::vector<LevelCellRange> ranges;
  ranges.reserve(m_spatial.size());

  SpatialLoadProgress prog;
  prog.levelsTotal = m_spatial.size();
  prog.levelIndex = 0;
  prog.totalCells = 0;
  prog.visitedCells = 0;
  prog.uniqueAnnotations = 0;

  for (const SpatialLevelSpec& level : m_spatial) {
    LevelCellRange r;
    r.level = &level;

    for (size_t d = 0; d < 3; ++d) {
      const double origin = m_lowerBoundCoord[d];
      const double chunk = level.chunkSize[d];
      const int64_t maxIdx = static_cast<int64_t>(level.gridShape[d]) - 1;
      CHECK(maxIdx >= 0);

      const double a = (qMinCoord[d] - origin) / chunk;
      const double b = (qMaxCoord[d] - origin) / chunk;
      int64_t lo = static_cast<int64_t>(std::floor(std::min(a, b)));
      int64_t hi = static_cast<int64_t>(std::floor(std::max(a, b)));

      lo = std::clamp<int64_t>(lo, 0, maxIdx);
      hi = std::clamp<int64_t>(hi, 0, maxIdx);

      r.cellMin[d] = lo;
      r.cellMax[d] = hi;
    }

    const uint64_t nx = static_cast<uint64_t>(r.cellMax[0] - r.cellMin[0] + 1);
    const uint64_t ny = static_cast<uint64_t>(r.cellMax[1] - r.cellMin[1] + 1);
    const uint64_t nz = static_cast<uint64_t>(r.cellMax[2] - r.cellMin[2] + 1);
    const uint64_t levelCells = safeMulU64(safeMulU64(nx, ny), nz);
    prog.totalCells = safeAddU64(prog.totalCells, levelCells);
    ranges.push_back(r);
  }

  std::unordered_set<uint64_t> seen;
  seen.reserve(1024);

  std::vector<Annotation> batch;
  batch.reserve(std::min<size_t>(maxAnnotationsPerUpdate, 2048));

  using Clock = std::chrono::steady_clock;
  auto lastUpdate = Clock::now();

  auto shouldCancel = [&]() -> bool {
    return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
  };

  auto flush = [&](bool force) {
    const auto now = Clock::now();
    if (!force) {
      if (batch.empty()) {
        return;
      }
      if (now - lastUpdate < minUpdateInterval) {
        return;
      }
    }

    SpatialLoadUpdate u;
    u.progress = prog;
    u.newAnnotations = std::move(batch);
    batch.clear();
    batch.reserve(std::min<size_t>(maxAnnotationsPerUpdate, 2048));

    onUpdate(std::move(u));
    lastUpdate = now;
  };

  for (size_t levelIdx = 0; levelIdx < ranges.size(); ++levelIdx) {
    prog.levelIndex = levelIdx;
    const LevelCellRange& r = ranges[levelIdx];
    CHECK(r.level);

    for (int64_t z = r.cellMin[2]; z <= r.cellMax[2]; ++z) {
      for (int64_t y = r.cellMin[1]; y <= r.cellMax[1]; ++y) {
        for (int64_t x = r.cellMin[0]; x <= r.cellMax[0]; ++x) {
          if (shouldCancel()) {
            flush(/*force=*/true);
            return;
          }

          const std::array<uint64_t, 3> cell = {static_cast<uint64_t>(x),
                                                static_cast<uint64_t>(y),
                                                static_cast<uint64_t>(z)};

          auto bytesOpt = loadSpatialCellEntryBlocking(*r.level, cell);
          prog.visitedCells = safeAddU64(prog.visitedCells, 1);

          if (bytesOpt) {
            const auto anns =
              decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytesOpt->data(), bytesOpt->size()));
            for (const auto& a : anns) {
              if (!annotationIntersectsVoxelBox(a, qVoxel)) {
                continue;
              }
              if (!seen.insert(a.id).second) {
                continue;
              }
              prog.uniqueAnnotations = safeAddU64(prog.uniqueAnnotations, 1);
              batch.push_back(a);
              if (batch.size() >= maxAnnotationsPerUpdate) {
                flush(/*force=*/true);
              }
            }
          }

          flush(/*force=*/false);
        }
      }
    }
  }

  flush(/*force=*/true);
}

ZNeuroglancerPrecomputedAnnotationsSource::Annotation ZNeuroglancerPrecomputedAnnotationsSource::decodeAnnotationPayload(
  std::span<const uint8_t> bytes,
  size_t& off) const
{
  auto ensure = [&](size_t need, const char* what) {
    if (off + need > bytes.size()) {
      throw ZException(fmt::format("Invalid neuroglancer annotations encoding: truncated {}", what));
    }
  };

  Annotation a;
  a.type = m_annotationType;

  auto readVec3 = [&]() -> glm::vec3 {
    ensure(12, "position");
    const float x = readLE<float>(bytes.data() + off);
    const float y = readLE<float>(bytes.data() + off + 4);
    const float z = readLE<float>(bytes.data() + off + 8);
    off += 12;
    return glm::vec3(x, y, z);
  };

  switch (m_annotationType) {
  case AnnotationType::Point: {
    const glm::vec3 p = voxelFromCoordUnits(readVec3());
    a.points = {p};
    break;
  }
  case AnnotationType::Line: {
    const glm::vec3 a0 = voxelFromCoordUnits(readVec3());
    const glm::vec3 a1 = voxelFromCoordUnits(readVec3());
    a.points = {a0, a1};
    break;
  }
  case AnnotationType::AxisAlignedBoundingBox: {
    const glm::vec3 p0 = voxelFromCoordUnits(readVec3());
    const glm::vec3 p1 = voxelFromCoordUnits(readVec3());
    a.points = {p0, p1};
    break;
  }
  case AnnotationType::Ellipsoid: {
    const glm::vec3 center = voxelFromCoordUnits(readVec3());
    const glm::vec3 radiiCoord = readVec3();
    const double rxV = std::abs(static_cast<double>(radiiCoord.x)) * m_dimScaleNm[0] / m_baseResolutionNm[0];
    const double ryV = std::abs(static_cast<double>(radiiCoord.y)) * m_dimScaleNm[1] / m_baseResolutionNm[1];
    const double rzV = std::abs(static_cast<double>(radiiCoord.z)) * m_dimScaleNm[2] / m_baseResolutionNm[2];
    if (!std::isfinite(rxV) || !std::isfinite(ryV) || !std::isfinite(rzV)) {
      throw ZException("Invalid neuroglancer ellipsoid radii (non-finite)");
    }
    a.points = {center};
    a.ellipsoidRadiiVoxel = glm::vec3(static_cast<float>(rxV), static_cast<float>(ryV), static_cast<float>(rzV));
    break;
  }
  case AnnotationType::Polyline: {
    ensure(4, "polyline point count");
    const uint32_t n = readLE<uint32_t>(bytes.data() + off);
    off += 4;
    if (n == 0) {
      a.points.clear();
      break;
    }
    if (n > (std::numeric_limits<uint32_t>::max() / 3U)) {
      throw ZException("Invalid neuroglancer polyline: point count too large");
    }
    a.points.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      a.points[i] = voxelFromCoordUnits(readVec3());
    }
    break;
  }
  }

  // Properties are encoded by size group (see Neuroglancer annotations spec), and within each
  // size group they are encoded in the order of the "properties" array from the info JSON.
  a.propertyValues.clear();
  if (!m_properties.empty()) {
    a.propertyValues.resize(m_properties.size());
  }

  for (size_t i = 0; i < m_properties.size(); ++i) {
    const auto& p = m_properties[i];
    if (p.type != PropertyType::Uint32 && p.type != PropertyType::Int32 && p.type != PropertyType::Float32) {
      continue;
    }
    ensure(4, "property");
    switch (p.type) {
    case PropertyType::Uint32: {
      const uint32_t v = readLE<uint32_t>(bytes.data() + off);
      a.propertyValues[i] = v;
      break;
    }
    case PropertyType::Int32: {
      const int32_t v = readLE<int32_t>(bytes.data() + off);
      a.propertyValues[i] = v;
      break;
    }
    case PropertyType::Float32: {
      const float v = readLE<float>(bytes.data() + off);
      a.propertyValues[i] = v;
      break;
    }
    default:
      CHECK(false);
      break;
    }
    off += 4;
  }

  for (size_t i = 0; i < m_properties.size(); ++i) {
    const auto& p = m_properties[i];
    if (p.type != PropertyType::Uint16 && p.type != PropertyType::Int16) {
      continue;
    }
    ensure(2, "property");
    switch (p.type) {
    case PropertyType::Uint16: {
      const uint16_t v = readLE<uint16_t>(bytes.data() + off);
      a.propertyValues[i] = v;
      break;
    }
    case PropertyType::Int16: {
      const int16_t v = readLE<int16_t>(bytes.data() + off);
      a.propertyValues[i] = v;
      break;
    }
    default:
      CHECK(false);
      break;
    }
    off += 2;
  }

  for (size_t i = 0; i < m_properties.size(); ++i) {
    const auto& p = m_properties[i];
    switch (p.type) {
    case PropertyType::Rgb: {
      ensure(3, "rgb property");
      const std::array<uint8_t, 3> v{bytes[off + 0], bytes[off + 1], bytes[off + 2]};
      a.propertyValues[i] = v;
      if (!a.rgba8) {
        a.rgba8 = std::array<uint8_t, 4>{v[0], v[1], v[2], 255};
      }
      off += 3;
      break;
    }
    case PropertyType::Rgba: {
      ensure(4, "rgba property");
      const std::array<uint8_t, 4> v{bytes[off + 0], bytes[off + 1], bytes[off + 2], bytes[off + 3]};
      a.propertyValues[i] = v;
      if (!a.rgba8) {
        a.rgba8 = v;
      }
      off += 4;
      break;
    }
    case PropertyType::Uint8: {
      ensure(1, "uint8 property");
      a.propertyValues[i] = static_cast<uint8_t>(bytes[off]);
      off += 1;
      break;
    }
    case PropertyType::Int8: {
      ensure(1, "int8 property");
      a.propertyValues[i] = static_cast<int8_t>(bytes[off]);
      off += 1;
      break;
    }
    default:
      break; // handled in previous passes
    }
  }

  // Up to 3 padding bytes to align to 4.
  const size_t pad = (4 - (off % 4)) % 4;
  ensure(pad, "padding");
  off += pad;

  return a;
}

std::vector<ZNeuroglancerPrecomputedAnnotationsSource::Annotation>
ZNeuroglancerPrecomputedAnnotationsSource::decodeMultipleAnnotationBytes(std::span<const uint8_t> bytes) const
{
  if (bytes.empty()) {
    return {};
  }

  auto ensure = [&](size_t off, size_t need, const char* what) {
    if (off + need > bytes.size()) {
      throw ZException(fmt::format("Invalid neuroglancer annotations encoding: truncated {}", what));
    }
  };

  size_t off = 0;
  ensure(off, 8, "count");
  const uint64_t count = ZNeuroglancerUint64Sharding::readU64LE(bytes.data());
  off += 8;

  if (count == 0) {
    return {};
  }
  if (count > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 2)) {
    throw ZException("Invalid neuroglancer annotations encoding: count too large");
  }

  std::vector<Annotation> out;
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    Annotation a = decodeAnnotationPayload(bytes, off);
    out.push_back(std::move(a));
  }

  // Annotation IDs appended at end (one per annotation).
  ensure(off, static_cast<size_t>(count) * 8, "annotation id list");
  for (uint64_t i = 0; i < count; ++i) {
    out[static_cast<size_t>(i)].id = ZNeuroglancerUint64Sharding::readU64LE(bytes.data() + off);
    off += 8;
  }

  if (off != bytes.size()) {
    VLOG(2) << fmt::format("Neuroglancer annotations entry has {} trailing bytes", bytes.size() - off);
  }

  return out;
}

std::vector<ZNeuroglancerPrecomputedAnnotationsSource::Annotation>
ZNeuroglancerPrecomputedAnnotationsSource::loadAnnotationsForRelatedObjectBlocking(const QString& relationshipId,
                                                                                   uint64_t objectId) const
{
  const auto relOpt = findRelationship(relationshipId);
  if (!relOpt) {
    throw ZException(fmt::format("Unknown neuroglancer relationship id '{}'", toStdString(relationshipId)));
  }

  const RelationshipSpec rel = *relOpt;
  const std::vector<uint8_t> bytes = loadIndexEntryBlocking(rel.indexDirUrl, rel.sharding, objectId);
  return decodeMultipleAnnotationBytes(std::span<const uint8_t>(bytes.data(), bytes.size()));
}

} // namespace nim
