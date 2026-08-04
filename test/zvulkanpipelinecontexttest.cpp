#include "zvulkanpipelinecontext_raii.h"

#include "zcommandlineflags.h"
#include "z3drendererbackend.h"
#include "zvulkanfinalreadbackcompletion_p.h"
#include <gtest/gtest.h>

ABSL_DECLARE_FLAG(bool, atlas_vk_enforce_pipeline_context);

namespace {

#ifndef NDEBUG
class VulkanPipelineDebugTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    previousFlag_ = absl::GetFlag(FLAGS_atlas_vk_enforce_pipeline_context);
    absl::SetFlag(&FLAGS_atlas_vk_enforce_pipeline_context, true);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }

  void TearDown() override
  {
    absl::SetFlag(&FLAGS_atlas_vk_enforce_pipeline_context, previousFlag_);
  }

private:
  bool previousFlag_{true};
};
#endif

} // namespace

TEST(ReadbackCompletionPolicyTest, SeparatesCompletionFromRenderQuality)
{
  using nim::ReadbackCompletionPolicy;
  using nim::readbackCompletionRequiresWait;

  EXPECT_FALSE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::FollowRenderQuality,
                                              /*progressiveRenderQuality=*/true));
  EXPECT_TRUE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::FollowRenderQuality,
                                             /*progressiveRenderQuality=*/false));

  EXPECT_TRUE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::WaitForCompletion,
                                             /*progressiveRenderQuality=*/true));
  EXPECT_TRUE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::WaitForCompletion,
                                             /*progressiveRenderQuality=*/false));

  EXPECT_FALSE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::ReturnAfterSubmit,
                                              /*progressiveRenderQuality=*/true));
  EXPECT_FALSE(readbackCompletionRequiresWait(ReadbackCompletionPolicy::ReturnAfterSubmit,
                                              /*progressiveRenderQuality=*/false));
}

TEST(VulkanFinalReadbackCompletionTest, RejectsInvalidPublicationIdentity)
{
  using Completion = nim::ZVulkanFinalReadbackCompletion;
  using Decision = Completion::PublicationDecision;

  const glm::uvec2 extent{64u, 48u};
  auto decide = [&](bool ownerAvailable,
                    uint64_t expectedRevision,
                    uint64_t currentRevision,
                    const glm::uvec2& readbackExtent,
                    const glm::uvec2& currentExtent,
                    nim::RenderBackend backend,
                    uint64_t renderFrameToken,
                    uint64_t lastPublishedToken) {
    return Completion::publicationDecision(ownerAvailable,
                                           expectedRevision,
                                           currentRevision,
                                           extent,
                                           readbackExtent,
                                           currentExtent,
                                           backend,
                                           renderFrameToken,
                                           lastPublishedToken);
  };

  EXPECT_EQ(decide(true, 7u, 7u, extent, extent, nim::RenderBackend::Vulkan, 12u, 11u), Decision::Accept);
  EXPECT_EQ(decide(false, 7u, 7u, extent, extent, nim::RenderBackend::Vulkan, 12u, 11u), Decision::OwnerUnavailable);
  EXPECT_EQ(decide(true, 7u, 8u, extent, extent, nim::RenderBackend::Vulkan, 12u, 11u),
            Decision::OwnerRevisionMismatch);
  EXPECT_EQ(decide(true, 7u, 7u, {63u, 48u}, extent, nim::RenderBackend::Vulkan, 12u, 11u), Decision::ExtentMismatch);
  EXPECT_EQ(decide(true, 7u, 7u, extent, {64u, 47u}, nim::RenderBackend::Vulkan, 12u, 11u), Decision::ExtentMismatch);
  EXPECT_EQ(decide(true, 7u, 7u, extent, extent, nim::RenderBackend::OpenGL, 12u, 11u), Decision::BackendMismatch);
  EXPECT_EQ(decide(true, 7u, 7u, extent, extent, nim::RenderBackend::Vulkan, 10u, 11u), Decision::RenderFrameStale);
}

TEST(VulkanFinalReadbackCompletionTest, RetiresExactlyOnceOrTransfersOwnership)
{
  const glm::uvec2 extent{8u, 8u};
  nim::Z3DLocalColorBuffer localBuffer{};
  nim::Z3DScratchResourcePool::RenderTargetLease target;

  auto retiredOnDestruction = std::make_shared<nim::ZVulkanReadbackRetirement>();
  ASSERT_TRUE(retiredOnDestruction->tryAcquire());
  retiredOnDestruction->notifyProducerFinished();
  {
    nim::ZVulkanFinalReadbackCompletion completion(QPointer<nim::Z3DCompositor>{},
                                                   1u,
                                                   1u,
                                                   extent,
                                                   extent,
                                                   &localBuffer,
                                                   nim::MonoEye,
                                                   &localBuffer,
                                                   &target,
                                                   false,
                                                   retiredOnDestruction);
    nim::ZVulkanFinalReadbackCompletion moved(std::move(completion));
    EXPECT_EQ(moved.renderFrameToken, 1u);
    EXPECT_TRUE(retiredOnDestruction->occupied());
  }
  EXPECT_FALSE(retiredOnDestruction->occupied());

  auto transferredRetirement = std::make_shared<nim::ZVulkanReadbackRetirement>();
  ASSERT_TRUE(transferredRetirement->tryAcquire());
  transferredRetirement->notifyProducerFinished();
  std::function<void()> externalRetirement;
  {
    nim::ZVulkanFinalReadbackCompletion completion(QPointer<nim::Z3DCompositor>{},
                                                   1u,
                                                   2u,
                                                   extent,
                                                   extent,
                                                   &localBuffer,
                                                   nim::MonoEye,
                                                   &localBuffer,
                                                   &target,
                                                   false,
                                                   transferredRetirement);
    completion.transferRetirementTo(externalRetirement);
  }
  EXPECT_TRUE(transferredRetirement->occupied());
  ASSERT_TRUE(externalRetirement);
  externalRetirement();
  externalRetirement = {};
  EXPECT_FALSE(transferredRetirement->occupied());
}

TEST(VulkanFinalReadbackCompletionTest, SupportsMultipleLiveAttemptCompletions)
{
  const glm::uvec2 extent{8u, 8u};
  nim::Z3DLocalColorBuffer firstLocalBuffer{};
  nim::Z3DLocalColorBuffer secondLocalBuffer{};
  nim::Z3DScratchResourcePool::RenderTargetLease firstTarget;
  nim::Z3DScratchResourcePool::RenderTargetLease secondTarget;
  auto firstRetirement = std::make_shared<nim::ZVulkanReadbackRetirement>();
  auto secondRetirement = std::make_shared<nim::ZVulkanReadbackRetirement>();
  ASSERT_TRUE(firstRetirement->tryAcquire());
  ASSERT_TRUE(secondRetirement->tryAcquire());
  firstRetirement->notifyProducerFinished();
  secondRetirement->notifyProducerFinished();

  {
    nim::ZVulkanFinalReadbackCompletion first(QPointer<nim::Z3DCompositor>{},
                                              3u,
                                              41u,
                                              extent,
                                              extent,
                                              &firstLocalBuffer,
                                              nim::MonoEye,
                                              &firstLocalBuffer,
                                              &firstTarget,
                                              false,
                                              firstRetirement);
    nim::ZVulkanFinalReadbackCompletion second(QPointer<nim::Z3DCompositor>{},
                                               3u,
                                               42u,
                                               extent,
                                               extent,
                                               &secondLocalBuffer,
                                               nim::MonoEye,
                                               &secondLocalBuffer,
                                               &secondTarget,
                                               false,
                                               secondRetirement);

    EXPECT_NE(first.renderFrameToken, second.renderFrameToken);
    EXPECT_TRUE(firstRetirement->occupied());
    EXPECT_TRUE(secondRetirement->occupied());
  }

  EXPECT_FALSE(firstRetirement->occupied());
  EXPECT_FALSE(secondRetirement->occupied());
}

#ifndef NDEBUG

TEST_F(VulkanPipelineDebugTest, MissingScissorTriggersCheck)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanGraphicsDrawSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.viewports.emplace_back(0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f);
  spec.scissors.emplace_back(vk::Rect2D{
    {0,   0  },
    {16u, 16u}
  });

  tracker.reset(spec);
  tracker.markViewport();

  EXPECT_DEATH(tracker.assertGraphicsPreDraw(spec), "Scissor must be set");
}

TEST_F(VulkanPipelineDebugTest, DescriptorCoverageEnforced)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanGraphicsDrawSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.viewports.emplace_back(0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f);
  spec.scissors.emplace_back(vk::Rect2D{
    {0,  0 },
    {8u, 8u}
  });
  spec.expectedDescriptorSetCount = 2;

  tracker.reset(spec);
  tracker.markViewport();
  tracker.markScissor();
  tracker.markDescriptorSets(0, 1);

  EXPECT_DEATH(tracker.assertGraphicsPreDraw(spec), "Descriptor set coverage incomplete");
}

TEST_F(VulkanPipelineDebugTest, DescriptorCoverageAllowsUnusedLeadingSet)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanGraphicsDrawSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.viewports.emplace_back(0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f);
  spec.scissors.emplace_back(vk::Rect2D{
    {0,  0 },
    {8u, 8u}
  });
  // expectedDescriptorSetCount is the exclusive upper set bound. Vulkan does
  // not require set 0 to be bound when no shader in this pipeline uses it.
  spec.expectedDescriptorSetCount = 3;

  tracker.reset(spec);
  tracker.markViewport();
  tracker.markScissor();
  tracker.markDescriptorSets(1, 2);

  EXPECT_NO_FATAL_FAILURE(tracker.assertGraphicsPreDraw(spec));
}

TEST_F(VulkanPipelineDebugTest, ComputePushConstantsRequired)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanComputePassSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.requirePushConstants = true;
  spec.pushConstantsSize = 16;

  tracker.reset(spec);

  EXPECT_DEATH(tracker.assertComputePreDispatch(spec), "push constants");
}

TEST_F(VulkanPipelineDebugTest, CompleteGraphicsStatePasses)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanGraphicsDrawSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.viewports.emplace_back(0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f);
  spec.scissors.emplace_back(vk::Rect2D{
    {0,   0  },
    {32u, 32u}
  });
  spec.lineWidth = 2.0f;
  spec.depthBiasEnable = VK_TRUE;
  spec.blendConstants = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  spec.cullMode = vk::CullModeFlagBits::eBack;
  spec.frontFace = vk::FrontFace::eCounterClockwise;
  spec.primitiveRestartEnable = VK_TRUE;
  spec.depthTestEnable = VK_TRUE;
  spec.depthWriteEnable = VK_TRUE;
  spec.depthCompareOp = vk::CompareOp::eLess;
  spec.stencilTestEnable = VK_FALSE;
  spec.topology = vk::PrimitiveTopology::eTriangleList;
  spec.rasterizerDiscardEnable = VK_FALSE;
  spec.expectedDescriptorSetCount = 1;
  spec.requirePushConstants = true;
  spec.pushConstantsSize = 4;

  tracker.reset(spec);
  tracker.markViewport();
  tracker.markScissor();
  tracker.markLineWidth();
  tracker.markDepthBias();
  tracker.markBlendConstants();
  tracker.markCullMode();
  tracker.markFrontFace();
  tracker.markPrimitiveRestart();
  tracker.markDepthTest();
  tracker.markDepthWrite();
  tracker.markDepthCompare();
  tracker.markStencilTest();
  tracker.markTopology();
  tracker.markRasterizerDiscard();
  tracker.markDescriptorSets(0, 1);
  tracker.markPushConstants(4);

  EXPECT_NO_FATAL_FAILURE(tracker.assertGraphicsPreDraw(spec));
}

#endif // NDEBUG

#ifndef NDEBUG
TEST_F(VulkanPipelineDebugTest, TrackerResetsBetweenPasses)
{
  nim::ZVulkanDebugStateTracker tracker;

  // Pass 1: sets viewport and scissor; should succeed
  {
    nim::ZVulkanGraphicsDrawSpec spec{};
    spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
    spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
    spec.viewports.emplace_back(0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f);
    spec.scissors.emplace_back(vk::Rect2D{
      {0,   0  },
      {16u, 16u}
    });
    tracker.reset(spec);
    tracker.markViewport();
    tracker.markScissor();
    EXPECT_NO_FATAL_FAILURE(tracker.assertGraphicsPreDraw(spec));
  }

  // Pass 2: only sets viewport, not scissor; reset must force a failure on scissor
  {
    nim::ZVulkanGraphicsDrawSpec spec{};
    spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
    spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
    spec.viewports.emplace_back(0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f);
    spec.scissors.emplace_back(vk::Rect2D{
      {0,   0  },
      {16u, 16u}
    });
    tracker.reset(spec);
    tracker.markViewport();
    EXPECT_DEATH(tracker.assertGraphicsPreDraw(spec), "Scissor must be set");
  }
}
#endif // NDEBUG
