#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace nim {

std::chrono::milliseconds httpRetryBackoffForAttempt(uint32_t attempt);

// Conservative retry heuristics for transient transport failures. These are
// shared across HTTP backends so unstable-network behavior stays aligned.
bool isRetryableHttpExceptionMessage(std::string_view message);

// Retry transient upstream/proxy HTTP responses for idempotent GETs. Missing
// resources (403/404 soft miss in Atlas) are handled separately by callers.
bool isRetryableHttpStatus(uint16_t status);

} // namespace nim
