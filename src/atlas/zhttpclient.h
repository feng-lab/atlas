#pragma once

#include "zfolly.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nim {

enum class ZHttpGetBytesSource : std::uint8_t
{
  Unknown = 0,
  Network = 1,
  DiskCache = 2,
};

enum class ZHttpMissingResourcePolicy : std::uint8_t
{
  Treat403And404AsMissing,
  Treat404AsMissing,
};

struct ZHttpByteRange
{
  uint64_t offset = 0;
  uint64_t length = 0;

  auto operator<=>(const ZHttpByteRange&) const = default;
};

struct ZHttpGetRequest
{
  std::string url;
  std::chrono::milliseconds timeout{0};
  std::vector<std::pair<std::string, std::string>> headers;

  // When present, this request requires the exact byte range [offset, offset + length).
  // Transport backends synthesize the HTTP Range header from this field rather than
  // asking higher layers to encode range semantics in raw header text.
  std::optional<ZHttpByteRange> exactByteRange;

  // Non-secret partition for persistent cache entries. This lets provider-aware
  // object stores separate anonymous/public bytes from credential-scoped bytes
  // while keeping Authorization headers and tokens out of cache keys.
  std::string cachePartition;

  // Public object stores sometimes report absent objects as either 403 or 404.
  // Signed/private requests must usually preserve 403 as an access-denied error.
  ZHttpMissingResourcePolicy missingResourcePolicy = ZHttpMissingResourcePolicy::Treat403And404AsMissing;
};

struct ZHttpGetBytesResult
{
  uint16_t status = 0;
  std::string contentType;
  std::string contentEncoding;
  std::vector<uint8_t> body;

  // Size (in bytes) of the HTTP response body as received over the wire, before
  // applying Content-Encoding decompression. For disk-cache hits, this is 0
  // because no network I/O was performed.
  uint64_t encodedBodyBytes = 0;

  // Where the bytes were served from. This is primarily intended for telemetry
  // and debugging; core logic should use HTTP status codes and content fields.
  ZHttpGetBytesSource source = ZHttpGetBytesSource::Unknown;
};

[[nodiscard]] std::string formatHttpByteRangeHeaderValue(const ZHttpByteRange& range);

[[nodiscard]] std::vector<std::pair<std::string, std::string>>
httpRequestHeadersForTransport(const ZHttpGetRequest& request);

[[nodiscard]] bool isHttpMissingResourceStatus(const ZHttpGetRequest& request, uint16_t status);

class ZHttpClient
{
public:
  static ZHttpClient& instance();

  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request);

private:
  ZHttpClient() = default;
};

} // namespace nim
