#pragma once

#include <folly/coro/Task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

enum class ZHttpGetBytesSource : std::uint8_t
{
  Unknown = 0,
  Network = 1,
  DiskCache = 2,
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

class ZHttpClient
{
public:
  static ZHttpClient& instance();

  folly::coro::Task<std::optional<ZHttpGetBytesResult>>
  getBytes(std::string url,
           std::chrono::milliseconds timeout,
           std::vector<std::pair<std::string, std::string>> requestHeaders = {});

private:
  ZHttpClient() = default;
};

} // namespace nim
