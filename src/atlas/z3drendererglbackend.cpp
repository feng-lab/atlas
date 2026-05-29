#include "z3drendererbackend.h"

#include "z3dgpuinfo.h"
#include "z3drendererbase.h"
#include "z3dshaderprogram.h"

#include <algorithm>

namespace nim {

namespace detail {

class Z3DRendererGLBackend final : public Z3DRendererBackend
{
public:
  void setGlobalShaderParameters(Z3DRendererBase& renderer, Z3DShaderProgram& shader, Z3DEye eye) override
  {
    const auto& frameState = renderer.frameState();
    shader.setScreenDimUniform(glm::vec2(frameState.viewport.z, frameState.viewport.w));
    shader.setScreenDimRCPUniform(1.f / glm::vec2(frameState.viewport.z, frameState.viewport.w));

    const auto& eyeState = renderer.viewState().eyes[eye];
    shader.setCameraPositionUniform(eyeState.eyePosition);
    shader.setViewMatrixUniform(eyeState.viewMatrix);
    shader.setViewMatrixInverseUniform(eyeState.inverseViewMatrix);
    shader.setProjectionMatrixUniform(eyeState.projectionMatrix);
    shader.setProjectionMatrixInverseUniform(eyeState.inverseProjectionMatrix);
    shader.setNormalMatrixUniform(eyeState.normalMatrix);
    shader.setProjectionViewMatrixUniform(eyeState.projectionViewMatrix);
    shader.setOrthoUniform(eyeState.isPerspective ? 0.f : 1.f);

    shader.setViewportMatrixUniform(frameState.viewportMatrix);
    shader.setViewportMatrixInverseUniform(frameState.inverseViewportMatrix);

    shader.setGammaUniform(2.f);

    const auto& params = renderer.parameterState();
    shader.setSizeScaleUniform(params.sizeScale);
    shader.setPosTransformUniform(params.coordTransform);
    if (shader.hasPosTransformNormalMatrixUniform()) {
      const glm::mat4 combined = eyeState.viewMatrix * params.coordTransform;
      const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
      shader.setPosTransformNormalMatrixUniform(normalMatrix);
    }

    const auto& scene = renderer.sceneState();
    constexpr int kMaxLights = 5;
    const auto& lighting = scene.lighting;
    int requestedLightCount = std::max(0, std::min(lighting.lightCount, static_cast<int>(lighting.positions.size())));
    const int clampedLightCount = std::min(requestedLightCount, kMaxLights);
    if (clampedLightCount > 0) {
      shader.setLightsPositionUniform(lighting.positions.data(), clampedLightCount);
      shader.setLightsAmbientUniform(lighting.ambient.data(), clampedLightCount);
      shader.setLightsDiffuseUniform(lighting.diffuse.data(), clampedLightCount);
      shader.setLightsSpecularUniform(lighting.specular.data(), clampedLightCount);
      shader.setLightsSpotCutoffUniform(lighting.spotCutoff.data(), clampedLightCount);
      shader.setLightsAttenuationUniform(lighting.attenuation.data(), clampedLightCount);
      shader.setLightsSpotExponentUniform(lighting.spotExponent.data(), clampedLightCount);
      shader.setLightsSpotDirectionUniform(lighting.spotDirection.data(), clampedLightCount);
    }
    shader.setLightCountUniform(clampedLightCount);

    shader.setMaterialSpecularUniform(params.materialSpecular);
    shader.setMaterialShininessUniform(params.materialShininess);
    shader.setMaterialAmbientUniform(params.materialAmbient);
    shader.setSceneAmbientUniform(scene.sceneAmbient);
    shader.setAlphaUniform(params.opacity);

    constexpr float kLog2e = 1.44269504088896340735992468100189214f;

    switch (scene.fog.mode) {
      case FogMode::Linear:
        shader.setFogColorTopUniform(scene.fog.topColor);
        shader.setFogColorBottomUniform(scene.fog.bottomColor);
        shader.setFogEndUniform(static_cast<float>(scene.fog.range.y));
        shader.setFogScaleUniform(static_cast<float>(1.f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f)));
        break;
      case FogMode::Exponential:
        shader.setFogColorTopUniform(scene.fog.topColor);
        shader.setFogColorBottomUniform(scene.fog.bottomColor);
        shader.setFogDensityLog2eUniform(scene.fog.density * kLog2e);
        break;
      case FogMode::ExponentialSquared:
        shader.setFogColorTopUniform(scene.fog.topColor);
        shader.setFogColorBottomUniform(scene.fog.bottomColor);
        shader.setFogDensityDensityLog2eUniform(scene.fog.density * scene.fog.density * kLog2e);
        break;
      case FogMode::None:
        break;
    }

    shader.setClipPlanesUniform(renderer.clipPlanes().data(), renderer.clipPlanes().size());
    const size_t clipPlaneCount = renderer.clipPlanes().size();
    const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
    const size_t clipDistanceCount = std::min(clipPlaneCount, static_cast<size_t>(deviceMaxClipDistances));
    if (clipPlaneCount > clipDistanceCount) {
      shader.setUniform("clip_planes_enabled", renderer.clipEnabled() ? 1 : 0);
    }
  }

  [[nodiscard]] std::string generateHeader(const Z3DRendererBase& renderer) const override
  {
    std::string glslVer =
      fmt::format("{}{}", Z3DGpuInfo::instance().glslMajorVersion(), Z3DGpuInfo::instance().glslMinorVersion());
    if (glslVer.size() < 3U) {
      glslVer.push_back('0');
    }

    std::string header;
    header.reserve(256);

    fmt::format_to(std::back_inserter(header), "#version {}\n", glslVer);
    header.append("#define lowp\n#define mediump\n#define highp\n");
    fmt::format_to(std::back_inserter(header), "#define GLSL_VERSION {}\n", glslVer);
    if (!renderer.clipPlanes().empty()) {
      header.append("#define HAS_CLIP_PLANE\n");
    }
    const size_t clipPlaneCount = renderer.clipPlanes().size();
    const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
    const size_t clipDistanceCount = std::min(clipPlaneCount, static_cast<size_t>(deviceMaxClipDistances));

    fmt::format_to(std::back_inserter(header), "#define CLIP_PLANE_COUNT {}\n", clipPlaneCount);
    fmt::format_to(std::back_inserter(header), "#define CLIP_DISTANCE_COUNT {}\n", clipDistanceCount);
    fmt::format_to(std::back_inserter(header),
                   "#define EXTRA_CLIP_PLANE_COUNT {}\n",
                   clipPlaneCount - clipDistanceCount);
    // Runtime uniform that mirrors Z3DRendererBase::clipEnabled(). When extra clip
    // planes overflow the fixed-function clip distance limit, shaders fall back
    // to fragment-stage discard using interpolated distances.
    header.append("uniform bool clip_planes_enabled;\n");

    auto appendFogMacro = [&](FogMode mode) {
      switch (mode) {
        case FogMode::Linear:
          header.append("#define USE_LINEAR_FOG\n");
          break;
        case FogMode::Exponential:
          header.append("#define USE_EXPONENTIAL_FOG\n");
          break;
        case FogMode::ExponentialSquared:
          header.append("#define USE_SQUARED_EXPONENTIAL_FOG\n");
          break;
        case FogMode::None:
          break;
      }
    };

    appendFogMacro(renderer.sceneState().fog.mode);

    return header;
  }

  [[nodiscard]] std::string generateGeomHeader(const Z3DRendererBase& renderer) const override
  {
    std::string glslVer =
      fmt::format("{}{}", Z3DGpuInfo::instance().glslMajorVersion(), Z3DGpuInfo::instance().glslMinorVersion());
    if (glslVer.size() < 3U) {
      glslVer.push_back('0');
    }

    std::string header;
    header.reserve(128);

    fmt::format_to(std::back_inserter(header), "#version {}\n", glslVer);
    fmt::format_to(std::back_inserter(header), "#define GLSL_VERSION {}\n", glslVer);

    if (!renderer.clipPlanes().empty()) {
      header.append("#define HAS_CLIP_PLANE\n");
    }
    const size_t clipPlaneCount = renderer.clipPlanes().size();
    const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
    const size_t clipDistanceCount = std::min(clipPlaneCount, static_cast<size_t>(deviceMaxClipDistances));

    fmt::format_to(std::back_inserter(header), "#define CLIP_PLANE_COUNT {}\n", clipPlaneCount);
    fmt::format_to(std::back_inserter(header), "#define CLIP_DISTANCE_COUNT {}\n", clipDistanceCount);
    fmt::format_to(std::back_inserter(header),
                   "#define EXTRA_CLIP_PLANE_COUNT {}\n",
                   clipPlaneCount - clipDistanceCount);
    header.append("uniform bool clip_planes_enabled;\n");

    return header;
  }

  void beginRender(Z3DRendererBase&) override {}

  void endRender(Z3DRendererBase&) override {}

  [[nodiscard]] bool supportsCommandLists() const override
  {
    return false;
  }

  RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& /*lease*/) override
  {
    return RendererFrameState::ActiveSurface{};
  }
};

} // namespace detail

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend()
{
  return std::make_unique<detail::Z3DRendererGLBackend>();
}

} // namespace nim
