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
#include "z3dboundedfilter.h"
#include "zlog.h"
#include <absl/strings/str_cat.h>
#include <algorithm>
#include <vector>
#include <glm/gtc/type_ptr.hpp>
#include <glm/glm.hpp>

namespace nim {

namespace {

void prepareFilterForLease(Z3DBoundedFilter& filter, Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (lease.renderTarget != nullptr) {
    filter.setViewport(lease.renderTarget->size());
  } else {
    filter.setViewport(lease.descriptor.size);
  }
  filter.rendererBase().setActiveSurfaceForNextPass(lease);
}

void requestSurfaceClear(Z3DRendererBase& renderer,
                         bool clearColor,
                         const glm::vec4& color,
                         bool clearDepth,
                         float depth,
                         uint32_t stencil = 0u)
{
  if (!renderer.supportsCommandLists()) {
    return;
  }

  ClearValue clearValue{};
  clearValue.color = color;
  clearValue.depth = depth;
  clearValue.stencil = stencil;

  if (clearColor) {
    renderer.setPendingColorAttachmentsLoadStore(LoadOp::Clear, StoreOp::Store, clearValue);
  }
  if (clearDepth) {
    renderer.setPendingDepthAttachmentLoadStore(LoadOp::Clear, StoreOp::Store, clearValue);
  }
}

} // namespace

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
      ScopedFramebufferBind framebufferScope(batch.pass);
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

    }
  }

  void processCompositorPass(Z3DRendererBase& renderer, const Z3DCompositorPass& pass) override;

  [[nodiscard]] bool supportsCommandLists() const override
  {
    return true;
  }

  RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease) override
  {
    RendererFrameState::ActiveSurface surface;
    if (!lease || lease.backend != RenderBackend::OpenGL || !lease.hasGLRenderTarget()) {
      return surface;
    }

    auto& target = lease.glRenderTarget();
    const auto attachments = target.attachments();
    const GLuint colorBase = static_cast<GLuint>(GL_COLOR_ATTACHMENT0);
    const GLuint colorMax = colorBase + 31u;
    const GLuint depthAttachment = static_cast<GLuint>(GL_DEPTH_ATTACHMENT);
    const GLuint depthStencilAttachment = static_cast<GLuint>(GL_DEPTH_STENCIL_ATTACHMENT);

    for (const auto& [attachmentEnum, /*texture*/ _] : attachments) {
      const GLuint attachmentValue = static_cast<GLuint>(attachmentEnum);
      AttachmentDesc desc;
      desc.handle.backend = AttachmentBackend::OpenGL;
      desc.handle.id = reinterpret_cast<uint64_t>(&target);
      desc.handle.index = attachmentValue;

      if (attachmentValue >= colorBase && attachmentValue <= colorMax) {
        surface.colorAttachments.push_back(desc);
      } else if (attachmentValue == depthAttachment || attachmentValue == depthStencilAttachment) {
        surface.depthAttachment = desc;
      }
    }

    return surface;
  }

private:
  class ScopedFramebufferBind
  {
  public:
    explicit ScopedFramebufferBind(const BackendPassDesc& pass)
      : m_target(resolveTarget(pass))
    {
      if (m_target) {
        m_target->bind();
        configureDrawBuffers(pass.colorAttachments);
        applyLoadOps(pass);
      } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        applyLoadOps(pass);
      }
    }

    ScopedFramebufferBind(const ScopedFramebufferBind&) = delete;
    ScopedFramebufferBind& operator=(const ScopedFramebufferBind&) = delete;

    ~ScopedFramebufferBind()
    {
      if (m_target) {
        m_target->release();
      }
    }

  private:
    static Z3DRenderTarget* resolveTarget(const BackendPassDesc& pass)
    {
      for (const auto& attachment : pass.colorAttachments) {
        if (attachment.handle.backend == AttachmentBackend::OpenGL && attachment.handle.id != 0) {
          return reinterpret_cast<Z3DRenderTarget*>(attachment.handle.id);
        }
      }

      if (pass.depthAttachment && pass.depthAttachment->handle.backend == AttachmentBackend::OpenGL &&
          pass.depthAttachment->handle.id != 0) {
        return reinterpret_cast<Z3DRenderTarget*>(pass.depthAttachment->handle.id);
      }

      return nullptr;
    }

    static void configureDrawBuffers(const std::vector<AttachmentDesc>& colorAttachments)
    {
      if (colorAttachments.empty()) {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        return;
      }

      std::vector<GLenum> buffers;
      buffers.reserve(colorAttachments.size());
      for (const auto& attachment : colorAttachments) {
        buffers.push_back(static_cast<GLenum>(attachment.handle.index));
      }

      glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    }

    static void applyLoadOps(const BackendPassDesc& pass)
    {
      std::vector<GLenum> invalidateAttachments;
      invalidateAttachments.reserve(pass.colorAttachments.size() + (pass.depthAttachment ? 1 : 0));

      auto handleColorAttachment = [&](const AttachmentDesc& attachment) {
        const GLenum attachmentEnum = static_cast<GLenum>(attachment.handle.index);
        switch (attachment.loadOp) {
          case LoadOp::Clear: {
            const GLint drawBufferIndex = static_cast<GLint>(static_cast<int>(attachmentEnum) -
                                                             static_cast<int>(GL_COLOR_ATTACHMENT0));
            const glm::vec4& color = attachment.clearValue.color;
            const GLfloat clearColor[4] = {
              color.r,
              color.g,
              color.b,
              color.a
            };
            glClearBufferfv(GL_COLOR, drawBufferIndex, clearColor);
            break;
          }
          case LoadOp::DontCare:
            invalidateAttachments.push_back(attachmentEnum);
            break;
          case LoadOp::Load:
          default:
            break;
        }
      };

      for (const auto& attachment : pass.colorAttachments) {
        handleColorAttachment(attachment);
      }

      if (pass.depthAttachment) {
        const auto& depth = *pass.depthAttachment;
        const GLenum attachmentEnum = static_cast<GLenum>(depth.handle.index);
        switch (depth.loadOp) {
          case LoadOp::Clear: {
            const GLfloat depthValue = depth.clearValue.depth;
            const GLint stencilValue = static_cast<GLint>(depth.clearValue.stencil);
            if (attachmentEnum == GL_DEPTH_STENCIL_ATTACHMENT) {
              glClearBufferfi(GL_DEPTH_STENCIL, 0, depthValue, stencilValue);
            } else if (attachmentEnum == GL_DEPTH_ATTACHMENT) {
              glClearBufferfv(GL_DEPTH, 0, &depthValue);
            } else if (attachmentEnum == GL_STENCIL_ATTACHMENT) {
              glClearBufferiv(GL_STENCIL, 0, &stencilValue);
            }
            break;
          }
          case LoadOp::DontCare:
            invalidateAttachments.push_back(attachmentEnum);
            break;
          case LoadOp::Load:
          default:
            break;
        }
      }

      if (!invalidateAttachments.empty()) {
        glInvalidateFramebuffer(GL_FRAMEBUFFER,
                                static_cast<GLsizei>(invalidateAttachments.size()),
                                invalidateAttachments.data());
      }
    }

    Z3DRenderTarget* m_target = nullptr;
  };

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

} // namespace detail

void detail::Z3DRendererGLBackend::processCompositorPass(Z3DRendererBase& renderer,
                                                         const Z3DCompositorPass& pass)
{
  if (pass.kind != Z3DCompositorPass::Kind::Geometry) {
    LOG_FIRST_N(WARNING, 5) << "GL backend received unsupported compositor pass kind.";
    return;
  }

  if (pass.transparency != TransparencyMode::BlendDelayed &&
      pass.transparency != TransparencyMode::BlendNoDepthMask) {
    LOG_FIRST_N(WARNING, 5) << "GL compositor pass requested unsupported transparency path.";
    return;
  }

  if (pass.targetLease == nullptr || pass.targetLease->renderTarget == nullptr) {
    LOG_FIRST_N(WARNING, 5) << "GL compositor pass missing render target lease.";
    return;
  }

  if (!pass.imageLayers.empty()) {
    LOG_FIRST_N(WARNING, 5) << "GL compositor pass still relies on legacy image-layer blending; falling back not implemented.";
    return;
  }

  auto& lease = *pass.targetLease;
  auto* glTarget = lease.renderTarget;
  glTarget->bind();

  GLbitfield clearMask = 0;
  if (pass.clearColor) {
    clearMask |= static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
  }
  if (pass.clearDepth) {
    clearMask |= static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT);
  }
  if (pass.clearStencil) {
    clearMask |= static_cast<GLbitfield>(GL_STENCIL_BUFFER_BIT);
  }
  if (clearMask != 0u) {
    glClear(static_cast<gl::ClearBufferMask>(clearMask));
  }

  requestSurfaceClear(renderer,
                      pass.clearColor,
                      pass.clearValue.color,
                      pass.clearDepth,
                      pass.clearValue.depth,
                      pass.clearValue.stencil);

  for (auto* filter : pass.opaqueFilters) {
    if (!filter) {
      continue;
    }
    prepareFilterForLease(*filter, lease);
    filter->renderOpaque(pass.eye);
  }

  if (!pass.transparentFilters.empty()) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    const bool disableDepthWrites = (pass.transparency == TransparencyMode::BlendNoDepthMask);
    if (disableDepthWrites) {
      glDepthMask(GL_FALSE);
    }

    for (const auto& batch : pass.transparentFilters) {
      if (!batch.filter) {
        continue;
      }
      prepareFilterForLease(*batch.filter, lease);
      batch.filter->renderTransparent(pass.eye);
    }

    if (disableDepthWrites) {
      glDepthMask(GL_TRUE);
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
  }

  glTarget->release();
}

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend()
{
  return std::make_unique<detail::Z3DRendererGLBackend>();
}

} // namespace nim
