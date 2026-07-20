#include "z3dglobalparameters.h"

#include <gtest/gtest.h>

namespace nim {
namespace {

TEST(Z3DGlobalParametersTest, FailedVulkanInitializationRestoresOpenGLAndCanRetry)
{
  Z3DGlobalParameters parameters(RenderBackend::OpenGL);
  const QString openGL = enumToQString(RenderBackend::OpenGL);
  const QString vulkan = enumToQString(RenderBackend::Vulkan);
  const QString ddp = QStringLiteral("Dual Depth Peeling");
  const QString ppll = QStringLiteral("Per-Pixel Fragment List (PPLL Exact)");
  parameters.transparencyMethod.select(ddp);

  bool failFirstInitialization = true;
  int backendChangeCount = 0;
  QObject::connect(&parameters.renderBackend, &ZStringIntOptionParameter::valueChanged, [&]() {
    ++backendChangeCount;
    if (failFirstInitialization) {
      failFirstInitialization = false;
      EXPECT_EQ(parameters.renderBackend.get(), vulkan);
      EXPECT_EQ(parameters.transparencyMethod.get(), ppll);
      parameters.restoreOpenGLAfterFailedVulkanInitialization();
    }
  });

  parameters.renderBackend.select(vulkan);
  EXPECT_EQ(backendChangeCount, 1);
  EXPECT_EQ(parameters.renderBackend.get(), openGL);
  EXPECT_EQ(parameters.renderBackend.associatedData(), static_cast<int>(RenderBackend::OpenGL));
  EXPECT_EQ(parameters.camera.get().getBackend(), RenderBackend::OpenGL);
  EXPECT_FALSE(parameters.transparencyMethod.hasOption(ppll));
  EXPECT_EQ(parameters.transparencyMethod.get(), ddp);

  // A retry must behave like the original OpenGL-to-Vulkan request.
  parameters.renderBackend.select(vulkan);
  EXPECT_EQ(backendChangeCount, 2);
  EXPECT_EQ(parameters.renderBackend.associatedData(), static_cast<int>(RenderBackend::Vulkan));
  EXPECT_TRUE(parameters.transparencyMethod.hasOption(ppll));
  EXPECT_EQ(parameters.transparencyMethod.get(), ppll);
}

} // namespace
} // namespace nim
