#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

template<typename T>
class QList;
class QNetworkProxy;

namespace nim {

enum class HttpProxyStrategy
{
  Auto,
  NoProxy,
  ProxyIfAvailable,
};

struct ZHttpProxySupport
{
  bool supportsHttp = false;
  bool supportsSocks5 = false;
  bool supportsAuthentication = false;
};

enum class ZHttpProxyKind
{
  Http,
  Socks5,
};

struct ZHttpProxyEndpoint
{
  ZHttpProxyKind kind = ZHttpProxyKind::Http;
  std::string host;
  uint16_t port = 0;
  std::string username;
  std::string password;
};

struct ZSystemHttpProxyResolution
{
  std::optional<ZHttpProxyEndpoint> endpoint;
  std::vector<std::string> warnings;
  std::optional<std::string> error;
};

[[nodiscard]] ZSystemHttpProxyResolution resolveSystemHttpProxyCandidates(const QList<QNetworkProxy>& proxies,
                                                                          std::string_view urlForLog,
                                                                          const ZHttpProxySupport& support);

[[nodiscard]] ZSystemHttpProxyResolution querySystemHttpProxyForUrl(const std::string& url,
                                                                    const ZHttpProxySupport& support);

} // namespace nim
