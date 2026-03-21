#include "zneuroglancerurl.h"

#include "zexception.h"

#include <gtest/gtest.h>

namespace nim {

TEST(ZNeuroglancerUrl, DecodeSupportedPrecomputedSourceUrl)
{
  auto direct = decodeSupportedNeuroglancerPrecomputedSourceUrl("precomputed://gs://bucket/path");
  ASSERT_TRUE(direct.has_value());
  EXPECT_EQ(direct->toStdString(), "precomputed://gs://bucket/path");

  auto wrapped = decodeSupportedNeuroglancerPrecomputedSourceUrl("s3://bucket/path|neuroglancer-precomputed:");
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped->toStdString(), "s3://bucket/path");

  EXPECT_FALSE(decodeSupportedNeuroglancerPrecomputedSourceUrl("https://example.com/data").has_value());
}

TEST(ZNeuroglancerUrl, NormalizeUrlDropFragment)
{
  EXPECT_EQ(normalizeNeuroglancerUrlDropFragment(" precomputed://gs://bucket/path#type=mesh ").toStdString(),
            "precomputed://gs://bucket/path");
}

TEST(ZNeuroglancerUrl, MapCloudStorageUrlToHttps)
{
  EXPECT_EQ(mapCloudStorageUrlToHttps("gs://bucket/path/to/object").toStdString(),
            "https://storage.googleapis.com/bucket/path/to/object");
  EXPECT_EQ(mapCloudStorageUrlToHttps("s3://bucket/path/to/object").toStdString(),
            "https://bucket.s3.amazonaws.com/path/to/object");
  EXPECT_EQ(mapCloudStorageUrlToHttps("s3://bucket.with.dot/path/to/object").toStdString(),
            "https://s3.amazonaws.com/bucket.with.dot/path/to/object");
}

TEST(ZNeuroglancerUrl, NormalizePrecomputedRootUrl)
{
  EXPECT_EQ(normalizeNeuroglancerPrecomputedRootUrl(" precomputed://gs://bucket/volume ").toStdString(),
            "https://storage.googleapis.com/bucket/volume/");
  EXPECT_EQ(normalizeNeuroglancerPrecomputedRootUrl("https://example.com/dataset/info").toStdString(),
            "https://example.com/dataset/");
  EXPECT_THROW((void)normalizeNeuroglancerPrecomputedRootUrl("https://example.com/state.json"), ZException);
}

} // namespace nim
