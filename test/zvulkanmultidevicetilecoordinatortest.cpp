#include "z3drenderingengine.h"
#include "z3dtiledescriptor.h"
#include "zcommandlineflags.h"
#include "zdoc.h"
#include "zjson.h"
#include "zrenderthreadexecutor_tls.h"
#include "zswc.h"
#include "zswcdoc.h"
#include "zvulkanmultidevicetilecoordinator.h"

#include <absl/flags/flag.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

ABSL_DECLARE_FLAG(nim::RenderBackend, atlas_default_render_backend);

namespace nim {
namespace {

bool vulkanSmokeEnabled()
{
  const char* enabled = std::getenv("ATLAS_ENABLE_VULKAN_SMOKE_TEST");
  return enabled != nullptr && std::string_view(enabled) == "1";
}

TEST(ZVulkanMultiDeviceTileCoordinatorTest, SynchronousPpllTilesPreserveParityAndCanonicalEngine)
{
  if (!vulkanSmokeEnabled()) {
    GTEST_SKIP() << "Set ATLAS_ENABLE_VULKAN_SMOKE_TEST=1 with a Vulkan ICD to run the complete-engine worker smoke";
  }

  std::unique_ptr<QApplication> ownedApplication;
  if (QApplication::instance() == nullptr) {
#if defined(__linux__)
    static int argc = 3;
    static char arg0[] = "zvulkanmultidevicetilecoordinatortest";
    static char arg1[] = "-platform";
    static char arg2[] = "offscreen";
    static char* argv[] = {arg0, arg1, arg2, nullptr};
#else
    static int argc = 1;
    static char arg0[] = "zvulkanmultidevicetilecoordinatortest";
    static char* argv[] = {arg0, nullptr};
#endif
    ownedApplication = std::make_unique<QApplication>(argc, argv);
  }

  absl::FlagSaver flagSaver;
  absl::SetFlag(&FLAGS_atlas_default_render_backend, RenderBackend::Vulkan);

  ZDoc doc;
  ZSwc swc;
  swc.addLine({glm::dvec3(-30.0, -20.0, 0.0), glm::dvec3(0.0, 30.0, 0.0), glm::dvec3(30.0, -20.0, 0.0)}, 4.0);
  const size_t swcId = doc.swcDoc().addSwcFromMemory(std::move(swc), QStringLiteral("worker_parity.swc"));

  auto canonical = std::make_unique<Z3DRenderingEngine>(doc);
  canonical->init();
  canonical->globalParas().transparencyMethod.select(QStringLiteral("Per-Pixel Fragment List (PPLL Exact)"));
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
  const QString canonicalBeforePath = outputDirectory.filePath(QStringLiteral("canonical_before_worker.png"));
  const QString canonicalTiledPath = outputDirectory.filePath(QStringLiteral("canonical_tiled.png"));
  const QString canonicalAfterPath = outputDirectory.filePath(QStringLiteral("canonical_after_worker.png"));
  QSignalSpy canonicalRenderingErrors(canonical.get(), &Z3DRenderingEngine::renderingError);
  canonical->takeFixedSizeScreenShot(canonicalBeforePath,
                                     kReferenceWidth,
                                     kReferenceHeight,
                                     Z3DScreenShotType::MonoView);
  ASSERT_TRUE(canonicalRenderingErrors.empty());

  const ZImg canonicalBeforePixels = ZImg::readImgPixelsOnly(canonicalBeforePath);

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

  {
    ZVulkanMultiDeviceTileCoordinator coordinator(*canonical);

    ZImg assembled(ZImgInfo(kReferenceWidth, kReferenceHeight, 1, 4));
    assembled.infoRef().lastChannelIsAlphaChannel = true;
    for (const Z3DTileDescriptor& tile : tiles) {
      Z3DRenderedTile renderedTile = coordinator.renderTile(tile);
      EXPECT_FALSE(renderedTile.rightColor.has_value());
      const glm::uvec2 assemblyOrigin = tile.topLeftAssemblyOrigin();
      assembled.pasteImg(renderedTile.primaryColor, ZVoxelCoordinate(assemblyOrigin.x, assemblyOrigin.y));
    }
    EXPECT_TRUE(assembled == canonicalTiledPixels)
      << "assembled=" << assembled.info() << ", canonical_tiled=" << canonicalTiledPixels.info();
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

  canonical.reset();
  EXPECT_EQ(currentRenderThreadExecutorOrNull(), nullptr);
}

} // namespace
} // namespace nim
