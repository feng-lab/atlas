#pragma once

#include "zbbox.h"
#include "znumericparameter.h"
#include "z3dcameraparameter.h"
#include "z3drendererstates.h"
#include "z3dpickingmanager.h"
#include "z3dtypes.h"
#include "zoptionparameter.h"
#include "zcutspanparameter.h"
#include "z3dinteractionhandler.h"
#include "zfolly.h"
#include <vector>
#include <mutex>
#include <functional>

namespace nim {

class ZWidgetsGroup;

class Z3DRenderingEngine;
class Z3DScratchResourcePool;

struct Z3DLocalColorBuffer
{
  std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> data;
  size_t width;
  size_t height;
  // Optional external view for zero-copy (Vulkan readback ring)
  const uint8_t* external = nullptr; // mapped pointer (lifetime managed externally)
  size_t externalStride = 0; // bytes per row when using external pointer
  std::function<void()> externalRelease; // call to release mapped slot when replaced
};

class Z3DGlobalParameters : public QObject
{
  Q_OBJECT

private:
  Z3DScratchResourcePool& m_scratchPool;
  RendererViewState m_rendererViewState;
  RendererSceneState m_rendererSceneState;
  bool m_rendererViewStateDirty = true;
  bool m_rendererSceneStateDirty = true;

public:
  explicit Z3DGlobalParameters(Z3DScratchResourcePool& scratchPool, RenderBackend backend = RenderBackend::OpenGL);

  [[nodiscard]] Z3DScratchResourcePool& scratchPool()
  {
    return m_scratchPool;
  }

  [[nodiscard]] const Z3DScratchResourcePool& scratchPool() const
  {
    return m_scratchPool;
  }

  [[nodiscard]] RendererViewState& rendererViewState()
  {
    return m_rendererViewState;
  }

  [[nodiscard]] const RendererViewState& rendererViewState() const
  {
    return m_rendererViewState;
  }

  [[nodiscard]] RendererSceneState& rendererSceneState()
  {
    return m_rendererSceneState;
  }

  [[nodiscard]] const RendererSceneState& rendererSceneState() const
  {
    return m_rendererSceneState;
  }

  void ensureRendererState();

  void setDevicePixelRatio(float f);

  [[nodiscard]] const std::vector<ZParameter*>& parameters() const
  {
    return m_parameters;
  }

  void read(const json::object& json);

  void write(json::object& json) const;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup(bool includeCamera, Z3DRenderingEngine& engine);

  void cameraFocusesOn(double x, double y, double z, double radius = 64);

  void cameraFocusesOn(const ZBBox<glm::dvec3>& bound, double minRadius = 64);

  void cameraPointsTo(double x, double y, double z);

  void cameraPointsTo(const ZBBox<glm::dvec3>& bound);

  folly::CancellationToken currentMeshLodViewCancellationToken() const;

  void requestMeshLodViewCancellation();

  // Vulkan initialization is an external, recoverable failure. Restore the
  // selector and Vulkan-only transparency options before reporting the error.
  void restoreOpenGLAfterFailedVulkanInitialization();

private:
  void updateLightsArray();

  void updateTransparencyOptionsForBackend(RenderBackend backend);

  void markGlobalSceneStateDirty()
  {
    m_rendererSceneStateDirty = true;
  }

  void markGlobalViewStateDirty()
  {
    m_rendererViewStateDirty = true;
  }

  void addParameter(ZParameter& para)
  {
    m_parameters.push_back(&para);
  }

public:
  ZStringIntOptionParameter renderBackend;
  ZStringIntOptionParameter geometriesAAMode;
  ZStringIntOptionParameter transparencyMethod;
  ZFloatParameter weightedBlendedDepthScale;
  ZIntParameter lightCount;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightPositions;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightAmbients;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightDiffuses;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightSpeculars;
  // The light source's attenuation factors (x = constant, y = linear, z = quadratic)
  std::vector<std::unique_ptr<ZVec3Parameter>> lightAttenuations;
  std::vector<std::unique_ptr<ZFloatParameter>> lightSpotCutoff;
  std::vector<std::unique_ptr<ZFloatParameter>> lightSpotExponent;
  std::vector<std::unique_ptr<ZVec3Parameter>> lightSpotDirection;
  ZVec4Parameter sceneAmbient;

  // fog
  ZStringIntOptionParameter fogMode;
  ZVec3Parameter fogTopColor;
  ZVec3Parameter fogBottomColor;
  ZIntSpanParameter fogRange;
  ZFloatParameter fogDensity;

  Z3DCameraParameter camera;
  Z3DPickingManager pickingManager;
  // must add to network
  Z3DTrackballInteractionHandler interactionHandler;

  ZCutSpanParameter globalXCut;
  ZCutSpanParameter globalYCut;
  ZCutSpanParameter globalZCut;

  ZFloatParameter devicePixelRatio;

  std::mutex targetSwitchMutex;
  std::atomic_bool hasNewRendering = false;

private:
  std::vector<ZParameter*> m_parameters;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGrp;
  std::shared_ptr<ZWidgetsGroup> m_widgetsGrpNoCamera;

  size_t m_cameraParameterIndex = 0;
  mutable std::mutex m_meshLodViewCancellationMutex;
  std::shared_ptr<folly::CancellationSource> m_meshLodViewCancellationSource =
    std::make_shared<folly::CancellationSource>();

public:
  // Recompute cut spans in response to new scene bounds based on binding modes.
  void applyBoundsForCuts(const ZBBox<glm::dvec3>& bound);
};

} // namespace nim
