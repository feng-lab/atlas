#include "z3dglobalparameters.h"

#include "zwidgetsgroup.h"
#include "z3dgpuinfo.h"
#include "z3dcameracontrolwidget.h"
#include "z3drendererstates.h"
#include <algorithm>
#include <utility>

namespace nim {

Z3DGlobalParameters::Z3DGlobalParameters(RenderBackend backend)
  : renderBackend("Render Backend")
  , geometriesAAMode("Anti-Aliasing")
  , transparencyMethod("Transparency")
  , weightedBlendedDepthScale("Weighted Blended Depth Scale", 1.f, 1e-3f, 1e3f)
  , lightCount("Light Count", 5, 1, 5)
  , sceneAmbient("Scene Ambient", glm::vec4(0.2f, 0.2f, 0.2f, 1.f))
  , fogMode("Fog Mode")
  , fogTopColor("Fog Top Color", glm::vec3(.9f, .9f, .9f))
  , fogBottomColor("Fog Bottom Color", glm::vec3(.9f, .9f, .9f))
  , fogRange("Fog Range", glm::ivec2(100, 50000), 1, 1e5)
  , fogDensity("Fog Density", 1.f, 0.0001f, 10.f)
  , camera("Camera", Z3DCamera(backend))
  , pickingManager()
  , interactionHandler("Interaction Handler", &camera)
  , globalXCut("Global X Cut", glm::vec2(0, 0), 0, 0)
  , globalYCut("Global Y Cut", glm::vec2(0, 0), 0, 0)
  , globalZCut("Global Z Cut", glm::vec2(0, 0), 0, 0)
  , devicePixelRatio("Device Pixel Ratio", 1.f, 1.f, 16.f)
{
  CHECK(backend == RenderBackend::OpenGL || backend == RenderBackend::Vulkan)
    << "Z3DGlobalParameters constructed with invalid RenderBackend value";

  renderBackend.clearOptions();
  renderBackend.addOptionsWithData(
    std::make_pair(enumToQString(RenderBackend::OpenGL), static_cast<int>(RenderBackend::OpenGL)),
    std::make_pair(enumToQString(RenderBackend::Vulkan), static_cast<int>(RenderBackend::Vulkan)));
  renderBackend.select(enumToQString(backend));
  renderBackend.setDescription(
    QStringLiteral("Rendering backend selection. OpenGL is the primary path today; Vulkan is experimental."));
  // addParameter(renderBackend);

  geometriesAAMode.addLegacyName(QStringLiteral("Multisample Anti-Aliasing"));
  geometriesAAMode.clearOptions();
  geometriesAAMode.addOptionsWithData(
    std::make_pair(QStringLiteral("None"), static_cast<int>(GeometryAAMode::None)),
    std::make_pair(QStringLiteral("2x2"), static_cast<int>(GeometryAAMode::Supersample2x2)));
  geometriesAAMode.select(QStringLiteral("2x2"));
  geometriesAAMode.setDescription(QStringLiteral(
    "Geometry supersampling for crisper edges. '2x2' improves quality at higher cost; 'None' is faster."));
  addParameter(geometriesAAMode);

  transparencyMethod.clearOptions();
  transparencyMethod.addOptionsWithData(
    std::make_pair(QStringLiteral("Blend Delayed"), static_cast<int>(TransparencyMode::BlendDelayed)),
    std::make_pair(QStringLiteral("Blend No Depth Mask"), static_cast<int>(TransparencyMode::BlendNoDepthMask)),
    std::make_pair(QStringLiteral("Weighted Average"), static_cast<int>(TransparencyMode::WeightedAverage)),
    std::make_pair(QStringLiteral("Weighted Blended"), static_cast<int>(TransparencyMode::WeightedBlended)),
    std::make_pair(QStringLiteral("Dual Depth Peeling"), static_cast<int>(TransparencyMode::DualDepthPeeling)));
  transparencyMethod.select(QStringLiteral("Weighted Average"));
  transparencyMethod.setDescription(
    QStringLiteral("Transparency compositing method. Weighted Average (default) is fast and stable;"
                   " Weighted Blended reduces bleed-through with 'Weighted Blended Depth Scale';"
                   " Dual Depth Peeling is more accurate but heavier;"
                   " Per-Pixel Fragment List (PPLL) is exact OIT on Vulkan (falls back to DDP on OpenGL)."));
  // weightedBlendedDepthScale.setStyle("SPINBOX");

  //  if (Z3DGpuInfoInstance.isLinkedListSupported())
  //    m_transparencyMethod.addOption("Linked List");

  addParameter(transparencyMethod);

  // Vulkan-only option: expose exact OIT (PPLL) only when Vulkan backend is active.
  // Keep OpenGL UI clean (and avoid misleading selection of a Vulkan-only mode).
  const QString ddpLabel = QStringLiteral("Dual Depth Peeling");
  const QString ppllLabel = QStringLiteral("Per-Pixel Fragment List (PPLL Exact)");
  RenderBackend lastBackendForTransparencyOptions = backend;
  auto updateTransparencyOptionsForBackend = [this, ddpLabel, ppllLabel, lastBackendForTransparencyOptions]() mutable {
    const RenderBackend backend = static_cast<RenderBackend>(renderBackend.associatedData());
    const bool shouldExposePPLL = (backend == RenderBackend::Vulkan);
    const bool hasPPLL = transparencyMethod.hasOption(ppllLabel);
    if (shouldExposePPLL) {
      if (!hasPPLL) {
        transparencyMethod.addOptionWithData(
          std::make_pair(ppllLabel, static_cast<int>(TransparencyMode::PerPixelFragmentList)));
      }
      // Temporary Vulkan performance workaround:
      //
      // If the user is switching from OpenGL->Vulkan while using Dual Depth Peeling,
      // default to PPLL on Vulkan. Our current Vulkan DDP implementation performs a
      // CPU readback for device-limit handling and is therefore much slower than
      // PPLL in practice. Users can still manually re-select DDP in the UI if
      // they need it.
      if (lastBackendForTransparencyOptions == RenderBackend::OpenGL && transparencyMethod.isSelected(ddpLabel)) {
        transparencyMethod.select(ppllLabel);
      }
      lastBackendForTransparencyOptions = backend;
      return;
    }

    if (!hasPPLL) {
      lastBackendForTransparencyOptions = backend;
      return;
    }
    if (transparencyMethod.isSelected(ppllLabel)) {
      transparencyMethod.select(ddpLabel);
    }
    transparencyMethod.removeOption(ppllLabel);
    lastBackendForTransparencyOptions = backend;
  };

  updateTransparencyOptionsForBackend();
  addParameter(weightedBlendedDepthScale);
  weightedBlendedDepthScale.setDescription(
    QStringLiteral("Tuning scalar for Weighted Blended transparency. Increase to reduce bleed-through;"
                   " affects only 'Weighted Blended' mode."));

  m_cameraParameterIndex = m_parameters.size();
  addParameter(camera);
  camera.setDescription(QStringLiteral(
    "3D camera bundle with fields: 'Eye Position Vec3', 'Center Position Vec3', 'Up Vector Vec3', 'Field of View Float', 'Projection Type StringIntOption'."));

  globalXCut.setSingleStep(1);
  globalYCut.setSingleStep(1);
  globalZCut.setSingleStep(1);
  addParameter(globalXCut);
  addParameter(globalYCut);
  addParameter(globalZCut);
  // Keep ZCutSpanParameter's own rich description (LLM + formulas). No override here.

  connect(&camera, &Z3DCameraParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalViewStateDirty);
  connect(&sceneAmbient, &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&weightedBlendedDepthScale,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&devicePixelRatio, &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  // Global cut changes affect scene state
  connect(&globalXCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&globalYCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&globalZCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&transparencyMethod,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&geometriesAAMode,
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
  connect(&renderBackend, &ZStringIntOptionParameter::valueChanged, this, updateTransparencyOptionsForBackend);

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
  lightCount.setDescription(QStringLiteral("Number of active lights used for shading (1–5)."));

  for (size_t i = 0; i < lightPositions.size(); ++i) {
    lightAmbients[i]->setStyle("COLOR");
    lightDiffuses[i]->setStyle("COLOR");
    lightSpeculars[i]->setStyle("COLOR");
    addParameter(*lightPositions[i]);
    connect(lightPositions[i].get(),
            &ZVec4Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightAmbients[i]);
    connect(lightAmbients[i].get(),
            &ZVec4Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightDiffuses[i]);
    connect(lightDiffuses[i].get(),
            &ZVec4Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpeculars[i]);
    connect(lightSpeculars[i].get(),
            &ZVec4Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightAttenuations[i]);
    connect(lightAttenuations[i].get(),
            &ZVec3Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotCutoff[i]);
    connect(lightSpotCutoff[i].get(),
            &ZFloatParameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotExponent[i]);
    connect(lightSpotExponent[i].get(),
            &ZFloatParameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
    addParameter(*lightSpotDirection[i]);
    connect(lightSpotDirection[i].get(),
            &ZVec3Parameter::valueChanged,
            this,
            &Z3DGlobalParameters::markGlobalSceneStateDirty);
  }

  sceneAmbient.setStyle("COLOR");
  addParameter(sceneAmbient);
  sceneAmbient.setDescription(QStringLiteral("Ambient scene color applied globally."));

  // fog
  fogMode.clearOptions();
  fogMode.addOptionsWithData(
    std::make_pair(QStringLiteral("None"), static_cast<int>(FogMode::None)),
    std::make_pair(QStringLiteral("Linear"), static_cast<int>(FogMode::Linear)),
    std::make_pair(QStringLiteral("Exponential"), static_cast<int>(FogMode::Exponential)),
    std::make_pair(QStringLiteral("Squared Exponential"), static_cast<int>(FogMode::ExponentialSquared)));
  fogMode.select(QStringLiteral("None"));
  fogMode.setDescription(QStringLiteral("Fog model applied in the scene: None, Linear, or Exponential."));
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
  fogTopColor.setDescription(QStringLiteral("Top color of vertical fog gradient (when fog is enabled)."));
  fogBottomColor.setDescription(QStringLiteral("Bottom color of vertical fog gradient (when fog is enabled)."));
  fogRange.setDescription(QStringLiteral("Near/Far depth range where fog is applied."));
  fogDensity.setDescription(QStringLiteral("Fog density factor (used by exponential modes)."));

  markGlobalSceneStateDirty();
  markGlobalViewStateDirty();

  devicePixelRatio.setEnabled(false);
  devicePixelRatio.setDescription(
    QStringLiteral("Detected display scale (read-only); used to auto-tune anti-aliasing."));

  pickingManager.setDevicePixelRatio(devicePixelRatio.get());
}

void Z3DGlobalParameters::setDevicePixelRatio(float f)
{
  if (f != devicePixelRatio.get()) {
    devicePixelRatio.set(f);
    pickingManager.setDevicePixelRatio(f);
    // Auto-tune geometry anti-aliasing mode based on display DPI.
    // - On high-DPI (Retina) screens (DPR >= 2), disable 2x2 supersample to avoid redundant cost.
    // - On standard-DPI screens (DPR < 2), prefer 2x2 supersample for crisper edges when enabled.
    const auto current = static_cast<GeometryAAMode>(geometriesAAMode.associatedData());
    if (f >= 2.0f) {
      if (current != GeometryAAMode::None) {
        geometriesAAMode.select(QStringLiteral("None"));
      }
    } else { // f < 2.0f
      if (current == GeometryAAMode::None) {
        geometriesAAMode.select(QStringLiteral("2x2"));
      }
    }
  }
}

folly::CancellationToken Z3DGlobalParameters::currentMeshLodViewCancellationToken() const
{
  const std::scoped_lock lock(m_meshLodViewCancellationMutex);
  CHECK(m_meshLodViewCancellationSource);
  return m_meshLodViewCancellationSource->getToken();
}

void Z3DGlobalParameters::requestMeshLodViewCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_meshLodViewCancellationMutex);
    CHECK(m_meshLodViewCancellationSource);
    source = m_meshLodViewCancellationSource;
    m_meshLodViewCancellationSource = std::make_shared<folly::CancellationSource>();
  }
  CHECK(source);
  source->requestCancellation();
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

void Z3DGlobalParameters::applyBoundsForCuts(const ZBBox<glm::dvec3>& bound)
{
  // Compute new axis-aligned ranges with small padding
  const auto nx = std::floor(bound.minCorner.x) - 1.0;
  const auto xx = std::ceil(bound.maxCorner.x) + 1.0;
  const auto ny = std::floor(bound.minCorner.y) - 1.0;
  const auto xy = std::ceil(bound.maxCorner.y) + 1.0;
  const auto nz = std::floor(bound.minCorner.z) - 1.0;
  const auto xz = std::ceil(bound.maxCorner.z) + 1.0;
  globalXCut.applyBounds(nx, xx);
  globalYCut.applyBounds(ny, xy);
  globalZCut.applyBounds(nz, xz);
}

} // namespace nim
