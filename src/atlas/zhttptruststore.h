#pragma once

#include <string>

namespace nim {

enum class ZHttpTrustBackend
{
  Curl,
  Proxygen,
};

struct ZHttpTrustStoreConfig
{
  std::string caBundlePath;
  std::string sourceDescription;
};

[[nodiscard]] ZHttpTrustStoreConfig resolveHttpTrustStoreConfig(ZHttpTrustBackend backend);

} // namespace nim
