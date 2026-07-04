#include <curl/curl.h>
#include <gtest/gtest.h>

#include <string_view>

namespace {

struct CurlGlobalCleanup
{
  ~CurlGlobalCleanup()
  {
    curl_global_cleanup();
  }
};

bool hasProtocol(const curl_version_info_data* info, std::string_view protocol)
{
  if (info->protocols == nullptr) {
    return false;
  }
  for (const char* const* p = info->protocols; *p != nullptr; ++p) {
    if (std::string_view(*p) == protocol) {
      return true;
    }
  }
  return false;
}

bool hasFeature(const curl_version_info_data* info, int feature)
{
  return (info->features & feature) != 0;
}

} // namespace

TEST(ZCurlRuntimeTest, ReportsRequiredCoreFeatures)
{
  ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
  const CurlGlobalCleanup cleanup;

  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  ASSERT_NE(nullptr, info);

  EXPECT_TRUE(hasProtocol(info, "http"));
  EXPECT_TRUE(hasProtocol(info, "https"));
  EXPECT_TRUE(hasFeature(info, CURL_VERSION_SSL));

#if defined(__linux__)
#ifdef CURL_VERSION_HTTP2
  EXPECT_TRUE(hasFeature(info, CURL_VERSION_HTTP2));
#else
  ADD_FAILURE() << "libcurl headers do not expose CURL_VERSION_HTTP2";
#endif

#ifdef CURL_VERSION_HTTP3
  EXPECT_TRUE(hasFeature(info, CURL_VERSION_HTTP3));
#else
  ADD_FAILURE() << "libcurl headers do not expose CURL_VERSION_HTTP3";
#endif

#ifdef CURL_VERSION_BROTLI
  EXPECT_TRUE(hasFeature(info, CURL_VERSION_BROTLI));
#else
  ADD_FAILURE() << "libcurl headers do not expose CURL_VERSION_BROTLI";
#endif

#ifdef CURL_VERSION_ZSTD
  EXPECT_TRUE(hasFeature(info, CURL_VERSION_ZSTD));
#else
  ADD_FAILURE() << "libcurl headers do not expose CURL_VERSION_ZSTD";
#endif
#endif
}
