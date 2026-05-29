#include "zcurlhttpclient.h"

#include "zcancellation.h"
#include "zdiskcacheutils.h"
#include "zexception.h"
#include "zhttpdiskcache.h"
#include "zhttpsystemproxy.h"
#include "zhttpretrypolicy.h"
#include "zhttptruststore.h"
#include "zfolly.h"
#include "zlog.h"

#include "zcommandlineflags.h"

#include <curl/curl.h>

#include <folly/CancellationToken.h>
#include <folly/String.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Sleep.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string_view>

ABSL_DECLARE_FLAG(nim::HttpProxyStrategy, atlas_http_proxy_strategy);
ABSL_DECLARE_FLAG(uint32_t, atlas_http_max_redirect_hops);
ABSL_DECLARE_FLAG(uint32_t, atlas_http_max_retries);
ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_http_max_bytes);

namespace nim {
namespace {

constexpr ZHttpProxySupport kCurlProxySupport{
  .supportsHttp = true,
  .supportsSocks5 = true,
  .supportsAuthentication = true,
};

class CurlRequestException : public ZException
{
public:
  CurlRequestException(CURLcode curlCode, std::string message)
    : ZException(std::move(message))
    , code(curlCode)
  {}

  CURLcode code;
};

struct CurlResponseHeaders
{
  std::string contentType;
  std::string contentEncoding;
};

struct CurlProgressState
{
  std::atomic_bool cancelled{false};
};

std::vector<std::string> parseContentEncodings(std::string_view contentEncoding)
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

bool isIdentityContentEncoding(std::string_view contentEncoding)
{
  const auto encs = parseContentEncodings(contentEncoding);
  if (encs.empty()) {
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

bool isMissingResourceHttpStatus(uint16_t status)
{
  return status == 403 || status == 404;
}

bool curlFeatureEnabled(const curl_version_info_data* info, long featureBit)
{
  CHECK(info);
  return (info->features & featureBit) != 0;
}

std::string joinNonEmpty(const std::vector<std::string>& values, std::string_view separator)
{
  std::string out;
  for (const auto& value : values) {
    if (value.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += separator;
    }
    out += value;
  }
  return out;
}

std::string curlFeatureSummary(const curl_version_info_data* info)
{
  CHECK(info);
  std::vector<std::string> features;
  features.reserve(10);

  if (curlFeatureEnabled(info, CURL_VERSION_SSL)) {
    features.emplace_back("ssl");
  }
  if (curlFeatureEnabled(info, CURL_VERSION_LIBZ)) {
    features.emplace_back("libz");
  }
#ifdef CURL_VERSION_BROTLI
  if (curlFeatureEnabled(info, CURL_VERSION_BROTLI)) {
    features.emplace_back("brotli");
  }
#endif
#ifdef CURL_VERSION_ZSTD
  if (curlFeatureEnabled(info, CURL_VERSION_ZSTD)) {
    features.emplace_back("zstd");
  }
#endif
#ifdef CURL_VERSION_HTTP2
  if (curlFeatureEnabled(info, CURL_VERSION_HTTP2)) {
    features.emplace_back("http2");
  }
#endif
#ifdef CURL_VERSION_HTTP3
  if (curlFeatureEnabled(info, CURL_VERSION_HTTP3)) {
    features.emplace_back("http3");
  }
#endif
#ifdef CURL_VERSION_HTTPS_PROXY
  if (curlFeatureEnabled(info, CURL_VERSION_HTTPS_PROXY)) {
    features.emplace_back("https-proxy");
  }
#endif
#ifdef CURL_VERSION_ASYNCHDNS
  if (curlFeatureEnabled(info, CURL_VERSION_ASYNCHDNS)) {
    features.emplace_back("asynchdns");
  }
#endif
  if (curlFeatureEnabled(info, CURL_VERSION_IPV6)) {
    features.emplace_back("ipv6");
  }

  return joinNonEmpty(features, ",");
}

std::string curlProtocolSummary(const curl_version_info_data* info)
{
  CHECK(info);
  std::vector<std::string> protocols;
  if (info->protocols != nullptr) {
    for (const char* const* p = info->protocols; *p != nullptr; ++p) {
      protocols.emplace_back(*p);
    }
  }
  return joinNonEmpty(protocols, ",");
}

std::string proxyKindName(ZHttpProxyKind kind)
{
  switch (kind) {
    case ZHttpProxyKind::Http:
      return "http";
    case ZHttpProxyKind::Socks5:
      return "socks5";
  }
  return "unknown";
}

std::string proxyDescription(const ZHttpProxyEndpoint& proxy)
{
  return fmt::format("{}://{}:{} auth={}",
                     proxyKindName(proxy.kind),
                     proxy.host,
                     proxy.port,
                     (!proxy.username.empty() || !proxy.password.empty()) ? "present" : "none");
}

bool looksLikeTlsTrustStoreError(std::string_view message)
{
  std::string m(message);
  folly::toLowerAscii(m);
  return m.find("certificate") != std::string::npos || m.find("ca cert") != std::string::npos ||
         m.find("ssl") != std::string::npos || m.find("tls") != std::string::npos;
}

void configureCurlProxy(CURL* handle, const ZHttpProxyEndpoint& proxy)
{
  CHECK(handle);

  const std::string proxyTarget = fmt::format("{}:{}", proxy.host, proxy.port);
  long proxyType = CURLPROXY_HTTP;
  switch (proxy.kind) {
    case ZHttpProxyKind::Http:
      proxyType = CURLPROXY_HTTP;
      break;
    case ZHttpProxyKind::Socks5:
      proxyType = CURLPROXY_SOCKS5_HOSTNAME;
      break;
  }

  curl_easy_setopt(handle, CURLOPT_PROXYTYPE, proxyType);
  curl_easy_setopt(handle, CURLOPT_PROXY, proxyTarget.c_str());
  if (!proxy.username.empty() || !proxy.password.empty()) {
    curl_easy_setopt(handle, CURLOPT_PROXYUSERNAME, proxy.username.c_str());
    curl_easy_setopt(handle, CURLOPT_PROXYPASSWORD, proxy.password.c_str());
    if (proxy.kind == ZHttpProxyKind::Http) {
      curl_easy_setopt(handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    }
  }
}

bool isRetryableCurlCode(CURLcode code)
{
  switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_HTTP2:
#ifdef CURLE_HTTP2_STREAM
    case CURLE_HTTP2_STREAM:
#endif
#ifdef CURLE_HTTP3
    case CURLE_HTTP3:
#endif
#ifdef CURLE_QUIC_CONNECT_ERROR
    case CURLE_QUIC_CONNECT_ERROR:
#endif
#ifdef CURLE_PROXY
    case CURLE_PROXY:
#endif
      return true;
    default:
      return false;
  }
}

bool requestUsesProxy(HttpProxyStrategy strategy, bool proxyAvailable, uint32_t attempt)
{
  if (!proxyAvailable) {
    return false;
  }
  switch (strategy) {
    case HttpProxyStrategy::NoProxy:
      return false;
    case HttpProxyStrategy::ProxyIfAvailable:
      return true;
    case HttpProxyStrategy::Auto:
      return (attempt % 2u) == 0u;
  }
  return false;
}

size_t writeBodyCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  CHECK(ptr);
  CHECK(userdata);
  const size_t bytes = size * nmemb;
  auto* body = static_cast<std::vector<uint8_t>*>(userdata);
  const size_t oldSize = body->size();
  body->resize(oldSize + bytes);
  std::memcpy(body->data() + oldSize, ptr, bytes);
  return bytes;
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

size_t writeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  CHECK(buffer);
  CHECK(userdata);
  const size_t bytes = size * nitems;
  auto* headers = static_cast<CurlResponseHeaders*>(userdata);

  std::string_view line(buffer, bytes);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return bytes;
  }

  if (line.rfind("HTTP/", 0) == 0) {
    headers->contentType.clear();
    headers->contentEncoding.clear();
    return bytes;
  }

  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return bytes;
  }

  std::string key(line.substr(0, colon));
  folly::toLowerAscii(key);
  const std::string value = trimAscii(line.substr(colon + 1));
  if (key == "content-type") {
    headers->contentType = value;
  } else if (key == "content-encoding") {
    headers->contentEncoding = value;
  }

  return bytes;
}

int transferProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  auto* state = static_cast<CurlProgressState*>(clientp);
  CHECK(state);
  return state->cancelled.load(std::memory_order_relaxed) ? 1 : 0;
}

ZHttpGetBytesResult performCurlRequestBlocking(const ZHttpGetRequest& request,
                                               const std::optional<ZHttpProxyEndpoint>& proxy,
                                               bool useProxy,
                                               const std::string& caBundlePath,
                                               const folly::CancellationToken& cancellationToken)
{
  CURL* rawHandle = curl_easy_init();
  if (!rawHandle) {
    throw ZException("curl_easy_init failed");
  }
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(rawHandle, &curl_easy_cleanup);

  curl_slist* rawHeaderList = nullptr;
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerList(rawHeaderList, &curl_slist_free_all);

  std::vector<uint8_t> body;
  CurlResponseHeaders responseHeaders;
  CurlProgressState progressState;
  folly::CancellationCallback cancellationCallback(cancellationToken, [&progressState]() {
    progressState.cancelled.store(true, std::memory_order_relaxed);
  });

  const bool isRangeRequest = request.exactByteRange.has_value();
  const std::vector<std::pair<std::string, std::string>> requestHeaders = httpRequestHeadersForTransport(request);
  std::optional<std::string> acceptEncodingHeader;
  for (const auto& [k, v] : requestHeaders) {
    std::string keyLower(k);
    folly::toLowerAscii(keyLower);
    if (keyLower == "accept-encoding") {
      acceptEncodingHeader = v;
      continue;
    }

    const std::string headerLine = fmt::format("{}: {}", k, v);
    curl_slist* next = curl_slist_append(headerList.get(), headerLine.c_str());
    if (!next) {
      throw ZException("curl_slist_append failed");
    }
    headerList.release();
    headerList.reset(next);
  }

  const std::string acceptEncoding = isRangeRequest ? "identity" : acceptEncodingHeader.value_or("");

  std::array<char, CURL_ERROR_SIZE> errorBuffer{};

  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle.get(),
                   CURLOPT_MAXREDIRS,
                   static_cast<long>(absl::GetFlag(FLAGS_atlas_http_max_redirect_hops)));
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, acceptEncoding.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &writeBodyCallback);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, &writeHeaderCallback);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &responseHeaders);
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &transferProgressCallback);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progressState);
  curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());

  if (headerList) {
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headerList.get());
  }

  if (!caBundlePath.empty()) {
    curl_easy_setopt(handle.get(), CURLOPT_CAINFO, caBundlePath.c_str());
  }

  if (useProxy) {
    CHECK(proxy.has_value());
    configureCurlProxy(handle.get(), *proxy);
    VLOG(1) << fmt::format("Using system proxy via curl: {}", proxyDescription(*proxy));
  } else {
    curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
  }

  const CURLcode res = curl_easy_perform(handle.get());
  if (res != CURLE_OK) {
    if (progressState.cancelled.load(std::memory_order_relaxed) || res == CURLE_ABORTED_BY_CALLBACK) {
      throw ZCancellationException();
    }

    std::string message =
      errorBuffer.data()[0] != '\0' ? std::string(errorBuffer.data()) : std::string(curl_easy_strerror(res));
    throw CurlRequestException(res, fmt::format("curl GET failed for '{}': {}", request.url, message));
  }

  long statusCode = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &statusCode);

  curl_off_t encodedBytes = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_SIZE_DOWNLOAD_T, &encodedBytes);

  ZHttpGetBytesResult out{};
  out.status = static_cast<uint16_t>(statusCode);
  out.contentType = responseHeaders.contentType;
  out.contentEncoding = responseHeaders.contentEncoding.empty() ? "identity" : responseHeaders.contentEncoding;
  out.body = std::move(body);
  out.encodedBodyBytes = encodedBytes > 0 ? static_cast<uint64_t>(encodedBytes) : 0;
  out.source = ZHttpGetBytesSource::Network;

  if (isRangeRequest && !isIdentityContentEncoding(out.contentEncoding)) {
    throw ZException(fmt::format("HTTP Range response for '{}' used Content-Encoding='{}' (expected identity)",
                                 request.url,
                                 out.contentEncoding));
  }

  if (!isRangeRequest) {
    out.contentEncoding = "identity";
  }

  return out;
}

} // namespace

ZCurlHttpClient& ZCurlHttpClient::instance()
{
  static ZCurlHttpClient client;
  return client;
}

ZCurlHttpClient::ZCurlHttpClient()
{
  const CURLcode initResult = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initResult != CURLE_OK) {
    throw ZException(fmt::format("curl_global_init failed: {}", curl_easy_strerror(initResult)));
  }

  const curl_version_info_data* curlInfo = curl_version_info(CURLVERSION_NOW);
  CHECK(curlInfo);

  const ZHttpTrustStoreConfig trustStore = resolveHttpTrustStoreConfig(ZHttpTrustBackend::Curl);
  m_caBundlePath = trustStore.caBundlePath;
  m_trustSourceDescription = trustStore.sourceDescription;

  LOG(INFO) << fmt::format(
    "curl backend runtime: libcurl='{}' ssl='{}' libz='{}' features='{}' protocols='{}' trustSource='{}' caBundle='{}'",
    curlInfo->version != nullptr ? curlInfo->version : "<unknown>",
    curlInfo->ssl_version != nullptr ? curlInfo->ssl_version : "<unknown>",
    curlInfo->libz_version != nullptr ? curlInfo->libz_version : "<none>",
    curlFeatureSummary(curlInfo),
    curlProtocolSummary(curlInfo),
    m_trustSourceDescription,
    m_caBundlePath.empty() ? "<default trust store>" : m_caBundlePath);

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
}

ZCurlHttpClient::~ZCurlHttpClient()
{
  curl_global_cleanup();
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZCurlHttpClient::getBytes(ZHttpGetRequest request)
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

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

  const HttpProxyStrategy proxyStrategy = absl::GetFlag(FLAGS_atlas_http_proxy_strategy);
  std::optional<ZHttpProxyEndpoint> systemProxy;
  if (proxyStrategy != HttpProxyStrategy::NoProxy) {
    const ZSystemHttpProxyResolution proxyResolution = querySystemHttpProxyForUrl(request.url, kCurlProxySupport);
    if (proxyResolution.error) {
      throw ZException(fmt::format("curl backend: {}", *proxyResolution.error));
    }
    for (const auto& warning : proxyResolution.warnings) {
      LOG(WARNING) << "curl backend: " << warning
                   << " Proceeding with direct connection because the OS proxy settings explicitly allow DIRECT.";
    }
    systemProxy = proxyResolution.endpoint;
  }

  const uint32_t maxRetries = absl::GetFlag(FLAGS_atlas_http_max_retries);
  for (uint32_t attempt = 0; attempt <= maxRetries; ++attempt) {
    maybeCancel(cancellationToken);

    const bool useProxy = requestUsesProxy(proxyStrategy, systemProxy.has_value(), attempt);
    auto attemptResult = co_await folly::coro::co_awaitTry(folly::coro::co_withExecutor(
      getAtlasBackgroundExecutor(),
      [this, request, systemProxy, useProxy, cancellationToken]() -> folly::coro::Task<ZHttpGetBytesResult> {
        co_await folly::coro::co_reschedule_on_current_executor;
        co_return performCurlRequestBlocking(request, systemProxy, useProxy, m_caBundlePath, cancellationToken);
      }()));

    if (attemptResult.hasValue()) {
      ZHttpGetBytesResult value = std::move(attemptResult).value();
      if (isMissingResourceHttpStatus(value.status)) {
        co_return std::nullopt;
      }
      if (isRetryableHttpStatus(value.status)) {
        if (attempt < maxRetries) {
          VLOG(1) << fmt::format("curl HTTP GET transient status (attempt {}/{}): '{}' (status {})",
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
    if (cancellationToken.isCancellationRequested() || error.is_compatible_with<ZCancellationException>()) {
      throw ZCancellationException();
    }

    if (const auto* curlErr = error.get_exception<CurlRequestException>(); curlErr != nullptr) {
      const bool retryable = isRetryableCurlCode(curlErr->code) || isRetryableHttpExceptionMessage(curlErr->what());
      if (attempt < maxRetries && retryable) {
        VLOG(1) << fmt::format("curl HTTP GET transient error (attempt {}/{}): '{}' ({})",
                               attempt + 1,
                               maxRetries + 1,
                               request.url,
                               curlErr->what());
        co_await folly::coro::sleepReturnEarlyOnCancel(httpRetryBackoffForAttempt(attempt));
        continue;
      }

      std::string msg(curlErr->what());
      if (looksLikeTlsTrustStoreError(msg)) {
        msg += fmt::format(" (trust source: '{}'; CA bundle: '{}'; override with --atlas_http_ca_bundle=...)",
                           m_trustSourceDescription,
                           m_caBundlePath.empty() ? "<default trust store>" : m_caBundlePath);
      }
      throw ZException(msg);
    }

    std::string msg(error.what().toStdString());
    const bool retryable = isRetryableHttpExceptionMessage(msg);
    if (attempt < maxRetries && retryable) {
      VLOG(1) << fmt::format("curl HTTP GET transient exception (attempt {}/{}): '{}' ({})",
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
