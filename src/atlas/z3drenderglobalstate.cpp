#include "z3drenderglobalstate.h"

#include "z3dscratchresourcepool.h"
#include "z3dglobalparameters.h"
#include "z3dcamera.h"
#include "z3dtypes.h"
#include "zlog.h"

#include <algorithm>

namespace nim {

RendererSceneState::LightingState buildLightingState(const Z3DGlobalParameters& params)
{
  RendererSceneState::LightingState lighting;

  const auto configuredCount = params.lightCount.get();
  const auto maxCount = static_cast<int>(params.lightPositions.size());
  constexpr int kMaxLights = 5;
  const auto clampedCount = std::clamp(configuredCount, 1, std::min(maxCount, kMaxLights));
  lighting.lightCount = clampedCount;

  const auto fill = [clampedCount](auto& destination, const auto& sources) {
    destination.resize(static_cast<size_t>(clampedCount));
    for (int i = 0; i < clampedCount; ++i) {
      destination[static_cast<size_t>(i)] = sources[static_cast<size_t>(i)]->get();
    }
  };

  fill(lighting.positions, params.lightPositions);
  fill(lighting.ambient, params.lightAmbients);
  fill(lighting.diffuse, params.lightDiffuses);
  fill(lighting.specular, params.lightSpeculars);
  fill(lighting.attenuation, params.lightAttenuations);
  fill(lighting.spotCutoff, params.lightSpotCutoff);
  fill(lighting.spotExponent, params.lightSpotExponent);
  fill(lighting.spotDirection, params.lightSpotDirection);

  return lighting;
}

RendererSceneState buildSceneState(const Z3DGlobalParameters& params)
{
  RendererSceneState state;
  state.sceneAmbient = params.sceneAmbient.get();
  state.weightedBlendedDepthScale = params.weightedBlendedDepthScale.get();
  state.devicePixelRatio = params.devicePixelRatio.get();
  TransparencyMode transparency = static_cast<TransparencyMode>(params.transparencyMethod.associatedData());
  const RenderBackend backend = static_cast<RenderBackend>(params.renderBackend.associatedData());
  if (backend == RenderBackend::OpenGL && transparency == TransparencyMode::PerPixelFragmentList) {
    // PPLL is Vulkan-only; fall back to DDP for OpenGL while preserving the UI selection.
    transparency = TransparencyMode::DualDepthPeeling;
  }
  state.transparency = transparency;
  state.geometryAAMode = static_cast<GeometryAAMode>(params.geometriesAAMode.associatedData());

  state.lighting = buildLightingState(params);

  state.fog.mode = static_cast<FogMode>(params.fogMode.associatedData());
  state.fog.topColor = params.fogTopColor.get();
  state.fog.bottomColor = params.fogBottomColor.get();
  const glm::ivec2 fogRange = params.fogRange.get();
  state.fog.range = glm::vec2(fogRange);
  state.fog.density = params.fogDensity.get();

  return state;
}

RendererViewState buildViewState(const Z3DCamera& camera)
{
  RendererViewState state;
  state.nearClip = camera.nearDist();
  state.farClip = camera.farDist();

  const auto eyePosition = camera.eye();
  const auto isPerspective = camera.isPerspectiveProjection();
  const auto frustumNearPlaneSize = camera.frustumNearPlaneSize();
  const auto fieldOfView = camera.fieldOfView();

  for (int eyeValue = LeftEye; eyeValue <= RightEye; ++eyeValue) {
    auto eye = static_cast<Z3DEye>(eyeValue);
    auto& eyeState = state.eyes[static_cast<size_t>(eye)];
    eyeState.viewMatrix = camera.viewMatrix(eye);
    eyeState.projectionMatrix = camera.projectionMatrix(eye);
    eyeState.projectionViewMatrix = camera.projectionViewMatrix(eye);
    eyeState.inverseViewMatrix = camera.inverseViewMatrix(eye);
    eyeState.inverseProjectionMatrix = camera.inverseProjectionMatrix(eye);
    eyeState.normalMatrix = camera.normalMatrix(eye);
    eyeState.eyePosition = eyePosition;
    eyeState.isPerspective = isPerspective;
    eyeState.frustumNearPlaneSize = frustumNearPlaneSize;
    eyeState.fieldOfView = fieldOfView;
  }

  return state;
}

Z3DRenderGlobalState& Z3DRenderGlobalState::instance()
{
  static Z3DRenderGlobalState state;
  return state;
}

Z3DRenderGlobalState::Z3DRenderGlobalState() = default;

uint32_t Z3DRenderGlobalState::nextPerfFrameSubmissionId(uint64_t token)
{
  CHECK_GT(token, 0u);
  CHECK_EQ(token, m_currentPerfFrameToken) << "Perf submission ID requested for a non-current token. token=" << token
                                           << " current=" << m_currentPerfFrameToken;
  return ++m_currentPerfFrameSubmissionCursor;
}

Z3DScratchResourcePool& Z3DRenderGlobalState::accessScratchPool() const
{
  CHECK(m_scratchPool != nullptr) << "Scratch pool not initialized";
  return *m_scratchPool;
}

Z3DScratchResourcePool& Z3DRenderGlobalState::scratchPool()
{
  return accessScratchPool();
}

void Z3DRenderGlobalState::trimScratchPool()
{
  if (m_scratchPool) {
    m_scratchPool->trim();
  }
}

bool Z3DRenderGlobalState::hasScratchPool() const
{
  return m_scratchPool != nullptr;
}

void Z3DRenderGlobalState::setScratchPool(Z3DScratchResourcePool* pool)
{
  m_scratchPool = pool;
}

void Z3DRenderGlobalState::resetScratchPool()
{
  m_scratchPool = nullptr;
}

bool Z3DRenderGlobalState::hasCancellationSource() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  return static_cast<bool>(m_cancellationSource);
}

std::shared_ptr<folly::CancellationSource> Z3DRenderGlobalState::ensureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (!m_cancellationSource) {
    m_cancellationSource = std::make_shared<folly::CancellationSource>();
  }
  return m_cancellationSource;
}

void Z3DRenderGlobalState::resetCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  m_cancellationSource.reset();
}

void Z3DRenderGlobalState::requestCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_cancellationMutex);
    source = m_cancellationSource;
  }
  if (source) {
    source->requestCancellation();
  }
}

std::shared_ptr<folly::CancellationSource> Z3DRenderGlobalState::ensureCaptureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (!m_captureCancellationSource) {
    m_captureCancellationSource = std::make_shared<folly::CancellationSource>();
  }
  return m_captureCancellationSource;
}

void Z3DRenderGlobalState::resetCaptureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  m_captureCancellationSource.reset();
}

void Z3DRenderGlobalState::requestCaptureCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_cancellationMutex);
    source = m_captureCancellationSource;
  }
  if (source) {
    source->requestCancellation();
  }
}

folly::CancellationToken Z3DRenderGlobalState::currentCancellationToken() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (m_cancellationSource && m_captureCancellationSource) {
    return folly::cancellation_token_merge(m_cancellationSource->getToken(), m_captureCancellationSource->getToken());
  }
  if (m_cancellationSource) {
    return m_cancellationSource->getToken();
  }
  if (m_captureCancellationSource) {
    return m_captureCancellationSource->getToken();
  }
  return folly::CancellationToken();
}

void Z3DRenderGlobalState::markViewStateDirty()
{
  m_rendererState.viewDirty = true;
}

void Z3DRenderGlobalState::markSceneStateDirty()
{
  m_rendererState.sceneDirty = true;
}

void Z3DRenderGlobalState::ensureViewState(const Z3DCamera& camera)
{
  if (!m_rendererState.viewDirty) {
    return;
  }

  m_rendererState.viewState = buildViewState(camera);
  m_rendererState.viewDirty = false;
}

void Z3DRenderGlobalState::ensureSceneState(const Z3DGlobalParameters& params)
{
  if (!m_rendererState.sceneDirty) {
    return;
  }

  m_rendererState.sceneState = buildSceneState(params);
  m_rendererState.sceneDirty = false;
}

} // namespace nim
