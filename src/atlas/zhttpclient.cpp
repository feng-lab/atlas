#include "zhttpclient.h"

#include "zcurlhttpclient.h"
#include "zproxygenhttpclient.h"
#include "zlog.h"

#include <gflags/gflags.h>

#include <folly/String.h>

namespace {

#ifdef _WIN32
constexpr const char* kDefaultAtlasHttpBackend = "curl";
#else
constexpr const char* kDefaultAtlasHttpBackend = "proxygen";
#endif

} // namespace

DEFINE_string(atlas_http_backend,
              kDefaultAtlasHttpBackend,
              "HTTP backend for remote datasets. Values: proxygen, curl.");

namespace nim {
namespace {

enum class HttpBackendKind
{
  Proxygen,
  Curl,
};

HttpBackendKind backendKindFromFlag()
{
  std::string backend = FLAGS_atlas_http_backend;
  folly::toLowerAscii(backend);
  if (backend.empty() || backend == "proxygen") {
    return HttpBackendKind::Proxygen;
  }
  if (backend == "curl") {
    return HttpBackendKind::Curl;
  }
  throw ZException(
    fmt::format("Invalid --atlas_http_backend='{}' (expected: proxygen or curl)", FLAGS_atlas_http_backend));
}

} // namespace

ZHttpClient& ZHttpClient::instance()
{
  static ZHttpClient client;
  return client;
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>>
ZHttpClient::getBytes(std::string url,
                      std::chrono::milliseconds timeout,
                      std::vector<std::pair<std::string, std::string>> requestHeaders)
{
  switch (backendKindFromFlag()) {
    case HttpBackendKind::Proxygen:
      co_return co_await ZProxygenHttpClient::instance().getBytes(std::move(url), timeout, std::move(requestHeaders));
    case HttpBackendKind::Curl:
      co_return co_await ZCurlHttpClient::instance().getBytes(std::move(url), timeout, std::move(requestHeaders));
  }

  CHECK(false);
  co_return std::nullopt;
}

} // namespace nim
