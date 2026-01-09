#include "zhttpdiskcache.h"

#include "zproxygenhttpclient.h"
#include "zlog.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>

#include <algorithm>
#include <cstring>

namespace nim {

namespace {

constexpr uint32_t kCacheVersion = 1;

constexpr char kMagic[8] = {'A', 'T', 'L', 'S', 'H', 'D', 'C', '1'};

constexpr std::chrono::seconds kPruneMinInterval{30};

[[nodiscard]] QByteArray toQByteArray(const std::string& s)
{
  return QByteArray(s.data(), static_cast<int>(s.size()));
}

[[nodiscard]] std::string toStdStringUtf8(const QString& s)
{
  const QByteArray u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

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

  m_entriesDir = QDir(m_rootDir).filePath(QStringLiteral("atlas_http_disk_cache_v1/entries"));
  m_lockPath = QDir(m_rootDir).filePath(QStringLiteral("atlas_http_disk_cache_v1/lock"));

  // Ensure base directories exist.
  {
    QDir dir;
    if (!dir.mkpath(m_entriesDir)) {
      LOG(WARNING) << "HTTP disk cache disabled: failed to create directory "
                   << toStdStringUtf8(m_entriesDir);
      return;
    }
  }

  // Single-writer guard across processes.
  m_lock = std::make_unique<QLockFile>(m_lockPath);
  m_lock->setStaleLockTime(static_cast<int>(std::chrono::milliseconds(std::chrono::seconds(10)).count()));
  if (!m_lock->tryLock(/*timeout=*/0)) {
    LOG(INFO) << "HTTP disk cache disabled: could not acquire lock at " << toStdStringUtf8(m_lockPath);
    m_lock.reset();
    return;
  }

  m_enabled = true;
}

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

QByteArray ZHttpDiskCache::sha256Hex(const QByteArray& bytes)
{
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QString ZHttpDiskCache::entryPathForKeyHash(const QByteArray& hexHash) const
{
  CHECK(hexHash.size() >= 2);
  const QString subdir = QString::fromLatin1(hexHash.left(2));
  const QString filename = QString::fromLatin1(hexHash) + QStringLiteral(".bin");
  return QDir(m_entriesDir).filePath(QDir(subdir).filePath(filename));
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::tryGet(
  const std::string& url,
  const std::vector<std::pair<std::string, std::string>>& requestHeaders)
{
  std::lock_guard<std::mutex> g(m_mu);
  if (!m_enabled) {
    return std::nullopt;
  }

  const KeyParts parts = keyPartsFrom(url, requestHeaders);

  QByteArray keyBytes;
  keyBytes.append("GET\n", 4);
  keyBytes.append(toQByteArray(parts.url));
  keyBytes.append('\n');
  keyBytes.append("range=", 6);
  keyBytes.append(toQByteArray(parts.range));
  keyBytes.append('\n');

  const QByteArray keyHash = sha256Hex(keyBytes);
  const QString path = entryPathForKeyHash(keyHash);

  QFileInfo fi(path);
  if (!fi.exists() || !fi.isFile()) {
    return std::nullopt;
  }

  auto resOpt = readEntryFile(path);
  if (!resOpt) {
    // Corrupt entry; best-effort cleanup.
    QFile::remove(path);
    return std::nullopt;
  }

  // Best-effort LRU: touch mtime so prune keeps recently used entries.
  {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      (void)f.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    }
  }

  return resOpt;
}

void ZHttpDiskCache::put(const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& requestHeaders,
                         const ZHttpGetBytesResult& result)
{
  std::lock_guard<std::mutex> g(m_mu);
  if (!m_enabled) {
    return;
  }
  if (result.status != 200 && result.status != 206) {
    return;
  }

  if (!m_sizeKnown) {
    updateTotalBytesFromDisk();
  }

  const KeyParts parts = keyPartsFrom(url, requestHeaders);
  QByteArray keyBytes;
  keyBytes.append("GET\n", 4);
  keyBytes.append(toQByteArray(parts.url));
  keyBytes.append('\n');
  keyBytes.append("range=", 6);
  keyBytes.append(toQByteArray(parts.range));
  keyBytes.append('\n');
  const QByteArray keyHash = sha256Hex(keyBytes);
  const QString path = entryPathForKeyHash(keyHash);

  const QFileInfo before(path);
  const uint64_t beforeSize = (before.exists() && before.isFile()) ? static_cast<uint64_t>(before.size()) : 0;

  // Ensure subdir exists.
  {
    const QDir dir = QFileInfo(path).dir();
    if (!dir.exists()) {
      QDir mk;
      if (!mk.mkpath(dir.absolutePath())) {
        return;
      }
    }
  }

  try {
    writeEntryFile(path, result);
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "HTTP disk cache write failed: " << e.what();
    return;
  }

  const QFileInfo after(path);
  if (after.exists() && after.isFile()) {
    const uint64_t afterSize = static_cast<uint64_t>(after.size());
    if (afterSize >= beforeSize) {
      m_totalBytes += (afterSize - beforeSize);
    } else {
      m_totalBytes -= (beforeSize - afterSize);
    }
  }

  maybePrune();
}

std::optional<ZHttpGetBytesResult> ZHttpDiskCache::readEntryFile(const QString& path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  QDataStream ds(&f);
  ds.setByteOrder(QDataStream::LittleEndian);

  char magic[sizeof(kMagic)]{};
  if (ds.readRawData(magic, static_cast<int>(sizeof(magic))) != static_cast<int>(sizeof(magic))) {
    return std::nullopt;
  }
  if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    return std::nullopt;
  }

  uint32_t version = 0;
  ds >> version;
  if (version != kCacheVersion) {
    return std::nullopt;
  }

  uint16_t status = 0;
  ds >> status;

  uint32_t contentTypeLen = 0;
  uint32_t contentEncLen = 0;
  uint64_t bodyLen = 0;
  ds >> contentTypeLen;
  ds >> contentEncLen;
  ds >> bodyLen;

  if (contentTypeLen > (1u << 20) || contentEncLen > (1u << 20)) {
    return std::nullopt;
  }

  const qint64 remaining = f.size() - f.pos();
  const qint64 need = static_cast<qint64>(contentTypeLen) + static_cast<qint64>(contentEncLen) +
                      static_cast<qint64>(bodyLen);
  if (need < 0 || remaining < need) {
    return std::nullopt;
  }

  QByteArray contentTypeBytes;
  contentTypeBytes.resize(static_cast<int>(contentTypeLen));
  if (contentTypeLen > 0) {
    if (ds.readRawData(contentTypeBytes.data(), static_cast<int>(contentTypeLen)) != static_cast<int>(contentTypeLen)) {
      return std::nullopt;
    }
  }

  QByteArray contentEncBytes;
  contentEncBytes.resize(static_cast<int>(contentEncLen));
  if (contentEncLen > 0) {
    if (ds.readRawData(contentEncBytes.data(), static_cast<int>(contentEncLen)) != static_cast<int>(contentEncLen)) {
      return std::nullopt;
    }
  }

  std::vector<uint8_t> body;
  body.resize(static_cast<size_t>(bodyLen));
  if (bodyLen > 0) {
    if (ds.readRawData(reinterpret_cast<char*>(body.data()), static_cast<int>(bodyLen)) != static_cast<int>(bodyLen)) {
      return std::nullopt;
    }
  }

  ZHttpGetBytesResult out{};
  out.status = status;
  out.contentType.assign(contentTypeBytes.data(), static_cast<size_t>(contentTypeBytes.size()));
  out.contentEncoding.assign(contentEncBytes.data(), static_cast<size_t>(contentEncBytes.size()));
  out.body = std::move(body);
  return out;
}

void ZHttpDiskCache::writeEntryFile(const QString& path, const ZHttpGetBytesResult& result)
{
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    throw ZException("Failed to open cache entry for write");
  }
  QDataStream ds(&f);
  ds.setByteOrder(QDataStream::LittleEndian);

  ds.writeRawData(kMagic, static_cast<int>(sizeof(kMagic)));
  ds << kCacheVersion;
  ds << static_cast<uint16_t>(result.status);

  const QByteArray contentType = QByteArray(result.contentType.data(), static_cast<int>(result.contentType.size()));
  const QByteArray contentEnc = QByteArray(result.contentEncoding.data(), static_cast<int>(result.contentEncoding.size()));

  ds << static_cast<uint32_t>(contentType.size());
  ds << static_cast<uint32_t>(contentEnc.size());
  ds << static_cast<uint64_t>(result.body.size());

  if (!contentType.isEmpty()) {
    ds.writeRawData(contentType.data(), contentType.size());
  }
  if (!contentEnc.isEmpty()) {
    ds.writeRawData(contentEnc.data(), contentEnc.size());
  }
  if (!result.body.empty()) {
    ds.writeRawData(reinterpret_cast<const char*>(result.body.data()), static_cast<int>(result.body.size()));
  }

  if (!f.commit()) {
    throw ZException("Failed to commit cache entry");
  }
}

void ZHttpDiskCache::updateTotalBytesFromDisk()
{
  uint64_t total = 0;
  QDirIterator it(m_entriesDir, QStringList() << "*.bin", QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    total += static_cast<uint64_t>(it.fileInfo().size());
  }
  m_totalBytes = total;
  m_sizeKnown = true;
}

void ZHttpDiskCache::maybePrune()
{
  if (m_maxBytes == 0 || m_totalBytes <= m_maxBytes) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_lastPrune.time_since_epoch() != std::chrono::steady_clock::duration::zero() &&
      (now - m_lastPrune) < kPruneMinInterval) {
    return;
  }
  m_lastPrune = now;

  struct Entry
  {
    QString path;
    uint64_t size = 0;
    QDateTime mtime;
  };

  std::vector<Entry> entries;
  entries.reserve(1024);
  QDirIterator it(m_entriesDir, QStringList() << "*.bin", QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    const QFileInfo fi = it.fileInfo();
    Entry e;
    e.path = fi.absoluteFilePath();
    e.size = static_cast<uint64_t>(fi.size());
    e.mtime = fi.lastModified();
    entries.push_back(std::move(e));
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.mtime < b.mtime;
  });

  const uint64_t target = static_cast<uint64_t>(static_cast<double>(m_maxBytes) * 0.9);
  uint64_t bytes = m_totalBytes;
  size_t removed = 0;
  for (const auto& e : entries) {
    if (bytes <= target) {
      break;
    }
    if (QFile::remove(e.path)) {
      bytes -= std::min<uint64_t>(bytes, e.size);
      ++removed;
    }
  }
  m_totalBytes = bytes;
  if (removed > 0) {
    VLOG(1) << "HTTP disk cache pruned " << removed << " entries; totalBytes=" << m_totalBytes << " maxBytes=" << m_maxBytes;
  }
}

} // namespace nim
