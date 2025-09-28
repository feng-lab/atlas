#include "z3drendererbase.h"

#include "z3dgl.h"
#include "z3dprimitiverenderer.h"
#include "z3drendererbackend.h"
#include "z3dcamera.h"
#include "zlog.h"
#include <algorithm>
#include <utility>

namespace nim {

Z3DRendererBase::Z3DRendererBase(RendererParameterState& parameterState,
                                 RendererFrameState& frameState,
                                 RendererViewState& viewState,
                                 RendererSceneState& sceneState)
  : m_parameters(parameterState)
  , m_frameState(frameState)
  , m_viewState(viewState)
  , m_sceneState(sceneState)
  , m_clipEnabled(true)
  , m_shaderHookType(ShaderHookType::Normal)
  , m_renderMethod(RenderMethod::GLSL)
{
  setBackend(createGLRendererBackend());
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_legacyGLState = std::make_unique<LegacyGLState>();
#endif
}

RendererViewState Z3DRendererBase::pushViewStateFromCamera(const Z3DCamera& camera)
{
  const RendererViewState previous = m_viewState;
  m_viewState = buildViewStateFromCamera(camera);
  return previous;
}

void Z3DRendererBase::restoreViewState(const RendererViewState& state)
{
  m_viewState = state;
}

RendererViewState Z3DRendererBase::buildViewStateFromCamera(const Z3DCamera& camera)
{
  RendererViewState state;
  state.nearClip = camera.nearDist();
  state.farClip = camera.farDist();

  for (int eyeValue = LeftEye; eyeValue <= RightEye; ++eyeValue) {
    auto eye = static_cast<Z3DEye>(eyeValue);
    auto& eyeState = state.eyes[static_cast<size_t>(eye)];
    eyeState.viewMatrix = camera.viewMatrix(eye);
    eyeState.projectionMatrix = camera.projectionMatrix(eye);
    eyeState.projectionViewMatrix = camera.projectionViewMatrix(eye);
    eyeState.inverseViewMatrix = camera.inverseViewMatrix(eye);
    eyeState.inverseProjectionMatrix = camera.inverseProjectionMatrix(eye);
    eyeState.normalMatrix = camera.normalMatrix(eye);
    eyeState.eyePosition = camera.eye();
    eyeState.isPerspective = camera.isPerspectiveProjection();
    eyeState.frustumNearPlaneSize = camera.frustumNearPlaneSize();
    eyeState.fieldOfView = camera.fieldOfView();
  }

  return state;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)

struct Z3DRendererBase::LegacyGLState
{
  GLuint displayList = 0;
  GLuint pickingDisplayList = 0;
  std::set<Z3DPrimitiveRenderer*> lastRenderingState;
  std::set<Z3DPrimitiveRenderer*> lastPickingRenderingState;
};

Z3DRendererBase::LegacyGLState& Z3DRendererBase::legacyGL()
{
  DCHECK(m_legacyGLState != nullptr);
  return *m_legacyGLState;
}

const Z3DRendererBase::LegacyGLState& Z3DRendererBase::legacyGL() const
{
  DCHECK(m_legacyGLState != nullptr);
  return *m_legacyGLState;
}

#endif

void Z3DRendererBase::setBackend(std::unique_ptr<Z3DRendererBackend> backend)
{
  CHECK(backend != nullptr) << "Renderer backend must not be null";
  m_backend = std::move(backend);
}

Z3DRendererBackend& Z3DRendererBase::backend()
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  return *m_backend;
}

const Z3DRendererBackend& Z3DRendererBase::backend() const
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  return *m_backend;
}

void Z3DRendererBase::setGlobalShaderParameters(Z3DShaderProgram& shader, Z3DEye eye)
{
  backend().setGlobalShaderParameters(*this, shader, eye);
}

void Z3DRendererBase::setGlobalShaderParameters(Z3DShaderProgram* shader, Z3DEye eye)
{
  CHECK(shader != nullptr) << "Shader pointer must not be null";
  backend().setGlobalShaderParameters(*this, *shader, eye);
}

std::string Z3DRendererBase::generateHeader() const
{
  return backend().generateHeader(*this);
}

std::string Z3DRendererBase::generateGeomHeader() const
{
  return backend().generateGeomHeader(*this);
}

void Z3DRendererBase::registerRenderer(Z3DPrimitiveRenderer* renderer)
{
  CHECK(renderer && !m_renderers.contains(renderer));

  m_renderers.insert(renderer);
}

void Z3DRendererBase::unregisterRenderer(Z3DPrimitiveRenderer* renderer)
{
  CHECK(renderer && m_renderers.contains(renderer));

  m_renderers.erase(renderer);
}

void Z3DRendererBase::setClipPlanes(std::vector<glm::vec4>* clipPlanes)
{
  size_t nOldClipPlanes = m_clipPlanes.size();
  m_clipPlanes.clear();
  m_doubleClipPlanes.clear();
  if (clipPlanes && !clipPlanes->empty()) {
    glm::mat4 itCoordTrans = glm::inverse(glm::transpose(m_parameters.coordTransform));
    for (auto& clipPlane : *clipPlanes) {
      m_clipPlanes.push_back(itCoordTrans * clipPlane);
    }
  }
  size_t nNewClipPlanes = m_clipPlanes.size();
  if (nNewClipPlanes != nOldClipPlanes) { // need to recompile shader to define or undefine HAS_CLIP_PLANE
    compile();
  }
  for (auto& m_clipPlane : m_clipPlanes) {
    m_doubleClipPlanes.emplace_back(m_clipPlane);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateDisplayList();
  invalidatePickingDisplayList();
#endif
}

void Z3DRendererBase::render(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  for (auto* renderer : renderers) {
    CHECK(m_renderers.contains(renderer));
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (m_renderMethod == RenderMethod::LegacyOpenGL) {
    const auto& eyeState = m_viewState.eyes[static_cast<size_t>(eye)];
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.projectionMatrix));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.viewMatrix));

    if (!useDisplayList(renderers)) {
      renderInstant(renderers);
    } else {
      // check if render state changed and we need to regenerate display list
      auto& legacy = legacyGL();
      if (legacy.displayList != 0 && legacy.lastRenderingState != m_renderers) {
        invalidateDisplayList();
      }

      if (legacy.displayList == 0) {
        generateDisplayList(renderers);
      }

      if (glIsList(legacy.displayList)) {
        glCallList(legacy.displayList);
      }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  } else {
    renderUsingGLSL(eye, renderers);
  }
#else
  renderUsingGLSL(eye, renderers);
#endif
}

void Z3DRendererBase::renderPicking(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  for (auto* renderer : renderers) {
    CHECK(m_renderers.contains(renderer));
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (m_renderMethod == RenderMethod::LegacyOpenGL) {
    const auto& eyeState = m_viewState.eyes[static_cast<size_t>(eye)];
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.projectionMatrix));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.viewMatrix));

    if (!useDisplayList(renderers)) {
      renderPickingInstant(renderers);
    } else {
      // check if render state changed and we need to regenerate display list
      auto& legacy = legacyGL();
      if (legacy.pickingDisplayList != 0 && legacy.lastPickingRenderingState != m_renderers) {
        invalidatePickingDisplayList();
      }

      if (legacy.pickingDisplayList == 0) {
        generatePickingDisplayList(renderers);
      }

      // render display list
      if (glIsList(legacy.pickingDisplayList)) {
        glCallList(legacy.pickingDisplayList);
      }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  } else {
    renderPickingUsingGLSL(eye, renderers);
  }
#else
  renderPickingUsingGLSL(eye, renderers);
#endif
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::generateDisplayList(Z3DRendererBase::RendererSpan renderers)
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.displayList)) {
    glDeleteLists(legacy.displayList, 1);
  }

  legacy.displayList = glGenLists(1);
  glNewList(legacy.displayList, GL_COMPILE);
  renderInstant(renderers);
  glEndList();
  legacy.lastRenderingState = m_renderers;
}

void Z3DRendererBase::generatePickingDisplayList(Z3DRendererBase::RendererSpan renderers)
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.pickingDisplayList)) {
    glDeleteLists(legacy.pickingDisplayList, 1);
  }

  legacy.pickingDisplayList = glGenLists(1);
  glNewList(legacy.pickingDisplayList, GL_COMPILE);
  renderPickingInstant(renderers);
  glEndList();
  legacy.lastPickingRenderingState = m_renderers;
}

void Z3DRendererBase::renderInstant(Z3DRendererBase::RendererSpan renderers)
{
  glPushAttrib(GL_ALL_ATTRIB_BITS);

  if (needLighting(renderers)) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_LIGHTING);
    const auto& lighting = m_sceneState.lighting;
    const int lightCount = std::max(0, std::min(lighting.lightCount, static_cast<int>(lighting.positions.size())));

    for (int lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
      const GLenum lightEnum = static_cast<GLenum>(GL_LIGHT0 + lightIndex);
      glEnable(lightEnum);
      glLightfv(lightEnum, GL_AMBIENT, glm::value_ptr(lighting.ambient[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_DIFFUSE, glm::value_ptr(lighting.diffuse[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_SPECULAR, glm::value_ptr(lighting.specular[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_POSITION, glm::value_ptr(lighting.positions[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_SPOT_DIRECTION, glm::value_ptr(lighting.spotDirection[static_cast<size_t>(lightIndex)]));
      glLightf(lightEnum, GL_SPOT_EXPONENT, lighting.spotExponent[static_cast<size_t>(lightIndex)]);
      glLightf(lightEnum, GL_SPOT_CUTOFF, lighting.spotCutoff[static_cast<size_t>(lightIndex)]);
      const glm::vec3& attenuation = lighting.attenuation[static_cast<size_t>(lightIndex)];
      glLightf(lightEnum, GL_CONSTANT_ATTENUATION, attenuation.x);
      glLightf(lightEnum, GL_LINEAR_ATTENUATION, attenuation.y);
      glLightf(lightEnum, GL_QUADRATIC_ATTENUATION, attenuation.z);
    }

    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, glm::value_ptr(m_parameters.materialAmbient));
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, std::min(m_parameters.materialShininess, 128.f));
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, glm::value_ptr(m_parameters.materialSpecular));
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_NORMALIZE);

    glPopMatrix();
  }

  activateClipPlanesOpenGL();
  for (auto* renderer : renderers) {
    renderer->renderUsingOpengl();
  }
  deactivateClipPlanesOpenGL();

  glPopAttrib();
}

void Z3DRendererBase::renderPickingInstant(Z3DRendererBase::RendererSpan renderers)
{
  glPushAttrib(GL_ALL_ATTRIB_BITS);

  activateClipPlanesOpenGL();
  for (auto* renderer : renderers) {
    renderer->renderPickingUsingOpengl();
  }
  deactivateClipPlanesOpenGL();

  glPopAttrib();
}
#endif

void Z3DRendererBase::renderUsingGLSL(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  backend().beginRender(*this);
  for (auto* renderer : renderers) {
    renderer->render(eye);
  }
  backend().endRender(*this);
}

void Z3DRendererBase::renderPickingUsingGLSL(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  backend().beginRender(*this);
  for (auto* renderer : renderers) {
    renderer->renderPicking(eye);
  }
  backend().endRender(*this);
}

bool Z3DRendererBase::needLighting(Z3DRendererBase::RendererSpan renderers) const
{
  bool needLighting = false;
  for (auto* renderer : renderers) {
    needLighting = needLighting || renderer->needLighting();
  }
  return needLighting;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
bool Z3DRendererBase::useDisplayList(Z3DRendererBase::RendererSpan renderers) const
{
  bool useDisplayList = false;
  for (auto* renderer : renderers) {
    useDisplayList = useDisplayList || renderer->useDisplayList();
  }
  return useDisplayList;
}
#endif

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::activateClipPlanesOpenGL()
{
  if (!m_clipEnabled) {
    return;
  }
  for (size_t i = 0; i < m_clipPlanes.size(); ++i) {
    glClipPlane(GL_CLIP_PLANE0 + i, glm::value_ptr(m_doubleClipPlanes[i]));
    glEnable(GL_CLIP_PLANE0 + i);
  }
}

void Z3DRendererBase::deactivateClipPlanesOpenGL()
{
  if (!m_clipEnabled) {
    return;
  }
  for (size_t i = 0; i < m_clipPlanes.size(); ++i) {
    glDisable(GL_CLIP_PLANE0 + i);
  }
}
#endif

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::invalidateDisplayList()
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.displayList)) {
    glDeleteLists(legacy.displayList, 1);
  }
  legacy.displayList = 0;
}

void Z3DRendererBase::invalidatePickingDisplayList()
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.pickingDisplayList)) {
    glDeleteLists(legacy.pickingDisplayList, 1);
  }
  legacy.pickingDisplayList = 0;
}
#endif

void Z3DRendererBase::compile()
{
  for (auto renderer : m_renderers) {
    renderer->compile();
  }
}

} // namespace nim
