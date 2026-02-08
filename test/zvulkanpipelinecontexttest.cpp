#include "zvulkanpipelinecontext_raii.h"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

DECLARE_bool(atlas_vk_enforce_pipeline_context);

namespace {

#ifndef NDEBUG
class VulkanPipelineDebugTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    previousFlag_ = FLAGS_atlas_vk_enforce_pipeline_context;
    FLAGS_atlas_vk_enforce_pipeline_context = true;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }

  void TearDown() override
  {
    FLAGS_atlas_vk_enforce_pipeline_context = previousFlag_;
  }

private:
  bool previousFlag_{true};
};
#endif

} // namespace

#ifndef NDEBUG

TEST_F(VulkanPipelineDebugTest, MissingScissorTriggersCheck)
{
  nim::ZVulkanDebugStateTracker tracker;
  nim::ZVulkanGraphicsDrawSpec spec{};
  spec.pipeline = reinterpret_cast<const vk::raii::Pipeline*>(0x1);
  spec.pipelineLayout = reinterpret_cast<const vk::raii::PipelineLayout*>(0x1);
  spec.viewports.emplace_back(0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f);
  spec.scissors.emplace_back(vk::Rect2D{{0, 0}, {16u, 16u}});

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
  spec.scissors.emplace_back(vk::Rect2D{{0, 0}, {8u, 8u}});
  spec.expectedDescriptorSetCount = 2;

  tracker.reset(spec);
  tracker.markViewport();
  tracker.markScissor();
  tracker.markDescriptorSets(0, 1);

  EXPECT_DEATH(tracker.assertGraphicsPreDraw(spec), "Descriptor set coverage incomplete");
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
  spec.scissors.emplace_back(vk::Rect2D{{0, 0}, {32u, 32u}});
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
    spec.scissors.emplace_back(vk::Rect2D{{0, 0}, {16u, 16u}});
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
    spec.scissors.emplace_back(vk::Rect2D{{0, 0}, {16u, 16u}});
    tracker.reset(spec);
    tracker.markViewport();
    EXPECT_DEATH(tracker.assertGraphicsPreDraw(spec), "Scissor must be set");
  }
}
#endif // NDEBUG
