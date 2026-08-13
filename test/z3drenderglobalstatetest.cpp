#include "z3drenderglobalstate.h"

#include <gtest/gtest.h>

#include <barrier>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace nim {
namespace {

TEST(Z3DRenderGlobalStateTest, NewFrameResetsThreadSubmissionSequence)
{
  auto& state = Z3DRenderGlobalState::instance();

  const uint64_t firstToken = state.beginNewRenderFrameToken(false);
  EXPECT_EQ(state.currentRenderFrameToken(), firstToken);
  EXPECT_EQ(state.currentPerfFrameStartTime(), std::chrono::steady_clock::time_point{});
  EXPECT_EQ(state.nextRenderFrameSubmissionId(firstToken), 1u);
  EXPECT_EQ(state.nextRenderFrameSubmissionId(firstToken), 2u);

  const uint64_t secondToken = state.beginNewRenderFrameToken(false);
  EXPECT_GT(secondToken, firstToken);
  EXPECT_EQ(state.currentRenderFrameToken(), secondToken);
  EXPECT_EQ(state.nextRenderFrameSubmissionId(secondToken), 1u);
}

TEST(Z3DRenderGlobalStateTest, PerformanceStartTimeBelongsToCurrentThreadFrame)
{
  auto& state = Z3DRenderGlobalState::instance();
  const auto earliestStart = std::chrono::steady_clock::now();
  const uint64_t token = state.beginNewRenderFrameToken(true);
  const auto latestStart = std::chrono::steady_clock::now();

  EXPECT_EQ(state.currentRenderFrameToken(), token);
  EXPECT_GE(state.currentPerfFrameStartTime(), earliestStart);
  EXPECT_LE(state.currentPerfFrameStartTime(), latestStart);
}

TEST(Z3DRenderGlobalStateTest, ConcurrentThreadsKeepIndependentFrameSequences)
{
  constexpr size_t kRenderThreadCount = 8u;
  constexpr size_t kFramesPerThread = 32u;

  struct ThreadResult
  {
    std::vector<uint64_t> allocatedTokens;
    uint64_t observedActiveToken = 0u;
    uint32_t firstSubmissionId = 0u;
    uint32_t secondSubmissionId = 0u;
  };

  auto& state = Z3DRenderGlobalState::instance();
  const uint64_t callerToken = state.beginNewRenderFrameToken(false);
  EXPECT_EQ(state.nextRenderFrameSubmissionId(callerToken), 1u);

  std::barrier allocationBoundary(static_cast<std::ptrdiff_t>(kRenderThreadCount));
  std::vector<ThreadResult> results(kRenderThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kRenderThreadCount);
  for (size_t threadIndex = 0u; threadIndex < kRenderThreadCount; ++threadIndex) {
    threads.emplace_back([threadIndex, &allocationBoundary, &results, &state]() {
      auto& result = results[threadIndex];
      result.allocatedTokens.reserve(kFramesPerThread);
      allocationBoundary.arrive_and_wait();
      for (size_t frameIndex = 0u; frameIndex < kFramesPerThread; ++frameIndex) {
        result.allocatedTokens.push_back(state.beginNewRenderFrameToken(false));
      }

      allocationBoundary.arrive_and_wait();
      result.observedActiveToken = state.currentRenderFrameToken();
      result.firstSubmissionId = state.nextRenderFrameSubmissionId(result.observedActiveToken);
      result.secondSubmissionId = state.nextRenderFrameSubmissionId(result.observedActiveToken);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  for (const auto& result : results) {
    ASSERT_EQ(result.allocatedTokens.size(), kFramesPerThread);
    for (size_t frameIndex = 0u; frameIndex < kFramesPerThread; ++frameIndex) {
      EXPECT_EQ(result.allocatedTokens[frameIndex], frameIndex + 1u);
    }
    EXPECT_EQ(result.observedActiveToken, result.allocatedTokens.back());
    EXPECT_EQ(result.firstSubmissionId, 1u);
    EXPECT_EQ(result.secondSubmissionId, 2u);
  }

  EXPECT_EQ(state.currentRenderFrameToken(), callerToken);
  EXPECT_EQ(state.nextRenderFrameSubmissionId(callerToken), 2u);
}

TEST(Z3DRenderGlobalStateTest, CancellationRequestInvalidatesIdleCheckpoint)
{
  auto& state = Z3DRenderGlobalState::instance();
  ASSERT_FALSE(state.hasCancellationSource());

  const auto preparationCheckpoint = state.idleCancellationCheckpoint();
  ASSERT_TRUE(preparationCheckpoint.has_value());
  state.requestCancellation();
  EXPECT_EQ(state.tryAcquireCancellationSource(*preparationCheckpoint), nullptr);

  const auto currentCheckpoint = state.idleCancellationCheckpoint();
  ASSERT_TRUE(currentCheckpoint.has_value());
  auto source = state.tryAcquireCancellationSource(*currentCheckpoint);
  ASSERT_NE(source, nullptr);
  EXPECT_FALSE(source->getToken().isCancellationRequested());

  state.requestCancellation();
  EXPECT_TRUE(source->getToken().isCancellationRequested());
  state.releaseCancellationSource(source);
  EXPECT_FALSE(state.hasCancellationSource());
}

TEST(Z3DRenderGlobalStateTest, ConcurrentCancellationAcquisitionHasOneOwner)
{
  constexpr size_t kAcquirerCount = 8u;

  auto& state = Z3DRenderGlobalState::instance();
  ASSERT_FALSE(state.hasCancellationSource());
  const auto checkpoint = state.idleCancellationCheckpoint();
  ASSERT_TRUE(checkpoint.has_value());

  std::barrier acquisitionBoundary(static_cast<std::ptrdiff_t>(kAcquirerCount));
  std::vector<std::shared_ptr<folly::CancellationSource>> results(kAcquirerCount);
  std::vector<std::thread> threads;
  threads.reserve(kAcquirerCount);
  for (size_t threadIndex = 0u; threadIndex < kAcquirerCount; ++threadIndex) {
    threads.emplace_back([threadIndex, &acquisitionBoundary, &results, &state, checkpoint]() {
      acquisitionBoundary.arrive_and_wait();
      results[threadIndex] = state.tryAcquireCancellationSource(*checkpoint);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::shared_ptr<folly::CancellationSource> owner;
  size_t ownerCount = 0u;
  for (const auto& result : results) {
    if (result) {
      owner = result;
      ++ownerCount;
    }
  }
  ASSERT_EQ(ownerCount, 1u);
  ASSERT_NE(owner, nullptr);

  state.requestCancellation();
  EXPECT_TRUE(owner->getToken().isCancellationRequested());
  state.releaseCancellationSource(owner);
  EXPECT_FALSE(state.hasCancellationSource());
}

TEST(Z3DRenderGlobalStateTest, StaleCancellationOwnerCannotReleaseReplacement)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  auto& state = Z3DRenderGlobalState::instance();
  ASSERT_FALSE(state.hasCancellationSource());

  const auto firstCheckpoint = state.idleCancellationCheckpoint();
  ASSERT_TRUE(firstCheckpoint.has_value());
  auto firstOwner = state.tryAcquireCancellationSource(*firstCheckpoint);
  ASSERT_NE(firstOwner, nullptr);
  state.releaseCancellationSource(firstOwner);

  const auto replacementCheckpoint = state.idleCancellationCheckpoint();
  ASSERT_TRUE(replacementCheckpoint.has_value());
  auto replacementOwner = state.tryAcquireCancellationSource(*replacementCheckpoint);
  ASSERT_NE(replacementOwner, nullptr);
  EXPECT_DEATH_IF_SUPPORTED(state.releaseCancellationSource(firstOwner),
                            "Only the render that acquired a cancellation source may release it");
  state.releaseCancellationSource(replacementOwner);
  EXPECT_FALSE(state.hasCancellationSource());
}

} // namespace
} // namespace nim
