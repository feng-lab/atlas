#pragma once

#include "z3drendererbackend.h"
#include "z3drendererstates.h"
#include "zglmutils.h"
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nim {

class Z3DPrimitiveRenderer;

class Z3DTexture;

class Z3DShaderProgram;

class Z3DCamera;

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

  enum class RenderMethod
  {
    GLSL,
    LegacyOpenGL
  };

  struct ParameterState
  {
    glm::mat4 coordTransform{glm::mat4(1.f)};
    float sizeScale{1.f};
    float opacity{1.f};
    glm::vec4 materialAmbient{glm::vec4(0.1f, 0.1f, 0.1f, 1.f)};
    glm::vec4 materialSpecular{glm::vec4(1.f, 1.f, 1.f, 1.f)};
    float materialShininess{100.f};
    bool filterNotFrontFacing{true};
    RenderMethod renderMethod{RenderMethod::GLSL};
  };

  Z3DRendererBase(ParameterState& parameterState,
                  RendererFrameState& frameState,
                  RendererViewState& viewState,
                  RendererSceneState& sceneState);

  [[nodiscard]] RendererViewState pushViewStateFromCamera(const Z3DCamera& camera);

  void restoreViewState(const RendererViewState& state);

  static RendererViewState buildViewStateFromCamera(const Z3DCamera& camera, const glm::mat4& coordTransform);

  const glm::mat4& coordTransform() const
  {
    return m_parameters.coordTransform;
  }

  float sizeScale() const
  {
    return m_parameters.sizeScale;
  }

  void markRenderDataDirty();

  ParameterState& parameterState()
  {
    return m_parameters;
  }

  const ParameterState& parameterState() const
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

  void render(Z3DEye eye, Z3DPrimitiveRenderer& renderer)
  {
    render(eye, &renderer);
  }

  void render(Z3DEye eye, Z3DPrimitiveRenderer& renderer1, Z3DPrimitiveRenderer& renderer2)
  {
    render(eye, &renderer1, &renderer2);
  }

  void
  render(Z3DEye eye, Z3DPrimitiveRenderer& renderer1, Z3DPrimitiveRenderer& renderer2, Z3DPrimitiveRenderer& renderer3)
  {
    render(eye, &renderer1, &renderer2, &renderer3);
  }

  void render(Z3DEye eye,
              Z3DPrimitiveRenderer& renderer1,
              Z3DPrimitiveRenderer& renderer2,
              Z3DPrimitiveRenderer& renderer3,
              Z3DPrimitiveRenderer& renderer4)
  {
    render(eye, &renderer1, &renderer2, &renderer3, &renderer4);
  }

  void render(Z3DEye eye, Z3DPrimitiveRenderer* renderer);

  void render(Z3DEye eye, Z3DPrimitiveRenderer* renderer1, Z3DPrimitiveRenderer* renderer2);

  void
  render(Z3DEye eye, Z3DPrimitiveRenderer* renderer1, Z3DPrimitiveRenderer* renderer2, Z3DPrimitiveRenderer* renderer3);

  void render(Z3DEye eye,
              Z3DPrimitiveRenderer* renderer1,
              Z3DPrimitiveRenderer* renderer2,
              Z3DPrimitiveRenderer* renderer3,
              Z3DPrimitiveRenderer* renderer4);

  void render(Z3DEye eye, const std::vector<Z3DPrimitiveRenderer*>& renderers);

  void renderPicking(Z3DEye eye, Z3DPrimitiveRenderer& renderer)
  {
    renderPicking(eye, &renderer);
  }

  void renderPicking(Z3DEye eye, Z3DPrimitiveRenderer& renderer1, Z3DPrimitiveRenderer& renderer2)
  {
    renderPicking(eye, &renderer1, &renderer2);
  }

  void renderPicking(Z3DEye eye,
                     Z3DPrimitiveRenderer& renderer1,
                     Z3DPrimitiveRenderer& renderer2,
                     Z3DPrimitiveRenderer& renderer3)
  {
    renderPicking(eye, &renderer1, &renderer2, &renderer3);
  }

  void renderPicking(Z3DEye eye, Z3DPrimitiveRenderer* renderer);

  void renderPicking(Z3DEye eye, Z3DPrimitiveRenderer* renderer1, Z3DPrimitiveRenderer* renderer2);

  void renderPicking(Z3DEye eye,
                     Z3DPrimitiveRenderer* renderer1,
                     Z3DPrimitiveRenderer* renderer2,
                     Z3DPrimitiveRenderer* renderer3);

  void renderPicking(Z3DEye eye, const std::vector<Z3DPrimitiveRenderer*>& renderers);

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

  void setBackend(std::unique_ptr<Z3DRendererBackend> backend);

  Z3DRendererBackend& backend();

  const Z3DRendererBackend& backend() const;

protected:
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void generateDisplayList(const std::vector<Z3DPrimitiveRenderer*>& renderers);
  void generatePickingDisplayList(const std::vector<Z3DPrimitiveRenderer*>& renderers);

  void renderInstant(const std::vector<Z3DPrimitiveRenderer*>& renderers);
  void renderPickingInstant(const std::vector<Z3DPrimitiveRenderer*>& renderers);
#endif

  void renderUsingGLSL(Z3DEye eye, const std::vector<Z3DPrimitiveRenderer*>& renderers);

  void renderPickingUsingGLSL(Z3DEye eye, const std::vector<Z3DPrimitiveRenderer*>& renderers);

  bool needLighting(const std::vector<Z3DPrimitiveRenderer*>& renderers) const;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  bool useDisplayList(const std::vector<Z3DPrimitiveRenderer*>& renderers) const;
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

  void compile();

protected:
  ParameterState& m_parameters;
  RendererFrameState& m_frameState;
  RendererViewState& m_viewState;
  RendererSceneState& m_sceneState;
  // renderers
  std::set<Z3DPrimitiveRenderer*> m_renderers;

  std::vector<glm::vec4> m_clipPlanes;
  std::vector<glm::dvec4> m_doubleClipPlanes;
  bool m_clipEnabled;

  ShaderHookType m_shaderHookType;
  ShaderHookParameter m_shaderHookPara{};

private:
  std::set<Z3DPrimitiveRenderer*>::iterator m_renderersIt;
  std::unique_ptr<Z3DRendererBackend> m_backend;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  struct LegacyGLState;
  LegacyGLState& legacyGL();
  const LegacyGLState& legacyGL() const;
  std::unique_ptr<LegacyGLState> m_legacyGLState;
#endif
};

} // namespace nim
