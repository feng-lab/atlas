#include "zcloudobjectstore.h"

#include "zcommandlineflags.h"
#include "zexception.h"
#include "zlog.h"

#include <folly/coro/BlockingWait.h>
#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nim {
namespace {

class ScopedEnvVar
{
public:
  ScopedEnvVar(const char* name, const char* value)
    : m_name(name)
    , m_hadPrevious(qEnvironmentVariableIsSet(name))
    , m_previous(qgetenv(name))
  {
    qputenv(m_name.constData(), value);
  }

  ~ScopedEnvVar()
  {
    if (m_hadPrevious) {
      qputenv(m_name.constData(), m_previous);
    } else {
      qunsetenv(m_name.constData());
    }
  }

private:
  QByteArray m_name;
  bool m_hadPrevious = false;
  QByteArray m_previous;
};

class ScopedFlag
{
public:
  ScopedFlag(std::string name, std::string value)
    : m_name(std::move(name))
  {
    CHECK(getCommandLineOption(m_name, &m_previous));
    std::string error;
    CHECK(setCommandLineOption(m_name, value, &error)) << error;
  }

  ~ScopedFlag()
  {
    std::string error;
    CHECK(setCommandLineOption(m_name, m_previous, &error)) << error;
  }

private:
  std::string m_name;
  std::string m_previous;
};

class ScopedAwsFileEnv
{
public:
  ScopedAwsFileEnv()
    : m_configPath(m_tmp.filePath(QStringLiteral("config")).toUtf8())
    , m_credentialsPath(m_tmp.filePath(QStringLiteral("credentials")).toUtf8())
    , m_configFile("AWS_CONFIG_FILE", m_configPath.constData())
    , m_credentialsFile("AWS_SHARED_CREDENTIALS_FILE", m_credentialsPath.constData())
  {
    CHECK(m_tmp.isValid());
  }

  [[nodiscard]] QString configPath() const
  {
    return QString::fromUtf8(m_configPath);
  }

  [[nodiscard]] QString credentialsPath() const
  {
    return QString::fromUtf8(m_credentialsPath);
  }

private:
  QTemporaryDir m_tmp;
  QByteArray m_configPath;
  QByteArray m_credentialsPath;
  ScopedEnvVar m_configFile;
  ScopedEnvVar m_credentialsFile;
};

class FakeRemoteObjectStore final : public ZRemoteObjectStore
{
public:
  [[nodiscard]] folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request) const override
  {
    std::optional<ZHttpGetBytesResult> next;
    {
      const std::scoped_lock lock(m_mutex);
      m_requests.push_back(std::move(request));
      if (m_responses.empty()) {
        throw ZException("FakeRemoteObjectStore called without a queued response");
      }
      next = std::move(m_responses.front());
      m_responses.pop_front();
    }
    co_return next;
  }

  void pushResponse(ZHttpGetBytesResult response) const
  {
    const std::scoped_lock lock(m_mutex);
    m_responses.emplace_back(std::move(response));
  }

  [[nodiscard]] std::vector<ZHttpGetRequest> requests() const
  {
    const std::scoped_lock lock(m_mutex);
    return m_requests;
  }

private:
  mutable std::mutex m_mutex;
  mutable std::deque<std::optional<ZHttpGetBytesResult>> m_responses;
  mutable std::vector<ZHttpGetRequest> m_requests;
};

ZHttpGetBytesResult okResponse()
{
  ZHttpGetBytesResult response;
  response.status = 200;
  response.body = std::vector<uint8_t>{'o', 'k'};
  response.encodedBodyBytes = response.body.size();
  response.source = ZHttpGetBytesSource::Network;
  return response;
}

std::optional<std::string> headerValue(const ZHttpGetRequest& request, std::string_view name)
{
  const auto equalCaseInsensitive = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char lhs, char rhs) {
             return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
           });
  };

  for (const auto& [key, value] : request.headers) {
    if (equalCaseInsensitive(key, name)) {
      return value;
    }
  }
  return std::nullopt;
}

void expectNoHeader(const ZHttpGetRequest& request, std::string_view name)
{
  EXPECT_FALSE(headerValue(request, name).has_value());
}

bool writeTextFile(const QString& path, const QByteArray& contents)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return false;
  }
  return file.write(contents) == contents.size();
}

} // namespace

TEST(ZCloudObjectStore, ParsesCloudObjectUrls)
{
  const auto gcs = parseCloudObjectUrl(QStringLiteral("precomputed://gs://bucket/path/to/object"));
  ASSERT_TRUE(gcs.has_value());
  EXPECT_EQ(gcs->provider, ZCloudObjectProvider::Gcs);
  EXPECT_EQ(gcs->bucket, QStringLiteral("bucket"));
  EXPECT_EQ(gcs->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(gcs->httpsUrl, QStringLiteral("https://storage.googleapis.com/bucket/path/to/object"));

  const auto s3 = parseCloudObjectUrl(QStringLiteral("s3://bucket/path/to/object"));
  ASSERT_TRUE(s3.has_value());
  EXPECT_EQ(s3->provider, ZCloudObjectProvider::S3);
  EXPECT_EQ(s3->bucket, QStringLiteral("bucket"));
  EXPECT_EQ(s3->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(s3->httpsUrl, QStringLiteral("https://bucket.s3.amazonaws.com/path/to/object"));
  EXPECT_FALSE(s3->preserveUrl);

  const auto dottedS3 = parseCloudObjectUrl(QStringLiteral("s3://bucket.with.dot/path/to/object"));
  ASSERT_TRUE(dottedS3.has_value());
  EXPECT_EQ(dottedS3->provider, ZCloudObjectProvider::S3);
  EXPECT_EQ(dottedS3->bucket, QStringLiteral("bucket.with.dot"));
  EXPECT_EQ(dottedS3->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(dottedS3->httpsUrl, QStringLiteral("https://s3.amazonaws.com/bucket.with.dot/path/to/object"));

  const auto gcsHttps = parseCloudObjectUrl(QStringLiteral("https://storage.googleapis.com/bucket/path/to/object"));
  ASSERT_TRUE(gcsHttps.has_value());
  EXPECT_EQ(gcsHttps->provider, ZCloudObjectProvider::Gcs);
  EXPECT_EQ(gcsHttps->bucket, QStringLiteral("bucket"));
  EXPECT_EQ(gcsHttps->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(gcsHttps->httpsUrl, QStringLiteral("https://storage.googleapis.com/bucket/path/to/object"));
  EXPECT_TRUE(gcsHttps->preserveUrl);

  const auto virtualGcsHttps =
    parseCloudObjectUrl(QStringLiteral("https://bucket.storage.googleapis.com/path/to/object?alt=media"));
  ASSERT_TRUE(virtualGcsHttps.has_value());
  EXPECT_EQ(virtualGcsHttps->provider, ZCloudObjectProvider::Gcs);
  EXPECT_EQ(virtualGcsHttps->bucket, QStringLiteral("bucket"));
  EXPECT_EQ(virtualGcsHttps->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(virtualGcsHttps->httpsUrl,
            QStringLiteral("https://bucket.storage.googleapis.com/path/to/object?alt=media"));
  EXPECT_TRUE(virtualGcsHttps->preserveUrl);

  const auto regionalS3Https =
    parseCloudObjectUrl(QStringLiteral("https://bucket.s3.us-west-2.amazonaws.com/path/to/object?versionId=abc"));
  ASSERT_TRUE(regionalS3Https.has_value());
  EXPECT_EQ(regionalS3Https->provider, ZCloudObjectProvider::S3);
  EXPECT_EQ(regionalS3Https->bucket, QStringLiteral("bucket"));
  EXPECT_EQ(regionalS3Https->key, QStringLiteral("path/to/object"));
  EXPECT_EQ(regionalS3Https->httpsUrl,
            QStringLiteral("https://bucket.s3.us-west-2.amazonaws.com/path/to/object?versionId=abc"));
  EXPECT_TRUE(regionalS3Https->preserveUrl);
}

TEST(ZCloudObjectStore, S3AnonymousLeavesRequestPublic)
{
  ScopedFlag s3Auth("atlas_s3_auth", "anonymous");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("s3://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("s3://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://bucket.s3.amazonaws.com/dataset/info");
  expectNoHeader(requests[0], "authorization");
  EXPECT_TRUE(requests[0].cachePartition.empty());
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat403And404AsMissing);
}

TEST(ZCloudObjectStore, S3CredentialsSignAndPartitionRequest)
{
  ScopedFlag s3Auth("atlas_s3_auth", "auto");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar region("AWS_REGION", "us-west-2");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "SESSIONTOKEN");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("s3://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("s3://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://bucket.s3.us-west-2.amazonaws.com/dataset/info");
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
  EXPECT_NE(requests[0].cachePartition.find("s3:bucket:us-west-2:AKIDEXAMPLE"), std::string::npos);

  const auto authorization = headerValue(requests[0], "authorization");
  ASSERT_TRUE(authorization.has_value());
  EXPECT_NE(authorization->find("AWS4-HMAC-SHA256"), std::string::npos);
  EXPECT_NE(authorization->find("Credential=AKIDEXAMPLE/"), std::string::npos);
  EXPECT_NE(authorization->find("SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token"),
            std::string::npos);
  EXPECT_TRUE(headerValue(requests[0], "x-amz-date").has_value());
  EXPECT_TRUE(headerValue(requests[0], "x-amz-content-sha256").has_value());
  EXPECT_EQ(headerValue(requests[0], "x-amz-security-token"), std::optional<std::string>("SESSIONTOKEN"));
}

TEST(ZCloudObjectStore, S3EnvEndpointUsesStandardAwsEndpointVariables)
{
  ScopedFlag s3Auth("atlas_s3_auth", "auto");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar region("AWS_REGION", "us-west-2");
  ScopedEnvVar globalEndpoint("AWS_ENDPOINT_URL", "https://ignored.example.org");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "https://s3.example.org");
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("s3://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("s3://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://s3.example.org/bucket/dataset/info");
  EXPECT_NE(requests[0].cachePartition.find("s3:bucket:us-west-2:AKIDEXAMPLE:https://s3.example.org"),
            std::string::npos);
  EXPECT_TRUE(headerValue(requests[0], "authorization").has_value());
}

TEST(ZCloudObjectStore, S3SignedHttpsPreservesOriginalRequestUrl)
{
  ScopedFlag s3Auth("atlas_s3_auth", "auto");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar region("AWS_REGION", "us-west-2");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store =
    makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("https://bucket.s3.us-west-2.amazonaws.com/dataset/"), fake);

  ZHttpGetRequest request;
  request.url = "https://bucket.s3.us-west-2.amazonaws.com/dataset/info?versionId=abc";
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://bucket.s3.us-west-2.amazonaws.com/dataset/info?versionId=abc");
  EXPECT_TRUE(headerValue(requests[0], "authorization").has_value());
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
}

TEST(ZCloudObjectStore, S3PresignedHttpsSkipsAtlasAuthorization)
{
  ScopedFlag s3Auth("atlas_s3_auth", "required");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar region("AWS_REGION", "");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store =
    makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("https://bucket.s3.us-west-2.amazonaws.com/dataset/"), fake);

  ZHttpGetRequest request;
  request.url =
    "https://bucket.s3.us-west-2.amazonaws.com/dataset/info?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=abc";
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(
    requests[0].url,
    "https://bucket.s3.us-west-2.amazonaws.com/dataset/info?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=abc");
  expectNoHeader(requests[0], "authorization");
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
  EXPECT_EQ(requests[0].cachePartition.find("s3:presigned:"), 0U);
}

TEST(ZCloudObjectStore, S3PresignedHttpsRootQueryAppliesToChildRequests)
{
  ScopedFlag s3Auth("atlas_s3_auth", "required");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar region("AWS_REGION", "");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(
    QStringLiteral(
      "https://bucket.s3.us-west-2.amazonaws.com/dataset/?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=abc"),
    fake);

  ZHttpGetRequest request;
  request.url = "https://bucket.s3.us-west-2.amazonaws.com/dataset/info";
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(
    requests[0].url,
    "https://bucket.s3.us-west-2.amazonaws.com/dataset/info?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=abc");
  expectNoHeader(requests[0], "authorization");
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
  EXPECT_EQ(requests[0].cachePartition.find("s3:presigned:"), 0U);
}

TEST(ZCloudObjectStore, S3AwsProfileConfigEndpointAndPathStyleDecorateRequest)
{
  ScopedFlag s3Auth("atlas_s3_auth", "auto");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar profile("AWS_PROFILE", "lab");
  ScopedEnvVar region("AWS_REGION", "");
  ScopedEnvVar defaultRegion("AWS_DEFAULT_REGION", "");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  ASSERT_TRUE(writeTextFile(awsFiles.credentialsPath(),
                            QByteArray("[lab]\n"
                                       "aws_access_key_id = AKIDPROFILE\n"
                                       "aws_secret_access_key = PROFILESECRET\n"
                                       "aws_session_token = PROFILETOKEN\n")));
  ASSERT_TRUE(writeTextFile(awsFiles.configPath(),
                            QByteArray("[profile lab]\n"
                                       "region = us-east-2\n"
                                       "services = lab-services\n"
                                       "s3 =\n"
                                       "  addressing_style = path\n"
                                       "[services lab-services]\n"
                                       "s3 =\n"
                                       "  endpoint_url = https://minio.example.org\n")));

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("s3://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("s3://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://minio.example.org/bucket/dataset/info");
  EXPECT_NE(requests[0].cachePartition.find("s3:bucket:us-east-2:AKIDPROFILE:https://minio.example.org"),
            std::string::npos);

  const auto authorization = headerValue(requests[0], "authorization");
  ASSERT_TRUE(authorization.has_value());
  EXPECT_NE(authorization->find("Credential=AKIDPROFILE/"), std::string::npos);
  EXPECT_EQ(headerValue(requests[0], "x-amz-security-token"), std::optional<std::string>("PROFILETOKEN"));
}

TEST(ZCloudObjectStore, S3EnvCredentialsTakePrecedenceOverProfile)
{
  ScopedFlag s3Auth("atlas_s3_auth", "auto");
  ScopedFlag s3Profile("atlas_s3_profile", "lab");
  ScopedAwsFileEnv awsFiles;
  ScopedEnvVar profile("AWS_PROFILE", "");
  ScopedEnvVar region("AWS_REGION", "us-west-2");
  ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "");
  ScopedEnvVar serviceEndpoint("AWS_ENDPOINT_URL_S3", "");
  ScopedEnvVar accessKey("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
  ScopedEnvVar secretKey("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  ScopedEnvVar sessionToken("AWS_SESSION_TOKEN", "");

  ASSERT_TRUE(writeTextFile(awsFiles.credentialsPath(),
                            QByteArray("[lab]\n"
                                       "aws_access_key_id = AKIDPROFILE\n"
                                       "aws_secret_access_key = PROFILESECRET\n")));
  ASSERT_TRUE(writeTextFile(awsFiles.configPath(),
                            QByteArray("[profile lab]\n"
                                       "region = us-east-2\n")));

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("s3://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("s3://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://bucket.s3.us-west-2.amazonaws.com/dataset/info");
  EXPECT_NE(requests[0].cachePartition.find("s3:bucket:us-west-2:AKIDEXAMPLE"), std::string::npos);

  const auto authorization = headerValue(requests[0], "authorization");
  ASSERT_TRUE(authorization.has_value());
  EXPECT_NE(authorization->find("Credential=AKIDEXAMPLE/"), std::string::npos);
}

TEST(ZCloudObjectStore, GcsBearerTokenFileDecoratesRequest)
{
  ScopedFlag gcsAuth("atlas_gcs_auth", "auto");

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString tokenPath = tmp.filePath(QStringLiteral("gcs-token.txt"));
  QFile tokenFile(tokenPath);
  ASSERT_TRUE(tokenFile.open(QIODevice::WriteOnly | QIODevice::Text));
  ASSERT_EQ(tokenFile.write("ya29.file-token\n"), 16);
  tokenFile.close();
  ScopedFlag tokenFileFlag("atlas_gcs_bearer_token_file", tokenPath.toStdString());

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("gs://bucket/dataset"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("gs://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://storage.googleapis.com/bucket/dataset/info");
  EXPECT_EQ(headerValue(requests[0], "authorization"), std::optional<std::string>("Bearer ya29.file-token"));
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
  EXPECT_EQ(requests[0].cachePartition.find("gcs:bucket:bearer:"), 0U);
}

TEST(ZCloudObjectStore, GcsSignedHttpsSkipsAtlasAuthorization)
{
  ScopedFlag gcsAuth("atlas_gcs_auth", "auto");

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString tokenPath = tmp.filePath(QStringLiteral("gcs-token.txt"));
  QFile tokenFile(tokenPath);
  ASSERT_TRUE(tokenFile.open(QIODevice::WriteOnly | QIODevice::Text));
  ASSERT_EQ(tokenFile.write("ya29.file-token\n"), 16);
  tokenFile.close();
  ScopedFlag tokenFileFlag("atlas_gcs_bearer_token_file", tokenPath.toStdString());

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store =
    makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("https://bucket.storage.googleapis.com/dataset/"), fake);

  ZHttpGetRequest request;
  request.url =
    "https://bucket.storage.googleapis.com/dataset/info?X-Goog-Algorithm=GOOG4-RSA-SHA256&X-Goog-Signature=abc";
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(
    requests[0].url,
    "https://bucket.storage.googleapis.com/dataset/info?X-Goog-Algorithm=GOOG4-RSA-SHA256&X-Goog-Signature=abc");
  expectNoHeader(requests[0], "authorization");
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
  EXPECT_EQ(requests[0].cachePartition.find("gcs:signed:"), 0U);
}

TEST(ZCloudObjectStore, RootTrailingSlashDecoratesChildRequest)
{
  ScopedFlag gcsAuth("atlas_gcs_auth", "auto");

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString tokenPath = tmp.filePath(QStringLiteral("gcs-token.txt"));
  QFile tokenFile(tokenPath);
  ASSERT_TRUE(tokenFile.open(QIODevice::WriteOnly | QIODevice::Text));
  ASSERT_EQ(tokenFile.write("ya29.file-token\n"), 16);
  tokenFile.close();
  ScopedFlag tokenFileFlag("atlas_gcs_bearer_token_file", tokenPath.toStdString());

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store =
    makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("https://storage.googleapis.com/bucket/dataset/"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("gs://bucket/dataset/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(headerValue(requests[0], "authorization"), std::optional<std::string>("Bearer ya29.file-token"));
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat404AsMissing);
}

TEST(ZCloudObjectStore, RequestOutsideRootDelegatesUndecorated)
{
  ScopedFlag gcsAuth("atlas_gcs_auth", "auto");

  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString tokenPath = tmp.filePath(QStringLiteral("gcs-token.txt"));
  QFile tokenFile(tokenPath);
  ASSERT_TRUE(tokenFile.open(QIODevice::WriteOnly | QIODevice::Text));
  ASSERT_EQ(tokenFile.write("ya29.file-token\n"), 16);
  tokenFile.close();
  ScopedFlag tokenFileFlag("atlas_gcs_bearer_token_file", tokenPath.toStdString());

  auto fake = std::make_shared<FakeRemoteObjectStore>();
  fake->pushResponse(okResponse());
  const auto store = makeCloudAwareRemoteObjectStoreForUrl(QStringLiteral("gs://bucket/a"), fake);

  ZHttpGetRequest request;
  request.url = cloudObjectUrlToHttps(QStringLiteral("gs://bucket/b/info")).toStdString();
  (void)folly::coro::blockingWait(store->getBytes(std::move(request)));

  const auto requests = fake->requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].url, "https://storage.googleapis.com/bucket/b/info");
  expectNoHeader(requests[0], "authorization");
  EXPECT_TRUE(requests[0].cachePartition.empty());
  EXPECT_EQ(requests[0].missingResourcePolicy, ZHttpMissingResourcePolicy::Treat403And404AsMissing);
}

} // namespace nim
