#include "zhttpdiskcache.h"

#include "zdiskcacheutils.h"
#include "zproxygenhttpclient.h"
#include "zlog.h"
#include "zsqlitelrucache.h"
#include "zstructutils.h"

#include <QDir>
#include <QFile>
#include <QLockFile>

#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace nim {

namespace {

constexpr uint32_t kCacheVersion = 1;

constexpr char kMagic[8] = {'A', 'T', 'L', 'S', 'H', 'D', 'C', '1'};

struct HttpCacheEntryHeader
{
  std::array<char, 8> magic{};
  uint32_t version = 0;
  uint16_t status = 0;
  uint32_t contentTypeLen = 0;
  uint32_t contentEncLen = 0;
  uint64_t bodyLen = 0;
};

[[nodiscard]] bool isSafeAsciiLower(std::string_view s)
{
  for (char c : s) {
    if (c < 'a' || c > 'z') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string lowerAscii(std::string_view in)
{
  std::string out(in);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

[[nodiscard]] std::optional<std::string> findHeaderValueLowerKey(
  const std::vector<std::pair<std::string, std::string>>& headers,
  std::string_view keyLowerAscii)
{
  CHECK(isSafeAsciiLower(keyLowerAscii));
  for (const auto& [k, v] : headers) {
    std::string kLower = lowerAscii(k);
    if (kLower == keyLowerAscii) {
      return v;
    }
  }
  return std::nullopt;
}

} // namespace

ZHttpDiskCache::ZHttpDiskCache(QString rootDir, uint64_t maxBytes)
  : m_rootDir(std::move(rootDir))
  , m_maxBytes(maxBytes)
{
  m_rootDir = m_rootDir.trimmed();
  if (m_rootDir.isEmpty() || m_maxBytes == 0) {
    return;
  }

  m_cacheDir = atlasDiskCacheDirFromRoot(m_rootDir);
  m_dbPath = QDir(m_cacheDir).filePath(QStringLiteral("http.sqlite"));
  m_lockPath = QDir(m_cacheDir).filePath(QStringLiteral("http.lock"));

  // Ensure base directories exist.
  {
    QDir dir;
    if (!dir.mkpath(m_cacheDir)) {
      LOG(WARNING) << "HTTP disk cache disabled: failed to create directory " << m_cacheDir;
      return;
    }
  }

  // Single-writer guard across processes.
  m_lock = std::make_unique<QLockFile>(m_lockPath);
  m_lock->setStaleLockTime(static_cast<int>(std::chrono::milliseconds(std::chrono::seconds(10)).count()));
  if (!m_lock->tryLock(/*timeout=*/0)) {
    LOG(INFO) << "HTTP disk cache disabled: could not acquire lock at " << m_lockPath;
    m_lock.reset();
    return;
  }

  m_cache = std::make_unique<ZSqliteLRUCache>(m_dbPath, m_maxBytes);
  if (!m_cache->isOpen()) {
    LOG(WARNING) << "HTTP disk cache disabled: failed to open SQLite DB at " << m_dbPath;
    m_cache.reset();
    m_lock.reset();
    return;
  }

  m_enabled = true;
}

ZHttpDiskCache::~ZHttpDiskCache() = default;

ZHttpDiskCache::KeyParts ZHttpDiskCache::keyPartsFrom(
  const std::string& url,
  const std::vector<std::pair<std::string, std::string>>& requestHeaders)
{
  KeyParts out;
  out.url = url;
  if (auto rangeOpt = findHeaderValueLowerKey(requestHeaders, "range")) {
    out.range = *rangeOpt;
  }
  return out;
}

ZHttpDiskCache::Blob ZHttpDiskCache::sha256(const void* data, size_t bytes)
{
  boost::hash2::sha2_256 hash;
  if (bytes > 0) {
    CHECK(data != nullptr);
    hash.update(data, bytes);
  }
  const boost::hash2::sha2_256::result_type digest = hash.result();
  Blob out;
  out.resize(digest.size());
  CHECK(out.size() == digest.size());
  std::memcpy(out.data(), digest.data(), digest.size());
  return out;
}

ZHttpDiskCache::Blob ZHttpDiskCache::serializeEntry(const ZHttpGetBytesResult& result)
{
  CHECK(std::endian::native == std::endian::little);

  if (result.status < 0 || result.status > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  if (result.contentType.size() > std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  if (result.contentEncoding.size() > std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  if (result.body.size() > std::numeric_limits<uint64_t>::max()) {
    return {};
  }

  HttpCacheEntryHeader hdr{};
  std::memcpy(hdr.magic.data(), kMagic, sizeof(kMagic));
  hdr.version = kCacheVersion;
  hdr.status = static_cast<uint16_t>(result.status);
  hdr.contentTypeLen = static_cast<uint32_t>(result.contentType.size());
  hdr.contentEncLen = static_cast<uint32_t>(result.contentEncoding.size());
  hdr.bodyLen = static_cast<uint64_t>(result.body.size());

  const size_t headerBytes = compactSize(hdr);
  if (hdr.bodyLen > std::numeric_limits<size_t>::max()) {
    return {};
  }
  const size_t bodyBytes = static_cast<size_t>(hdr.bodyLen);
  size_t totalBytes = headerBytes;
  if (hdr.contentTypeLen > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += static_cast<size_t>(hdr.contentTypeLen);
  if (hdr.contentEncLen > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += static_cast<size_t>(hdr.contentEncLen);
  if (bodyBytes > std::numeric_limits<size_t>::max() - totalBytes) {
    return {};
  }
  totalBytes += bodyBytes;

  Blob out;
  out.resize(totalBytes);
  CHECK(compactStructToMemory(out.data(), headerBytes, hdr) == headerBytes);

  size_t offset = headerBytes;
  if (hdr.contentTypeLen > 0) {
    const char* p = result.contentType.data();
    std::memcpy(out.data() + offset, p, hdr.contentTypeLen);
    offset += hdr.contentTypeLen;
  }
  if (hdr.contentEncLen > 0) {
    const char* p = result.contentEncoding.data();
    std::memcpy(out.data() + offset, p, hdr.contentEncLen);
    offset += hdr.contentEncLen;
  }
  if (hdr.bodyLen > 0) {
    std::memcpy(out.data() + offset, result.body.data(), static_cast<size_t>(hdr.bodyLen));
    offset += static_cast<size_t>(hdr.bodyLen);
  }
  CHECK(offset == totalBytes);

  return out;
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::tryGet(
  const std::string& url,
  const std::vector<std::pair<std::string, std::string>>& requestHeaders)
{
  std::lock_guard<std::mutex> g(m_mu);
  if (!m_enabled || !m_cache) {
    return std::nullopt;
  }
  if (!m_cache->isOpen()) {
    // Best-effort: persistent DB failures should not impact callers.
    m_cache.reset();
    m_lock.reset();
    m_enabled = false;
    return std::nullopt;
  }

  const KeyParts parts = keyPartsFrom(url, requestHeaders);

  std::string keyBytes;
  keyBytes.reserve(16 + parts.url.size() + parts.range.size());
  keyBytes.append("GET\n");
  keyBytes.append(parts.url);
  keyBytes.push_back('\n');
  keyBytes.append("range=");
  keyBytes.append(parts.range);
  keyBytes.push_back('\n');

  const Blob keyHash = sha256(keyBytes.data(), keyBytes.size());
  auto valueOpt = m_cache->tryGet(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
  if (!valueOpt.has_value()) {
    return std::nullopt;
  }

  auto resOpt = parseEntry(std::span<const std::uint8_t>(valueOpt->data(), valueOpt->size()));
  if (!resOpt.has_value()) {
    // Corrupt entry; best-effort cleanup.
    m_cache->erase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
    return std::nullopt;
  }

  return resOpt;
}

void ZHttpDiskCache::put(const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& requestHeaders,
                         const ZHttpGetBytesResult& result)
{
  std::lock_guard<std::mutex> g(m_mu);
  if (!m_enabled || !m_cache) {
    return;
  }
  if (!m_cache->isOpen()) {
    m_cache.reset();
    m_lock.reset();
    m_enabled = false;
    return;
  }
  if (result.status != 200 && result.status != 206) {
    return;
  }
  if (static_cast<uint64_t>(result.body.size()) > m_maxBytes) {
    // An entry larger than the entire budget would cause immediate thrash; skip caching.
    return;
  }

  const KeyParts parts = keyPartsFrom(url, requestHeaders);
  std::string keyBytes;
  keyBytes.reserve(16 + parts.url.size() + parts.range.size());
  keyBytes.append("GET\n");
  keyBytes.append(parts.url);
  keyBytes.push_back('\n');
  keyBytes.append("range=");
  keyBytes.append(parts.range);
  keyBytes.push_back('\n');

  const Blob keyHash = sha256(keyBytes.data(), keyBytes.size());

  const Blob value = serializeEntry(result);
  if (value.empty()) {
    return;
  }
  if (static_cast<uint64_t>(value.size()) > m_maxBytes) {
    // Guard against extreme header sizes; treat as uncacheable.
    return;
  }
  m_cache->put(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
               std::span<const std::uint8_t>(value.data(), value.size()));
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::parseEntry(std::span<const std::uint8_t> value)
{
  CHECK(std::endian::native == std::endian::little);

  static const size_t headerBytes = compactSize(HttpCacheEntryHeader{});
  if (value.size() < headerBytes) {
    return std::nullopt;
  }

  HttpCacheEntryHeader hdr{};
  readStructFromCompactMemory(hdr, value.data(), headerBytes);

  if (std::memcmp(hdr.magic.data(), kMagic, sizeof(kMagic)) != 0) {
    return std::nullopt;
  }
  if (hdr.version != kCacheVersion) {
    return std::nullopt;
  }

  size_t offset = headerBytes;
  if (hdr.contentTypeLen > value.size() - offset) {
    return std::nullopt;
  }
  const std::string_view contentTypeView(reinterpret_cast<const char*>(value.data() + offset), hdr.contentTypeLen);
  offset += hdr.contentTypeLen;
  if (hdr.contentEncLen > value.size() - offset) {
    return std::nullopt;
  }
  const std::string_view contentEncView(reinterpret_cast<const char*>(value.data() + offset), hdr.contentEncLen);
  offset += hdr.contentEncLen;

  if (hdr.bodyLen > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  const size_t bodyLen = static_cast<size_t>(hdr.bodyLen);
  if (bodyLen > value.size() - offset) {
    return std::nullopt;
  }

  std::vector<uint8_t> body;
  body.resize(bodyLen);
  if (!body.empty()) {
    std::memcpy(body.data(), value.data() + offset, body.size());
  }

  ZHttpGetBytesResult out{};
  out.status = hdr.status;
  out.contentType.assign(contentTypeView.data(), contentTypeView.size());
  out.contentEncoding.assign(contentEncView.data(), contentEncView.size());
  out.body = std::move(body);
  return out;
}

} // namespace nim
