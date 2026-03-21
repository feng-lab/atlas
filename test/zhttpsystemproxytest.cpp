#include "zhttpsystemproxy.h"

#include <QList>
#include <QNetworkProxy>

#include <gtest/gtest.h>

namespace nim {
namespace {

constexpr ZHttpProxySupport kProxygenProxySupport{
  .supportsHttp = true,
};

constexpr ZHttpProxySupport kCurlProxySupport{
  .supportsHttp = true,
  .supportsSocks5 = true,
  .supportsAuthentication = true,
};

TEST(ZHttpSystemProxy, ProxygenReturnsUsableHttpProxy)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("proxy.example.org"), 8080));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kProxygenProxySupport);
  ASSERT_FALSE(result.error.has_value());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.endpoint.has_value());
  EXPECT_EQ(result.endpoint->kind, ZHttpProxyKind::Http);
  EXPECT_EQ(result.endpoint->host, "proxy.example.org");
  EXPECT_EQ(result.endpoint->port, 8080);
}

TEST(ZHttpSystemProxy, ProxygenRejectsAuthenticatedHttpProxyWithoutDirectFallback)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::HttpProxy,
                                  QStringLiteral("proxy.example.org"),
                                  8080,
                                  QStringLiteral("user"),
                                  QStringLiteral("secret")));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kProxygenProxySupport);
  ASSERT_FALSE(result.endpoint.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("Authenticated system proxy"), std::string::npos);
}

TEST(ZHttpSystemProxy, ProxygenWarnsAndFallsBackToDirectWhenSystemProxyAllowsDirect)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::NoProxy));
  proxies.push_back(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("proxy.example.org"), 1080));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kProxygenProxySupport);
  ASSERT_FALSE(result.endpoint.has_value());
  ASSERT_FALSE(result.error.has_value());
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_NE(result.warnings.front().find("Unsupported system proxy"), std::string::npos);
}

TEST(ZHttpSystemProxy, ProxygenPrefersUsableHttpProxyLaterInList)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("proxy.example.org"), 1080));
  proxies.push_back(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("proxy2.example.org"), 8080));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kProxygenProxySupport);
  ASSERT_FALSE(result.error.has_value());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.endpoint.has_value());
  EXPECT_EQ(result.endpoint->kind, ZHttpProxyKind::Http);
  EXPECT_EQ(result.endpoint->host, "proxy2.example.org");
  EXPECT_EQ(result.endpoint->port, 8080);
}

TEST(ZHttpSystemProxy, CurlAcceptsAuthenticatedHttpProxy)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::HttpProxy,
                                  QStringLiteral("proxy.example.org"),
                                  8080,
                                  QStringLiteral("user"),
                                  QStringLiteral("secret")));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kCurlProxySupport);
  ASSERT_FALSE(result.error.has_value());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.endpoint.has_value());
  EXPECT_EQ(result.endpoint->kind, ZHttpProxyKind::Http);
  EXPECT_EQ(result.endpoint->host, "proxy.example.org");
  EXPECT_EQ(result.endpoint->port, 8080);
  EXPECT_EQ(result.endpoint->username, "user");
  EXPECT_EQ(result.endpoint->password, "secret");
}

TEST(ZHttpSystemProxy, CurlAcceptsSocks5Proxy)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("proxy.example.org"), 1080));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kCurlProxySupport);
  ASSERT_FALSE(result.error.has_value());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.endpoint.has_value());
  EXPECT_EQ(result.endpoint->kind, ZHttpProxyKind::Socks5);
  EXPECT_EQ(result.endpoint->host, "proxy.example.org");
  EXPECT_EQ(result.endpoint->port, 1080);
}

TEST(ZHttpSystemProxy, CurlPrefersSupportedProxyLaterInList)
{
  QList<QNetworkProxy> proxies;
  proxies.push_back(QNetworkProxy(QNetworkProxy::FtpCachingProxy, QStringLiteral("proxy.example.org"), 2121));
  proxies.push_back(QNetworkProxy(QNetworkProxy::Socks5Proxy,
                                  QStringLiteral("proxy2.example.org"),
                                  1080,
                                  QStringLiteral("user"),
                                  QStringLiteral("secret")));

  const ZSystemHttpProxyResolution result =
    resolveSystemHttpProxyCandidates(proxies, "https://example.org/data", kCurlProxySupport);
  ASSERT_FALSE(result.error.has_value());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.endpoint.has_value());
  EXPECT_EQ(result.endpoint->kind, ZHttpProxyKind::Socks5);
  EXPECT_EQ(result.endpoint->host, "proxy2.example.org");
  EXPECT_EQ(result.endpoint->port, 1080);
  EXPECT_EQ(result.endpoint->username, "user");
  EXPECT_EQ(result.endpoint->password, "secret");
}

} // namespace
} // namespace nim
