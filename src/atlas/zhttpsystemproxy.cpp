#include "zhttpsystemproxy.h"

#include "zexception.h"

#include <QCoreApplication>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QUrl>

#include <folly/String.h>

namespace nim {
namespace {

std::string proxyTypeName(QNetworkProxy::ProxyType type)
{
  switch (type) {
    case QNetworkProxy::NoProxy:
      return "NoProxy";
    case QNetworkProxy::DefaultProxy:
      return "DefaultProxy";
    case QNetworkProxy::Socks5Proxy:
      return "Socks5Proxy";
    case QNetworkProxy::HttpProxy:
      return "HttpProxy";
    case QNetworkProxy::HttpCachingProxy:
      return "HttpCachingProxy";
    case QNetworkProxy::FtpCachingProxy:
      return "FtpCachingProxy";
  }
  return fmt::format("ProxyType({})", static_cast<int>(type));
}

std::string supportedProxySummary(const ZHttpProxySupport& support)
{
  std::vector<std::string> parts;
  if (support.supportsHttp) {
    parts.push_back(support.supportsAuthentication ? "HTTP proxies with optional credentials" : "plain HTTP proxies");
  }
  if (support.supportsSocks5) {
    parts.push_back(support.supportsAuthentication ? "SOCKS5 proxies with optional credentials"
                                                   : "SOCKS5 proxies without credentials");
  }
  if (parts.empty()) {
    return "no proxy types";
  }
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) {
      out += "; ";
    }
    out += part;
  }
  return out;
}

std::string proxyDisplayString(const QNetworkProxy& proxy)
{
  const QString host = proxy.hostName().trimmed();
  const int port = proxy.port();
  const bool hasAuth = !proxy.user().isEmpty() || !proxy.password().isEmpty();
  return fmt::format("{} host='{}' port={} auth={}",
                     proxyTypeName(proxy.type()),
                     host.toStdString(),
                     port,
                     hasAuth ? "present" : "none");
}

std::string joinMessages(const std::vector<std::string>& messages)
{
  std::string out;
  for (const auto& message : messages) {
    if (!out.empty()) {
      out += '\n';
    }
    out += message;
  }
  return out;
}

bool isSupportedProxyType(const ZHttpProxySupport& support, QNetworkProxy::ProxyType type, ZHttpProxyKind& kind)
{
  switch (type) {
    case QNetworkProxy::HttpProxy:
    case QNetworkProxy::HttpCachingProxy:
      if (!support.supportsHttp) {
        return false;
      }
      kind = ZHttpProxyKind::Http;
      return true;
    case QNetworkProxy::Socks5Proxy:
      if (!support.supportsSocks5) {
        return false;
      }
      kind = ZHttpProxyKind::Socks5;
      return true;
    case QNetworkProxy::NoProxy:
    case QNetworkProxy::DefaultProxy:
    case QNetworkProxy::FtpCachingProxy:
      return false;
  }
  return false;
}

} // namespace

ZSystemHttpProxyResolution resolveSystemHttpProxyCandidates(const QList<QNetworkProxy>& proxies,
                                                            std::string_view urlForLog,
                                                            const ZHttpProxySupport& support)
{
  ZSystemHttpProxyResolution out;
  bool sawExplicitDirect = false;
  std::vector<std::string> problems;

  for (const QNetworkProxy& candidate : proxies) {
    if (candidate.type() == QNetworkProxy::NoProxy) {
      sawExplicitDirect = true;
      continue;
    }

    const bool hasAuth = !candidate.user().isEmpty() || !candidate.password().isEmpty();
    const QString host = candidate.hostName().trimmed();
    const int port = candidate.port();
    const bool validEndpoint = !host.isEmpty() && port > 0 && port <= 65535;

    ZHttpProxyKind kind = ZHttpProxyKind::Http;
    const bool supportedType = isSupportedProxyType(support, candidate.type(), kind);
    if (supportedType && (!hasAuth || support.supportsAuthentication) && validEndpoint) {
      ZHttpProxyEndpoint endpoint{};
      endpoint.kind = kind;
      endpoint.host = host.toStdString();
      endpoint.port = static_cast<uint16_t>(port);
      endpoint.username = candidate.user().toStdString();
      endpoint.password = candidate.password().toStdString();
      out.endpoint = std::move(endpoint);
      return out;
    }

    if (!supportedType) {
      problems.push_back(fmt::format("Unsupported system proxy for '{}': {}. Supported proxy types: {}.",
                                     urlForLog,
                                     proxyDisplayString(candidate),
                                     supportedProxySummary(support)));
      continue;
    }

    if (hasAuth && !support.supportsAuthentication) {
      problems.push_back(
        fmt::format("Authenticated system proxy for '{}' is not supported: {}. Supported proxy types: {}.",
                    urlForLog,
                    proxyDisplayString(candidate),
                    supportedProxySummary(support)));
      continue;
    }

    problems.push_back(fmt::format("Invalid system proxy for '{}': {}.", urlForLog, proxyDisplayString(candidate)));
  }

  if (!problems.empty()) {
    if (sawExplicitDirect) {
      out.warnings = std::move(problems);
    } else {
      out.error = joinMessages(problems);
    }
  }

  return out;
}

ZSystemHttpProxyResolution querySystemHttpProxyForUrl(const std::string& url, const ZHttpProxySupport& support)
{
  if (QCoreApplication::instance() == nullptr) {
    return {};
  }

  const QUrl qurl(QString::fromStdString(url));
  if (!qurl.isValid()) {
    return {};
  }

  const QNetworkProxyQuery query(qurl);
  const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
  if (proxies.isEmpty()) {
    return {};
  }

  return resolveSystemHttpProxyCandidates(proxies, url, support);
}

} // namespace nim
