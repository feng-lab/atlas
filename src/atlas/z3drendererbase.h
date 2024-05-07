#pragma once

#include "z3dtransformparameter.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "z3dcamera.h"
#include "z3dglobalparameters.h"
#include <QObject>
#include <set>
#include <vector>

namespace nim {

class Z3DPrimitiveRenderer;

class Z3DTexture;

class Z3DShaderProgram;

// contains basic properties such as lighting, method, size for rendering.
// A renderBase usually contains multiple primitive renderers. Some of those are
// combined to draw a complicated object. Some of those are just sharing the environment
// (rendering parameters).
class Z3DRendererBase : public QObject
{
  Q_OBJECT

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
    const Z3DTexture* dualDepthPeelingDepthBlenderTexture;
    const Z3DTexture* dualDepthPeelingFrontBlenderTexture;
  };

  explicit Z3DRendererBase(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setCamera(const Z3DCamera& c)
  {
    m_camera = c;
    m_hasCustomCamera = true;
    makeCoordTransformNormalMatrix();
  }

  void unsetCamera()
  {
    m_hasCustomCamera = false;
  }

  Z3DCamera& camera()
  {
    return m_hasCustomCamera ? m_camera : m_globalParas.camera.get();
  }

  Z3DCamera& globalCamera()
  {
    return m_globalParas.camera.get();
  }

  Z3DCameraParameter& globalCameraPara()
  {
    return m_globalParas.camera;
  }

  const ZFloatSpanParameter& globalXCutPara() const
  {
    return m_globalParas.globalXCut;
  }

  const ZFloatSpanParameter& globalYCutPara() const
  {
    return m_globalParas.globalYCut;
  }

  const ZFloatSpanParameter& globalZCutPara() const
  {
    return m_globalParas.globalZCut;
  }

  Z3DGlobalParameters& globalParas()
  {
    return m_globalParas;
  }

  const Z3DGlobalParameters& globalParas() const
  {
    return m_globalParas;
  }

  ZStringIntOptionParameter& geometriesMultisampleModePara()
  {
    return m_globalParas.geometriesMultisampleMode;
  }

  ZStringIntOptionParameter& transparencyMethodPara()
  {
    return m_globalParas.transparencyMethod;
  }

  void setViewport(const glm::uvec4& viewport)
  {
    if (m_viewport != viewport) {
      m_viewport = viewport;
      makeViewportMatrix();
    }
  }

  void setViewport(const glm::uvec2& viewport)
  {
    if (m_viewport.zw() != viewport) {
      m_viewport = glm::ivec4(0, 0, viewport);
      makeViewportMatrix();
    }
  }

  glm::uvec4 viewport() const
  {
    return m_viewport;
  }

  // need valid camera and viewport
  void setGlobalShaderParameters(Z3DShaderProgram& shader, Z3DEye eye);

  void setGlobalShaderParameters(Z3DShaderProgram* shader, Z3DEye eye);

  QString generateHeader() const;

  QString generateGeomHeader() const;

  // renderer's constructor and deconstructor will take care of this
  void registerRenderer(Z3DPrimitiveRenderer* renderer);

  void unregisterRenderer(Z3DPrimitiveRenderer* renderer);

  Z3DTransformParameter& coordTransformPara()
  {
    return m_coordTransform;
  }

  void setSizeScale(float s)
  {
    m_sizeScale.set(s);
  }

  void setXScale(float s)
  {
    m_coordTransform.setXScale(s);
  }

  void setYScale(float s)
  {
    m_coordTransform.setYScale(s);
  }

  void setZScale(float s)
  {
    m_coordTransform.setZScale(s);
  }

  void setScale(float x, float y, float z)
  {
    m_coordTransform.setScale(glm::vec3(x, y, z));
  }

  void setOffset(float x, float y, float z)
  {
    m_coordTransform.translate(x, y, z);
  }

  void setRotationCenter(const glm::vec3& c)
  {
    m_coordTransform.setRotationCenter(c);
  }

  void setOpacity(float o)
  {
    m_opacity.set(o);
  }

  void setMaterialSpecular(const glm::vec4& v)
  {
    m_materialSpecular.set(v);
  }

  void setMaterialAmbient(const glm::vec4& v)
  {
    m_materialAmbient.set(v);
  }

  void setClipPlanes(std::vector<glm::vec4>* clipPlanes);

  void setClipEnabled(bool v)
  {
    m_clipEnabled = v;
  }

  void addParameter(ZParameter& para)
  {
    addParameter(&para);
  }

  void addParameter(ZParameter* para)
  {
    m_parameters.push_back(para);
  }

  // parameters of rendererbase
  const std::vector<ZParameter*>& parameters() const
  {
    return m_parameters;
  }

  // only global parameters
  const std::vector<ZParameter*>& globalParameters() const
  {
    return m_globalParas.parameters();
  }

  glm::mat4 coordTransform() const
  {
    return m_coordTransform.get();
  }

  glm::mat4 inverseCoordTransform() const
  {
    return glm::inverse(m_coordTransform.get());
  }

  float opacity() const
  {
    return m_opacity.get();
  }

  float sizeScale() const
  {
    return m_sizeScale.get();
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

  const glm::mat4& viewportMatrix() const
  {
    return m_viewportMatrix;
  }

  const glm::mat4& inverseViewportMatrix() const
  {
    return m_inverseViewportMatrix;
  }

Q_SIGNALS:
  void coordTransformChanged();

  void sizeScaleChanged();

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

  void activateClipPlanesGLSL();

  void deactivateClipPlanesGLSL();

  void makeViewportMatrix();

private:
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void invalidateDisplayList();
  void invalidatePickingDisplayList();
#endif

  void compile();

  void makeCoordTransformNormalMatrix();

protected:
  Z3DGlobalParameters& m_globalParas;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  // display list generated from the geometry.
  GLuint m_displayList;
  GLuint m_pickingDisplayList;
#endif

  Z3DTransformParameter m_coordTransform;
  ZStringIntOptionParameter m_renderMethod;
  glm::mat3 m_coordTransformNormalMatrices[3];

  ZFloatParameter m_sizeScale;
  ZFloatParameter m_opacity;

  ZBoolParameter m_filterNotFrontFacing;

  ZVec4Parameter m_materialAmbient;
  ZVec4Parameter m_materialSpecular;
  ZFloatParameter m_materialShininess;

  bool m_hasCustomCamera;
  Z3DCamera m_camera;
  glm::uvec4 m_viewport;
  glm::mat4 m_viewportMatrix;
  glm::mat4 m_inverseViewportMatrix;

  std::vector<ZParameter*> m_parameters;
  // renderers
  std::set<Z3DPrimitiveRenderer*> m_renderers;

  std::vector<glm::vec4> m_clipPlanes;
  std::vector<glm::dvec4> m_doubleClipPlanes;
  bool m_clipEnabled;

  ShaderHookType m_shaderHookType;
  ShaderHookParameter m_shaderHookPara;

private:
  std::set<Z3DPrimitiveRenderer*>::iterator m_renderersIt;
};

} // namespace nim
