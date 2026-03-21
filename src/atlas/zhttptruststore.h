#pragma once

#include <string>
#include <string_view>

namespace nim {

enum class ZHttpTrustBackend
{
  Curl,
  Proxygen,
};

enum class ZHttpWindowsTrustSource
{
  Auto,
  WindowsStore,
  BundledPem,
};

struct ZHttpTrustStoreConfig
{
  std::string caBundlePath;
  std::string sourceDescription;
};

[[nodiscard]] ZHttpWindowsTrustSource windowsTrustSourceFromString(std::string_view value);

[[nodiscard]] ZHttpTrustStoreConfig resolveHttpTrustStoreConfig(ZHttpTrustBackend backend);

} // namespace nim
