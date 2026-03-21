#include "zhttpretrypolicy.h"

#include <gflags/gflags.h>

#include <folly/String.h>

#include <algorithm>
#include <string>

DECLARE_uint32(atlas_http_retry_backoff_initial_ms);
DECLARE_uint32(atlas_http_retry_backoff_max_ms);

namespace nim {

std::chrono::milliseconds httpRetryBackoffForAttempt(uint32_t attempt)
{
  const uint32_t initial = FLAGS_atlas_http_retry_backoff_initial_ms;
  const uint32_t maxDelay = std::max<uint32_t>(initial, FLAGS_atlas_http_retry_backoff_max_ms);
  uint64_t delay = initial;
  delay <<= std::min<uint32_t>(attempt, 20u);
  if (delay > maxDelay) {
    delay = maxDelay;
  }
  return std::chrono::milliseconds(static_cast<int64_t>(delay));
}

bool isRetryableHttpExceptionMessage(std::string_view message)
{
  std::string m(message);
  folly::toLowerAscii(m);

  auto contains = [&](std::string_view needle) {
    return m.find(needle) != std::string::npos;
  };

  // TLS / transport interruptions.
  if (contains("handshake") || contains("network error") || contains("tls") || contains("ssl") || contains("eof") ||
      contains("unexpected eof")) {
    return true;
  }

  // Connect / route failures.
  if (contains("connect failed") || contains("connect error") || contains("connection refused") ||
      contains("no route to host") || contains("network is unreachable") || contains("host is down") ||
      contains("connection reset") || contains("broken pipe")) {
    return true;
  }

  // DNS / name resolution.
  if (contains("temporary failure in name resolution") || contains("name or service not known") ||
      contains("nodename nor servname provided") || contains("unknown host") || contains("host not found")) {
    return true;
  }

  // Timeout variants.
  if (contains("timeout") || contains("timed out")) {
    return true;
  }

  // Truncated or prematurely terminated transfers. This includes curl's
  // "Transferred a partial file" path and similar proxy/server truncation.
  if (contains("partial file") || contains("partial transfer") || contains("truncated") ||
      contains("transfer closed") || contains("outstanding read data remaining") ||
      contains("empty reply from server")) {
    return true;
  }

  return false;
}

bool isRetryableHttpStatus(uint16_t status)
{
  switch (status) {
    case 408:
    case 421:
    case 425:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
      return true;
    default:
      return false;
  }
}

} // namespace nim
