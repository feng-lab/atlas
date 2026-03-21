#include "zneuroglancershardedreader.h"

#include <gtest/gtest.h>

namespace nim {
namespace {

TEST(ZNeuroglancerShardedReader, FindsPayloadLocationForExactKey)
{
  ZNeuroglancerUint64Sharding::DecodedMinishardIndex decoded{};
  decoded.keys = {10, 42, 100};
  decoded.starts = {1000, 2000, 3000};
  decoded.ends = {1100, 2100, 3200};

  auto locationOpt = findNeuroglancerShardedPayloadLocation(decoded, 42);
  ASSERT_TRUE(locationOpt.has_value());
  EXPECT_EQ(locationOpt->start, 2000);
  EXPECT_EQ(locationOpt->end, 2100);
}

TEST(ZNeuroglancerShardedReader, MissingKeyReturnsNullopt)
{
  ZNeuroglancerUint64Sharding::DecodedMinishardIndex decoded{};
  decoded.keys = {10, 42, 100};
  decoded.starts = {1000, 2000, 3000};
  decoded.ends = {1100, 2100, 3200};

  EXPECT_FALSE(findNeuroglancerShardedPayloadLocation(decoded, 41).has_value());
  EXPECT_FALSE(findNeuroglancerShardedPayloadLocation(decoded, 101).has_value());
}

TEST(ZNeuroglancerShardedReader, NonPositiveRangeReturnsNullopt)
{
  ZNeuroglancerUint64Sharding::DecodedMinishardIndex decoded{};
  decoded.keys = {7};
  decoded.starts = {500};
  decoded.ends = {500};

  EXPECT_FALSE(findNeuroglancerShardedPayloadLocation(decoded, 7).has_value());
}

} // namespace
} // namespace nim
