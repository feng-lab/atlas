#include "zhttpretrypolicy.h"

#include <gtest/gtest.h>

namespace {

TEST(ZHttpRetryPolicy, RetriesTruncatedTransferMessages)
{
  EXPECT_TRUE(nim::isRetryableHttpExceptionMessage(
    "curl GET failed for 'https://example.invalid/chunk': Transferred a partial file"));
  EXPECT_TRUE(nim::isRetryableHttpExceptionMessage("transfer closed with outstanding read data remaining"));
}

TEST(ZHttpRetryPolicy, RetriesConnectionAndTimeoutMessages)
{
  EXPECT_TRUE(nim::isRetryableHttpExceptionMessage("connect failed: Connection reset by peer"));
  EXPECT_TRUE(nim::isRetryableHttpExceptionMessage("operation timed out after 1000 milliseconds"));
  EXPECT_TRUE(nim::isRetryableHttpExceptionMessage("Temporary failure in name resolution"));
}

TEST(ZHttpRetryPolicy, DoesNotRetryPermanentInputErrors)
{
  EXPECT_FALSE(nim::isRetryableHttpExceptionMessage("Invalid URL 'not a url'"));
  EXPECT_FALSE(nim::isRetryableHttpExceptionMessage("HTTP 404 not found"));
}

TEST(ZHttpRetryPolicy, RetriesTransientHttpStatuses)
{
  EXPECT_TRUE(nim::isRetryableHttpStatus(408));
  EXPECT_TRUE(nim::isRetryableHttpStatus(421));
  EXPECT_TRUE(nim::isRetryableHttpStatus(425));
  EXPECT_TRUE(nim::isRetryableHttpStatus(429));
  EXPECT_TRUE(nim::isRetryableHttpStatus(500));
  EXPECT_TRUE(nim::isRetryableHttpStatus(502));
  EXPECT_TRUE(nim::isRetryableHttpStatus(503));
  EXPECT_TRUE(nim::isRetryableHttpStatus(504));
}

TEST(ZHttpRetryPolicy, DoesNotRetrySoftMissOrPermanentStatuses)
{
  EXPECT_FALSE(nim::isRetryableHttpStatus(403));
  EXPECT_FALSE(nim::isRetryableHttpStatus(404));
  EXPECT_FALSE(nim::isRetryableHttpStatus(401));
  EXPECT_FALSE(nim::isRetryableHttpStatus(416));
}

} // namespace
