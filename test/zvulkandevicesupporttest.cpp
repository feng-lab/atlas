#include "zvulkandevicesupport.h"
#include "zvulkancontext.h"
#include "zvulkanbindlessdescriptorset.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zcommandlineflags.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

ABSL_DECLARE_FLAG(int32_t, atlas_vk_frames_in_flight);

namespace nim {
namespace {

ZVulkanDeviceSupport incompatibleSupport(std::string detail)
{
  ZVulkanDeviceSupport support;
  support.rejections.push_back(
    {.code = ZVulkanDeviceSupport::RejectionCode::DynamicRendering, .detail = std::move(detail)});
  return support;
}

bool vulkanSmokeEnabled()
{
  const char* enabled = std::getenv("ATLAS_ENABLE_VULKAN_SMOKE_TEST");
  return enabled != nullptr && std::string_view(enabled) == "1";
}

void enableThreadsafeDeathTests()
{
  // macOS Vulkan implementations can start system/Metal helper threads.
  // Re-exec the test process instead of forking that multithreaded state.
  GTEST_FLAG_SET(death_test_style, "threadsafe");
}

TEST(ZVulkanDeviceSupportTest, AutomaticSelectionUsesFirstCompatibleDevice)
{
  const std::vector<ZVulkanDeviceSupport> devices{incompatibleSupport("first rejection"),
                                                  ZVulkanDeviceSupport{},
                                                  ZVulkanDeviceSupport{}};

  const auto selection = ZVulkanDeviceSupport::select(devices, std::nullopt);

  ASSERT_TRUE(selection.index.has_value());
  EXPECT_EQ(*selection.index, 1u);
  EXPECT_TRUE(selection.warning.empty());
  EXPECT_TRUE(selection.error.empty());
}

TEST(ZVulkanDeviceSupportTest, ExplicitIncompatibleDeviceFallsBackToFirstCompatibleDevice)
{
  const std::vector<ZVulkanDeviceSupport> devices{incompatibleSupport("dynamic rendering unavailable"),
                                                  ZVulkanDeviceSupport{}};

  const auto selection = ZVulkanDeviceSupport::select(devices, 0u);

  ASSERT_TRUE(selection.index.has_value());
  EXPECT_EQ(*selection.index, 1u);
  EXPECT_NE(selection.warning.find("device index 0"), std::string::npos);
  EXPECT_NE(selection.warning.find("dynamic rendering unavailable"), std::string::npos);
  EXPECT_NE(selection.warning.find("device index 1"), std::string::npos);
  EXPECT_TRUE(selection.error.empty());
}

TEST(ZVulkanDeviceSupportTest, ExplicitCompatibleDeviceIsSelectedExactly)
{
  const std::vector<ZVulkanDeviceSupport> devices{ZVulkanDeviceSupport{},
                                                  ZVulkanDeviceSupport{},
                                                  ZVulkanDeviceSupport{}};

  const auto selection = ZVulkanDeviceSupport::select(devices, 2u);

  ASSERT_TRUE(selection.index.has_value());
  EXPECT_EQ(*selection.index, 2u);
  EXPECT_TRUE(selection.warning.empty());
  EXPECT_TRUE(selection.error.empty());
}

TEST(ZVulkanDeviceSupportTest, OutOfRangeExplicitIndexFallsBackToFirstCompatibleDevice)
{
  const std::vector<ZVulkanDeviceSupport> devices{ZVulkanDeviceSupport{}};

  const auto selection = ZVulkanDeviceSupport::select(devices, 4u);

  ASSERT_TRUE(selection.index.has_value());
  EXPECT_EQ(*selection.index, 0u);
  EXPECT_NE(selection.warning.find("out of range"), std::string::npos);
  EXPECT_NE(selection.warning.find("1 enumerated device"), std::string::npos);
  EXPECT_NE(selection.warning.find("device index 0"), std::string::npos);
  EXPECT_TRUE(selection.error.empty());
}

TEST(ZVulkanDeviceSupportTest, NoCompatibleDeviceReportsEveryRejection)
{
  const std::vector<ZVulkanDeviceSupport> devices{incompatibleSupport("device zero reason"),
                                                  incompatibleSupport("device one reason")};

  const auto selection = ZVulkanDeviceSupport::select(devices, std::nullopt);

  EXPECT_FALSE(selection.index.has_value());
  EXPECT_TRUE(selection.warning.empty());
  EXPECT_NE(selection.error.find("device zero reason"), std::string::npos);
  EXPECT_NE(selection.error.find("device one reason"), std::string::npos);
}

TEST(ZVulkanDeviceSupportTest, RejectedPreferredDevicePreservesWarningWhenNoFallbackExists)
{
  const std::vector<ZVulkanDeviceSupport> devices{incompatibleSupport("preferred device reason"),
                                                  incompatibleSupport("other device reason")};

  const auto selection = ZVulkanDeviceSupport::select(devices, 0u);

  EXPECT_FALSE(selection.index.has_value());
  EXPECT_NE(selection.warning.find("device index 0"), std::string::npos);
  EXPECT_NE(selection.warning.find("preferred device reason"), std::string::npos);
  EXPECT_NE(selection.warning.find("no compatible Vulkan fallback device"), std::string::npos);
  EXPECT_NE(selection.error.find("preferred device reason"), std::string::npos);
  EXPECT_NE(selection.error.find("other device reason"), std::string::npos);
}

TEST(ZVulkanDeviceSupportTest, PhysicalDevicePreferenceOrdersByPowerThenUuid)
{
  using Preference = ZVulkanDeviceSupport::PhysicalDevicePreference;

  Preference preferred;
  preferred.deviceType = vk::PhysicalDeviceType::eDiscreteGpu;
  preferred.deviceLocalMemoryBytes = 8ull * 1024u * 1024u * 1024u;
  preferred.apiVersion = VK_API_VERSION_1_3;

  auto lowerRanked = preferred;
  lowerRanked.deviceType = vk::PhysicalDeviceType::eIntegratedGpu;
  EXPECT_TRUE(Preference::isPreferredBefore(preferred, lowerRanked));
  EXPECT_FALSE(Preference::isPreferredBefore(lowerRanked, preferred));

  lowerRanked = preferred;
  lowerRanked.deviceLocalMemoryBytes /= 2u;
  EXPECT_TRUE(Preference::isPreferredBefore(preferred, lowerRanked));
  EXPECT_FALSE(Preference::isPreferredBefore(lowerRanked, preferred));

  lowerRanked = preferred;
  lowerRanked.apiVersion = VK_API_VERSION_1_2;
  EXPECT_TRUE(Preference::isPreferredBefore(preferred, lowerRanked));
  EXPECT_FALSE(Preference::isPreferredBefore(lowerRanked, preferred));

  auto laterUuid = preferred;
  preferred.deviceUuid.back() = 1u;
  laterUuid.deviceUuid.back() = 2u;
  EXPECT_TRUE(Preference::isPreferredBefore(preferred, laterUuid));
  EXPECT_FALSE(Preference::isPreferredBefore(laterUuid, preferred));
  EXPECT_FALSE(Preference::isPreferredBefore(preferred, preferred));
}

TEST(ZVulkanDeviceSupportTest, UpdateAfterBindBudgetCountsSharedBindlessStateOncePerFrameSlot)
{
  using Policy = ZVulkanDeviceSupport::DescriptorPoolPolicy;
  constexpr uint32_t frameSlots = 2u;
  constexpr uint32_t bindlessPerFrameSlot = 2048u;
  const uint64_t required = (bindlessPerFrameSlot + Policy::kBindlessSamplerDescriptors) * frameSlots;
  ASSERT_LE(required, std::numeric_limits<uint32_t>::max());

  const auto budget = Policy::maxBindlessSampledImagesPerFrameSlot(static_cast<uint32_t>(required), frameSlots);

  ASSERT_TRUE(budget.has_value());
  EXPECT_EQ(*budget, bindlessPerFrameSlot);

  const auto budgetBelowBoundary =
    Policy::maxBindlessSampledImagesPerFrameSlot(static_cast<uint32_t>(required - 1u), frameSlots);
  ASSERT_TRUE(budgetBelowBoundary.has_value());
  EXPECT_LT(*budgetBelowBoundary, bindlessPerFrameSlot);
}

TEST(ZVulkanDeviceSupportTest, RequiredUpdateAfterBindBudgetUsesEffectiveCapacitiesExactly)
{
  using Policy = ZVulkanDeviceSupport::DescriptorPoolPolicy;
  const ZVulkanDeviceSupport::BindlessSampledImageCapacities capacities{
    .texture2D = 512u,
    .texture2DArray = 256u,
    .texture3D = 1024u,
    .uTexture2D = 128u,
    .uTexture3D = 128u,
  };
  constexpr uint32_t frameSlots = 3u;

  const auto required = Policy::requiredUpdateAfterBindDescriptors(capacities, frameSlots);

  ASSERT_TRUE(required.has_value());
  EXPECT_EQ(*required, (capacities.totalSampledImages() + Policy::kBindlessSamplerDescriptors) * frameSlots);
}

TEST(ZVulkanDeviceSupportTest, UpdateAfterBindBudgetHandlesSamplerAndFrameSlotBoundaries)
{
  using Policy = ZVulkanDeviceSupport::DescriptorPoolPolicy;

  EXPECT_FALSE(Policy::maxBindlessSampledImagesPerFrameSlot(5u, 2u).has_value());
  const auto samplerOnlyBudget = Policy::maxBindlessSampledImagesPerFrameSlot(6u, 2u);
  ASSERT_TRUE(samplerOnlyBudget.has_value());
  EXPECT_EQ(*samplerOnlyBudget, 0u);
  EXPECT_FALSE(Policy::maxBindlessSampledImagesPerFrameSlot(100u, 0u).has_value());

  const ZVulkanDeviceSupport::BindlessSampledImageCapacities capacities{
    .texture2D = 1u,
    .texture2DArray = 1u,
    .texture3D = 1u,
    .uTexture2D = 1u,
    .uTexture3D = 1u,
  };
  EXPECT_FALSE(Policy::requiredUpdateAfterBindDescriptors(capacities, 0u).has_value());
}

TEST(ZVulkanDeviceSupportTest, RequiredUpdateAfterBindBudgetRejectsOverflow)
{
  using Policy = ZVulkanDeviceSupport::DescriptorPoolPolicy;
  const ZVulkanDeviceSupport::BindlessSampledImageCapacities capacities{
    .texture2D = std::numeric_limits<uint32_t>::max(),
    .texture2DArray = std::numeric_limits<uint32_t>::max(),
    .texture3D = std::numeric_limits<uint32_t>::max(),
    .uTexture2D = std::numeric_limits<uint32_t>::max(),
    .uTexture3D = std::numeric_limits<uint32_t>::max(),
  };

  EXPECT_TRUE(Policy::requiredUpdateAfterBindDescriptors(capacities, 1u).has_value());
  EXPECT_FALSE(
    Policy::requiredUpdateAfterBindDescriptors(capacities, std::numeric_limits<uint32_t>::max()).has_value());
}

TEST(ZVulkanDeviceSupportTest, FragmentAggregateBudgetReservesEveryFixedPipelineResource)
{
  using Policy = ZVulkanDeviceSupport::ShaderResourcePolicy;
  constexpr uint32_t bindlessCapacity = 37u;

  const auto budget = Policy::fragmentBindlessBudget(Policy::kGraphicsFragmentFixedResources + bindlessCapacity);

  ASSERT_TRUE(budget.has_value());
  EXPECT_EQ(*budget, bindlessCapacity);
  EXPECT_FALSE(Policy::fragmentBindlessBudget(Policy::kGraphicsFragmentFixedResources).has_value());
}

TEST(ZVulkanDeviceSupportTest, ComputeAggregateBudgetReservesStorageBufferAndImage)
{
  using Policy = ZVulkanDeviceSupport::ShaderResourcePolicy;
  constexpr uint32_t bindlessCapacity = 11u;

  const auto budget = Policy::computeBindlessBudget(Policy::kComputeFixedResources + bindlessCapacity);

  ASSERT_TRUE(budget.has_value());
  EXPECT_EQ(*budget, bindlessCapacity);
  EXPECT_FALSE(Policy::computeBindlessBudget(Policy::kComputeFixedResources).has_value());
}

TEST(ZVulkanDeviceSupportTest, ContextRejectsSecondLiveDeviceWrapper)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a software ICD to run the Vulkan lifetime smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto firstDevice = context.createDevice();
      auto secondDevice = context.createDevice();
      (void)firstDevice;
      (void)secondDevice;
    },
    "only one live ZVulkanDevice wrapper");
}

TEST(ZVulkanDeviceSupportTest, ContextRejectsDirectSecondLiveDeviceWrapper)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the Vulkan lifetime smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto firstDevice = context.createDevice();
      ZVulkanDevice secondDevice(context);
      (void)firstDevice;
      (void)secondDevice;
    },
    "only one live ZVulkanDevice wrapper");
}

TEST(ZVulkanDeviceSupportTest, DeviceRejectsDescriptorAccountingFromAnotherThread)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a software ICD to run the Vulkan threading smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto device = context.createDevice();
      std::thread wrongThread([&device]() {
        (void)device->updateAfterBindDescriptorsReserved();
      });
      wrongThread.join();
    },
    "owning rendering thread");
}

TEST(ZVulkanDeviceSupportTest, DeviceRejectsBindlessTableAccessFromAnotherThread)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the Vulkan threading smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto device = context.createDevice();
      device->prepareBindlessDescriptorState();
      auto frame = device->frameExecutor().beginFrame();
      device->beginBindlessFrameSlot(frame);
      auto& table = device->bindlessSampledImagesForFrame(frame);
      std::thread wrongThread([&table]() {
        (void)table.used(ZVulkanBindlessDescriptorSet::Kind::Texture2D);
      });
      wrongThread.join();
    },
    "owning rendering thread");
}

TEST(ZVulkanDeviceSupportTest, DeviceRejectsActiveFrameAccessFromAnotherThread)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the Vulkan threading smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto device = context.createDevice();
      auto frame = device->frameExecutor().beginFrame();
      std::thread wrongThread([&frame]() {
        (void)frame.slotIndex();
      });
      wrongThread.join();
    },
    "owning rendering thread");
}

TEST(ZVulkanDeviceSupportTest, DeviceRejectsTeardownWithLiveActiveFrameLease)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the Vulkan lifetime smoke";
  }
  enableThreadsafeDeathTests();
  EXPECT_DEATH_IF_SUPPORTED(
    {
      ZVulkanContext context;
      auto device = context.createDevice();
      auto frame = device->frameExecutor().beginFrame();
      device.reset();
      (void)frame;
    },
    "live ActiveFrame leases");
}

TEST(ZVulkanDeviceSupportTest, SoftwareIcdStartupSmoke)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a software ICD to run the Vulkan startup smoke";
  }

  absl::FlagSaver flagSaver;
  absl::SetFlag(&FLAGS_atlas_vk_frames_in_flight, 2);
  ZVulkanContext context;
  EXPECT_EQ(context.frameSlotCount(), 2u);
  // The context has already evaluated descriptor limits for two slots. A later
  // flag mutation must not change the topology of any wrapper it creates.
  absl::SetFlag(&FLAGS_atlas_vk_frames_in_flight, 1);
  EXPECT_GT(context.deviceCount(), 0u);
  EXPECT_LT(context.selectedDeviceIndex(), context.deviceCount());
  EXPECT_TRUE(context.selectedDeviceSupport().compatible());

  auto device = context.createDevice();
  ASSERT_NE(device, nullptr);
  ASSERT_EQ(device->frameSlotCount(), context.frameSlotCount());
  EXPECT_NE(device->allocator(), nullptr);

  device->prepareBindlessDescriptorState();
  EXPECT_NE(device->bindlessSampledImageDescriptorSetLayout(), vk::DescriptorSetLayout{});

  ZVulkanBindlessDescriptorSet* firstSlotTable = nullptr;
  for (uint32_t expectedSlot = 0u; expectedSlot < device->frameSlotCount(); ++expectedSlot) {
    auto frame = device->frameExecutor().beginFrame();
    ASSERT_TRUE(frame.valid());
    EXPECT_EQ(frame.slotIndex(), expectedSlot);
    EXPECT_TRUE(device->frameExecutor().isPreRecordSafePoint(frame));
    device->beginBindlessFrameSlot(frame);
    auto* table = &device->bindlessSampledImagesForFrame(frame);
    EXPECT_EQ(table, &device->bindlessSampledImagesForFrame(frame));
    if (expectedSlot == 0u) {
      firstSlotTable = table;
    } else {
      EXPECT_NE(table, firstSlotTable);
    }
  }

  const auto makeSampledTexture = [&device]() {
    constexpr vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    constexpr vk::MemoryPropertyFlags memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
    auto info = ZVulkanTexture::CreateInfo::make2D(1u,
                                                   1u,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   usage,
                                                   memory,
                                                   1u,
                                                   false,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
    auto texture = device->createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    texture->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
    return texture;
  };

  constexpr uint32_t kBulkRetirementTextureCount = 4u;
  std::vector<std::unique_ptr<ZVulkanTexture>> retiredTextures;
  retiredTextures.reserve(kBulkRetirementTextureCount);
  for (uint32_t textureIndex = 0u; textureIndex < kBulkRetirementTextureCount; ++textureIndex) {
    retiredTextures.push_back(makeSampledTexture());
  }
  std::vector<std::vector<uint32_t>> retiredIndices;
  std::vector<ZVulkanBindlessDescriptorSet*> retiredTables;
  std::vector<uint64_t> compatibilityGenerationsAfterRegistration;
  retiredIndices.reserve(device->frameSlotCount());
  retiredTables.reserve(device->frameSlotCount());
  compatibilityGenerationsAfterRegistration.reserve(device->frameSlotCount());
  for (uint32_t expectedSlot = 0u; expectedSlot < device->frameSlotCount(); ++expectedSlot) {
    auto frame = device->frameExecutor().beginFrame();
    ASSERT_TRUE(frame.valid());
    EXPECT_EQ(frame.slotIndex(), expectedSlot);
    device->beginBindlessFrameSlot(frame);
    auto& table = device->bindlessSampledImagesForFrame(frame);
    const uint64_t compatibilityGenerationBefore = table.commandBufferCompatibilityGeneration();
    std::vector<uint32_t> slotIndices;
    slotIndices.reserve(retiredTextures.size());
    for (auto& retiredTexture : retiredTextures) {
      ZVulkanBindlessDescriptorSet::RegisterRequest request{};
      request.kind = ZVulkanBindlessDescriptorSet::Kind::Texture2D;
      request.texture = retiredTexture.get();
      request.debugLabel = "retirement_smoke_original";
      const uint32_t index = table.registerTexture(request);
      EXPECT_GT(index, 0u);
      slotIndices.push_back(index);
    }
    EXPECT_EQ(table.used(ZVulkanBindlessDescriptorSet::Kind::Texture2D), 1u + kBulkRetirementTextureCount);
    const uint64_t compatibilityGenerationAfter = table.commandBufferCompatibilityGeneration();
    if (context.supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
      EXPECT_EQ(compatibilityGenerationBefore, 0u);
      EXPECT_EQ(compatibilityGenerationAfter, 0u);
    } else {
      EXPECT_EQ(compatibilityGenerationAfter, compatibilityGenerationBefore + kBulkRetirementTextureCount);
    }
    retiredIndices.push_back(std::move(slotIndices));
    retiredTables.push_back(&table);
    compatibilityGenerationsAfterRegistration.push_back(compatibilityGenerationAfter);
  }

  // Model every backend-local pre-switch drain completing before renderer and
  // scratch resources release their final registered textures.
  ASSERT_TRUE(device->frameExecutor().allFrameSlotsDescriptorMutationSafe());
  ASSERT_TRUE(device->waitForAllFramesAndDrainBindlessRetirements());
  retiredTextures.clear();
  // OpenGL will not acquire another Vulkan slot to consume these late
  // retirements, so the engine's final device drain must empty every table.
  ASSERT_TRUE(device->frameExecutor().allFrameSlotsDescriptorMutationSafe());
  ASSERT_TRUE(device->waitForAllFramesAndDrainBindlessRetirements());
  ASSERT_EQ(retiredTables.size(), device->frameSlotCount());
  for (size_t slot = 0u; slot < retiredTables.size(); ++slot) {
    const auto* table = retiredTables[slot];
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->used(ZVulkanBindlessDescriptorSet::Kind::Texture2D), 1u);
    if (context.supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
      EXPECT_EQ(table->commandBufferCompatibilityGeneration(), 0u);
    } else {
      EXPECT_EQ(table->commandBufferCompatibilityGeneration(), compatibilityGenerationsAfterRegistration[slot] + 1u);
    }
  }

  std::vector<std::unique_ptr<ZVulkanTexture>> replacementTextures;
  replacementTextures.reserve(kBulkRetirementTextureCount);
  for (uint32_t textureIndex = 0u; textureIndex < kBulkRetirementTextureCount; ++textureIndex) {
    replacementTextures.push_back(makeSampledTexture());
  }
  for (uint32_t expectedSlot = 0u; expectedSlot < device->frameSlotCount(); ++expectedSlot) {
    auto frame = device->frameExecutor().beginFrame();
    ASSERT_TRUE(frame.valid());
    EXPECT_EQ(frame.slotIndex(), expectedSlot);
    device->beginBindlessFrameSlot(frame);
    auto& table = device->bindlessSampledImagesForFrame(frame);
    std::unordered_set<uint32_t> replacementIndices;
    for (auto& replacementTexture : replacementTextures) {
      ZVulkanBindlessDescriptorSet::RegisterRequest request{};
      request.kind = ZVulkanBindlessDescriptorSet::Kind::Texture2D;
      request.texture = replacementTexture.get();
      request.debugLabel = "retirement_smoke_replacement";
      replacementIndices.insert(table.registerTexture(request));
    }
    const std::unordered_set<uint32_t> expectedIndices(retiredIndices[expectedSlot].begin(),
                                                       retiredIndices[expectedSlot].end());
    EXPECT_EQ(replacementIndices, expectedIndices);
    EXPECT_EQ(table.used(ZVulkanBindlessDescriptorSet::Kind::Texture2D), 1u + kBulkRetirementTextureCount);
  }

  {
    auto frame = device->frameExecutor().beginFrame();
    ASSERT_TRUE(frame.valid());
    device->beginBindlessFrameSlot(frame);
    auto& table = device->bindlessSampledImagesForFrame(frame);
    ZVulkanBindlessDescriptorSet::RegisterRequest request{};
    request.kind = ZVulkanBindlessDescriptorSet::Kind::Texture2D;
    request.texture = replacementTextures.front().get();
    request.debugLabel = "retirement_smoke_stale_token";
    const uint32_t liveIndex = table.registerTexture(request);
    const auto liveHandle =
      table.retirementHandle(request.kind, request.texture, request.texture->descriptorIdentity());
    ASSERT_TRUE(liveHandle.has_value());
    auto staleHandle = *liveHandle;
    ASSERT_LT(staleHandle.identity, std::numeric_limits<uint64_t>::max());
    ++staleHandle.identity;
    const uint32_t usedBeforeStaleRetirement = table.used(request.kind);
    const uint64_t generationBeforeStaleRetirement = table.commandBufferCompatibilityGeneration();
    const ZVulkanBindlessDescriptorSet::PlaceholderDescriptorInfos unusedPlaceholders{};
    table.retireEntries(std::span<const ZVulkanBindlessDescriptorSet::EntryHandle>(&staleHandle, 1u),
                        unusedPlaceholders);
    EXPECT_EQ(table.lookupTexture(request), std::optional<uint32_t>(liveIndex));
    EXPECT_EQ(table.used(request.kind), usedBeforeStaleRetirement);
    EXPECT_EQ(table.commandBufferCompatibilityGeneration(), generationBeforeStaleRetirement);
  }

  uint32_t deferredCallbackCount = 0u;
  {
    auto frame = device->frameExecutor().beginFrame();
    ASSERT_TRUE(frame.valid());
    EXPECT_GT(frame.acquisitionSerial(), 0u);
    device->beginBindlessFrameSlot(frame);
    EXPECT_TRUE(device->frameExecutor().isPreRecordSafePoint(frame));
    (void)frame.commandBuffer();
    device->frameExecutor().scheduleAfterCompletion(frame, [&deferredCallbackCount]() {
      ++deferredCallbackCount;
    });
    EXPECT_FALSE(device->frameExecutor().isPreRecordSafePoint(frame));
    EXPECT_FALSE(device->frameExecutor().allFrameSlotsDescriptorMutationSafe());
    EXPECT_FALSE(device->waitForAllFramesAndDrainBindlessRetirements());
    EXPECT_EQ(deferredCallbackCount, 0u);
  }
  EXPECT_TRUE(device->frameExecutor().allFrameSlotsDescriptorMutationSafe());
  EXPECT_TRUE(device->waitForAllFramesAndDrainBindlessRetirements());
  EXPECT_EQ(deferredCallbackCount, 1u);

  const uint64_t expectedReservation = context.supportsDescriptorIndexingSampledImageUpdateAfterBind()
                                         ? context.selectedDeviceSupport().requiredUpdateAfterBindDescriptors
                                         : 0u;
  EXPECT_EQ(device->updateAfterBindDescriptorsReserved(), expectedReservation);
  const uint64_t reservationBeforeOrdinaryPools = device->updateAfterBindDescriptorsReserved();
  auto transientPool = device->createTransientDescriptorPool();
  auto persistentPool = device->createPersistentDescriptorPool();
  ASSERT_NE(transientPool, nullptr);
  ASSERT_NE(persistentPool, nullptr);
  EXPECT_EQ(device->updateAfterBindDescriptorsReserved(), reservationBeforeOrdinaryPools);

  persistentPool.reset();
  transientPool.reset();
  replacementTextures.clear();
  ASSERT_TRUE(device->waitForAllFramesAndDrainBindlessRetirements());
  device.reset();

  auto replacementDevice = context.createDevice();
  ASSERT_NE(replacementDevice, nullptr);
  EXPECT_EQ(replacementDevice->frameSlotCount(), context.frameSlotCount());
  EXPECT_NE(replacementDevice->allocator(), nullptr);
}

} // namespace
} // namespace nim
