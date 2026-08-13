#include "z3drenderingengine.h"
#include "z3dtiledescriptor.h"
#include "zcommandlineflags.h"
#include "zdoc.h"
#include "zjson.h"
#include "zrenderthreadexecutor_tls.h"
#include "zswc.h"
#include "zswcdoc.h"

#include <absl/flags/flag.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

ABSL_DECLARE_FLAG(nim::RenderBackend, atlas_default_render_backend);

namespace nim {
namespace {

bool vulkanSmokeEnabled()
{
  const char* enabled = std::getenv("ATLAS_ENABLE_VULKAN_SMOKE_TEST");
  return enabled != nullptr && std::string_view(enabled) == "1";
}

std::unique_ptr<QApplication> makeSmokeTestApplicationIfNeeded()
{
  if (QApplication::instance() != nullptr) {
    return {};
  }

#if defined(__linux__)
  static int argc = 3;
  static char arg0[] = "zvulkantileworkerpooltest";
  static char arg1[] = "-platform";
  static char arg2[] = "offscreen";
  static char* argv[] = {arg0, arg1, arg2, nullptr};
#else
  static int argc = 1;
  static char arg0[] = "zvulkantileworkerpooltest";
  static char* argv[] = {arg0, nullptr};
#endif
  return std::make_unique<QApplication>(argc, argv);
}

enum class WorkerExecution
{
  SameAdapterBatch,
  DistinctDevicesBatch
};

void runPpllTileWorkerSmoke(WorkerExecution execution)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the complete-engine worker smoke";
  }

  auto ownedApplication = makeSmokeTestApplicationIfNeeded();

  absl::FlagSaver flagSaver;
  absl::SetFlag(&FLAGS_atlas_default_render_backend, RenderBackend::Vulkan);

  ZDoc doc;
  ZSwc swc;
  swc.addLine({glm::dvec3(-30.0, -20.0, 0.0), glm::dvec3(0.0, 30.0, 0.0), glm::dvec3(30.0, -20.0, 0.0)}, 4.0);
  const size_t swcId = doc.swcDoc().addSwcFromMemory(std::move(swc), QStringLiteral("worker_parity.swc"));
  ZSwc translucentSwc;
  translucentSwc.addLine({glm::dvec3(-30.0, 15.0, -8.0), glm::dvec3(0.0, -25.0, -8.0), glm::dvec3(30.0, 15.0, -8.0)},
                         4.0);
  const size_t translucentSwcId =
    doc.swcDoc().addSwcFromMemory(std::move(translucentSwc), QStringLiteral("worker_parity_translucent.swc"));

  auto canonical = std::make_unique<Z3DRenderingEngine>(doc);
  canonical->init();
  canonical->globalParas().transparencyMethod.select(QStringLiteral("Per-Pixel Fragment List (PPLL Exact)"));

  std::array<ZVulkanDeviceSupport::DeviceSelection, 2u> distinctWorkerSelections;
  if (execution == WorkerExecution::DistinctDevicesBatch) {
    std::vector<ZVulkanDeviceSupport::DeviceSelection> distinctSelections;
    for (const auto& selection : canonical->compatibleVulkanTileWorkerSelections()) {
      const bool duplicateUuid =
        std::any_of(distinctSelections.begin(), distinctSelections.end(), [&](const auto& known) {
          return known.expectedDeviceUuid == selection.expectedDeviceUuid;
        });
      if (!duplicateUuid) {
        distinctSelections.push_back(selection);
      }
    }
    if (distinctSelections.size() < distinctWorkerSelections.size()) {
      GTEST_SKIP()
        << "The distinct-device worker smoke requires two devices in the canonical planning-compatible worker set";
    }
    std::copy_n(distinctSelections.begin(), distinctWorkerSelections.size(), distinctWorkerSelections.begin());
  }

  constexpr uint32_t kReferenceWidth = 65u;
  constexpr uint32_t kReferenceHeight = 63u;
  constexpr uint32_t kTileExtent = 32u;
  constexpr uint32_t kGuardPixels = 4u;
  const glm::uvec2 referenceExtent(kReferenceWidth, kReferenceHeight);
  canonical->setOutputSize(referenceExtent);
  const float untransformedXMaximum = canonical->globalParas().globalXCut.maximum();
  ZQtExecutor* const canonicalExecutor = &canonical->renderThreadExecutor();
  ASSERT_EQ(currentRenderThreadExecutorOrNull(), canonicalExecutor);

  json::object canonicalSwcState;
  canonical->write(swcId, canonicalSwcState);
  ASSERT_TRUE(canonicalSwcState.contains("ViewObjType"));
  ASSERT_TRUE(canonicalSwcState.contains("Coord Transform 3DTransform"));
  auto& transformState = canonicalSwcState.at("Coord Transform 3DTransform").as_object();
  transformState["Translation Vec3"] = json::value_from(glm::vec3(11.0f, -7.0f, 5.0f));
  canonical->read(swcId, canonicalSwcState);
  canonicalSwcState.clear();
  canonical->write(swcId, canonicalSwcState);

  constexpr double kTranslucentOpacity = 0.45;
  json::object canonicalTranslucentSwcState;
  canonical->write(translucentSwcId, canonicalTranslucentSwcState);
  ASSERT_TRUE(canonicalTranslucentSwcState.contains("ViewObjType"));
  ASSERT_TRUE(canonicalTranslucentSwcState.contains("Coord Transform 3DTransform"));
  ASSERT_TRUE(canonicalTranslucentSwcState.contains("Opacity Float"));
  auto& translucentTransformState = canonicalTranslucentSwcState.at("Coord Transform 3DTransform").as_object();
  translucentTransformState["Translation Vec3"] = json::value_from(glm::vec3(11.0f, -7.0f, 5.0f));
  canonicalTranslucentSwcState["Opacity Float"] = kTranslucentOpacity;
  canonical->read(translucentSwcId, canonicalTranslucentSwcState);
  canonicalTranslucentSwcState.clear();
  canonical->write(translucentSwcId, canonicalTranslucentSwcState);

  // Frame the transformed bounds so the transformed-only cut below remains visible.
  canonical->resetCamera();

  const float transformedXMaximum = canonical->globalParas().globalXCut.maximum();
  ASSERT_GT(transformedXMaximum, untransformedXMaximum);
  const float transformedOnlyCutLower = (untransformedXMaximum + transformedXMaximum) * 0.5f;
  canonical->globalParas().globalXCut.setMode(ZCutSpanParameter::Mode::Absolute);
  canonical->globalParas().globalXCut.setPins(false, false);
  canonical->globalParas().globalXCut.set(glm::vec2(transformedOnlyCutLower, transformedXMaximum));
  ASSERT_GT(canonical->globalParas().globalXCut.get().x, untransformedXMaximum)
    << "The regression cut must be outside an unsynchronized worker's provisional scene bounds";

  json::object canonicalGeneralState;
  canonical->write(canonicalGeneralState);

  QTemporaryDir outputDirectory;
  ASSERT_TRUE(outputDirectory.isValid());
  const QString canonicalOpaqueOnlyPath = outputDirectory.filePath(QStringLiteral("canonical_opaque_only.png"));
  const QString canonicalBeforePath = outputDirectory.filePath(QStringLiteral("canonical_before_worker.png"));
  const QString canonicalTiledPath = outputDirectory.filePath(QStringLiteral("canonical_tiled.png"));
  const QString workerTiledPath = outputDirectory.filePath(QStringLiteral("worker_tiled.png"));
  const QString canonicalAfterPath = outputDirectory.filePath(QStringLiteral("canonical_after_worker.png"));
  QSignalSpy canonicalRenderingErrors(canonical.get(), &Z3DRenderingEngine::renderingError);

  json::object opaqueOnlyTranslucentSwcState = canonicalTranslucentSwcState;
  opaqueOnlyTranslucentSwcState["Opacity Float"] = 0.0;
  canonical->read(translucentSwcId, opaqueOnlyTranslucentSwcState);
  canonical->takeFixedSizeScreenShot(canonicalOpaqueOnlyPath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView);
  ASSERT_TRUE(canonicalRenderingErrors.empty());

  canonical->read(translucentSwcId, canonicalTranslucentSwcState);
  canonical->takeFixedSizeScreenShot(canonicalBeforePath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView);
  ASSERT_TRUE(canonicalRenderingErrors.empty());

  const ZImg canonicalOpaqueOnlyPixels = ZImg::readImgPixelsOnly(canonicalOpaqueOnlyPath);
  const ZImg canonicalBeforePixels = ZImg::readImgPixelsOnly(canonicalBeforePath);
  ASSERT_FALSE(canonicalBeforePixels == canonicalOpaqueOnlyPixels)
    << "The PPLL smoke scene must contain a visible translucent contribution";

  const auto tiles = makeZ3DTileDescriptors(referenceExtent, glm::uvec2(kTileExtent), kGuardPixels);
  ASSERT_EQ(tiles.size(), 6u);
  canonical->takeFixedSizeScreenShot(canonicalTiledPath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView,
                                     static_cast<int>(kTileExtent),
                                     static_cast<int>(kGuardPixels));
  ASSERT_TRUE(canonicalRenderingErrors.empty());
  const ZImg canonicalTiledPixels = ZImg::readImgPixelsOnly(canonicalTiledPath);

  if (execution == WorkerExecution::DistinctDevicesBatch) {
    canonical->configureVulkanTileWorkers(distinctWorkerSelections);
  } else {
    const auto canonicalSelection = canonical->activeVulkanDeviceSelection();
    canonical->configureVulkanTileWorkers(std::span(&canonicalSelection, 1u));
  }
  canonical->takeFixedSizeScreenShot(workerTiledPath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView,
                                     static_cast<int>(kTileExtent),
                                     static_cast<int>(kGuardPixels));
  ASSERT_TRUE(canonicalRenderingErrors.empty());
  const ZImg assembled = ZImg::readImgPixelsOnly(workerTiledPath);
  EXPECT_FALSE(assembled == canonicalOpaqueOnlyPixels)
    << "The worker result must retain the scene's translucent contribution";
  if (execution == WorkerExecution::SameAdapterBatch) {
    EXPECT_TRUE(assembled == canonicalTiledPixels)
      << "assembled=" << assembled.info() << ", canonical_tiled=" << canonicalTiledPixels.info();
  } else {
    ASSERT_TRUE(assembled.isSameType(canonicalTiledPixels));
    ASSERT_TRUE(assembled.isSameSize(canonicalTiledPixels));
    size_t mismatchedBytes = 0u;
    int maximumByteDifference = 0;
    for (size_t byteIndex = 0u; byteIndex < assembled.timeByteNumber(); ++byteIndex) {
      const int difference = std::abs(static_cast<int>(assembled.timeData(0u)[byteIndex]) -
                                      static_cast<int>(canonicalTiledPixels.timeData(0u)[byteIndex]));
      mismatchedBytes += difference != 0 ? 1u : 0u;
      maximumByteDifference = std::max(maximumByteDifference, difference);
    }
    testing::Test::RecordProperty("cross_device_mismatched_bytes", mismatchedBytes);
    testing::Test::RecordProperty("cross_device_maximum_byte_difference", maximumByteDifference);
  }
  EXPECT_EQ(currentRenderThreadExecutorOrNull(), canonicalExecutor);

  canonical->takeFixedSizeScreenShot(canonicalAfterPath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView);
  EXPECT_TRUE(canonicalRenderingErrors.empty());

  const ZImg canonicalAfterPixels = ZImg::readImgPixelsOnly(canonicalAfterPath);
  EXPECT_TRUE(canonicalAfterPixels == canonicalBeforePixels)
    << "canonical_after=" << canonicalAfterPixels.info() << ", canonical_before=" << canonicalBeforePixels.info();

  json::object canonicalGeneralStateAfter;
  canonical->write(canonicalGeneralStateAfter);
  EXPECT_EQ(canonicalGeneralStateAfter, canonicalGeneralState);
  json::object canonicalSwcStateAfter;
  canonical->write(swcId, canonicalSwcStateAfter);
  EXPECT_EQ(canonicalSwcStateAfter, canonicalSwcState);
  json::object canonicalTranslucentSwcStateAfter;
  canonical->write(translucentSwcId, canonicalTranslucentSwcStateAfter);
  EXPECT_EQ(canonicalTranslucentSwcStateAfter, canonicalTranslucentSwcState);

  canonical.reset();
  EXPECT_EQ(currentRenderThreadExecutorOrNull(), nullptr);
}

TEST(ZVulkanTileWorkerPoolTest, CanonicalAdapterPpllBatchPreservesParityAndCanonicalEngine)
{
  runPpllTileWorkerSmoke(WorkerExecution::SameAdapterBatch);
}

TEST(ZVulkanTileWorkerPoolTest, DistinctPhysicalDevicesCompleteBatchAndPreserveCanonicalEngine)
{
  runPpllTileWorkerSmoke(WorkerExecution::DistinctDevicesBatch);
}

} // namespace
} // namespace nim
