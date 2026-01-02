#include "zproxygenhttpclient.h"

#include <folly/String.h>
#include <folly/compression/Compression.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <proxygen/lib/http/coro/client/HTTPCoroConnector.h>
#include <proxygen/lib/http/HTTPHeaders.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/coro/HTTPError.h>
#include <proxygen/lib/http/coro/client/HTTPClient.h>
#include <proxygen/lib/utils/URL.h>

#include <gflags/gflags.h>

#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
#include <wincrypt.h>
#endif

DEFINE_string(atlas_http_ca_bundle,
              "",
              "Path to a PEM CA bundle for HTTPS requests (overrides auto-detect; also respects env SSL_CERT_FILE).");

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

#if defined(_WIN32)
bool tryAddWindowsSystemCertStoreTo(/*inout*/ folly::SSLContext& ctx, const char* storeName, int& addedCerts, int& failedCerts)
{
  CHECK(storeName);

  HCERTSTORE storeHandle = CertOpenSystemStoreA(nullptr, storeName);
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

} // namespace

ZProxygenHttpClient& ZProxygenHttpClient::instance()
{
  static ZProxygenHttpClient client;
  return client;
}

ZProxygenHttpClient::ZProxygenHttpClient()
  : m_eventBaseThread("atlas_proxygen_http")
  , m_connCache(*m_eventBaseThread.getEventBase())
  , m_sslContext(makeClientSslContext(m_caBundlePath))
{}

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

  proxygen::coro::HTTPClient::RequestHeaderMap headerMap;
  headerMap.emplace("accept-encoding", "identity");
  for (auto& [k, v] : requestHeaders) {
    headerMap.emplace(std::move(k), std::move(v));
  }

  try {
    proxygen::coro::HTTPCoroConnector::ConnectionParams connParams{};
    const proxygen::coro::HTTPCoroConnector::ConnectionParams* connParamsPtr = nullptr;
    if (isSecure) {
      CHECK(m_sslContext);
      connParams = proxygen::coro::HTTPCoroConnector::defaultConnectionParams();
      connParams.sslContext = m_sslContext;
      connParams.serverName = parsedUrl.getHost();
      connParamsPtr = &connParams;
    }

    auto sessionRes = co_await m_connCache.getSessionWithReservation(url, timeout, connParamsPtr);
    auto response =
      co_await proxygen::coro::HTTPClient::get(sessionRes.session, std::move(sessionRes.reservation), parsedUrl, timeout, std::move(headerMap));

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
  }
  catch (const proxygen::coro::HTTPError& e) {
    if (e.httpMessage && e.httpMessage->getStatusCode() == 404) {
      co_return std::nullopt;
    }
    std::string msg = e.what();
    if (isSecure && looksLikeTlsTrustStoreError(msg)) {
      msg += fmt::format(" (CA bundle: '{}'; override with --atlas_http_ca_bundle=... or env SSL_CERT_FILE)",
                         m_caBundlePath.empty() ? "<auto>" : m_caBundlePath);
    }
    throw ZException(fmt::format("HTTP GET failed for '{}': {}", url, msg));
  }
  catch (const std::exception& e) {
    std::string msg = e.what();
    if (isSecure && looksLikeTlsTrustStoreError(msg)) {
      msg += fmt::format(" (CA bundle: '{}'; override with --atlas_http_ca_bundle=... or env SSL_CERT_FILE)",
                         m_caBundlePath.empty() ? "<auto>" : m_caBundlePath);
    }
    throw ZException(fmt::format("HTTP GET failed for '{}': {}", url, msg));
  }
}

} // namespace nim
