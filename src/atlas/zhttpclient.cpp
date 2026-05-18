#include "zhttpclient.h"

#include "zcurlhttpclient.h"
#include "zproxygenhttpclient.h"
#include "zlog.h"

#include "zcommandlineflags.h"

#include <folly/String.h>

#include <limits>

namespace {

#ifdef _WIN32
constexpr const char* kDefaultAtlasHttpBackend = "curl";
#else
constexpr const char* kDefaultAtlasHttpBackend = "proxygen";
#endif

} // namespace

ABSL_FLAG(std::string,
          atlas_http_backend,
          kDefaultAtlasHttpBackend,
          "HTTP backend for remote datasets. Values: proxygen, curl.");

namespace nim {
namespace {

[[nodiscard]] bool hasHeaderLowerKey(const std::vector<std::pair<std::string, std::string>>& headers,
                                     std::string_view keyLowerAscii)
{
  for (const auto& [key, value] : headers) {
    (void)value;
    std::string lowered = key;
    folly::toLowerAscii(lowered);
    if (lowered == keyLowerAscii) {
      return true;
    }
  }
  return false;
}

enum class HttpBackendKind
{
  Proxygen,
  Curl,
};

HttpBackendKind backendKindFromFlag()
{
  std::string backend = absl::GetFlag(FLAGS_atlas_http_backend);
  folly::toLowerAscii(backend);
  if (backend.empty() || backend == "proxygen") {
    return HttpBackendKind::Proxygen;
  }
  if (backend == "curl") {
    return HttpBackendKind::Curl;
  }
  throw ZException(fmt::format("Invalid --atlas_http_backend='{}' (expected: proxygen or curl)",
                               absl::GetFlag(FLAGS_atlas_http_backend)));
}

} // namespace

std::string formatHttpByteRangeHeaderValue(const ZHttpByteRange& range)
{
  CHECK(range.length > 0) << "Exact byte-range requests must have positive length";
  CHECK(range.offset <= std::numeric_limits<uint64_t>::max() - (range.length - 1))
    << "Exact byte-range request overflows uint64_t";
  return fmt::format("bytes={}-{}", range.offset, range.offset + range.length - 1);
}

std::vector<std::pair<std::string, std::string>> httpRequestHeadersForTransport(const ZHttpGetRequest& request)
{
  CHECK(!hasHeaderLowerKey(request.headers, "range"))
    << "Exact byte-range requests must use ZHttpGetRequest::exactByteRange, not a raw Range header";

  std::vector<std::pair<std::string, std::string>> headers = request.headers;
  if (request.exactByteRange.has_value()) {
    headers.emplace_back("range", formatHttpByteRangeHeaderValue(*request.exactByteRange));
  }
  return headers;
}

ZHttpClient& ZHttpClient::instance()
{
  static ZHttpClient client;
  return client;
}

folly::coro::Task<std::optional<ZHttpGetBytesResult>> ZHttpClient::getBytes(ZHttpGetRequest request)
{
  switch (backendKindFromFlag()) {
    case HttpBackendKind::Proxygen:
      co_return co_await ZProxygenHttpClient::instance().getBytes(std::move(request));
    case HttpBackendKind::Curl:
      co_return co_await ZCurlHttpClient::instance().getBytes(std::move(request));
  }

  CHECK(false);
  co_return std::nullopt;
}

} // namespace nim
