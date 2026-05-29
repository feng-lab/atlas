#include "zproxygenhttpclient.h"

#include "zdiskcacheutils.h"
#include "zhttpdiskcache.h"
#include "zhttpsystemproxy.h"
#include "zhttpretrypolicy.h"
#include "zhttptruststore.h"
#include "zcancellation.h"
#include "zqobjectthreadinvoker.h"
#include "zabslflagtypes.h"

#include <brotli/decode.h>
#include <folly/Optional.h>
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

#include "zcommandlineflags.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QUrl>

#include <array>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <exception>
#include <memory>
#include <optional>

ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_http_max_bytes);

ABSL_FLAG(std::optional<std::string>,
          atlas_http_ca_bundle,
          std::nullopt,
          "Path to a PEM CA bundle for HTTPS requests (overrides auto-detect).");

namespace nim {

inline constexpr std::array<AbslEnumFlagValue<HttpProxyStrategy>, 3> kHttpProxyStrategyFlagValues{
  {
   {"auto", HttpProxyStrategy::Auto},
   {"no_proxy", HttpProxyStrategy::NoProxy},
   {"proxy_if_available", HttpProxyStrategy::ProxyIfAvailable},
   }
};

inline bool AbslParseFlag(absl::string_view text, HttpProxyStrategy* value, std::string* error)
{
  return parseAbslEnumFlag(text, value, error, "HttpProxyStrategy", kHttpProxyStrategyFlagValues);
}

inline std::string AbslUnparseFlag(HttpProxyStrategy value)
{
  return unparseAbslEnumFlag(value, kHttpProxyStrategyFlagValues);
}

} // namespace nim

ABSL_FLAG(nim::HttpProxyStrategy,
          atlas_http_proxy_strategy,
          nim::HttpProxyStrategy::Auto,
          "Outbound HTTP proxy strategy using OS system proxy settings only (no proxy URL flags). "
          "Values: auto (alternate direct/proxy between retries), no_proxy (always direct), "
          "proxy_if_available (always use system proxy if one exists for the URL).");

ABSL_FLAG(uint32_t,
          atlas_http_max_redirect_hops,
          5,
          "Maximum number of HTTP redirects to follow for a single GET request (default 5).");

ABSL_FLAG(uint32_t,
          atlas_http_max_retries,
          7,
          "Maximum number of retries for transient network/handshake errors in HTTP GET (default 7).");

ABSL_FLAG(uint32_t,
          atlas_http_retry_backoff_initial_ms,
          200,
          "Initial backoff delay in milliseconds before retrying transient HTTP errors (default 200ms).");

ABSL_FLAG(uint32_t,
          atlas_http_retry_backoff_max_ms,
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

[[nodiscard]] std::vector<std::string> parseContentEncodings(std::string_view contentEncoding)
{
  std::string lower(contentEncoding);
  folly::toLowerAscii(lower);

  std::vector<std::string> parts;
  folly::split(',', lower, parts);

  std::vector<std::string> out;
  out.reserve(parts.size());
  for (auto& p : parts) {
    const folly::StringPiece trimmed = folly::trimWhitespace(folly::StringPiece(p));
    if (!trimmed.empty()) {
      out.emplace_back(trimmed.data(), trimmed.size());
    }
  }
  return out;
}

[[nodiscard]] bool isIdentityContentEncoding(std::string_view contentEncoding)
{
  const auto encs = parseContentEncodings(contentEncoding);
  if (encs.empty()) {
    // Empty Content-Encoding implies identity.
    return true;
  }
  for (const auto& enc : encs) {
    if (enc.empty() || enc == "identity") {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool isMissingResourceHttpStatus(uint16_t status)
{
  // Match Neuroglancer's HTTP-store semantics: object stores and proxies may
  // report an absent object as 403 rather than 404, and both are treated as a
  // soft "not found" at the read layer.
  return status == 403 || status == 404;
}

std::vector<uint8_t> brotliDecompress(folly::StringPiece encoded)
{
  struct StateDeleter
  {
    void operator()(BrotliDecoderState* state) const
    {
      BrotliDecoderDestroyInstance(state);
    }
  };

  std::unique_ptr<BrotliDecoderState, StateDeleter> state(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
  if (!state) {
    throw ZException("BrotliDecoderCreateInstance failed");
  }

  size_t availableIn = encoded.size();
  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(encoded.data());

  std::vector<uint8_t> out;

  // Stream into fixed-size chunks, growing the output vector as needed.
  constexpr size_t kChunkBytes = 64u * 1024u;
  std::array<uint8_t, kChunkBytes> chunk{};
  while (true) {
    size_t availableOut = chunk.size();
    uint8_t* nextOut = chunk.data();

    BrotliDecoderResult r = BrotliDecoderDecompressStream(
      state.get(), &availableIn, &nextIn, &availableOut, &nextOut, /*total_out=*/nullptr);
    const size_t produced = chunk.size() - availableOut;
    if (produced > 0) {
      const size_t oldSize = out.size();
      out.resize(oldSize + produced);
      std::memcpy(out.data() + oldSize, chunk.data(), produced);
    }

    switch (r) {
    case BROTLI_DECODER_RESULT_SUCCESS:
      return out;
    case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
      // Continue; output buffer was full.
      break;
    case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
      // We provided the entire payload up-front; additional input means truncation.
      if (availableIn == 0) {
        throw ZException("Brotli decode failed: unexpected end of input");
      }
      break;
    case BROTLI_DECODER_RESULT_ERROR: {
      const BrotliDecoderErrorCode code = BrotliDecoderGetErrorCode(state.get());
      const char* msg = BrotliDecoderErrorString(code);
      throw ZException(fmt::format("Brotli decode failed: {} ({})", msg ? msg : "<unknown>", static_cast<int>(code)));
    }
    }
  }
}

std::vector<uint8_t> decompressIfNeeded(std::vector<uint8_t> body, std::string& contentEncoding)
{
  if (body.empty()) {
    return body;
  }

  if (isIdentityContentEncoding(contentEncoding)) {
    return body;
  }

  const std::string originalEncoding = contentEncoding;

  // Decode in reverse order: Content-Encoding lists encodings in the order applied.
  const auto encodings = parseContentEncodings(contentEncoding);
  for (auto it = encodings.rbegin(); it != encodings.rend(); ++it) {
    const std::string& enc = *it;
    if (enc.empty() || enc == "identity") {
      continue;
    }

    if (enc == "gzip" || enc == "x-gzip") {
      auto codec = folly::compression::getCodec(folly::compression::CodecType::GZIP);
      auto uncompressed =
        codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(body.data()), body.size()));
      std::vector<uint8_t> out(uncompressed.size());
      std::memcpy(out.data(), uncompressed.data(), out.size());
      body = std::move(out);
      continue;
    }

    if (enc == "deflate") {
      auto codec = folly::compression::getCodec(folly::compression::CodecType::ZLIB);
      auto uncompressed =
        codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(body.data()), body.size()));
      std::vector<uint8_t> out(uncompressed.size());
      std::memcpy(out.data(), uncompressed.data(), out.size());
      body = std::move(out);
      continue;
    }

    if (enc == "zstd" || enc == "x-zstd") {
      auto codec = folly::compression::getCodec(folly::compression::CodecType::ZSTD);
      auto uncompressed =
        codec->uncompress(folly::StringPiece(reinterpret_cast<const char*>(body.data()), body.size()));
      std::vector<uint8_t> out(uncompressed.size());
      std::memcpy(out.data(), uncompressed.data(), out.size());
      body = std::move(out);
      continue;
    }

    if (enc == "br") {
      body = brotliDecompress(
        folly::StringPiece(reinterpret_cast<const char*>(body.data()), body.size()));
      continue;
    }

    throw ZException(fmt::format("Unsupported Content-Encoding '{}'", originalEncoding));
  }

  // The returned payload is now decoded.
  contentEncoding = "identity";
  return body;
}

std::vector<std::string> resolveHostToIpAddrsSystem(const std::string& host)
{
  CHECK(!host.empty());

  // Fast-path: already an IP literal.
  if (!QHostAddress(QString::fromStdString(host)).isNull()) {
    return {host};
  }

  const QHostInfo info = QHostInfo::fromName(QString::fromStdString(host));
  if (info.error() != QHostInfo::NoError) {
    throw ZException(fmt::format("System DNS resolution failed for host '{}': {}",
                                 host,
                                 info.errorString().toStdString()));
  }

  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(info.addresses().size()));
  for (const QHostAddress& addr : info.addresses()) {
    if (addr.isNull()) {
      continue;
    }
    // toString() yields a numeric address literal for resolved addresses.
    const std::string s = addr.toString().toStdString();
    if (s.empty()) {
      continue;
    }
    if (std::find(out.begin(), out.end(), s) == out.end()) {
      out.emplace_back(s);
    }
  }

  if (out.empty()) {
    throw ZException(fmt::format("System DNS resolution returned no usable addresses for host '{}'", host));
  }

  return out;
}

std::vector<std::string> resolveHostToIpAddrsSystemViaQtThread(const std::string& host, QObject* lookupInvoker)
{
  CHECK(lookupInvoker);

  const auto res = invokeOnObjectThreadWait(
    lookupInvoker,
    [host]() {
      return resolveHostToIpAddrsSystem(host);
    },
    fmt::format("QHostInfo::fromName('{}')", host));
  if (!res.ok) {
    throw ZException(res.error);
  }
  return res.value;
}

constexpr ZHttpProxySupport kProxygenProxySupport{
  .supportsHttp = true,
};

bool isRedirectStatus(uint16_t status)
{
  switch (status) {
  case 301:
  case 302:
  case 303:
  case 307:
  case 308:
    return true;
  default:
    return false;
  }
}

std::string resolveRedirectUrl(const std::string& baseUrl, const std::string& location)
{
  const QUrl base(QString::fromStdString(baseUrl));
  if (!base.isValid()) {
    throw ZException(fmt::format("Invalid base URL while resolving redirect: '{}'", baseUrl));
  }

  const QUrl loc(QString::fromStdString(location));
  const QUrl resolved = base.resolved(loc);
  if (!resolved.isValid()) {
    throw ZException(fmt::format("Invalid redirect target '{}' (base '{}')", location, baseUrl));
  }

  const QString schemeLower = resolved.scheme().trimmed().toLower();
  if (schemeLower != QStringLiteral("http") && schemeLower != QStringLiteral("https")) {
    throw ZException(
      fmt::format("Unsupported redirect scheme '{}' (from '{}' to '{}')", schemeLower, baseUrl, resolved.toString()));
  }

  return resolved.toString(QUrl::FullyEncoded).toStdString();
}

proxygen::coro::HTTPClientConnectionCache::ProxyParams makeConnectProxyParams(const ZHttpProxyEndpoint& endpoint)
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

std::shared_ptr<folly::SSLContext> makeClientSslContext(const ZHttpTrustStoreConfig& trustStore)
{
  auto ctx = std::make_shared<folly::SSLContext>();
  ctx->setAdvertisedNextProtocols({"h2", "http/1.1"});
  ctx->setVerificationOption(folly::SSLContext::SSLVerifyPeerEnum::VERIFY);
  ctx->authenticate(/*checkPeerCert=*/true, /*checkPeerName=*/true);

  if (!trustStore.caBundlePath.empty()) {
    ctx->loadTrustedCertificates(trustStore.caBundlePath.c_str());
  }

  // If we didn't find a bundle, rely on the OpenSSL defaults. If those are misconfigured
  // (common when building against an OpenSSL with Linux-centric defaults on macOS), the
  // request will fail with a message that points to --atlas_http_ca_bundle.
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

} // namespace

ZProxygenHttpClient& ZProxygenHttpClient::instance()
{
  static ZProxygenHttpClient client;
  return client;
}

ZProxygenHttpClient::ZProxygenHttpClient()
  : m_eventBaseThread("atlas_proxygen_http")
{
  const ZHttpTrustStoreConfig trustStore = resolveHttpTrustStoreConfig(ZHttpTrustBackend::Proxygen);
  m_caBundlePath = trustStore.caBundlePath;
  m_trustSourceDescription = trustStore.sourceDescription;
  m_sslContext = makeClientSslContext(trustStore);

  m_hostLookupThread = std::make_unique<QThread>();
  m_hostLookupThread->setObjectName(QStringLiteral("atlas_qhostinfo_lookup"));
  m_hostLookupThread->start();
  CHECK(m_hostLookupThread->isRunning());

  m_hostLookupInvoker = new QObject();
  m_hostLookupInvoker->moveToThread(m_hostLookupThread.get());

  folly::EventBase* evb = m_eventBaseThread.getEventBase();
  CHECK(evb);

  const uint64_t diskCacheMaxBytes = absl::GetFlag(FLAGS_atlas_disk_cache_http_max_bytes);
  if (diskCacheMaxBytes > 0) {
    const QString rootDir = atlasDiskCacheRootDirFromFlags();
    m_diskCache = std::make_unique<ZHttpDiskCache>(rootDir, diskCacheMaxBytes);
    if (!m_diskCache->isEnabled()) {
      m_diskCache.reset();
    } else {
      LOG(INFO) << "HTTP disk cache enabled: root='" << rootDir << "' maxBytes=" << diskCacheMaxBytes;
    }
  }

  m_directConnCache = std::make_unique<proxygen::coro::HTTPClientConnectionCache>(*evb);
  LOG(INFO) << fmt::format("proxygen backend trust source: '{}' caBundle='{}'",
                           m_trustSourceDescription,
                           m_caBundlePath.empty() ? "<default trust store>" : m_caBundlePath);
}

ZProxygenHttpClient::~ZProxygenHttpClient()
{
  if (m_hostLookupInvoker != nullptr) {
    if (m_hostLookupThread && m_hostLookupThread->isRunning()) {
      const bool invokeOk = QMetaObject::invokeMethod(m_hostLookupInvoker, &QObject::deleteLater, Qt::QueuedConnection);
      CHECK(invokeOk);
    } else {
      delete m_hostLookupInvoker;
    }
    m_hostLookupInvoker = nullptr;
  }

  if (m_hostLookupThread) {
    m_hostLookupThread->quit();
    CHECK(m_hostLookupThread->wait(5000));
  }
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZProxygenHttpClient::getBytes(ZHttpGetRequest request)
{
  co_return co_await folly::coro::co_withExecutor(m_eventBaseThread.getEventBase(),
                                                  getBytesOnEventBase(std::move(request)));
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZProxygenHttpClient::getBytesOnEventBase(ZHttpGetRequest request)
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  const bool isRangeRequest = request.exactByteRange.has_value();

  proxygen::URL initialUrl(request.url);
  if (!initialUrl.isValid() || initialUrl.getHost().empty()) {
    throw ZException(fmt::format("Invalid URL '{}'", request.url));
  }

  const HttpProxyStrategy proxyStrategy = absl::GetFlag(FLAGS_atlas_http_proxy_strategy);
  std::optional<ZHttpProxyEndpoint> systemProxy;
  if (proxyStrategy != HttpProxyStrategy::NoProxy) {
    const ZSystemHttpProxyResolution proxyResolution = querySystemHttpProxyForUrl(request.url, kProxygenProxySupport);
    if (proxyResolution.error) {
      throw ZException(fmt::format("proxygen backend: {}", *proxyResolution.error));
    }
    for (const auto& warning : proxyResolution.warnings) {
      LOG(WARNING) << "proxygen backend: " << warning
                   << " Proceeding with direct connection because the OS proxy settings explicitly allow DIRECT.";
    }
    systemProxy = proxyResolution.endpoint;
  }

  auto getOrCreateProxyCache = [&](const ZHttpProxyEndpoint& endpoint) -> proxygen::coro::HTTPClientConnectionCache* {
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
  for (const auto& [k, v] : httpRequestHeadersForTransport(request)) {
    std::string keyLower(k);
    folly::toLowerAscii(keyLower);
    headerMap.emplace(std::move(keyLower), v);
  }

  if (isRangeRequest) {
    // Range requests must operate on the identity representation; Content-Encoding makes
    // the payload length unpredictable relative to the requested byte range.
    headerMap["accept-encoding"] = "identity";
  } else if (headerMap.find("accept-encoding") == headerMap.end()) {
    headerMap.emplace("accept-encoding", "br, gzip, zstd");
  }

  if (m_diskCache && m_diskCache->isEnabled()) {
    maybeCancel(cancellationToken);
    auto cachedTry = co_await folly::coro::co_awaitTry(m_diskCache->tryGetAsync(request));
    maybeCancel(cancellationToken);
    if (cachedTry.hasValue() && cachedTry.value().has_value()) {
      auto cached = std::move(cachedTry).value();
      VLOG(2) << "HTTP disk cache hit: " << request.url;
      cached->source = ZHttpGetBytesSource::DiskCache;
      cached->encodedBodyBytes = 0;
      co_return std::move(*cached);
    }
  }

  const uint32_t maxRetries = absl::GetFlag(FLAGS_atlas_http_max_retries);
  for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
    maybeCancel(cancellationToken);

    proxygen::coro::HTTPClientConnectionCache* connCache = m_directConnCache.get();
    bool usingProxyForAttempt = false;
    if (systemProxy) {
      const bool useProxy = [&]() {
        switch (proxyStrategy) {
          case HttpProxyStrategy::NoProxy:
            return false;
          case HttpProxyStrategy::ProxyIfAvailable:
            return true;
          case HttpProxyStrategy::Auto:
            // Alternate proxy/direct between attempts when a proxy exists (attempt 0 is proxy).
            return (attempt % 2u) == 0u;
        }
        return false;
      }();
      if (useProxy) {
        connCache = getOrCreateProxyCache(*systemProxy);
        usingProxyForAttempt = true;
      }
    }
    CHECK(connCache);

    auto attemptResult = co_await folly::coro::co_awaitTry([&]() -> folly::coro::Task<ZHttpGetBytesResult> {
      maybeCancel(cancellationToken);

      std::string currentUrl = request.url;
      const uint32_t maxRedirectHops = absl::GetFlag(FLAGS_atlas_http_max_redirect_hops);
      for (uint32_t hop = 0; hop <= maxRedirectHops; ++hop) {
        maybeCancel(cancellationToken);

        proxygen::URL parsedUrl(currentUrl);
        if (!parsedUrl.isValid() || parsedUrl.getHost().empty()) {
          throw ZException(fmt::format("Invalid URL '{}'", currentUrl));
        }

        bool isSecure = false;
        {
          std::string scheme(parsedUrl.getScheme());
          folly::toLowerAscii(scheme);
          isSecure = (scheme == "https");
        }

        proxygen::coro::HTTPCoroConnector::ConnectionParams connParams{};
        const proxygen::coro::HTTPCoroConnector::ConnectionParams* connParamsPtr = nullptr;
        if (isSecure) {
          CHECK(m_sslContext);
          connParams = proxygen::coro::HTTPCoroConnector::defaultConnectionParams();
          connParams.sslContext = m_sslContext;
          connParams.serverName = parsedUrl.getHost();
          connParamsPtr = &connParams;
        }

        folly::Optional<std::string> serverAddress = folly::none;
        if (!usingProxyForAttempt) {
          const std::string host(parsedUrl.getHost());
          CHECK(!host.empty());

          // Cache results per-host and rotate through returned addresses. This keeps
          // behavior closer to Proxygen's per-request resolution (and supports
          // multi-A records) while avoiding Proxygen's CoroDNSResolver / c-ares path.
          //
          // Rationale:
          // Under heavy timeout churn with some unstable servers (observed with S3-hosted
          // BossDB data), Proxygen's direct connect path that performs coroutine DNS
          // resolution via c-ares has exhibited reproducible SIGSEGV crashes (stacks
          // include ares_* frames and then crash during static init in
          // HTTPCommonHeaders::initNames(), consistent with memory corruption elsewhere).
          //
          // Do not revert to the "URL overload" below unless the underlying Proxygen/c-ares
          // issue is fixed:
          //   auto sessionRes = co_await connCache->getSessionWithReservation(currentUrl, timeout, connParamsPtr);
          auto it = m_directDnsCache.find(host);
          if (it == m_directDnsCache.end() || it->second.addresses.empty()) {
            DirectDnsCacheEntry entry{};
            entry.addresses = resolveHostToIpAddrsSystemViaQtThread(host, m_hostLookupInvoker);
            entry.nextIndex = 0;
            it = m_directDnsCache.emplace(host, std::move(entry)).first;
          }

          CHECK(!it->second.addresses.empty());
          const size_t idx = it->second.nextIndex % it->second.addresses.size();
          serverAddress = it->second.addresses[idx];
          ++it->second.nextIndex;
        }

        auto sessionRes = co_await connCache->getSessionWithReservation(parsedUrl.getHost(),
                                                                        parsedUrl.getPort(),
                                                                        isSecure,
                                                                        request.timeout,
                                                                        connParamsPtr,
                                                                        serverAddress);
        maybeCancel(cancellationToken);

        proxygen::coro::HTTPClient::Response response =
          co_await proxygen::coro::HTTPClient::get(sessionRes.session,
                                                   std::move(sessionRes.reservation),
                                                   parsedUrl,
                                                   request.timeout,
                                                   headerMap);

        CHECK(response.headers);
        const uint16_t status = response.headers->getStatusCode();
        const auto& headers = response.headers->getHeaders();

        if (isRedirectStatus(status)) {
          const std::string location = headers.getSingleOrEmpty("location");
          if (location.empty()) {
            throw ZException(fmt::format("HTTP {} redirect without Location header for '{}'", status, currentUrl));
          }
          if (hop == maxRedirectHops) {
            throw ZException(fmt::format("Too many HTTP redirects (>{}) while fetching '{}' (last Location='{}')",
                                         maxRedirectHops,
                                         currentUrl,
                                         location));
          }
          currentUrl = resolveRedirectUrl(currentUrl, location);
          continue;
        }

        ZHttpGetBytesResult out{};
        out.status = status;
        out.contentType = headers.getSingleOrEmpty("content-type");
        out.contentEncoding = headers.getSingleOrEmpty("content-encoding");

        auto bodyBuf = response.body.move();
        out.body = iobufToBytes(bodyBuf.get());
        out.encodedBodyBytes = static_cast<uint64_t>(out.body.size());
        out.source = ZHttpGetBytesSource::Network;

        // We depend on Range requests returning exact bytes; Content-Encoding would make the
        // payload length unpredictable relative to the requested byte range.
        if (isRangeRequest && !isIdentityContentEncoding(out.contentEncoding)) {
          throw ZException(fmt::format("HTTP Range response for '{}' used Content-Encoding='{}' (expected identity)",
                                       currentUrl,
                                       out.contentEncoding));
        }

        out.body = decompressIfNeeded(std::move(out.body), out.contentEncoding);
        co_return out;
      }

      CHECK(false);
      co_return ZHttpGetBytesResult{};
    }());

    if (attemptResult.hasValue()) {
      ZHttpGetBytesResult value = std::move(attemptResult).value();
      if (isMissingResourceHttpStatus(value.status)) {
        co_return std::nullopt;
      }
      if (isRetryableHttpStatus(value.status)) {
        if (attempt < maxRetries) {
          VLOG(1) << fmt::format("HTTP GET transient status (attempt {}/{}): '{}' (status {})",
                                 attempt + 1,
                                 maxRetries + 1,
                                 request.url,
                                 value.status);
          co_await folly::coro::sleepReturnEarlyOnCancel(httpRetryBackoffForAttempt(attempt));
          continue;
        }
        throw ZException(
          fmt::format("HTTP GET failed for '{}' (status {}): exhausted retries", request.url, value.status));
      }
      if (value.status >= 400) {
        throw ZException(fmt::format("HTTP GET failed for '{}' (status {})", request.url, value.status));
      }
      if (m_diskCache && m_diskCache->isEnabled()) {
        m_diskCache->put(request, value);
      }
      co_return std::move(value);
    }

    CHECK(attemptResult.hasException());
    folly::exception_wrapper error = std::move(attemptResult).exception();

    // A superseded render can cancel the request while proxygen is still reading
    // the response body. In that case some stacks surface a transport/read error
    // instead of folly::OperationCancelled. Once the request token is cancelled,
    // treat any failure as cancellation so higher layers reschedule rendering
    // rather than aborting the whole progressive frame.
    if (cancellationToken.isCancellationRequested() || error.is_compatible_with<folly::OperationCancelled>()) {
      // Folly coroutines report cancellation via OperationCancelled.
      // Convert to our cancellation type so callers treat it as expected control-flow.
      throw ZCancellationException();
    }

    if (const auto* httpErr = error.get_exception<proxygen::coro::HTTPError>(); httpErr != nullptr) {
      if (httpErr->httpMessage) {
        const uint16_t status = httpErr->httpMessage->getStatusCode();
        if (isMissingResourceHttpStatus(status)) {
          co_return std::nullopt;
        }
      }

      const bool retryable = isRetryableHttpError(*httpErr);
      if (attempt < maxRetries && retryable) {
        VLOG(1) << fmt::format("HTTP GET transient error (attempt {}/{}): '{}' ({})",
                               attempt + 1,
                               maxRetries + 1,
                               request.url,
                               httpErr->describe());
        co_await folly::coro::sleepReturnEarlyOnCancel(httpRetryBackoffForAttempt(attempt));
        continue;
      }

      std::string msg(httpErr->what());
      if (looksLikeTlsTrustStoreError(msg)) {
        msg += fmt::format(" (trust source: '{}'; CA bundle: '{}'; override with --atlas_http_ca_bundle=...)",
                           m_trustSourceDescription,
                           m_caBundlePath.empty() ? "<default trust store>" : m_caBundlePath);
      }
      throw ZException(fmt::format("HTTP GET failed for '{}': {}", request.url, msg));
    }

    // Fall back to message-based retry heuristics for non-proxygen errors.
    std::string msg(error.what().toStdString());
    const bool retryable = error.is_compatible_with<proxygen::coro::HTTPCoroSessionPool::Exception>() ||
                           isRetryableHttpExceptionMessage(msg);
    if (attempt < maxRetries && retryable) {
      VLOG(1) << fmt::format("HTTP GET transient exception (attempt {}/{}): '{}' ({})",
                             attempt + 1,
                             maxRetries + 1,
                             request.url,
                             msg);
      co_await folly::coro::sleepReturnEarlyOnCancel(httpRetryBackoffForAttempt(attempt));
      continue;
    }

    if (looksLikeTlsTrustStoreError(msg)) {
      msg += fmt::format(" (trust source: '{}'; CA bundle: '{}'; override with --atlas_http_ca_bundle=...)",
                         m_trustSourceDescription,
                         m_caBundlePath.empty() ? "<default trust store>" : m_caBundlePath);
    }
    throw ZException(fmt::format("HTTP GET failed for '{}': {}", request.url, msg));
  }

  throw ZException(fmt::format("HTTP GET failed for '{}': exhausted retries", request.url));
}

} // namespace nim
