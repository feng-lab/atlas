#include "z3drendererbackend.h"

#include "z3dcamera.h"
#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3drendererbase.h"
#include "z3drendertarget.h"
#include "z3dscratchresourcepool.h"
#include "z3dlinerenderer.h"
#include "z3dmeshrenderer.h"
#include "z3dellipsoidrenderer.h"
#include "z3dconerenderer.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include <absl/strings/str_cat.h>
#include <algorithm>

namespace nim {

namespace {

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
        shader.setFogEndUniform(static_cast<GLfloat>(scene.fog.range.y));
        shader.setFogScaleUniform(static_cast<GLfloat>(1.f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f)));
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
    absl::StrAppend(&header, "#define lowp\n#define mediump\n#define highp\n");
    fmt::format_to(std::back_inserter(header), "#define GLSL_VERSION {}\n", glslVer);
    if (!renderer.clipPlanes().empty()) {
      absl::StrAppend(&header, "#define HAS_CLIP_PLANE\n");
    }
    fmt::format_to(std::back_inserter(header), "#define CLIP_PLANE_COUNT {}\n", renderer.clipPlanes().size());

    auto appendFogMacro = [&](FogMode mode) {
      switch (mode) {
        case FogMode::Linear:
          absl::StrAppend(&header, "#define USE_LINEAR_FOG\n");
          break;
        case FogMode::Exponential:
          absl::StrAppend(&header, "#define USE_EXPONENTIAL_FOG\n");
          break;
        case FogMode::ExponentialSquared:
          absl::StrAppend(&header, "#define USE_SQUARED_EXPONENTIAL_FOG\n");
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
      absl::StrAppend(&header, "#define HAS_CLIP_PLANE\n");
    }
    fmt::format_to(std::back_inserter(header), "#define CLIP_PLANE_COUNT {}\n", renderer.clipPlanes().size());

    return header;
  }

  void beginRender(Z3DRendererBase& renderer) override
  {
    if (!renderer.clipEnabled()) {
      return;
    }
    const auto& clipPlanes = renderer.clipPlanes();
    for (size_t i = 0; i < clipPlanes.size(); ++i) {
      glEnable(GL_CLIP_DISTANCE0 + i);
    }
  }

  void endRender(Z3DRendererBase& renderer) override
  {
    if (!renderer.clipEnabled()) {
      return;
    }
    const auto& clipPlanes = renderer.clipPlanes();
    for (size_t i = 0; i < clipPlanes.size(); ++i) {
      glDisable(GL_CLIP_DISTANCE0 + i);
    }
  }

  void processBatches(Z3DRendererBase& /*renderer*/, const RendererCPUState& state) override
  {
    if (state.batches.empty()) {
      return;
    }

    for (const auto& batch : state.batches) {
      applyPassState(batch.pass);
      if (const auto* line = std::get_if<LinePayload>(&batch.geometry)) {
        if (line->renderer != nullptr) {
          line->renderer->executeBatchGL(batch);
        }
        continue;
      }
      if (const auto* mesh = std::get_if<MeshPayload>(&batch.geometry)) {
        if (mesh->renderer != nullptr) {
          mesh->renderer->executeBatchGL(batch);
        }
        continue;
      }
      if (const auto* ellipsoid = std::get_if<EllipsoidPayload>(&batch.geometry)) {
        if (ellipsoid->renderer != nullptr) {
          ellipsoid->renderer->executeBatchGL(batch);
        }
        continue;
      }
      if (const auto* cone = std::get_if<ConePayload>(&batch.geometry)) {
        if (cone->renderer != nullptr) {
          cone->renderer->executeBatchGL(batch);
        }
        continue;
      }
    }
  }

  [[nodiscard]] bool supportsCommandLists() const override
  {
    return true;
  }

  RendererFrameState::ActiveSurface
  describeSurfaceFromRenderTarget(const Z3DRenderTarget& target) override
  {
    RendererFrameState::ActiveSurface surface;
    const auto attachments = target.attachments();
    for (const auto& [attachmentEnum, texture] : attachments) {
      if (!texture) {
        continue;
      }
      AttachmentDesc desc;
      desc.handle.backend = AttachmentBackend::OpenGL;
      desc.handle.id = reinterpret_cast<uint64_t>(texture);
      if (attachmentEnum >= GL_COLOR_ATTACHMENT0 && attachmentEnum <= GL_COLOR_ATTACHMENT31) {
        desc.handle.index = static_cast<uint32_t>(attachmentEnum - GL_COLOR_ATTACHMENT0);
        surface.colorAttachments.push_back(desc);
      } else if (attachmentEnum == GL_DEPTH_ATTACHMENT || attachmentEnum == GL_DEPTH_STENCIL_ATTACHMENT) {
        desc.handle.index = 0u;
        surface.depthAttachment = desc;
      }
    }
    return surface;
  }

  RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease) override
  {
    if (lease.hasGLRenderTarget()) {
      return describeSurfaceFromRenderTarget(lease.glRenderTarget());
    }
    return RendererFrameState::ActiveSurface{};
  }

private:
  static void applyPassState(const BackendPassDesc& pass)
  {
    const auto& vp = pass.viewport;
    glViewport(static_cast<GLint>(vp.origin.x),
               static_cast<GLint>(vp.origin.y),
               static_cast<GLsizei>(vp.extent.x),
               static_cast<GLsizei>(vp.extent.y));

    if (pass.enableScissor) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(static_cast<GLint>(pass.scissorRect.x),
                static_cast<GLint>(pass.scissorRect.y),
                static_cast<GLsizei>(pass.scissorRect.z),
                static_cast<GLsizei>(pass.scissorRect.w));
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
  }
};

} // namespace

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend()
{
  return std::make_unique<Z3DRendererGLBackend>();
}

} // namespace nim
