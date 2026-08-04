#include "zvulkanreadbackretirement_p.h"

#include <gtest/gtest.h>

namespace {

using Retirement = nim::ZVulkanReadbackRetirement;

TEST(VulkanReadbackRetirementTest, ProducerFirstWaitsForConsumer)
{
  Retirement retirement;
  ASSERT_TRUE(retirement.tryAcquire());

  retirement.notifyProducerFinished();
  EXPECT_TRUE(retirement.occupied());
  EXPECT_FALSE(retirement.tryAcquire());

  retirement.notifyConsumerFinished();
  EXPECT_FALSE(retirement.occupied());
}

TEST(VulkanReadbackRetirementTest, ConsumerFirstWaitsForProducer)
{
  Retirement retirement;
  ASSERT_TRUE(retirement.tryAcquire());

  retirement.notifyConsumerFinished();
  EXPECT_TRUE(retirement.occupied());

  retirement.notifyProducerFinished();
  EXPECT_FALSE(retirement.occupied());
}

TEST(VulkanReadbackRetirementTest, DefinitelyUnsubmittedProducerStillWaitsForConsumer)
{
  Retirement retirement;
  ASSERT_TRUE(retirement.tryAcquire());

  retirement.notifyProducerFinished();
  EXPECT_TRUE(retirement.occupied());
  EXPECT_FALSE(retirement.tryAcquire());

  retirement.notifyConsumerFinished();
  EXPECT_FALSE(retirement.occupied());

  ASSERT_TRUE(retirement.tryAcquire());
  retirement.notifyConsumerFinished();
  EXPECT_TRUE(retirement.occupied());
  retirement.notifyProducerFinished();
  EXPECT_FALSE(retirement.occupied());
}

TEST(VulkanReadbackRetirementDeathTest, DuplicateOrUnownedCompletionFailsFast)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  Retirement retirement;
  EXPECT_DEATH_IF_SUPPORTED(retirement.notifyProducerFinished(), "unoccupied staging slot");

  ASSERT_TRUE(retirement.tryAcquire());
  retirement.notifyProducerFinished();
  EXPECT_DEATH_IF_SUPPORTED(retirement.notifyProducerFinished(), "same staging acquisition twice");
  retirement.notifyConsumerFinished();
}

} // namespace
