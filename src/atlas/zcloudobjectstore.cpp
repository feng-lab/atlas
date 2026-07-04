#include "zcloudobjectstore.h"

#include "zabslflagtypes.h"
#include "zexception.h"
#include "zlog.h"

#include "zcommandlineflags.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>

#include <folly/String.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nim {

enum class ZCloudAuthMode
{
  Auto,
  Anonymous,
  Required,
};

inline constexpr std::array<AbslEnumFlagValue<ZCloudAuthMode>, 3> kCloudAuthModeFlagValues{
  {
   {"auto", ZCloudAuthMode::Auto},
   {"anonymous", ZCloudAuthMode::Anonymous},
   {"required", ZCloudAuthMode::Required},
   }
};

inline bool AbslParseFlag(absl::string_view text, ZCloudAuthMode* value, std::string* error)
{
  return parseAbslEnumFlag(text, value, error, "ZCloudAuthMode", kCloudAuthModeFlagValues);
}

inline std::string AbslUnparseFlag(ZCloudAuthMode value)
{
  return unparseAbslEnumFlag(value, kCloudAuthModeFlagValues);
}

} // namespace nim

ABSL_FLAG(
  nim::ZCloudAuthMode,
  atlas_s3_auth,
  nim::ZCloudAuthMode::Auto,
  "S3 credential policy for cloud-backed remote datasets. auto signs requests when the AWS credential chain has credentials and otherwise reads anonymously; anonymous reads anonymously; required signs requests and fails if credentials are unavailable.");

ABSL_FLAG(
  std::string,
  atlas_s3_profile,
  "",
  "AWS profile name for S3 remote datasets. Empty uses AWS_PROFILE, then 'default'. AWS profile/config data supplies S3 region, endpoint URL, and addressing_style.");

ABSL_FLAG(
  nim::ZCloudAuthMode,
  atlas_gcs_auth,
  nim::ZCloudAuthMode::Auto,
  "GCS credential policy for cloud-backed remote datasets. auto uses atlas_gcs_bearer_token_file when configured and reads anonymously when no token file is configured; anonymous reads anonymously; required requires a readable token file.");

ABSL_FLAG(std::string,
          atlas_gcs_bearer_token_file,
          "",
          "Path to a file containing a GCS OAuth bearer token for private cloud-backed remote datasets.");

namespace nim {
namespace {

enum class S3AddressingStyle
{
  Auto,
  Path,
  Virtual,
};

struct S3Credentials
{
  std::string accessKeyId;
  std::string secretAccessKey;
  std::string sessionToken;
  std::string region;
  QString endpointUrl;
  S3AddressingStyle addressingStyle = S3AddressingStyle::Auto;
};

using IniSection = std::map<QString, QString>;
using IniFile = std::map<QString, IniSection>;

std::string toStdString(const QString& s)
{
  const auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

QString fromStdString(const std::string& s)
{
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString envQString(const char* name)
{
  return QString::fromUtf8(qgetenv(name)).trimmed();
}

QString stripPrecomputedScheme(QString url)
{
  url = url.trimmed();
  if (url.startsWith(QStringLiteral("precomputed://"), Qt::CaseInsensitive)) {
    url = url.mid(QStringLiteral("precomputed://").size()).trimmed();
  }
  return url;
}

std::pair<QString, QString> splitBucketAndKey(const QString& rest)
{
  const int slash = rest.indexOf('/');
  if (slash < 0) {
    return {rest, QString{}};
  }
  return {rest.left(slash), rest.mid(slash + 1)};
}

QString makeGcsHttpsUrl(const QString& bucket, const QString& key)
{
  QString out = QStringLiteral("https://storage.googleapis.com/") + bucket;
  if (!key.isEmpty()) {
    out += '/';
    out += key;
  }
  return out;
}

QString makeDefaultS3HttpsUrl(const QString& bucket, const QString& key)
{
  if (bucket.contains('.')) {
    QString out = QStringLiteral("https://s3.amazonaws.com/") + bucket;
    if (!key.isEmpty()) {
      out += '/';
      out += key;
    }
    return out;
  }

  QString out = QStringLiteral("https://") + bucket + QStringLiteral(".s3.amazonaws.com");
  if (!key.isEmpty()) {
    out += '/';
    out += key;
  }
  return out;
}

QString normalizedPathWithoutLeadingSlash(QUrl url)
{
  QString path = url.path(QUrl::FullyDecoded);
  while (path.startsWith('/')) {
    path = path.mid(1);
  }
  return path;
}

std::optional<ZCloudObjectUrl> parseGcsHttpsUrl(const QUrl& url, const QString& originalUrl)
{
  const QString host = url.host().toLower();
  QString bucket;
  QString key;

  if (host == QStringLiteral("storage.googleapis.com")) {
    const QString rest = normalizedPathWithoutLeadingSlash(url);
    auto [b, k] = splitBucketAndKey(rest);
    bucket = std::move(b);
    key = std::move(k);
  } else if (host.endsWith(QStringLiteral(".storage.googleapis.com"))) {
    bucket = host.left(host.size() - QStringLiteral(".storage.googleapis.com").size());
    key = normalizedPathWithoutLeadingSlash(url);
  } else {
    return std::nullopt;
  }

  if (bucket.isEmpty()) {
    return std::nullopt;
  }

  return ZCloudObjectUrl{
    .provider = ZCloudObjectProvider::Gcs,
    .originalUrl = originalUrl,
    .bucket = bucket,
    .key = key,
    .httpsUrl = originalUrl.trimmed(),
    .preserveUrl = true,
  };
}

std::optional<ZCloudObjectUrl> parseS3HttpsUrl(const QUrl& url, const QString& originalUrl)
{
  const QString host = url.host().toLower();
  QString bucket;
  QString key;

  const bool awsPathStyle =
    host == QStringLiteral("s3.amazonaws.com") ||
    (host.startsWith(QStringLiteral("s3.")) && host.endsWith(QStringLiteral(".amazonaws.com"))) ||
    (host.startsWith(QStringLiteral("s3-")) && host.endsWith(QStringLiteral(".amazonaws.com")));
  if (awsPathStyle) {
    const QString rest = normalizedPathWithoutLeadingSlash(url);
    auto [b, k] = splitBucketAndKey(rest);
    bucket = std::move(b);
    key = std::move(k);
  } else {
    int marker = host.indexOf(QStringLiteral(".s3."));
    if (marker < 0) {
      marker = host.indexOf(QStringLiteral(".s3-"));
    }
    if (marker >= 0 && host.endsWith(QStringLiteral(".amazonaws.com"))) {
      bucket = host.left(marker);
      key = normalizedPathWithoutLeadingSlash(url);
    } else if (host.endsWith(QStringLiteral(".s3.amazonaws.com"))) {
      bucket = host.left(host.size() - QStringLiteral(".s3.amazonaws.com").size());
      key = normalizedPathWithoutLeadingSlash(url);
    } else {
      return std::nullopt;
    }
  }

  if (bucket.isEmpty()) {
    return std::nullopt;
  }

  return ZCloudObjectUrl{
    .provider = ZCloudObjectProvider::S3,
    .originalUrl = originalUrl,
    .bucket = bucket,
    .key = key,
    .httpsUrl = originalUrl.trimmed(),
    .preserveUrl = true,
  };
}

std::optional<ZCloudObjectUrl> parseHttpsCloudObjectUrl(const QString& originalUrl)
{
  const QUrl url(originalUrl.trimmed());
  if (!url.isValid()) {
    return std::nullopt;
  }
  const QString scheme = url.scheme().toLower();
  if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
    return std::nullopt;
  }
  if (auto gcs = parseGcsHttpsUrl(url, originalUrl)) {
    return gcs;
  }
  if (auto s3 = parseS3HttpsUrl(url, originalUrl)) {
    return s3;
  }
  return std::nullopt;
}

QString trimTrailingSlash(QString value)
{
  while (value.endsWith('/') && value.size() > QStringLiteral("https://").size()) {
    value.chop(1);
  }
  return value;
}

bool isRequestInsideRoot(const ZCloudObjectUrl& root, const ZCloudObjectUrl& request)
{
  if (root.provider != request.provider || root.bucket != request.bucket) {
    return false;
  }
  QString rootKey = root.key.trimmed();
  while (rootKey.endsWith('/')) {
    rootKey.chop(1);
  }
  if (rootKey.isEmpty()) {
    return true;
  }
  return request.key == rootKey || request.key.startsWith(rootKey + QStringLiteral("/"));
}

std::string lowerAscii(std::string value)
{
  folly::toLowerAscii(value);
  return value;
}

std::string trimAscii(std::string_view s)
{
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return std::string(s.substr(start, end - start));
}

void upsertHeader(std::vector<std::pair<std::string, std::string>>* headers, std::string key, std::string value)
{
  CHECK(headers);
  const std::string keyLower = lowerAscii(key);
  for (auto& [existingKey, existingValue] : *headers) {
    if (lowerAscii(existingKey) == keyLower) {
      existingKey = std::move(key);
      existingValue = std::move(value);
      return;
    }
  }
  headers->emplace_back(std::move(key), std::move(value));
}

std::string sha256Hex(std::string_view data)
{
  CHECK(data.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
  const QByteArray bytes(data.data(), static_cast<int>(data.size()));
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

QByteArray hmacSha256(const QByteArray& key, std::string_view data)
{
  CHECK(data.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
  const QByteArray bytes(data.data(), static_cast<int>(data.size()));
  return QMessageAuthenticationCode::hash(bytes, key, QCryptographicHash::Sha256);
}

std::string hostHeaderForUrl(const QUrl& url)
{
  QString host = url.host().toLower();
  const int port = url.port(-1);
  if (port >= 0) {
    host += QStringLiteral(":") + QString::number(port);
  }
  return toStdString(host);
}

std::string canonicalUriForUrl(const QUrl& url)
{
  QString path = url.path(QUrl::FullyEncoded);
  if (path.isEmpty()) {
    path = QStringLiteral("/");
  }
  return toStdString(path);
}

std::string canonicalQueryForUrl(const QUrl& url)
{
  const QString query = url.query(QUrl::FullyEncoded);
  if (query.isEmpty()) {
    return {};
  }
  std::vector<std::string> parts;
  for (const QString& part : query.split('&', Qt::KeepEmptyParts)) {
    parts.emplace_back(toStdString(part));
  }
  std::sort(parts.begin(), parts.end());
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) {
      out.push_back('&');
    }
    out += part;
  }
  return out;
}

bool hasQueryItem(const QUrl& url, std::initializer_list<QStringView> names)
{
  const QUrlQuery query(url);
  const auto items = query.queryItems(QUrl::FullyDecoded);
  return std::any_of(items.begin(), items.end(), [names](const auto& item) {
    return std::any_of(names.begin(), names.end(), [&item](QStringView name) {
      return item.first.compare(name, Qt::CaseInsensitive) == 0;
    });
  });
}

bool isS3PresignedUrl(const std::string& urlString)
{
  const QUrl url(fromStdString(urlString));
  return hasQueryItem(url, {u"X-Amz-Signature", u"X-Amz-Credential", u"X-Amz-Algorithm"});
}

bool isGcsSignedUrl(const std::string& urlString)
{
  const QUrl url(fromStdString(urlString));
  if (hasQueryItem(url, {u"X-Goog-Signature", u"X-Goog-Credential", u"X-Goog-Algorithm"})) {
    return true;
  }
  return hasQueryItem(url, {u"GoogleAccessId"}) && hasQueryItem(url, {u"Expires"}) && hasQueryItem(url, {u"Signature"});
}

bool tryDecorateS3PresignedRequest(ZHttpGetRequest* request, const ZCloudObjectUrl& object)
{
  CHECK(request);
  if (!object.preserveUrl || !isS3PresignedUrl(request->url)) {
    return false;
  }

  request->cachePartition = fmt::format("s3:presigned:{}", sha256Hex(request->url));
  request->missingResourcePolicy = ZHttpMissingResourcePolicy::Treat404AsMissing;
  return true;
}

void applyPreservedRootQueryIfNeeded(ZHttpGetRequest* request, const ZCloudObjectUrl& rootObject)
{
  CHECK(request);
  if (!rootObject.preserveUrl) {
    return;
  }

  const QUrl rootUrl(rootObject.httpsUrl);
  const QString rootQuery = rootUrl.query(QUrl::FullyEncoded);
  if (rootQuery.isEmpty()) {
    return;
  }

  QUrl requestUrl(fromStdString(request->url));
  if (!requestUrl.isValid() || !requestUrl.query(QUrl::FullyEncoded).isEmpty()) {
    return;
  }

  requestUrl.setQuery(rootQuery);
  request->url = toStdString(requestUrl.toString(QUrl::FullyEncoded));
}

IniFile parseIniFile(const QString& path, bool awsConfigFile)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }

  IniFile out;
  QString currentSection;
  QString currentNestedPrefix;
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QString rawLine = stream.readLine();
    const QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) {
      continue;
    }
    if (line.startsWith('[') && line.endsWith(']')) {
      currentSection = line.mid(1, line.size() - 2).trimmed();
      if (awsConfigFile && currentSection.startsWith(QStringLiteral("profile "))) {
        currentSection = currentSection.mid(QStringLiteral("profile ").size()).trimmed();
      }
      currentNestedPrefix.clear();
      continue;
    }

    const int eq = line.indexOf('=');
    if (eq < 0 || currentSection.isEmpty()) {
      currentNestedPrefix.clear();
      continue;
    }
    const bool indented = !rawLine.isEmpty() && rawLine.at(0).isSpace();
    QString key = line.left(eq).trimmed().toLower();
    QString value = line.mid(eq + 1).trimmed();
    if (key.isEmpty()) {
      continue;
    }
    if (indented && !currentNestedPrefix.isEmpty()) {
      out[currentSection][currentNestedPrefix + QStringLiteral(".") + key] = value;
      continue;
    }
    currentNestedPrefix.clear();
    if (value.isEmpty()) {
      currentNestedPrefix = key;
      continue;
    }
    out[currentSection][key] = value;
  }
  return out;
}

QString iniValue(const IniFile& file, const QString& section, const QString& key)
{
  auto sectionIt = file.find(section);
  if (sectionIt == file.end()) {
    return {};
  }
  auto valueIt = sectionIt->second.find(key.toLower());
  if (valueIt == sectionIt->second.end()) {
    return {};
  }
  return valueIt->second.trimmed();
}

QString selectedS3ProfileName()
{
  QString profile = fromStdString(absl::GetFlag(FLAGS_atlas_s3_profile)).trimmed();
  if (!profile.isEmpty()) {
    return profile;
  }
  profile = envQString("AWS_PROFILE");
  if (!profile.isEmpty()) {
    return profile;
  }
  return QStringLiteral("default");
}

QString awsSharedCredentialsFilePath()
{
  const QString path = envQString("AWS_SHARED_CREDENTIALS_FILE");
  if (!path.isEmpty()) {
    return path;
  }
  return QDir::home().filePath(QStringLiteral(".aws/credentials"));
}

QString awsConfigFilePath()
{
  const QString path = envQString("AWS_CONFIG_FILE");
  if (!path.isEmpty()) {
    return path;
  }
  return QDir::home().filePath(QStringLiteral(".aws/config"));
}

QString s3RegionFromEnv()
{
  const QString awsRegion = envQString("AWS_REGION");
  if (!awsRegion.isEmpty()) {
    return awsRegion;
  }
  return envQString("AWS_DEFAULT_REGION");
}

QString s3EndpointUrlFromEnv()
{
  const QString serviceEndpoint = envQString("AWS_ENDPOINT_URL_S3");
  if (!serviceEndpoint.isEmpty()) {
    return serviceEndpoint;
  }
  return envQString("AWS_ENDPOINT_URL");
}

QString s3RegionFromProfile(const IniFile& configFile, const QString& profile)
{
  return iniValue(configFile, profile, QStringLiteral("region"));
}

S3AddressingStyle parseS3AddressingStyle(const QString& value, const QString& profile)
{
  const QString style = value.trimmed().toLower();
  if (style.isEmpty() || style == QStringLiteral("auto")) {
    return S3AddressingStyle::Auto;
  }
  if (style == QStringLiteral("path")) {
    return S3AddressingStyle::Path;
  }
  if (style == QStringLiteral("virtual")) {
    return S3AddressingStyle::Virtual;
  }
  throw ZException(fmt::format("Invalid S3 addressing_style '{}' in AWS profile '{}'. Expected auto, path, or virtual.",
                               toStdString(value),
                               toStdString(profile)));
}

QString s3EndpointUrlFromProfile(const IniFile& configFile, const QString& profile)
{
  const QString services = iniValue(configFile, profile, QStringLiteral("services"));
  if (!services.isEmpty()) {
    const QString serviceEndpoint =
      iniValue(configFile, QStringLiteral("services ") + services, QStringLiteral("s3.endpoint_url"));
    if (!serviceEndpoint.isEmpty()) {
      return serviceEndpoint;
    }
  }
  return iniValue(configFile, profile, QStringLiteral("endpoint_url"));
}

QString resolvedS3Region(const IniFile& configFile, const QString& profile)
{
  QString region = s3RegionFromEnv();
  if (region.isEmpty()) {
    region = s3RegionFromProfile(configFile, profile);
  }
  if (region.isEmpty()) {
    region = QStringLiteral("us-east-1");
  }
  return region;
}

QString resolvedS3EndpointUrl(const IniFile& configFile, const QString& profile)
{
  const QString endpointUrl = s3EndpointUrlFromEnv();
  if (!endpointUrl.isEmpty()) {
    return endpointUrl;
  }
  return s3EndpointUrlFromProfile(configFile, profile);
}

QString profileCredentialValue(const IniFile& credentialsFile,
                               const IniFile& configFile,
                               const QString& profile,
                               const QString& key)
{
  const QString credentialsValue = iniValue(credentialsFile, profile, key);
  if (!credentialsValue.isEmpty()) {
    return credentialsValue;
  }
  return iniValue(configFile, profile, key);
}

S3Credentials makeS3Credentials(QString accessKeyId,
                                QString secretAccessKey,
                                QString sessionToken,
                                const QString& region,
                                const QString& endpointUrl,
                                S3AddressingStyle addressingStyle)
{
  return S3Credentials{
    .accessKeyId = toStdString(accessKeyId),
    .secretAccessKey = toStdString(secretAccessKey),
    .sessionToken = toStdString(sessionToken),
    .region = toStdString(region),
    .endpointUrl = endpointUrl,
    .addressingStyle = addressingStyle,
  };
}

std::optional<S3Credentials>
resolveS3CredentialsFromEnv(const QString& region, const QString& endpointUrl, S3AddressingStyle addressingStyle)
{
  QString accessKeyId = envQString("AWS_ACCESS_KEY_ID");
  QString secretAccessKey = envQString("AWS_SECRET_ACCESS_KEY");
  if (accessKeyId.isEmpty() || secretAccessKey.isEmpty()) {
    return std::nullopt;
  }

  return makeS3Credentials(std::move(accessKeyId),
                           std::move(secretAccessKey),
                           envQString("AWS_SESSION_TOKEN"),
                           region,
                           endpointUrl,
                           addressingStyle);
}

std::optional<S3Credentials> resolveS3CredentialsFromProfile(const IniFile& credentialsFile,
                                                             const IniFile& configFile,
                                                             const QString& profile,
                                                             const QString& region,
                                                             const QString& endpointUrl,
                                                             S3AddressingStyle addressingStyle)
{
  QString accessKeyId =
    profileCredentialValue(credentialsFile, configFile, profile, QStringLiteral("aws_access_key_id"));
  QString secretAccessKey =
    profileCredentialValue(credentialsFile, configFile, profile, QStringLiteral("aws_secret_access_key"));
  if (accessKeyId.isEmpty() || secretAccessKey.isEmpty()) {
    return std::nullopt;
  }

  QString sessionToken =
    profileCredentialValue(credentialsFile, configFile, profile, QStringLiteral("aws_session_token"));
  return makeS3Credentials(std::move(accessKeyId),
                           std::move(secretAccessKey),
                           std::move(sessionToken),
                           region,
                           endpointUrl,
                           addressingStyle);
}

std::optional<S3Credentials> resolveS3Credentials()
{
  const ZCloudAuthMode authMode = absl::GetFlag(FLAGS_atlas_s3_auth);
  if (authMode == ZCloudAuthMode::Anonymous) {
    return std::nullopt;
  }

  const QString profile = selectedS3ProfileName();
  const IniFile credentialsFile = parseIniFile(awsSharedCredentialsFilePath(), /*awsConfigFile=*/false);
  const IniFile configFile = parseIniFile(awsConfigFilePath(), /*awsConfigFile=*/true);
  const QString region = resolvedS3Region(configFile, profile);
  const QString endpointUrl = resolvedS3EndpointUrl(configFile, profile);
  const S3AddressingStyle addressingStyle =
    parseS3AddressingStyle(iniValue(configFile, profile, QStringLiteral("s3.addressing_style")), profile);

  std::optional<S3Credentials> credentials = resolveS3CredentialsFromEnv(region, endpointUrl, addressingStyle);
  if (!credentials) {
    credentials =
      resolveS3CredentialsFromProfile(credentialsFile, configFile, profile, region, endpointUrl, addressingStyle);
  }

  if (credentials) {
    return credentials;
  }

  if (authMode == ZCloudAuthMode::Required) {
    throw ZException(
      fmt::format("S3 credentials are required. The AWS credential chain did not provide credentials for profile '{}'.",
                  toStdString(profile)));
  }
  return std::nullopt;
}

std::optional<std::string> readGcsBearerTokenFile()
{
  const QString tokenFilePath = fromStdString(absl::GetFlag(FLAGS_atlas_gcs_bearer_token_file)).trimmed();
  if (tokenFilePath.isEmpty()) {
    return std::nullopt;
  }

  QFile tokenFile(tokenFilePath);
  if (!tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZException(fmt::format("Failed to read GCS bearer token file '{}'", toStdString(tokenFilePath)));
  }

  const QString token = QString::fromUtf8(tokenFile.readAll()).trimmed();
  if (token.isEmpty()) {
    throw ZException(fmt::format("GCS bearer token file '{}' is empty", toStdString(tokenFilePath)));
  }
  return toStdString(token);
}

std::optional<std::string> resolveGcsBearerToken()
{
  const ZCloudAuthMode authMode = absl::GetFlag(FLAGS_atlas_gcs_auth);
  if (authMode == ZCloudAuthMode::Anonymous) {
    return std::nullopt;
  }

  const std::optional<std::string> token = readGcsBearerTokenFile();
  if (token) {
    return token;
  }

  if (authMode == ZCloudAuthMode::Required) {
    throw ZException("GCS credentials are required but atlas_gcs_bearer_token_file is empty.");
  }
  return std::nullopt;
}

QString s3EndpointBaseUrl(const S3Credentials& credentials)
{
  QString endpoint = credentials.endpointUrl.trimmed();
  if (!endpoint.isEmpty()) {
    if (!endpoint.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !endpoint.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
      endpoint = QStringLiteral("https://") + endpoint;
    }
    return trimTrailingSlash(endpoint);
  }

  if (credentials.region == "us-east-1") {
    return QStringLiteral("https://s3.amazonaws.com");
  }
  return QStringLiteral("https://s3.") + fromStdString(credentials.region) + QStringLiteral(".amazonaws.com");
}

bool shouldUseS3PathStyle(const ZCloudObjectUrl& object, const S3Credentials& credentials)
{
  switch (credentials.addressingStyle) {
    case S3AddressingStyle::Path:
      return true;
    case S3AddressingStyle::Virtual:
      return false;
    case S3AddressingStyle::Auto:
      return object.bucket.contains('.') || !credentials.endpointUrl.trimmed().isEmpty();
  }
  return false;
}

QString signedS3HttpsUrlForObject(const ZCloudObjectUrl& object, const S3Credentials& credentials)
{
  QUrl endpoint(s3EndpointBaseUrl(credentials));
  if (!endpoint.isValid() || endpoint.host().isEmpty()) {
    throw ZException(fmt::format("Invalid S3 endpoint URL '{}'", toStdString(endpoint.toString())));
  }

  QUrl out = endpoint;
  QString basePath = out.path(QUrl::FullyDecoded);
  if (basePath.endsWith('/')) {
    basePath.chop(1);
  }

  if (shouldUseS3PathStyle(object, credentials)) {
    out.setPath(basePath + QStringLiteral("/") + object.bucket + QStringLiteral("/") + object.key);
  } else {
    out.setHost(object.bucket + QStringLiteral(".") + endpoint.host());
    out.setPath(basePath + QStringLiteral("/") + object.key);
  }
  return out.toString(QUrl::FullyEncoded);
}

void signS3Request(ZHttpGetRequest* request,
                   const ZCloudObjectUrl& object,
                   const S3Credentials& credentials,
                   bool preserveRequestUrl)
{
  CHECK(request);
  if (tryDecorateS3PresignedRequest(request, object)) {
    return;
  }

  CHECK(!credentials.accessKeyId.empty());
  CHECK(!credentials.secretAccessKey.empty());
  CHECK(!credentials.region.empty());

  if (!preserveRequestUrl) {
    request->url = toStdString(signedS3HttpsUrlForObject(object, credentials));
  }
  request->cachePartition = fmt::format("s3:{}:{}:{}:{}",
                                        toStdString(object.bucket),
                                        credentials.region,
                                        credentials.accessKeyId,
                                        s3EndpointBaseUrl(credentials).toStdString());
  request->missingResourcePolicy = ZHttpMissingResourcePolicy::Treat404AsMissing;

  const QDateTime now = QDateTime::currentDateTimeUtc();
  const std::string dateStamp = toStdString(now.toString(QStringLiteral("yyyyMMdd")));
  const std::string amzDate = toStdString(now.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")));
  constexpr std::string_view kEmptyPayloadSha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

  QUrl url(fromStdString(request->url));
  if (!url.isValid() || url.host().isEmpty()) {
    throw ZException(fmt::format("Invalid S3 request URL '{}'", request->url));
  }

  upsertHeader(&request->headers, "x-amz-date", amzDate);
  upsertHeader(&request->headers, "x-amz-content-sha256", std::string(kEmptyPayloadSha256));
  if (!credentials.sessionToken.empty()) {
    upsertHeader(&request->headers, "x-amz-security-token", credentials.sessionToken);
  }

  std::vector<std::pair<std::string, std::string>> headersForSigning{
    {"host",                 hostHeaderForUrl(url)           },
    {"x-amz-content-sha256", std::string(kEmptyPayloadSha256)},
    {"x-amz-date",           amzDate                         },
  };
  if (!credentials.sessionToken.empty()) {
    headersForSigning.emplace_back("x-amz-security-token", credentials.sessionToken);
  }
  std::sort(headersForSigning.begin(), headersForSigning.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });

  std::string canonicalHeaders;
  std::string signedHeaders;
  for (const auto& [key, value] : headersForSigning) {
    canonicalHeaders += key;
    canonicalHeaders += ':';
    canonicalHeaders += trimAscii(value);
    canonicalHeaders += '\n';
    if (!signedHeaders.empty()) {
      signedHeaders += ';';
    }
    signedHeaders += key;
  }

  const std::string credentialScope = fmt::format("{}/{}/s3/aws4_request", dateStamp, credentials.region);
  const std::string canonicalRequest = fmt::format("GET\n{}\n{}\n{}\n{}\n{}",
                                                   canonicalUriForUrl(url),
                                                   canonicalQueryForUrl(url),
                                                   canonicalHeaders,
                                                   signedHeaders,
                                                   kEmptyPayloadSha256);
  const std::string stringToSign =
    fmt::format("AWS4-HMAC-SHA256\n{}\n{}\n{}", amzDate, credentialScope, sha256Hex(canonicalRequest));

  QByteArray signingKey = QByteArray("AWS4") + QByteArray(credentials.secretAccessKey.data(),
                                                          static_cast<int>(credentials.secretAccessKey.size()));
  signingKey = hmacSha256(signingKey, dateStamp);
  signingKey = hmacSha256(signingKey, credentials.region);
  signingKey = hmacSha256(signingKey, std::string_view("s3"));
  signingKey = hmacSha256(signingKey, std::string_view("aws4_request"));
  const std::string signature = hmacSha256(signingKey, stringToSign).toHex().toStdString();

  const std::string authorization = fmt::format("AWS4-HMAC-SHA256 Credential={}/{}, SignedHeaders={}, Signature={}",
                                                credentials.accessKeyId,
                                                credentialScope,
                                                signedHeaders,
                                                signature);
  upsertHeader(&request->headers, "authorization", authorization);
}

void decorateGcsRequest(ZHttpGetRequest* request, const ZCloudObjectUrl& object)
{
  CHECK(request);
  if (object.preserveUrl && isGcsSignedUrl(request->url)) {
    request->cachePartition = fmt::format("gcs:signed:{}", sha256Hex(request->url));
    request->missingResourcePolicy = ZHttpMissingResourcePolicy::Treat404AsMissing;
    return;
  }

  const std::optional<std::string> token = resolveGcsBearerToken();
  if (!token) {
    return;
  }

  request->cachePartition = fmt::format("gcs:{}:bearer:{}", toStdString(object.bucket), sha256Hex(*token));
  request->missingResourcePolicy = ZHttpMissingResourcePolicy::Treat404AsMissing;
  upsertHeader(&request->headers, "authorization", "Bearer " + *token);
}

class ZCloudRemoteObjectStore final : public ZRemoteObjectStore
{
public:
  ZCloudRemoteObjectStore(ZCloudObjectUrl rootObject, std::shared_ptr<const ZRemoteObjectStore> delegate)
    : m_rootObject(std::move(rootObject))
    , m_delegate(delegate ? std::move(delegate) : ZHttpRemoteObjectStore::sharedInstance())
  {
    CHECK(m_delegate);
    CHECK(m_rootObject.provider != ZCloudObjectProvider::None);
    CHECK(!m_rootObject.bucket.isEmpty());
  }

  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request) const override
  {
    auto objectOpt = parseCloudObjectUrl(QString::fromStdString(request.url));
    if (!objectOpt || !isRequestInsideRoot(m_rootObject, *objectOpt)) {
      co_return co_await m_delegate->getBytes(std::move(request));
    }

    applyPreservedRootQueryIfNeeded(&request, m_rootObject);

    switch (objectOpt->provider) {
      case ZCloudObjectProvider::Gcs:
        decorateGcsRequest(&request, *objectOpt);
        break;
      case ZCloudObjectProvider::S3:
        if (tryDecorateS3PresignedRequest(&request, *objectOpt)) {
          break;
        }
        if (const auto credentials = resolveS3Credentials()) {
          signS3Request(&request, *objectOpt, *credentials, m_rootObject.preserveUrl);
        }
        break;
      case ZCloudObjectProvider::None:
        break;
    }

    co_return co_await m_delegate->getBytes(std::move(request));
  }

  [[nodiscard]] const void* contentCacheScopeToken() const override
  {
    return this;
  }

private:
  ZCloudObjectUrl m_rootObject;
  std::shared_ptr<const ZRemoteObjectStore> m_delegate;
};

} // namespace

std::optional<ZCloudObjectUrl> parseCloudObjectUrl(QString url)
{
  url = url.trimmed();
  if (url.isEmpty()) {
    return std::nullopt;
  }

  const QString originalUrl = url;
  url = stripPrecomputedScheme(std::move(url));

  if (url.startsWith(QStringLiteral("gs://"), Qt::CaseInsensitive)) {
    const QString rest = url.mid(QStringLiteral("gs://").size());
    auto [bucket, key] = splitBucketAndKey(rest);
    bucket = bucket.trimmed();
    if (bucket.isEmpty()) {
      return std::nullopt;
    }
    return ZCloudObjectUrl{
      .provider = ZCloudObjectProvider::Gcs,
      .originalUrl = originalUrl,
      .bucket = bucket,
      .key = key,
      .httpsUrl = makeGcsHttpsUrl(bucket, key),
    };
  }

  if (url.startsWith(QStringLiteral("s3://"), Qt::CaseInsensitive)) {
    const QString rest = url.mid(QStringLiteral("s3://").size());
    auto [bucket, key] = splitBucketAndKey(rest);
    bucket = bucket.trimmed();
    if (bucket.isEmpty()) {
      return std::nullopt;
    }
    return ZCloudObjectUrl{
      .provider = ZCloudObjectProvider::S3,
      .originalUrl = originalUrl,
      .bucket = bucket,
      .key = key,
      .httpsUrl = makeDefaultS3HttpsUrl(bucket, key),
    };
  }

  return parseHttpsCloudObjectUrl(url);
}

QString cloudObjectUrlToHttps(QString url)
{
  if (auto parsed = parseCloudObjectUrl(url)) {
    return parsed->httpsUrl;
  }
  return stripPrecomputedScheme(std::move(url));
}

std::shared_ptr<const ZRemoteObjectStore>
makeCloudAwareRemoteObjectStoreForUrl(QString url, std::shared_ptr<const ZRemoteObjectStore> delegate)
{
  auto parsed = parseCloudObjectUrl(std::move(url));
  if (!parsed) {
    return delegate ? std::move(delegate) : ZRemoteObjectStore::sharedDefaultStore();
  }
  return std::make_shared<ZCloudRemoteObjectStore>(std::move(*parsed), std::move(delegate));
}

} // namespace nim
