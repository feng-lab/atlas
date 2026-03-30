#pragma once

#include <folly/coro/Task.h>

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

class ZHttpClient
{
public:
  static ZHttpClient& instance();

  folly::coro::Task<std::optional<ZHttpGetBytesResult>> getBytes(ZHttpGetRequest request);

private:
  ZHttpClient() = default;
};

} // namespace nim
