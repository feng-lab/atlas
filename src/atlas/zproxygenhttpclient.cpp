#include "zproxygenhttpclient.h"

#include "zdiskcacheutils.h"
#include "zhttpdiskcache.h"
#include "zcancellation.h"

#include <folly/String.h>
#include <folly/compression/Compression.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/OperationCancelled.h>
#include <folly/coro/Sleep.h>
#include <proxygen/lib/http/coro/client/HTTPCoroConnector.h>
#include <proxygen/lib/http/HTTPHeaders.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/coro/HTTPError.h>
#include <proxygen/lib/http/coro/client/HTTPClient.h>
#include <proxygen/lib/utils/URL.h>

#include <gflags/gflags.h>

#include <QCoreApplication>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QUrl>

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
#include <wincrypt.h>
#endif

DECLARE_uint64(atlas_disk_cache_http_max_bytes);

DEFINE_string(atlas_http_ca_bundle,
              "",
              "Path to a PEM CA bundle for HTTPS requests (overrides auto-detect; also respects env SSL_CERT_FILE).");

DEFINE_string(atlas_http_proxy_strategy,
              "auto",
              "Outbound HTTP proxy strategy using OS system proxy settings only (no proxy URL flags). "
              "Values: auto (alternate direct/proxy between retries), no_proxy (always direct), "
              "proxy_if_available (always use system proxy if one exists for the URL).");

DEFINE_uint32(atlas_http_max_retries,
              3,
              "Maximum number of retries for transient network/handshake errors in HTTP GET (default 3).");

DEFINE_uint32(atlas_http_retry_backoff_initial_ms,
              200,
              "Initial backoff delay in milliseconds before retrying transient HTTP errors (default 200ms).");

DEFINE_uint32(atlas_http_retry_backoff_max_ms,
              2000,
              "Maximum backoff delay in milliseconds for transient HTTP error retries (default 2000ms).");

namespace nim {
namespace {

std::vector<uint8_t> iobufToBytes(/*nullable*/ const folly::IOBuf* buf)
{
  if (!buf) {
    return {};
  }
  const size_t len = buf->computeChainDataLength();
  std::vector<uint8_t> out(len);
  folly::io::Cursor cursor(buf);
  cursor.pull(out.data(), len);
  return out;
}

std::vector<uint8_t> decompressIfNeeded(std::vector<uint8_t> body, std::string_view contentEncoding)
{
  if (body.empty()) {
    return body;
  }

  std::string encLower(contentEncoding);
  folly::toLowerAscii(encLower);
  if (encLower.empty() || encLower == "identity") {
    return body;
  }

  folly::compression::CodecType codecType{};
  if (encLower == "gzip" || encLower == "x-gzip") {
    codecType = folly::compression::CodecType::GZIP;
  } else if (encLower == "deflate") {
    codecType = folly::compression::CodecType::ZLIB;
  } else {
    throw ZException(fmt::format("Unsupported Content-Encoding '{}'", contentEncoding));
  }

  auto codec = folly::compression::getCodec(codecType);
  auto uncompressed = codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(body.data()), body.size()));
  std::vector<uint8_t> out(uncompressed.size());
  std::memcpy(out.data(), uncompressed.data(), out.size());
  return out;
}

std::optional<std::string> envVarString(const char* key)
{
  if (!key) {
    return std::nullopt;
  }
  const char* v = std::getenv(key);
  if (!v) {
    return std::nullopt;
  }
  std::string s(v);
  auto trimmed = folly::trimWhitespace(folly::StringPiece(s));
  s.assign(trimmed.data(), trimmed.size());
  if (s.empty()) {
    return std::nullopt;
  }
  return s;
}

bool isReadableFile(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  std::ifstream f(path);
  return f.good();
}

std::vector<std::string> caBundleCandidates()
{
  std::vector<std::string> paths;

  if (!FLAGS_atlas_http_ca_bundle.empty()) {
    paths.emplace_back(FLAGS_atlas_http_ca_bundle);
    return paths;
  }

  if (auto v = envVarString("SSL_CERT_FILE")) {
    paths.emplace_back(std::move(*v));
  }
  if (auto v = envVarString("REQUESTS_CA_BUNDLE")) {
    paths.emplace_back(std::move(*v));
  }
  if (auto v = envVarString("CURL_CA_BUNDLE")) {
    paths.emplace_back(std::move(*v));
  }

  if (auto condaPrefix = envVarString("CONDA_PREFIX")) {
    paths.emplace_back(*condaPrefix + "/ssl/cert.pem");
  }

  // Common system locations (Linux/macOS/Homebrew).
  paths.emplace_back("/etc/ssl/cert.pem");
  paths.emplace_back("/etc/ssl/certs/ca-certificates.crt");
  paths.emplace_back("/etc/pki/tls/cert.pem");
  paths.emplace_back("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem");
  paths.emplace_back("/etc/ssl/certs/ca-bundle.crt");
  paths.emplace_back("/usr/local/etc/openssl@3/cert.pem");
  paths.emplace_back("/opt/homebrew/etc/openssl@3/cert.pem");
  paths.emplace_back("/usr/local/etc/openssl/cert.pem");

  return paths;
}

std::optional<std::string> findCaBundlePath()
{
  for (const auto& path : caBundleCandidates()) {
    if (isReadableFile(path)) {
      return path;
    }
  }
  return std::nullopt;
}

struct ProxyEndpoint
{
  std::string host;
  uint16_t port = 0;
};

enum class ProxyStrategy
{
  Auto,
  NoProxy,
  ProxyIfAvailable,
};

ProxyStrategy proxyStrategyFromFlag()
{
  std::string s = FLAGS_atlas_http_proxy_strategy;
  folly::toLowerAscii(s);
  if (s.empty() || s == "auto" || s == "automatic") {
    return ProxyStrategy::Auto;
  }
  if (s == "no_proxy" || s == "noproxy" || s == "none" || s == "direct") {
    return ProxyStrategy::NoProxy;
  }
  if (s == "proxy_if_available" || s == "proxyifavailable" || s == "use_proxy_if_available" || s == "proxy") {
    return ProxyStrategy::ProxyIfAvailable;
  }
  throw ZException(fmt::format(
    "Invalid --atlas_http_proxy_strategy='{}' (expected: auto, no_proxy, proxy_if_available)",
    FLAGS_atlas_http_proxy_strategy));
}

std::optional<ProxyEndpoint> systemHttpProxyForUrl(const std::string& url)
{
  if (QCoreApplication::instance() == nullptr) {
    return std::nullopt;
  }

  const QUrl qurl(QString::fromStdString(url));
  if (!qurl.isValid()) {
    return std::nullopt;
  }

  // systemProxyForQuery() returns proxies in the order they should be tried.
  const QNetworkProxyQuery query(qurl);
  const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
  if (proxies.isEmpty()) {
    return std::nullopt;
  }

  // Scan for the first supported HTTP proxy in the ordered list.
  // Note: proxy auto-config (PAC) can legitimately return DIRECT first, followed by
  //       one or more PROXY fallbacks. We still want to detect that a proxy is
  //       available so --atlas_http_proxy_strategy=auto can retry via proxy.
  // This handles environments where the OS provides multiple proxy candidates
  // (e.g. PAC) without introducing custom proxy config in Atlas.
  for (const QNetworkProxy& candidate : proxies) {
    if (candidate.type() == QNetworkProxy::HttpProxy || candidate.type() == QNetworkProxy::HttpCachingProxy) {
      const QString host = candidate.hostName();
      const int port = candidate.port();
      if (host.isEmpty() || port <= 0 || port > 65535) {
        continue;
      }

      ProxyEndpoint out{};
      out.host = host.toStdString();
      out.port = static_cast<uint16_t>(port);
      return out;
    }

    // If the system provided a non-HTTP proxy first (e.g. SOCKS), we can't use it
    // with Proxygen's HTTP CONNECT proxy support. We'll keep scanning for an HTTP
    // proxy candidate and fall back to direct if none exist.
    if (candidate.type() != QNetworkProxy::NoProxy) {
      VLOG(1) << fmt::format(
        "Ignoring unsupported system proxy type {} for URL '{}'",
        static_cast<int>(candidate.type()),
        url);
    }
  }

  return std::nullopt;
}

proxygen::coro::HTTPClientConnectionCache::ProxyParams makeConnectProxyParams(const ProxyEndpoint& endpoint)
{
  proxygen::coro::HTTPClientConnectionCache::ProxyParams params{};
  params.server = endpoint.host;
  params.port = endpoint.port;
  params.useConnect = true;
  params.poolParams = proxygen::coro::HTTPCoroSessionPool::defaultPoolParams();
  params.connParams = proxygen::coro::HTTPCoroConnector::defaultConnectionParams();
  // Proxy connection itself is usually plaintext; keep sslContext unset here.
  params.connParams.sslContext = nullptr;
  params.sessionParams = proxygen::coro::HTTPCoroConnector::defaultSessionParams();
  return params;
}

#if defined(_WIN32)
bool tryAddWindowsSystemCertStoreTo(/*inout*/ folly::SSLContext& ctx, const char* storeName, int& addedCerts, int& failedCerts)
{
  CHECK(storeName);

  // CertOpenSystemStoreA takes an integral "legacy crypto provider" handle.
  // Use 0 to indicate the default provider (nullptr is not implicitly convertible on MSVC).
  HCERTSTORE storeHandle = CertOpenSystemStoreA(static_cast<HCRYPTPROV_LEGACY>(0), storeName);
  if (!storeHandle) {
    return false;
  }

  SSL_CTX* sslCtx = ctx.getSSLCtx();
  CHECK(sslCtx);
  X509_STORE* x509Store = SSL_CTX_get_cert_store(sslCtx);
  CHECK(x509Store);

  PCCERT_CONTEXT certCtx = nullptr;
  while ((certCtx = CertEnumCertificatesInStore(storeHandle, certCtx)) != nullptr) {
    const unsigned char* encoded = reinterpret_cast<const unsigned char*>(certCtx->pbCertEncoded);
    folly::ssl::X509UniquePtr x509(d2i_X509(nullptr, &encoded, static_cast<long>(certCtx->cbCertEncoded)));
    if (!x509) {
      ++failedCerts;
      ERR_clear_error();
      continue;
    }

    if (X509_STORE_add_cert(x509Store, x509.get()) == 1) {
      ++addedCerts;
    } else {
      ++failedCerts;
      ERR_clear_error();
    }
  }

  CertCloseStore(storeHandle, /*dwFlags=*/0);
  return true;
}
#endif

std::shared_ptr<folly::SSLContext> makeClientSslContext(/*out*/ std::string& caBundlePath)
{
  caBundlePath.clear();

  auto ctx = std::make_shared<folly::SSLContext>();
  ctx->setAdvertisedNextProtocols({"h2", "http/1.1"});
  ctx->setVerificationOption(folly::SSLContext::SSLVerifyPeerEnum::VERIFY);
  ctx->authenticate(/*checkPeerCert=*/true, /*checkPeerName=*/true);

  if (!FLAGS_atlas_http_ca_bundle.empty()) {
    if (!isReadableFile(FLAGS_atlas_http_ca_bundle)) {
      throw ZException(fmt::format("--atlas_http_ca_bundle points to an unreadable file: '{}'", FLAGS_atlas_http_ca_bundle));
    }
    ctx->loadTrustedCertificates(FLAGS_atlas_http_ca_bundle.c_str());
    caBundlePath = FLAGS_atlas_http_ca_bundle;
    return ctx;
  }

  if (auto caPathOpt = findCaBundlePath()) {
    ctx->loadTrustedCertificates(caPathOpt->c_str());
    caBundlePath = *caPathOpt;
  }

#if defined(_WIN32)
  if (caBundlePath.empty()) {
    int added = 0;
    int failed = 0;
    const bool triedRoot = tryAddWindowsSystemCertStoreTo(*ctx, "ROOT", added, failed);
    const bool triedCA = tryAddWindowsSystemCertStoreTo(*ctx, "CA", added, failed);
    if ((triedRoot || triedCA) && added > 0) {
      caBundlePath = fmt::format("<win_system_store: added={}, failed={}>", added, failed);
    }
  }
#endif

  // If we didn't find a bundle, rely on the OpenSSL defaults. If those are misconfigured
  // (common when building against an OpenSSL with Linux-centric defaults on macOS), the
  // request will fail with a message that points to --atlas_http_ca_bundle/SSL_CERT_FILE.
  return ctx;
}

bool looksLikeTlsTrustStoreError(std::string_view message)
{
  return message.find("cert.pem") != std::string_view::npos || message.find("certificate") != std::string_view::npos ||
         message.find("SSL") != std::string_view::npos || message.find("tls") != std::string_view::npos;
}

bool isRetryableHttpError(const proxygen::coro::HTTPError& e)
{
  using proxygen::coro::HTTPErrorCode;
  switch (e.code) {
  case HTTPErrorCode::CONNECT_ERROR:
  case HTTPErrorCode::_H3_CONNECT_ERROR:
  case HTTPErrorCode::TRANSPORT_EOF:
  case HTTPErrorCode::TRANSPORT_READ_ERROR:
  case HTTPErrorCode::TRANSPORT_WRITE_ERROR:
  case HTTPErrorCode::READ_TIMEOUT:
  case HTTPErrorCode::WRITE_TIMEOUT:
  case HTTPErrorCode::DROPPED:
  case HTTPErrorCode::REFUSED_STREAM:
    return true;
  default:
    return false;
  }
}

bool isRetryableExceptionMessage(std::string_view message)
{
  // Heuristic for transient socket/TLS/proxy issues surfaced as std::exception.
  // Keep this conservative, but include common "direct path is blocked; proxy required" cases.
  std::string m(message);
  folly::toLowerAscii(m);

  auto contains = [&](std::string_view needle) -> bool {
    return m.find(needle) != std::string::npos;
  };

  // TLS / transport
  if (contains("handshake") || contains("network error") || contains("tls") || contains("ssl") || contains("eof")) {
    return true;
  }

  // Connect / route failures (often fixed by retrying via system proxy in auto mode).
  if (contains("connect failed") || contains("connect error") || contains("connection refused") ||
      contains("no route to host") || contains("network is unreachable") || contains("host is down") ||
      contains("connection reset") || contains("broken pipe")) {
    return true;
  }

  // DNS / name resolution
  if (contains("temporary failure in name resolution") || contains("name or service not known") ||
      contains("nodename nor servname provided") || contains("unknown host") || contains("host not found")) {
    return true;
  }

  // Timeout variants (some surfaces as generic exceptions, not HTTPErrorCode::READ_TIMEOUT/WRITE_TIMEOUT).
  if (contains("timeout") || contains("timed out")) {
    return true;
  }

  return false;
}

std::chrono::milliseconds retryBackoffForAttempt(uint32_t attempt)
{
  const uint32_t initial = FLAGS_atlas_http_retry_backoff_initial_ms;
  const uint32_t maxDelay = std::max<uint32_t>(initial, FLAGS_atlas_http_retry_backoff_max_ms);
  uint64_t delay = initial;
  // Exponential backoff: initial * 2^attempt
  delay <<= std::min<uint32_t>(attempt, 20u);
  if (delay > maxDelay) {
    delay = maxDelay;
  }
  return std::chrono::milliseconds(static_cast<int64_t>(delay));
}

} // namespace

ZProxygenHttpClient& ZProxygenHttpClient::instance()
{
  static ZProxygenHttpClient client;
  return client;
}

ZProxygenHttpClient::ZProxygenHttpClient()
  : m_eventBaseThread("atlas_proxygen_http")
  , m_sslContext(makeClientSslContext(m_caBundlePath))
{
  folly::EventBase* evb = m_eventBaseThread.getEventBase();
  CHECK(evb);

  if (FLAGS_atlas_disk_cache_http_max_bytes > 0) {
    const QString rootDir = atlasDiskCacheRootDirFromFlags();
    m_diskCache = std::make_unique<ZHttpDiskCache>(rootDir, FLAGS_atlas_disk_cache_http_max_bytes);
    if (!m_diskCache->isEnabled()) {
      m_diskCache.reset();
    } else {
      LOG(INFO) << "HTTP disk cache enabled: root='" << rootDir
                << "' maxBytes=" << FLAGS_atlas_disk_cache_http_max_bytes;
    }
  }

  m_directConnCache = std::make_unique<proxygen::coro::HTTPClientConnectionCache>(*evb);
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZProxygenHttpClient::getBytes(
  std::string url,
  std::chrono::milliseconds timeout,
  std::vector<std::pair<std::string, std::string>> requestHeaders)
{
  co_return co_await folly::coro::co_withExecutor(
    m_eventBaseThread.getEventBase(),
    getBytesOnEventBase(std::move(url), timeout, std::move(requestHeaders)));
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZProxygenHttpClient::getBytesOnEventBase(
  std::string url,
  std::chrono::milliseconds timeout,
  std::vector<std::pair<std::string, std::string>> requestHeaders)
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  proxygen::URL parsedUrl(url);
  if (!parsedUrl.isValid() || parsedUrl.getHost().empty()) {
    throw ZException(fmt::format("Invalid URL '{}'", url));
  }

  bool isSecure = false;
  {
    std::string scheme(parsedUrl.getScheme());
    folly::toLowerAscii(scheme);
    isSecure = (scheme == "https");
  }

  const ProxyStrategy proxyStrategy = proxyStrategyFromFlag();
  const std::optional<ProxyEndpoint> systemProxy = (proxyStrategy == ProxyStrategy::NoProxy) ? std::nullopt : systemHttpProxyForUrl(url);

  auto getOrCreateProxyCache = [&](const ProxyEndpoint& endpoint) -> proxygen::coro::HTTPClientConnectionCache* {
    const std::string key = fmt::format("{}:{}", endpoint.host, endpoint.port);
    auto it = m_proxyConnCaches.find(key);
    if (it != m_proxyConnCaches.end()) {
      return it->second.get();
    }
    folly::EventBase* evb = m_eventBaseThread.getEventBase();
    CHECK(evb);
    auto cache = std::make_unique<proxygen::coro::HTTPClientConnectionCache>(*evb, makeConnectProxyParams(endpoint));
    auto* cachePtr = cache.get();
    m_proxyConnCaches.emplace(key, std::move(cache));
    LOG(INFO) << fmt::format("Using system HTTP proxy: {}:{} (CONNECT)", endpoint.host, endpoint.port);
    return cachePtr;
  };

  proxygen::coro::HTTPClient::RequestHeaderMap headerMap;
  headerMap.emplace("accept-encoding", "identity");
  for (const auto& [k, v] : requestHeaders) {
    headerMap.emplace(k, v);
  }

  if (m_diskCache && m_diskCache->isEnabled()) {
    maybeCancel(cancellationToken);
    if (auto cached = m_diskCache->tryGet(url, requestHeaders)) {
      VLOG(2) << "HTTP disk cache hit: " << url;
      co_return std::move(*cached);
    }
  }

  const uint32_t maxRetries = FLAGS_atlas_http_max_retries;
  for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
    maybeCancel(cancellationToken);

    proxygen::coro::HTTPClientConnectionCache* connCache = m_directConnCache.get();
    if (systemProxy) {
      const bool useProxy = [&]() {
        switch (proxyStrategy) {
        case ProxyStrategy::NoProxy:
          return false;
        case ProxyStrategy::ProxyIfAvailable:
          return true;
        case ProxyStrategy::Auto:
          // Alternate proxy/direct between attempts when a proxy exists (attempt 0 is proxy).
          return (attempt % 2u) == 0u;
        }
        return false;
      }();
      if (useProxy) {
        connCache = getOrCreateProxyCache(*systemProxy);
      }
    }
    CHECK(connCache);

    auto attemptResult = co_await folly::coro::co_awaitTry([&]() -> folly::coro::Task<ZHttpGetBytesResult> {
      maybeCancel(cancellationToken);

      proxygen::coro::HTTPCoroConnector::ConnectionParams connParams{};
      const proxygen::coro::HTTPCoroConnector::ConnectionParams* connParamsPtr = nullptr;
      if (isSecure) {
        CHECK(m_sslContext);
        connParams = proxygen::coro::HTTPCoroConnector::defaultConnectionParams();
        connParams.sslContext = m_sslContext;
        connParams.serverName = parsedUrl.getHost();
        connParamsPtr = &connParams;
      }

      auto sessionRes = co_await connCache->getSessionWithReservation(url, timeout, connParamsPtr);
      maybeCancel(cancellationToken);

      auto response = co_await proxygen::coro::HTTPClient::get(
        sessionRes.session,
        std::move(sessionRes.reservation),
        parsedUrl,
        timeout,
        headerMap);

      ZHttpGetBytesResult out{};
      CHECK(response.headers);
      out.status = response.headers->getStatusCode();
      const auto& headers = response.headers->getHeaders();
      out.contentType = headers.getSingleOrEmpty("content-type");
      out.contentEncoding = headers.getSingleOrEmpty("content-encoding");

      auto bodyBuf = response.body.move();
      out.body = iobufToBytes(bodyBuf.get());
      out.body = decompressIfNeeded(std::move(out.body), out.contentEncoding);
      co_return out;
    }());

    if (attemptResult.hasValue()) {
      ZHttpGetBytesResult value = std::move(attemptResult).value();
      if (m_diskCache && m_diskCache->isEnabled()) {
        m_diskCache->put(url, requestHeaders, value);
      }
      co_return std::move(value);
    }

    CHECK(attemptResult.hasException());
    folly::exception_wrapper error = std::move(attemptResult).exception();

    if (error.is_compatible_with<folly::OperationCancelled>()) {
      // Folly coroutines report cancellation via OperationCancelled.
      // Convert to our cancellation type so callers treat it as expected control-flow.
      throw ZCancellationException();
    }

    if (const auto* httpErr = error.get_exception<proxygen::coro::HTTPError>(); httpErr != nullptr) {
      if (httpErr->httpMessage && httpErr->httpMessage->getStatusCode() == 404) {
        co_return std::nullopt;
      }

      const bool retryable = isRetryableHttpError(*httpErr);
      if (attempt < maxRetries && retryable) {
        VLOG(1) << fmt::format("HTTP GET transient error (attempt {}/{}): '{}' ({})",
                               attempt + 1,
                               maxRetries + 1,
                               url,
                               httpErr->describe());
        co_await folly::coro::sleepReturnEarlyOnCancel(retryBackoffForAttempt(attempt));
        continue;
      }

      std::string msg(httpErr->what());
      if (isSecure && looksLikeTlsTrustStoreError(msg)) {
        msg += fmt::format(" (CA bundle: '{}'; override with --atlas_http_ca_bundle=... or env SSL_CERT_FILE)",
                           m_caBundlePath.empty() ? "<auto>" : m_caBundlePath);
      }
      throw ZException(fmt::format("HTTP GET failed for '{}': {}", url, msg));
    }

    // Fall back to message-based retry heuristics for non-proxygen errors.
    std::string msg(error.what().toStdString());
    const bool retryable =
      error.is_compatible_with<proxygen::coro::HTTPCoroSessionPool::Exception>() || isRetryableExceptionMessage(msg);
    if (attempt < maxRetries && retryable) {
      VLOG(1) << fmt::format("HTTP GET transient exception (attempt {}/{}): '{}' ({})",
                             attempt + 1,
                             maxRetries + 1,
                             url,
                             msg);
      co_await folly::coro::sleepReturnEarlyOnCancel(retryBackoffForAttempt(attempt));
      continue;
    }

    if (isSecure && looksLikeTlsTrustStoreError(msg)) {
      msg += fmt::format(" (CA bundle: '{}'; override with --atlas_http_ca_bundle=... or env SSL_CERT_FILE)",
                         m_caBundlePath.empty() ? "<auto>" : m_caBundlePath);
    }
    throw ZException(fmt::format("HTTP GET failed for '{}': {}", url, msg));
  }

  throw ZException(fmt::format("HTTP GET failed for '{}': exhausted retries", url));
}

} // namespace nim
