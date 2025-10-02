#pragma once

#include "z3drendererbackend.h"
#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "zglmutils.h"
#include <array>
#include <concepts>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace nim {

class Z3DPrimitiveRenderer;

class Z3DTexture;

class Z3DShaderProgram;

class Z3DCamera;

class Z3DScratchResourcePool;

enum class ScratchFormat;

struct Z3DCompositorPass;

namespace detail {

template<typename T>
using Decayed = std::remove_cvref_t<T>;

template<typename T>
using Pointee = std::remove_cv_t<std::remove_pointer_t<Decayed<T>>>;

template<typename T>
concept RendererArgument = std::derived_from<Decayed<T>, Z3DPrimitiveRenderer> ||
                           (std::is_pointer_v<Decayed<T>> && std::derived_from<Pointee<T>, Z3DPrimitiveRenderer>);

inline Z3DPrimitiveRenderer* toRendererPointer(Z3DPrimitiveRenderer* renderer)
{
  return renderer;
}

inline Z3DPrimitiveRenderer* toRendererPointer(Z3DPrimitiveRenderer& renderer)
{
  return &renderer;
}

} // namespace detail

// contains basic properties such as lighting, method, size for rendering.
// A renderBase usually contains multiple primitive renderers. Some of those are
// combined to draw a complicated object. Some of those are just sharing the environment
// (rendering parameters).
class Z3DRendererBase
{
public:
  enum class ShaderHookType
  {
    Normal,
    DualDepthPeelingInit,
    DualDepthPeelingPeel,
    WeightedAverageInit,
    WeightedBlendedInit
  };

  struct ShaderHookParameter
  {
    const Z3DTexture* dualDepthPeelingDepthBlenderTexture = nullptr;
    const Z3DTexture* dualDepthPeelingFrontBlenderTexture = nullptr;
  };

  // need valid camera and viewport
  void setGlobalShaderParameters(Z3DShaderProgram& shader, Z3DEye eye);

  void setGlobalShaderParameters(Z3DShaderProgram* shader, Z3DEye eye);

  [[nodiscard]] std::string generateHeader() const;

  [[nodiscard]] std::string generateGeomHeader() const;

  // renderer's constructor and deconstructor will take care of this
  void registerRenderer(Z3DPrimitiveRenderer* renderer);

  void unregisterRenderer(Z3DPrimitiveRenderer* renderer);

  void releaseBackendResources();

  void buildBackendResources(RenderBackend backend);

  Z3DRendererBase(RendererParameterState& parameterState,
                  RendererFrameState& frameState,
                  RendererViewState& viewState,
                  RendererSceneState& sceneState);

  [[nodiscard]] RendererViewState pushViewStateFromCamera(const Z3DCamera& camera);

  void restoreViewState(const RendererViewState& state);

  // ---------------------------------------------------------------------------
  // Command façade helpers (no-op until renderers adopt them)
  // ---------------------------------------------------------------------------
  void resetCPUState();

  void appendBatch(RenderBatch batch);

  [[nodiscard]] const RendererCPUState& cpuState() const;

  [[nodiscard]] RendererCPUState& cpuState();

  void submitBatches();

  virtual ~Z3DRendererBase();

  void releasePersistentLeases();

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentTempRenderTarget2D(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size,
    ScratchFormat colorFormat = ScratchFormat::RGBA16,
    ScratchFormat depthFormat = ScratchFormat::Depth24);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentDualDepthPeelRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentWeightedAverageRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentWeightedBlendedRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentRaycastAccumulatorRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentLayerArrayRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size,
    uint32_t layers,
    ScratchFormat colorFormat = ScratchFormat::RGBA16,
    ScratchFormat depthFormat = ScratchFormat::Depth24);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentEntryExitRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& size,
    uint32_t layers = 2,
    ScratchFormat colorFormat = ScratchFormat::RGBA32F);

  Z3DScratchResourcePool::RenderTargetLease& acquirePersistentBlockIdRenderTarget(
    Z3DScratchResourcePool::RenderTargetLease& lease,
    const glm::uvec2& viewport,
    int requestedAttachments = -1,
    double scale = -1.0);

  void executeCompositorPass(const Z3DCompositorPass& pass);

  void setActiveSurfaceForNextPass(const RendererFrameState::ActiveSurface& surface);
  void setActiveSurfaceForNextPass(RendererFrameState::ActiveSurface&& surface);
  void setActiveSurfaceForNextPass(const Z3DScratchResourcePool::RenderTargetLease& lease);
  void setPendingColorAttachmentsLoadStore(LoadOp loadOp,
                                           StoreOp storeOp,
                                           const ClearValue& clearValue = {});
  void setPendingDepthAttachmentLoadStore(LoadOp loadOp,
                                          StoreOp storeOp,
                                          const ClearValue& clearValue = {});
  void clearPendingActiveSurface();

  RendererFrameState::ActiveSurface
  describeSurface(const Z3DScratchResourcePool::RenderTargetLease& lease);

  struct VulkanSurfaceBindings
  {
    RendererFrameState::ActiveSurface surface;
    std::vector<AttachmentHandle> colorHandles;
    std::optional<AttachmentHandle> depthHandle;

    [[nodiscard]] bool valid() const
    {
      return !colorHandles.empty() || depthHandle.has_value();
    }
  };

  VulkanSurfaceBindings
  prepareVulkanSurface(const Z3DScratchResourcePool::RenderTargetLease& lease);

  void executeVulkanBatches(const std::function<void()>& recordBatches);

  [[nodiscard]] bool supportsCommandLists() const;

  static RendererViewState buildViewStateFromCamera(const Z3DCamera& camera);

  void markRenderDataDirty()
  {
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
    invalidateDisplayList();
    invalidatePickingDisplayList();
#endif
  }

  RendererParameterState& parameterState()
  {
    return m_parameters;
  }

  const RendererParameterState& parameterState() const
  {
    return m_parameters;
  }

  RendererFrameState& frameState()
  {
    return m_frameState;
  }

  const RendererFrameState& frameState() const
  {
    return m_frameState;
  }

  RendererViewState& viewState()
  {
    return m_viewState;
  }

  const RendererViewState& viewState() const
  {
    return m_viewState;
  }

  RendererSceneState& sceneState()
  {
    return m_sceneState;
  }

  const RendererSceneState& sceneState() const
  {
    return m_sceneState;
  }

  void setClipPlanes(std::vector<glm::vec4>* clipPlanes);

  void setClipEnabled(bool v)
  {
    m_clipEnabled = v;
  }

  using RendererSpan = std::span<Z3DPrimitiveRenderer*>;

  template<detail::RendererArgument... Renderers>
  void render(Z3DEye eye, Renderers&&... renderers)
  {
    static_assert(sizeof...(Renderers) > 0, "render requires at least one renderer");
    std::array<Z3DPrimitiveRenderer*, sizeof...(Renderers)> rendererPtrs{
      detail::toRendererPointer(std::forward<Renderers>(renderers))...};
    render(eye, RendererSpan(rendererPtrs.data(), rendererPtrs.size()));
  }

  void render(Z3DEye eye, RendererSpan renderers);

  template<detail::RendererArgument... Renderers>
  void renderPicking(Z3DEye eye, Renderers&&... renderers)
  {
    static_assert(sizeof...(Renderers) > 0, "renderPicking requires at least one renderer");
    std::array<Z3DPrimitiveRenderer*, sizeof...(Renderers)> rendererPtrs{
      detail::toRendererPointer(std::forward<Renderers>(renderers))...};
    renderPicking(eye, RendererSpan(rendererPtrs.data(), rendererPtrs.size()));
  }

  void renderPicking(Z3DEye eye, RendererSpan renderers);

  void setShaderHookType(ShaderHookType t)
  {
    m_shaderHookType = t;
  }

  ShaderHookType shaderHookType() const
  {
    return m_shaderHookType;
  }

  ShaderHookParameter& shaderHookPara()
  {
    return m_shaderHookPara;
  }

  void setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t)
  {
    m_shaderHookPara.dualDepthPeelingDepthBlenderTexture = t;
  }

  void setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t)
  {
    m_shaderHookPara.dualDepthPeelingFrontBlenderTexture = t;
  }

  const std::vector<glm::vec4>& clipPlanes() const
  {
    return m_clipPlanes;
  }

  bool clipEnabled() const
  {
    return m_clipEnabled;
  }

  void setBackend(RenderBackend backendType);

  RenderBackend activeBackend() const
  {
    return m_activeBackend;
  }

  void compile();

protected:
  enum class RenderMethod
  {
    GLSL,
    LegacyOpenGL
  };

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void generateDisplayList(RendererSpan renderers);
  void generatePickingDisplayList(RendererSpan renderers);

  void renderInstant(RendererSpan renderers);
  void renderPickingInstant(RendererSpan renderers);
#endif

  void renderUsingGLSL(Z3DEye eye, RendererSpan renderers);

  void renderPickingUsingGLSL(Z3DEye eye, RendererSpan renderers);

  bool needLighting(RendererSpan renderers) const;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  bool useDisplayList(RendererSpan renderers) const;
#endif

  bool hasClipPlanes()
  {
    return !m_clipPlanes.empty();
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void activateClipPlanesOpenGL();
  void deactivateClipPlanesOpenGL();
#endif

private:
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void invalidateDisplayList();
  void invalidatePickingDisplayList();
#endif

  void registerPersistentLease(Z3DScratchResourcePool::RenderTargetLease& lease);

protected:
  RendererParameterState& m_parameters;
  RendererFrameState& m_frameState;
  RendererViewState& m_viewState;
  RendererSceneState& m_sceneState;

  std::optional<RendererFrameState::ActiveSurface> m_pendingActiveSurface;
  // renderers
  std::set<Z3DPrimitiveRenderer*> m_renderers;

  RendererCPUState m_cpuState;

  std::vector<glm::vec4> m_clipPlanes;
  std::vector<glm::dvec4> m_doubleClipPlanes;
  bool m_clipEnabled;

  ShaderHookType m_shaderHookType;
  ShaderHookParameter m_shaderHookPara{};

private:
  std::set<Z3DPrimitiveRenderer*>::iterator m_renderersIt;
  std::unique_ptr<Z3DRendererBackend> m_backend;
  RenderBackend m_activeBackend = RenderBackend::OpenGL;
  RenderMethod m_renderMethod;

  std::vector<Z3DScratchResourcePool::RenderTargetLease*> m_persistentLeases;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  struct LegacyGLState;
  LegacyGLState& legacyGL();
  const LegacyGLState& legacyGL() const;
  std::unique_ptr<LegacyGLState> m_legacyGLState;
#endif
};

} // namespace nim
