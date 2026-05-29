#include "zhttptruststore.h"

#include "zexception.h"
#include "zabslflagtypes.h"
#include "zioutils.h"
#include "zlog.h"
#include "zsysteminfo.h"

#include "zcommandlineflags.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include "zwindowsheader.h"

#include <wincrypt.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

ABSL_DECLARE_FLAG(std::optional<std::string>, atlas_http_ca_bundle);

namespace nim {

enum class HttpWindowsTrustSource
{
  Auto,
  WindowsStore,
  BundledPem,
};

inline constexpr std::array<AbslEnumFlagValue<HttpWindowsTrustSource>, 3> kHttpWindowsTrustSourceFlagValues{
  {
   {"auto", HttpWindowsTrustSource::Auto},
   {"windows_store", HttpWindowsTrustSource::WindowsStore},
   {"bundled_pem", HttpWindowsTrustSource::BundledPem},
   }
};

inline bool AbslParseFlag(absl::string_view text, HttpWindowsTrustSource* value, std::string* error)
{
  return parseAbslEnumFlag(text, value, error, "HttpWindowsTrustSource", kHttpWindowsTrustSourceFlagValues);
}

inline std::string AbslUnparseFlag(HttpWindowsTrustSource value)
{
  return unparseAbslEnumFlag(value, kHttpWindowsTrustSourceFlagValues);
}

} // namespace nim

ABSL_FLAG(nim::HttpWindowsTrustSource,
          atlas_http_windows_trust_source,
          nim::HttpWindowsTrustSource::Auto,
          "Windows HTTPS trust source for remote datasets. Values: auto (prefer exported Windows system trust store, "
          "fall back to PEM bundle), windows_store (export Windows ROOT/CA stores to PEM and use that for all HTTP "
          "backends), bundled_pem (use PEM bundle discovery such as curl-ca-bundle.crt).");

namespace nim {
namespace {

struct CaBundleCandidate
{
  QString path;
  std::string sourceDescription;
};

std::optional<CaBundleCandidate> appLocalPemBundleCandidate()
{
  if (QCoreApplication::instance() == nullptr) {
    return std::nullopt;
  }

  const QString appBundlePath =
    QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("curl-ca-bundle.crt"));
  if (!QFileInfo::exists(appBundlePath)) {
    return std::nullopt;
  }

  return CaBundleCandidate{appBundlePath, "app-local curl-ca-bundle.crt"};
}

std::vector<CaBundleCandidate> systemPemBundleCandidates()
{
  std::vector<CaBundleCandidate> paths;

  paths.push_back(CaBundleCandidate{QStringLiteral("/etc/ssl/cert.pem"), "system:/etc/ssl/cert.pem"});
  paths.push_back(CaBundleCandidate{QStringLiteral("/etc/ssl/certs/ca-certificates.crt"),
                                    "system:/etc/ssl/certs/ca-certificates.crt"});
  paths.push_back(CaBundleCandidate{QStringLiteral("/etc/pki/tls/cert.pem"), "system:/etc/pki/tls/cert.pem"});
  paths.push_back(CaBundleCandidate{QStringLiteral("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"),
                                    "system:/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"});
  paths.push_back(
    CaBundleCandidate{QStringLiteral("/etc/ssl/certs/ca-bundle.crt"), "system:/etc/ssl/certs/ca-bundle.crt"});
  paths.push_back(
    CaBundleCandidate{QStringLiteral("/usr/local/etc/openssl@3/cert.pem"), "system:/usr/local/etc/openssl@3/cert.pem"});
  paths.push_back(CaBundleCandidate{QStringLiteral("/opt/homebrew/etc/openssl@3/cert.pem"),
                                    "system:/opt/homebrew/etc/openssl@3/cert.pem"});
  paths.push_back(
    CaBundleCandidate{QStringLiteral("/usr/local/etc/openssl/cert.pem"), "system:/usr/local/etc/openssl/cert.pem"});

  return paths;
}

std::optional<CaBundleCandidate> findReadableCandidate(const std::vector<CaBundleCandidate>& candidates)
{
  for (const auto& candidate : candidates) {
    if (isReadableFile(candidate.path)) {
      return candidate;
    }
  }
  return std::nullopt;
}

#if defined(_WIN32)
QByteArray exportWindowsCertStoreToPem(const char* storeName, int& addedCerts, int& failedCerts)
{
  CHECK(storeName);

  QByteArray pemBytes;
  std::unordered_set<std::string> seenDer;

  HCERTSTORE storeHandle = CertOpenSystemStoreA(static_cast<HCRYPTPROV_LEGACY>(0), storeName);
  if (!storeHandle) {
    throw ZException(fmt::format("CertOpenSystemStoreA('{}') failed", storeName));
  }

  PCCERT_CONTEXT certCtx = nullptr;
  while ((certCtx = CertEnumCertificatesInStore(storeHandle, certCtx)) != nullptr) {
    const char* derPtr = reinterpret_cast<const char*>(certCtx->pbCertEncoded);
    const size_t derLen = static_cast<size_t>(certCtx->cbCertEncoded);
    std::string der(derPtr, derPtr + derLen);
    if (!seenDer.emplace(der).second) {
      continue;
    }

    const unsigned char* encoded = reinterpret_cast<const unsigned char*>(certCtx->pbCertEncoded);
    X509* rawX509 = d2i_X509(nullptr, &encoded, static_cast<long>(certCtx->cbCertEncoded));
    if (!rawX509) {
      ++failedCerts;
      ERR_clear_error();
      continue;
    }
    std::unique_ptr<X509, decltype(&X509_free)> x509(rawX509, &X509_free);

    BIO* rawBio = BIO_new(BIO_s_mem());
    if (!rawBio) {
      ++failedCerts;
      ERR_clear_error();
      continue;
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(rawBio, &BIO_free);

    if (PEM_write_bio_X509(bio.get(), x509.get()) != 1) {
      ++failedCerts;
      ERR_clear_error();
      continue;
    }

    char* pemPtr = nullptr;
    const long pemLen = BIO_get_mem_data(bio.get(), &pemPtr);
    if (pemLen <= 0 || pemPtr == nullptr) {
      ++failedCerts;
      continue;
    }

    pemBytes.append(pemPtr, static_cast<qsizetype>(pemLen));
    ++addedCerts;
  }

  CertCloseStore(storeHandle, /*dwFlags=*/0);
  return pemBytes;
}

ZHttpTrustStoreConfig exportWindowsSystemTrustStoreToPemOrThrow()
{
  int added = 0;
  int failed = 0;
  QByteArray pemBytes;

  // Match the prior Proxygen behavior by exporting both the ROOT and CA stores.
  pemBytes += exportWindowsCertStoreToPem("ROOT", added, failed);
  pemBytes += exportWindowsCertStoreToPem("CA", added, failed);

  if (added == 0 || pemBytes.isEmpty()) {
    throw ZException(
      fmt::format("Failed to export any certificates from the Windows ROOT/CA trust stores (failedCerts={})", failed));
  }

  QDir outDir = ZSystemInfo::configDir();
  if (!outDir.exists()) {
    CHECK(outDir.mkpath("."));
  }
  const QString outPath = outDir.filePath(QStringLiteral("atlas_windows_system_trust_store.pem"));

  QSaveFile file(outPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    throw ZException(fmt::format("Failed to open '{}' for writing exported Windows trust store PEM", outPath));
  }
  if (file.write(pemBytes) != pemBytes.size()) {
    throw ZException(fmt::format("Failed to write exported Windows trust store PEM to '{}'", outPath));
  }
  if (!file.commit()) {
    throw ZException(fmt::format("Failed to commit exported Windows trust store PEM to '{}'", outPath));
  }

  ZHttpTrustStoreConfig out{};
  out.caBundlePath = outPath.toStdString();
  out.sourceDescription = fmt::format("windows_store(ROOT+CA, added={}, failed={})", added, failed);
  return out;
}
#endif

} // namespace

ZHttpTrustStoreConfig resolveHttpTrustStoreConfig(ZHttpTrustBackend backend)
{
  const std::optional<std::string> caBundle = absl::GetFlag(FLAGS_atlas_http_ca_bundle);
  if (caBundle.has_value()) {
    const QString caBundlePath = QString::fromStdString(*caBundle);
    if (!isReadableFile(caBundlePath)) {
      throw ZException(fmt::format("--atlas_http_ca_bundle points to an unreadable file: '{}'", *caBundle));
    }
    return ZHttpTrustStoreConfig{*caBundle, "flag:atlas_http_ca_bundle"};
  }

#if defined(_WIN32)
  (void)backend;
  const HttpWindowsTrustSource trustSource = absl::GetFlag(FLAGS_atlas_http_windows_trust_source);
  if (trustSource == HttpWindowsTrustSource::WindowsStore) {
    return exportWindowsSystemTrustStoreToPemOrThrow();
  }

  if (trustSource == HttpWindowsTrustSource::Auto) {
    try {
      return exportWindowsSystemTrustStoreToPemOrThrow();
    }
    catch (const std::exception& e) {
      LOG(WARNING) << fmt::format("Failed to export Windows trust store PEM; falling back to PEM bundle discovery: {}",
                                  e.what());
    }
  }

  if (auto candidate = appLocalPemBundleCandidate()) {
    return ZHttpTrustStoreConfig{candidate->path.toStdString(), candidate->sourceDescription};
  }

  if (auto candidate = findReadableCandidate(systemPemBundleCandidates())) {
    return ZHttpTrustStoreConfig{candidate->path.toStdString(), candidate->sourceDescription};
  }

  throw ZException(
    "Failed to resolve a Windows HTTPS trust source. Set --atlas_http_ca_bundle=... or install/deploy curl-ca-bundle.crt.");
#else
#if defined(__APPLE__)
  if (backend == ZHttpTrustBackend::Curl) {
    return ZHttpTrustStoreConfig{"", "backend default trust store (macOS curl native/default trust)"};
  }
#endif

  if (auto candidate = appLocalPemBundleCandidate()) {
    return ZHttpTrustStoreConfig{candidate->path.toStdString(), candidate->sourceDescription};
  }

  if (auto candidate = findReadableCandidate(systemPemBundleCandidates())) {
    return ZHttpTrustStoreConfig{candidate->path.toStdString(), candidate->sourceDescription};
  }

  return ZHttpTrustStoreConfig{"", "backend default trust store"};
#endif
}

} // namespace nim
