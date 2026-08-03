#include "z3dglobalparameters.h"
#include "z3drendererbase.h"
#include "z3dscratchresourcepool.h"

#include <gtest/gtest.h>
#include <cstdint>

namespace nim {
namespace {

TEST(Z3DGlobalParametersTest, RendererStateAndScratchPoolAreInstanceLocal)
{
  Z3DScratchResourcePool firstPool(RenderBackend::OpenGL);
  Z3DScratchResourcePool secondPool(RenderBackend::OpenGL);
  Z3DGlobalParameters first(firstPool, RenderBackend::OpenGL);
  Z3DGlobalParameters second(secondPool, RenderBackend::OpenGL);

  EXPECT_EQ(&first.scratchPool(), &firstPool);
  EXPECT_EQ(&second.scratchPool(), &secondPool);
  EXPECT_NE(&first.rendererViewState(), &second.rendererViewState());
  EXPECT_NE(&first.rendererSceneState(), &second.rendererSceneState());

  first.ensureRendererState();
  second.ensureRendererState();
  const glm::vec4 firstCachedAmbient = first.rendererSceneState().sceneAmbient;
  const glm::vec4 secondCachedAmbient = second.rendererSceneState().sceneAmbient;

  const glm::vec4 updatedAmbient(0.7f, 0.6f, 0.5f, 1.0f);
  first.sceneAmbient.set(updatedAmbient);

  // Refreshing the second instance cannot consume the first instance's dirty
  // bit, and the first instance retains its cached value until it refreshes.
  second.ensureRendererState();
  EXPECT_EQ(first.rendererSceneState().sceneAmbient, firstCachedAmbient);
  EXPECT_EQ(second.rendererSceneState().sceneAmbient, secondCachedAmbient);

  first.ensureRendererState();
  EXPECT_EQ(first.rendererSceneState().sceneAmbient, updatedAmbient);
  EXPECT_EQ(second.rendererSceneState().sceneAmbient, secondCachedAmbient);
}

TEST(Z3DRendererOwnershipTest, RejectsRenderTargetLeaseFromAnotherPhysicalPipeline)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  Z3DScratchResourcePool rendererPool(RenderBackend::OpenGL);
  Z3DScratchResourcePool foreignPool(RenderBackend::OpenGL);
  RendererParameterState parameters;
  RendererFrameState frame;
  RendererViewState view;
  RendererSceneState scene;
  Z3DRendererBase renderer(parameters, frame, view, scene, rendererPool, RenderBackend::OpenGL);

  // The ownership boundary runs before a backend can dereference the native
  // resource. A non-null sentinel therefore exercises only the pool-affinity
  // invariant and needs no OpenGL context or allocation.
  Z3DScratchResourcePool::RenderTargetLease foreignLease(foreignPool);
  foreignLease.renderTarget = reinterpret_cast<Z3DRenderTarget*>(std::uintptr_t{1u});
  EXPECT_DEATH((void)renderer.describeSurface(foreignLease), "different physical pipeline");
  foreignLease.renderTarget = nullptr;
}

TEST(Z3DRendererOwnershipTest, BorrowedLeasePreservesPoolAffinityWithoutSharingReleaseOwnership)
{
  Z3DScratchResourcePool pool(RenderBackend::OpenGL);
  bool sourceReleased = false;
  Z3DScratchResourcePool::RenderTargetLease source(pool);
  source.backend = RenderBackend::Vulkan;
  source.renderTarget = reinterpret_cast<Z3DRenderTarget*>(std::uintptr_t{1u});
  source.attachments = 2u;
  source.releaser = {[](void* payload) {
                       *static_cast<bool*>(payload) = true;
                     },
                     &sourceReleased};

  {
    auto view = source.borrowedView();
    EXPECT_EQ(view.ownerPool(), &pool);
    EXPECT_EQ(view.backend, source.backend);
    EXPECT_EQ(view.renderTarget, source.renderTarget);
    EXPECT_EQ(view.attachments, source.attachments);
    EXPECT_FALSE(view.releaser);
  }

  EXPECT_FALSE(sourceReleased);
  source.release();
  EXPECT_TRUE(sourceReleased);
}

TEST(Z3DRendererOwnershipTest, ScratchPoolRejectsDirectVulkanDeviceRetargeting)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  Z3DScratchResourcePool pool(RenderBackend::OpenGL);
  auto* firstDevice = reinterpret_cast<ZVulkanDevice*>(std::uintptr_t{1u});
  auto* secondDevice = reinterpret_cast<ZVulkanDevice*>(std::uintptr_t{2u});

  // No Vulkan method is called: sentinels exercise only the pool's immutable
  // binding transition contract.
  pool.setVulkanDevice(firstDevice);
  EXPECT_DEATH(pool.setVulkanDevice(secondDevice), "explicitly unbound");
  pool.setVulkanDevice(nullptr);
}

TEST(Z3DGlobalParametersTest, FailedVulkanInitializationRestoresOpenGLAndCanRetry)
{
  Z3DScratchResourcePool scratchPool(RenderBackend::OpenGL);
  Z3DGlobalParameters parameters(scratchPool, RenderBackend::OpenGL);
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
