#include "zexception.h"
#include "zhttptruststore.h"

#include <gtest/gtest.h>

namespace nim {
namespace {

TEST(ZHttpTrustStore, ParsesWindowsTrustSourceValues)
{
  EXPECT_EQ(windowsTrustSourceFromString("auto"), ZHttpWindowsTrustSource::Auto);
  EXPECT_EQ(windowsTrustSourceFromString("windows_store"), ZHttpWindowsTrustSource::WindowsStore);
  EXPECT_EQ(windowsTrustSourceFromString("systemstore"), ZHttpWindowsTrustSource::WindowsStore);
  EXPECT_EQ(windowsTrustSourceFromString("bundled_pem"), ZHttpWindowsTrustSource::BundledPem);
  EXPECT_EQ(windowsTrustSourceFromString("bundle"), ZHttpWindowsTrustSource::BundledPem);
}

TEST(ZHttpTrustStore, RejectsInvalidWindowsTrustSource)
{
  EXPECT_THROW((void)windowsTrustSourceFromString("bad_value"), ZException);
}

} // namespace
} // namespace nim
