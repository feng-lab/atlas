#include "z3dglobalparameters.h"

#include "zwidgetsgroup.h"
#include "z3dgpuinfo.h"
#include "z3dcameracontrolwidget.h"
#include "z3drendererstates.h"
#include <algorithm>
#include <utility>

namespace nim {

Z3DGlobalParameters::Z3DGlobalParameters()
  : renderBackend("Render Backend")
  , geometriesMultisampleMode("Multisample Anti-Aliasing")
  , transparencyMethod("Transparency")
  , weightedBlendedDepthScale("Weighted Blended Depth Scale", 1.f, 1e-3f, 1e3f)
  , lightCount("Light Count", 5, 1, 5)
  , sceneAmbient("Scene Ambient", glm::vec4(0.2f, 0.2f, 0.2f, 1.f))
  , fogMode("Fog Mode")
  , fogTopColor("Fog Top Color", glm::vec3(.9f, .9f, .9f))
  , fogBottomColor("Fog Bottom Color", glm::vec3(.9f, .9f, .9f))
  , fogRange("Fog Range", glm::ivec2(100, 50000), 1, 1e5)
  , fogDensity("Fog Density", 1.f, 0.0001f, 10.f)
  , camera("Camera", Z3DCamera())
  , pickingManager()
  , interactionHandler("Interaction Handler", &camera)
  , globalXCut("Global X Cut", glm::vec2(0, 0), 0, 0)
  , globalYCut("Global Y Cut", glm::vec2(0, 0), 0, 0)
  , globalZCut("Global Z Cut", glm::vec2(0, 0), 0, 0)
  , devicePixelRatio("Device Pixel Ratio", 1.f, 1.f, 16.f)
{
  renderBackend.clearOptions();
  renderBackend.addOptionsWithData(
    std::make_pair(enumToQString(RenderBackend::OpenGL), static_cast<int>(RenderBackend::OpenGL)),
    std::make_pair(enumToQString(RenderBackend::Vulkan), static_cast<int>(RenderBackend::Vulkan)));
  renderBackend.select(enumToQString(RenderBackend::OpenGL));
  // addParameter(renderBackend);

  geometriesMultisampleMode.clearOptions();
  geometriesMultisampleMode.addOptionsWithData(
    std::make_pair(QStringLiteral("None"), static_cast<int>(GeometryMSAAMode::None)),
    std::make_pair(QStringLiteral("2x2"), static_cast<int>(GeometryMSAAMode::MSAA2x2)));
  geometriesMultisampleMode.select(QStringLiteral("2x2"));
  addParameter(geometriesMultisampleMode);

  transparencyMethod.clearOptions();
  transparencyMethod.addOptionsWithData(
    std::make_pair(QStringLiteral("Blend Delayed"), static_cast<int>(TransparencyMode::BlendDelayed)),
    std::make_pair(QStringLiteral("Blend No Depth Mask"), static_cast<int>(TransparencyMode::BlendNoDepthMask)),
    std::make_pair(QStringLiteral("Weighted Average"), static_cast<int>(TransparencyMode::WeightedAverage)),
    std::make_pair(QStringLiteral("Weighted Blended"), static_cast<int>(TransparencyMode::WeightedBlended)),
    std::make_pair(QStringLiteral("Dual Depth Peeling"), static_cast<int>(TransparencyMode::DualDepthPeeling)));
  transparencyMethod.select(QStringLiteral("Weighted Average"));
  // weightedBlendedDepthScale.setStyle("SPINBOX");

  //  if (Z3DGpuInfoInstance.isLinkedListSupported())
  //    m_transparencyMethod.addOption("Linked List");

  addParameter(transparencyMethod);
  addParameter(weightedBlendedDepthScale);

  m_cameraParameterIndex = m_parameters.size();
  addParameter(camera);

  globalXCut.setSingleStep(1);
  globalYCut.setSingleStep(1);
  globalZCut.setSingleStep(1);
  addParameter(globalXCut);
  addParameter(globalYCut);
  addParameter(globalZCut);

  connect(&camera, &Z3DCameraParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalViewStateDirty);
  connect(&sceneAmbient, &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&weightedBlendedDepthScale,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&devicePixelRatio, &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&transparencyMethod,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&geometriesMultisampleMode,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&lightCount, &ZIntParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&fogMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&fogTopColor, &ZVec3Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&fogBottomColor, &ZVec3Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&fogRange, &ZIntSpanParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&fogDensity, &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&renderBackend,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);

  // lights
  QString lightname = "Key Light";
  QString name = lightname + " Position";
  lightPositions.emplace_back(std::make_unique<ZVec4Parameter>(name,
                                                               glm::vec4(0.1116f, 0.7660f, 0.6330f, 0.0f),
                                                               glm::vec4(-1.0f),
                                                               glm::vec4(1.f)));
  name = lightname + " Ambient";
  lightAmbients.emplace_back(std::make_unique<ZVec4Parameter>(name, glm::vec4(0.1f, 0.1f, 0.1f, 1.0f)));
  name = lightname + " Diffuse";
  lightDiffuses.emplace_back(std::make_unique<ZVec4Parameter>(name, glm::vec4(0.75f, 0.75f, 0.75f, 1.0f)));
  name = lightname + " Specular";
  lightSpeculars.emplace_back(std::make_unique<ZVec4Parameter>(name, glm::vec4(0.85f, 0.85f, 0.85f, 1.0f)));
  name = lightname + " Attenuation";
  lightAttenuations.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(100.f)));
  name = lightname + " Spot Cutoff";
  lightSpotCutoff.emplace_back(std::make_unique<ZFloatParameter>(name, 180.f, 0.f, 180.f));
  name = lightname + " Spot Exponent";
  lightSpotExponent.emplace_back(std::make_unique<ZFloatParameter>(name, 1.f, 0.f, 50.f));
  name = lightname + " Spot Direction";
  lightSpotDirection.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(-0.1116f, -0.7660f, -0.6330f), glm::vec3(-1.f), glm::vec3(1.f)));

  lightname = "Head Light";
  name = lightname + " Position";
  lightPositions.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.f, 0.f, 1.f, 0.0f), glm::vec4(-1.0f), glm::vec4(1.f)));
  name = lightname + " Ambient";
  lightAmbients.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.1 * 0.333f, 0.1 * 0.333f, 0.1 * 0.333f, 1.0f)));
  name = lightname + " Diffuse";
  lightDiffuses.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.75 * 0.333f, 0.75 * 0.333f, 0.75 * 0.333f, 1.0f)));
  name = lightname + " Specular";
  lightSpeculars.emplace_back(std::make_unique<ZVec4Parameter>(name, glm::vec4(0.f, 0.f, 0.f, 1.0f)));
  name = lightname + " Attenuation";
  lightAttenuations.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(100.f)));
  name = lightname + " Spot Cutoff";
  lightSpotCutoff.emplace_back(std::make_unique<ZFloatParameter>(name, 180.f, 0.f, 180.f));
  name = lightname + " Spot Exponent";
  lightSpotExponent.emplace_back(std::make_unique<ZFloatParameter>(name, 1.f, 0.f, 50.f));
  name = lightname + " Spot Direction";
  lightSpotDirection.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(0.f, 0.f, -1.f), glm::vec3(-1.f), glm::vec3(1.f)));

  lightname = "Fill Light";
  name = lightname + " Position";
  lightPositions.emplace_back(std::make_unique<ZVec4Parameter>(name,
                                                               glm::vec4(-0.0449f, -0.9659f, 0.2549f, 0.0f),
                                                               glm::vec4(-1.0f),
                                                               glm::vec4(1.f)));
  name = lightname + " Ambient";
  lightAmbients.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.1 * 0.333f, 0.1 * 0.333f, 0.1 * 0.333f, 1.0f)));
  name = lightname + " Diffuse";
  lightDiffuses.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.75 * 0.333f, 0.75 * 0.333f, 0.75 * 0.333f, 1.0f)));
  name = lightname + " Specular";
  lightSpeculars.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.85 * 0.333f, 0.85 * 0.333f, 0.85 * 0.333f, 1.0f)));
  name = lightname + " Attenuation";
  lightAttenuations.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(100.f)));
  name = lightname + " Spot Cutoff";
  lightSpotCutoff.emplace_back(std::make_unique<ZFloatParameter>(name, 180.f, 0.f, 180.f));
  name = lightname + " Spot Exponent";
  lightSpotExponent.emplace_back(std::make_unique<ZFloatParameter>(name, 1.f, 0.f, 50.f));
  name = lightname + " Spot Direction";
  lightSpotDirection.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(0.0449f, 0.9659f, -0.2549f), glm::vec3(-1.f), glm::vec3(1.f)));

  lightname = "Back Light 1";
  name = lightname + " Position";
  lightPositions.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.9397f, 0.f, -0.3420f, 0.0f), glm::vec4(-1.0f), glm::vec4(1.f)));
  name = lightname + " Ambient";
  lightAmbients.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.1 * 0.27f, 0.1 * 0.27f, 0.1 * 0.27f, 1.0f)));
  name = lightname + " Diffuse";
  lightDiffuses.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.75 * 0.27f, 0.75 * 0.27f, 0.75 * 0.27f, 1.0f)));
  name = lightname + " Specular";
  lightSpeculars.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.85 * 0.27f, 0.85 * 0.27f, 0.85 * 0.27f, 1.0f)));
  name = lightname + " Attenuation";
  lightAttenuations.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(100.f)));
  name = lightname + " Spot Cutoff";
  lightSpotCutoff.emplace_back(std::make_unique<ZFloatParameter>(name, 180.f, 0.f, 180.f));
  name = lightname + " Spot Exponent";
  lightSpotExponent.emplace_back(std::make_unique<ZFloatParameter>(name, 1.f, 0.f, 50.f));
  name = lightname + " Spot Direction";
  lightSpotDirection.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(-0.9397f, 0.f, 0.3420f), glm::vec3(-1.f), glm::vec3(1.f)));

  lightname = "Back Light 2";
  name = lightname + " Position";
  lightPositions.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(-0.9397f, 0.f, -0.3420f, 0.0f), glm::vec4(-1.0f), glm::vec4(1.f)));
  name = lightname + " Ambient";
  lightAmbients.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.1 * 0.27f, 0.1 * 0.27f, 0.1 * 0.27f, 1.0f)));
  name = lightname + " Diffuse";
  lightDiffuses.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.75 * 0.27f, 0.75 * 0.27f, 0.75 * 0.27f, 1.0f)));
  name = lightname + " Specular";
  lightSpeculars.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0.85 * 0.27f, 0.85 * 0.27f, 0.85 * 0.27f, 1.0f)));
  name = lightname + " Attenuation";
  lightAttenuations.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(100.f)));
  name = lightname + " Spot Cutoff";
  lightSpotCutoff.emplace_back(std::make_unique<ZFloatParameter>(name, 180.f, 0.f, 180.f));
  name = lightname + " Spot Exponent";
  lightSpotExponent.emplace_back(std::make_unique<ZFloatParameter>(name, 1.f, 0.f, 50.f));
  name = lightname + " Spot Direction";
  lightSpotDirection.emplace_back(
    std::make_unique<ZVec3Parameter>(name, glm::vec3(0.9397f, 0.f, 0.3420f), glm::vec3(-1.f), glm::vec3(1.f)));

  addParameter(lightCount);

  for (size_t i = 0; i < lightPositions.size(); ++i) {
    lightAmbients[i]->setStyle("COLOR");
    lightDiffuses[i]->setStyle("COLOR");
    lightSpeculars[i]->setStyle("COLOR");
    addParameter(*lightPositions[i]);
    connect(lightPositions[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightAmbients[i]);
    connect(lightAmbients[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightDiffuses[i]);
    connect(lightDiffuses[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpeculars[i]);
    connect(lightSpeculars[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightAttenuations[i]);
    connect(lightAttenuations[i].get(), &ZVec3Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotCutoff[i]);
    connect(lightSpotCutoff[i].get(), &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotExponent[i]);
    connect(lightSpotExponent[i].get(), &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotDirection[i]);
    connect(lightSpotDirection[i].get(), &ZVec3Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  }

  sceneAmbient.setStyle("COLOR");
  addParameter(sceneAmbient);

  // fog
  fogMode.clearOptions();
  fogMode.addOptionsWithData(
    std::make_pair(QStringLiteral("None"), static_cast<int>(FogMode::None)),
    std::make_pair(QStringLiteral("Linear"), static_cast<int>(FogMode::Linear)),
    std::make_pair(QStringLiteral("Exponential"), static_cast<int>(FogMode::Exponential)),
    std::make_pair(QStringLiteral("Squared Exponential"), static_cast<int>(FogMode::ExponentialSquared)));
  fogMode.select(QStringLiteral("None"));
  addParameter(fogMode);
  fogTopColor.setStyle("COLOR");
  fogBottomColor.setStyle("COLOR");
  fogRange.setSingleStep(1);
  fogDensity.setSingleStep(0.00001);
  fogDensity.setDecimal(5);
  addParameter(fogTopColor);
  addParameter(fogBottomColor);
  addParameter(fogRange);
  addParameter(fogDensity);

  markGlobalSceneStateDirty();
  markGlobalViewStateDirty();

  devicePixelRatio.setEnabled(false);

  pickingManager.setDevicePixelRatio(devicePixelRatio.get());
}

void Z3DGlobalParameters::setDevicePixelRatio(float f)
{
  if (f != devicePixelRatio.get()) {
    devicePixelRatio.set(f);
    pickingManager.setDevicePixelRatio(f);
    // Auto-tune geometry multisample mode based on display DPI.
    // - On high-DPI (Retina) screens (DPR >= 2), disable 2x2 supersample to avoid redundant cost.
    // - On standard-DPI screens (DPR < 2), prefer 2x2 supersample for crisper edges when enabled.
    const auto current = static_cast<GeometryMSAAMode>(geometriesMultisampleMode.associatedData());
    if (f >= 2.0f) {
      if (current != GeometryMSAAMode::None) {
        geometriesMultisampleMode.select(QStringLiteral("None"));
      }
    } else { // f < 2.0f
      if (current == GeometryMSAAMode::None) {
        geometriesMultisampleMode.select(QStringLiteral("2x2"));
      }
    }
  }
}

void Z3DGlobalParameters::read(const json::object& json)
{
  for (auto& m_parameter : m_parameters) {
    m_parameter->read(json);
  }
}

void Z3DGlobalParameters::write(json::object& json) const
{
  for (auto m_parameter : m_parameters) {
    m_parameter->write(json);
  }
}

std::shared_ptr<ZWidgetsGroup> Z3DGlobalParameters::widgetsGroup(bool includeCamera, Z3DRenderingEngine& engine)
{
  if (!m_widgetsGrp) {
    m_widgetsGrp = std::make_shared<ZWidgetsGroup>("Global", 1);
    m_widgetsGrpNoCamera = std::make_shared<ZWidgetsGroup>("Lighting", 1);

    m_widgetsGrp->addChild(renderBackend, 1);
    m_widgetsGrpNoCamera->addChild(renderBackend, 1);

    for (size_t i = 0; i < m_parameters.size(); ++i) {
      if (i == m_cameraParameterIndex) {
        m_widgetsGrp->addChild(*(new Z3DCameraControlWidget(camera, engine)), 1);
        m_widgetsGrp->addChild(*m_parameters[i], 1);
      } else {
        m_widgetsGrp->addChild(*m_parameters[i], 1);
        m_widgetsGrpNoCamera->addChild(*m_parameters[i], 1);
      }
    }

    m_widgetsGrp->addChild(devicePixelRatio, 1);
    m_widgetsGrpNoCamera->addChild(devicePixelRatio, 1);
  }
  return includeCamera ? m_widgetsGrp : m_widgetsGrpNoCamera;
}

void Z3DGlobalParameters::cameraFocusesOn(double x, double y, double z, double radius)
{
  ZBBox<glm::dvec3> bound(glm::dvec3(x, y, z) - radius, glm::dvec3(x, y, z) + radius);
  camera.resetCamera(bound, Z3DCamera::ResetOption::ResetAll);
}

void Z3DGlobalParameters::cameraFocusesOn(const ZBBox<glm::dvec3>& bound, double minRadius)
{
  if (bound.empty()) {
    return;
  }
  glm::dvec3 cent = (bound.minCorner + bound.maxCorner) / 2.;
  auto bd = bound;
  bd.expand(ZBBox<glm::dvec3>(cent - minRadius, cent + minRadius));
  camera.resetCamera(bd, Z3DCamera::ResetOption::PreserveViewVector);
}

void Z3DGlobalParameters::cameraPointsTo(double x, double y, double z)
{
  camera.setCenter(glm::vec3(x, y, z));
}

void Z3DGlobalParameters::cameraPointsTo(const ZBBox<glm::dvec3>& bound)
{
  if (bound.empty()) {
    return;
  }
  auto cent = glm::vec3((bound.minCorner + bound.maxCorner) / 2.);
  camera.setCenter(cent);
}


} // namespace nim
