#include "zhttpclient.h"

#include "zcurlhttpclient.h"
#include "zproxygenhttpclient.h"
#include "zabslflagtypes.h"
#include "zlog.h"

#include "zcommandlineflags.h"

#include <folly/String.h>

#include <array>
#include <limits>

namespace nim {

enum class HttpBackend
{
  Proxygen,
  Curl,
};

inline constexpr std::array<AbslEnumFlagValue<HttpBackend>, 2> kHttpBackendFlagValues{
  {
   {"proxygen", HttpBackend::Proxygen},
   {"curl", HttpBackend::Curl},
   }
};

inline bool AbslParseFlag(absl::string_view text, HttpBackend* value, std::string* error)
{
  return parseAbslEnumFlag(text, value, error, "HttpBackend", kHttpBackendFlagValues);
}

inline std::string AbslUnparseFlag(HttpBackend value)
{
  return unparseAbslEnumFlag(value, kHttpBackendFlagValues);
}

#ifdef _WIN32
constexpr HttpBackend kDefaultAtlasHttpBackend = HttpBackend::Curl;
#else
constexpr HttpBackend kDefaultAtlasHttpBackend = HttpBackend::Proxygen;
#endif

} // namespace nim

ABSL_FLAG(nim::HttpBackend,
          atlas_http_backend,
          nim::kDefaultAtlasHttpBackend,
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
  switch (absl::GetFlag(FLAGS_atlas_http_backend)) {
    case HttpBackend::Proxygen:
      co_return co_await ZProxygenHttpClient::instance().getBytes(std::move(request));
    case HttpBackend::Curl:
      co_return co_await ZCurlHttpClient::instance().getBytes(std::move(request));
  }

  CHECK(false);
  co_return std::nullopt;
}

} // namespace nim
