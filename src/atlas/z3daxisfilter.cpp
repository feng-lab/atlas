#include "z3daxisfilter.h"

#include "zlog.h"
#include "zwidgetsgroup.h"
#include <utility>

namespace nim {

Z3DAxisFilter::Z3DAxisFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_lineRenderer(m_rendererBase)
  , m_arrowRenderer(m_rendererBase)
  , m_fontRenderer(m_rendererBase)
  , m_showAxis("Show Axis", true)
  , m_XAxisColor("X Axis Color", glm::vec4(1.f, 0.f, 0.f, 1.0f))
  , m_YAxisColor("Y Axis Color", glm::vec4(0.f, 1.f, 0.f, 1.0f))
  , m_ZAxisColor("Z Axis Color", glm::vec4(0.f, 0.f, 1.f, 1.0f))
  , m_axisRegionRatio("Axis Region Ratio", .2f, .1f, 1.f)
  , m_mode("mode")
  , m_fontName("Font")
  , m_fontSize("Font Size", 32.f, .1f, 5000.f)
  , m_fontSoftEdgeScale("Font Softedge Scale", 80.f, 0.f, 200.f)
  , m_showFontOutline("Show Font Outline", false)
  , m_fontOutlineMode("Font Outline Mode")
  , m_fontOutlineColor("Font Outline Color", glm::vec4(1.f))
  , m_showFontShadow("Show Font Shadow", false)
  , m_fontShadowColor("Font Shadow Color", glm::vec4(0.f, 0.f, 0.f, 1.f))
{
  m_XAxisColor.setStyle("COLOR");
  m_YAxisColor.setStyle("COLOR");
  m_ZAxisColor.setStyle("COLOR");
  m_mode.addOptions("Arrow", "Line");
  m_mode.select("Arrow");
  m_fontSize.setSingleStep(0.1);
  m_fontSize.setDecimal(1);
  m_fontSoftEdgeScale.setSingleStep(1.0);
  m_fontOutlineMode.clearOptions();
  m_fontOutlineMode.addOptionsWithData(
    std::make_pair(enumToQString(FontOutlineMode::Glow), static_cast<int>(FontOutlineMode::Glow)),
    std::make_pair(enumToQString(FontOutlineMode::Outline), static_cast<int>(FontOutlineMode::Outline)));
  m_fontOutlineMode.select(enumToQString(FontOutlineMode::Glow));
  m_fontOutlineColor.setStyle("COLOR");
  m_fontShadowColor.setStyle("COLOR");
  addParameter(m_showAxis);
  addParameter(m_XAxisColor);
  addParameter(m_YAxisColor);
  addParameter(m_ZAxisColor);
  addParameter(m_axisRegionRatio);
  addParameter(m_mode);
  addParameter(m_fontName);
  addParameter(m_fontSize);
  addParameter(m_fontSoftEdgeScale);
  addParameter(m_showFontOutline);
  addParameter(m_fontOutlineMode);
  addParameter(m_fontOutlineColor);
  addParameter(m_showFontShadow);
  addParameter(m_fontShadowColor);

  if (!m_fontRenderer.fontNames().isEmpty()) {
    int idx = 0;
    for (const auto& name : m_fontRenderer.fontNames()) {
      m_fontName.addOptionWithData(std::make_pair(name, idx++));
    }
    m_fontName.select(m_fontRenderer.selectedFontName());
  } else {
    m_fontName.setVisible(false);
  }

  auto updateFontWidgets = [this]() {
    const bool outlineVisible = m_showFontOutline.get();
    m_fontOutlineMode.setVisible(outlineVisible);
    m_fontOutlineColor.setVisible(outlineVisible);
    m_fontShadowColor.setVisible(m_showFontShadow.get());
  };

  connect(&m_fontName, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontName(m_fontName.get());
  });
  connect(&m_fontSize, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSize(m_fontSize.get());
  });
  connect(&m_fontSoftEdgeScale, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSoftEdgeScale(m_fontSoftEdgeScale.get());
  });
  connect(&m_showFontOutline, &ZBoolParameter::valueChanged, this, [this, updateFontWidgets]() mutable {
    m_fontRenderer.setShowFontOutline(m_showFontOutline.get());
    updateFontWidgets();
  });
  connect(&m_fontOutlineMode, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_fontOutlineMode.associatedData()));
  });
  connect(&m_fontOutlineColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineColor(m_fontOutlineColor.get());
  });
  connect(&m_showFontShadow, &ZBoolParameter::valueChanged, this, [this, updateFontWidgets]() mutable {
    m_fontRenderer.setShowFontShadow(m_showFontShadow.get());
    updateFontWidgets();
  });
  connect(&m_fontShadowColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontShadowColor(m_fontShadowColor.get());
  });

  m_fontRenderer.setFontName(m_fontName.get());
  m_fontRenderer.setFontSize(m_fontSize.get());
  m_fontRenderer.setFontSoftEdgeScale(m_fontSoftEdgeScale.get());
  m_fontRenderer.setShowFontOutline(m_showFontOutline.get());
  m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_fontOutlineMode.associatedData()));
  m_fontRenderer.setFontOutlineColor(m_fontOutlineColor.get());
  m_fontRenderer.setShowFontShadow(m_showFontShadow.get());
  m_fontRenderer.setFontShadowColor(m_fontShadowColor.get());
  updateFontWidgets();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_arrowRenderer.setUseDisplayList(false);
  m_lineRenderer.setUseDisplayList(false);
#endif
  m_fontRenderer.setFollowCoordTransform(false);
  setupCamera();
}

bool Z3DAxisFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_showAxis.get();
}

std::shared_ptr<ZWidgetsGroup> Z3DAxisFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Axis", 1);
    m_widgetsGroup->addChild(m_showAxis, 1);
    m_widgetsGroup->addChild(m_mode, 1);
    m_widgetsGroup->addChild(m_axisRegionRatio, 1);
    m_widgetsGroup->addChild(m_XAxisColor, 1);
    m_widgetsGroup->addChild(m_YAxisColor, 1);
    m_widgetsGroup->addChild(m_ZAxisColor, 1);
    auto& rendererParas = m_rendererParameters;
    m_widgetsGroup->addChild(rendererParas.sizeScale, 1);
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
    m_widgetsGroup->addChild(rendererParas.renderMethod, 3);
#endif
    m_widgetsGroup->addChild(rendererParas.opacity, 3);
    m_widgetsGroup->addChild(m_fontName, 4);
    m_widgetsGroup->addChild(m_fontSize, 4);
    m_widgetsGroup->addChild(m_fontSoftEdgeScale, 4);
    m_widgetsGroup->addChild(m_showFontOutline, 4);
    m_widgetsGroup->addChild(m_fontOutlineMode, 4);
    m_widgetsGroup->addChild(m_fontOutlineColor, 4);
    m_widgetsGroup->addChild(m_showFontShadow, 4);
    m_widgetsGroup->addChild(m_fontShadowColor, 4);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void Z3DAxisFilter::renderOpaque(Z3DEye eye)
{
  prepareData(eye);
  {
    const QSignalBlocker blocker(m_rendererParameters.coordTransform);
    m_rendererParameters.coordTransform.set(glm::mat4(m_globalParameters.camera.get().rotateMatrix(eye)));

    const glm::uvec4& viewport = currentViewport();
    GLsizei size = std::min(viewport.z, viewport.w) * m_axisRegionRatio.get();
    glViewport(viewport.x, viewport.y, size, size);

    if (m_mode.get() == "Arrow") {
      renderWithStateAndCamera(eye, m_axisCamera, m_arrowRenderer, m_fontRenderer);
    } else {
      renderWithStateAndCamera(eye, m_axisCamera, m_lineRenderer, m_fontRenderer);
    }

    glViewport(viewport.x, viewport.y, viewport.z, viewport.w);
  }
}

void Z3DAxisFilter::prepareData(Z3DEye eye)
{
  m_textPositions.clear();
  glm::mat3 rotMatrix = m_globalParameters.camera.get().rotateMatrix(eye);
  m_XEnd = rotMatrix * glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = rotMatrix * glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = rotMatrix * glm::vec3(0.f, 0.f, 256.f);

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));
  QStringList texts;
  texts.push_back("X");
  texts.push_back("Y");
  texts.push_back("Z");

  m_fontRenderer.setData(&m_textPositions, texts);
}

void Z3DAxisFilter::setupCamera()
{
  Z3DCamera camera;
  glm::vec3 center(0.f);
  camera.setFieldOfView(glm::radians(10.f));

  float radius = 300.f;

  float distance = radius / std::sin(camera.fieldOfView() * 0.5f);
  glm::vec3 vn(0, 0, 1); // plane normal
  glm::vec3 position = center + vn * distance;
  camera.setCamera(position, center, glm::vec3(0.0, 1.0, 0.0));
  camera.setNearDist(distance - radius - 1);
  camera.setFarDist(distance + radius);

  m_axisCamera = camera;

  m_tailPosAndTailRadius.clear();
  m_headPosAndHeadRadius.clear();
  m_lineColors.clear();
  m_lines.clear();
  m_textColors.clear();
  m_textPositions.clear();
  m_XEnd = glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = glm::vec3(0.f, 0.f, 256.f);
  glm::vec3 origin(0.f);
  m_lines.push_back(origin);
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(m_XEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(m_YEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_ZAxisColor.get());
  m_lines.push_back(m_ZEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_ZAxisColor.get());

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));
  m_textColors.push_back(m_XAxisColor.get());
  m_textColors.push_back(m_YAxisColor.get());
  m_textColors.push_back(m_ZAxisColor.get());
  QStringList texts;
  texts.push_back("X");
  texts.push_back("Y");
  texts.push_back("Z");

  float tailRadius = 5.f;
  float headRadius = 10.f;

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_XEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_YEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_ZEnd * glm::vec3(0.88), headRadius);

  m_lineRenderer.setData(&m_lines);
  m_lineRenderer.setDataColors(&m_lineColors);
  m_arrowRenderer.setArrowData(&m_tailPosAndTailRadius, &m_headPosAndHeadRadius, .1f);
  m_arrowRenderer.setArrowColors(&m_textColors);
  m_fontRenderer.setData(&m_textPositions, texts);
  m_fontRenderer.setDataColors(&m_textColors);
}

} // namespace nim
