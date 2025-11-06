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
  // Binding controls for global cuts
  , globalXCutMode("Global X Cut Mode")
  , globalYCutMode("Global Y Cut Mode")
  , globalZCutMode("Global Z Cut Mode")
  , globalXCutPinLower("Global X Cut Pin Lower", true)
  , globalXCutPinUpper("Global X Cut Pin Upper", true)
  , globalYCutPinLower("Global Y Cut Pin Lower", true)
  , globalYCutPinUpper("Global Y Cut Pin Upper", true)
  , globalZCutPinLower("Global Z Cut Pin Lower", true)
  , globalZCutPinUpper("Global Z Cut Pin Upper", true)
  , globalXCutNormalized("Global X Cut Normalized", glm::dvec2(0.0, 1.0), 0.0, 1.0)
  , globalYCutNormalized("Global Y Cut Normalized", glm::dvec2(0.0, 1.0), 0.0, 1.0)
  , globalZCutNormalized("Global Z Cut Normalized", glm::dvec2(0.0, 1.0), 0.0, 1.0)
  , devicePixelRatio("Device Pixel Ratio", 1.f, 1.f, 16.f)
{
  renderBackend.clearOptions();
  renderBackend.addOptionsWithData(
    std::make_pair(enumToQString(RenderBackend::OpenGL), static_cast<int>(RenderBackend::OpenGL)),
    std::make_pair(enumToQString(RenderBackend::Vulkan), static_cast<int>(RenderBackend::Vulkan)));
  renderBackend.select(enumToQString(RenderBackend::OpenGL));
  renderBackend.setDescription(QStringLiteral(
    "Rendering backend selection. OpenGL is the primary path today; Vulkan is experimental."));
  // addParameter(renderBackend);

  geometriesMultisampleMode.clearOptions();
  geometriesMultisampleMode.addOptionsWithData(
    std::make_pair(QStringLiteral("None"), static_cast<int>(GeometryMSAAMode::None)),
    std::make_pair(QStringLiteral("2x2"), static_cast<int>(GeometryMSAAMode::MSAA2x2)));
  geometriesMultisampleMode.select(QStringLiteral("2x2"));
  geometriesMultisampleMode.setDescription(QStringLiteral(
    "Geometry supersampling for crisper edges. '2x2' improves quality at higher cost; 'None' is faster."));
  addParameter(geometriesMultisampleMode);

  transparencyMethod.clearOptions();
  transparencyMethod.addOptionsWithData(
    std::make_pair(QStringLiteral("Blend Delayed"), static_cast<int>(TransparencyMode::BlendDelayed)),
    std::make_pair(QStringLiteral("Blend No Depth Mask"), static_cast<int>(TransparencyMode::BlendNoDepthMask)),
    std::make_pair(QStringLiteral("Weighted Average"), static_cast<int>(TransparencyMode::WeightedAverage)),
    std::make_pair(QStringLiteral("Weighted Blended"), static_cast<int>(TransparencyMode::WeightedBlended)),
    std::make_pair(QStringLiteral("Dual Depth Peeling"), static_cast<int>(TransparencyMode::DualDepthPeeling)));
  transparencyMethod.select(QStringLiteral("Weighted Average"));
  transparencyMethod.setDescription(QStringLiteral(
    "Transparency compositing method. Weighted Average (default) is fast and stable;"
    " Weighted Blended reduces bleed-through with 'Weighted Blended Depth Scale';"
    " Dual Depth Peeling is more accurate but heavier."));
  // weightedBlendedDepthScale.setStyle("SPINBOX");

  //  if (Z3DGpuInfoInstance.isLinkedListSupported())
  //    m_transparencyMethod.addOption("Linked List");

  addParameter(transparencyMethod);
  addParameter(weightedBlendedDepthScale);
  weightedBlendedDepthScale.setDescription(QStringLiteral(
    "Tuning scalar for Weighted Blended transparency. Increase to reduce bleed-through;"
    " affects only 'Weighted Blended' mode."));

  m_cameraParameterIndex = m_parameters.size();
  addParameter(camera);
  camera.setDescription(QStringLiteral(
    "Typed 3D camera value (position, center, up, frustum)."));

  globalXCut.setSingleStep(1);
  globalYCut.setSingleStep(1);
  globalZCut.setSingleStep(1);
  addParameter(globalXCut);
  addParameter(globalYCut);
  addParameter(globalZCut);
  globalXCut.setDescription(QStringLiteral(
    "Global clipping interval along X in world units. Updates on bounds changes follow ‘Global X Cut Mode’."));
  globalYCut.setDescription(QStringLiteral(
    "Global clipping interval along Y in world units. Updates on bounds changes follow ‘Global Y Cut Mode’."));
  globalZCut.setDescription(QStringLiteral(
    "Global clipping interval along Z in world units. Updates on bounds changes follow ‘Global Z Cut Mode’."));

  // Cut binding modes and helpers
  globalXCutMode.clearOptions();
  globalYCutMode.clearOptions();
  globalZCutMode.clearOptions();
  // 0=Absolute, 1=TrackEdges, 2=Normalized
  globalXCutMode.addOptionsWithData(std::make_pair(QStringLiteral("Absolute"), 0),
                                    std::make_pair(QStringLiteral("Track Edges"), 1),
                                    std::make_pair(QStringLiteral("Normalized [0..1]"), 2));
  globalYCutMode.addOptionsWithData(std::make_pair(QStringLiteral("Absolute"), 0),
                                    std::make_pair(QStringLiteral("Track Edges"), 1),
                                    std::make_pair(QStringLiteral("Normalized [0..1]"), 2));
  globalZCutMode.addOptionsWithData(std::make_pair(QStringLiteral("Absolute"), 0),
                                    std::make_pair(QStringLiteral("Track Edges"), 1),
                                    std::make_pair(QStringLiteral("Normalized [0..1]"), 2));
  // Default to Track Edges: shows full range and follows edges as bounds move
  globalXCutMode.select(QStringLiteral("Track Edges"));
  globalYCutMode.select(QStringLiteral("Track Edges"));
  globalZCutMode.select(QStringLiteral("Track Edges"));
  globalXCutMode.setDescription(QStringLiteral(
    "How to recompute X cut when scene bounds change:\n"
    "- Absolute: hold values in world units; clamp into new range.\n"
    "  newLower = clamp(oldLower, min, max); newUpper = clamp(oldUpper, min, max).\n"
    "- Track Edges: pin lower/upper to moving min/max when ‘Pin Lower/Pin Upper’ are ON;\n"
    "  otherwise hold absolute and clamp.\n"
    "  newLower = (PinLower ? min : clamp(oldLower, min, max));\n"
    "  newUpper = (PinUpper ? max : clamp(oldUpper, min, max)).\n"
    "- Normalized [0..1]: store fractions f0,f1; recompute by lerp.\n"
    "  newLower = min + (max-min)*f0; newUpper = min + (max-min)*f1."));
  globalYCutMode.setDescription(QStringLiteral(
    "How to recompute Y cut when scene bounds change. See X for formulas; applies to Y axis."));
  globalZCutMode.setDescription(QStringLiteral(
    "How to recompute Z cut when scene bounds change. See X for formulas; applies to Z axis."));

  globalXCutNormalized.setSingleStep(0.001);
  globalYCutNormalized.setSingleStep(0.001);
  globalZCutNormalized.setSingleStep(0.001);
  
  globalXCutPinLower.setDescription(QStringLiteral(
    "Track Edges: when ON, the lower endpoint pins to the axis minimum as bounds move; when OFF, it holds its absolute value (clamped)."));
  globalXCutPinUpper.setDescription(QStringLiteral(
    "Track Edges: when ON, the upper endpoint pins to the axis maximum as bounds move; when OFF, it holds its absolute value (clamped)."));
  globalYCutPinLower.setDescription(QStringLiteral(
    "Track Edges: when ON, the lower endpoint pins to the axis minimum as bounds move; when OFF, it holds its absolute value (clamped)."));
  globalYCutPinUpper.setDescription(QStringLiteral(
    "Track Edges: when ON, the upper endpoint pins to the axis maximum as bounds move; when OFF, it holds its absolute value (clamped)."));
  globalZCutPinLower.setDescription(QStringLiteral(
    "Track Edges: when ON, the lower endpoint pins to the axis minimum as bounds move; when OFF, it holds its absolute value (clamped)."));
  globalZCutPinUpper.setDescription(QStringLiteral(
    "Track Edges: when ON, the upper endpoint pins to the axis maximum as bounds move; when OFF, it holds its absolute value (clamped)."));

  globalXCutNormalized.setDescription(QStringLiteral(
    "Normalized X cut fractions used in Normalized mode. 0 = current min, 1 = current max. Lower/upper auto-ordered."));
  globalYCutNormalized.setDescription(QStringLiteral(
    "Normalized Y cut fractions used in Normalized mode. 0 = current min, 1 = current max. Lower/upper auto-ordered."));
  globalZCutNormalized.setDescription(QStringLiteral(
    "Normalized Z cut fractions used in Normalized mode. 0 = current min, 1 = current max. Lower/upper auto-ordered."));

  addParameter(globalXCutMode);
  addParameter(globalXCutPinLower);
  addParameter(globalXCutPinUpper);
  addParameter(globalXCutNormalized);

  addParameter(globalYCutMode);
  addParameter(globalYCutPinLower);
  addParameter(globalYCutPinUpper);
  addParameter(globalYCutNormalized);

  addParameter(globalZCutMode);
  addParameter(globalZCutPinLower);
  addParameter(globalZCutPinUpper);
  addParameter(globalZCutNormalized);

  updateCutUiEnabling();

  connect(&camera, &Z3DCameraParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalViewStateDirty);
  connect(&sceneAmbient, &ZVec4Parameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&weightedBlendedDepthScale,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&devicePixelRatio, &ZFloatParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  // Update UI enabling when modes change; mark scene state dirty to ensure refresh
  connect(&globalXCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::updateCutUiEnabling);
  connect(&globalYCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::updateCutUiEnabling);
  connect(&globalZCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::updateCutUiEnabling);
  connect(&globalXCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&globalYCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  connect(&globalZCutMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DGlobalParameters::markGlobalSceneStateDirty);
  // Keep normalized spans in sync when absolute spans edited in Normalized mode
  connect(&globalXCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::onAbsoluteCutEditedUpdateNormalized);
  connect(&globalYCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::onAbsoluteCutEditedUpdateNormalized);
  connect(&globalZCut, &ZFloatSpanParameter::valueChanged, this, &Z3DGlobalParameters::onAbsoluteCutEditedUpdateNormalized);
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
  lightCount.setDescription(QStringLiteral("Number of active lights used for shading (1–5)."));

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
  devicePixelRatio.setDescription(QStringLiteral(
    "Detected display scale (read-only); used to auto-tune anti-aliasing."));

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
  // Ensure helper controls reflect the loaded modes even if values matched defaults
  updateCutUiEnabling();
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

// Enable/disable helper parameters based on binding modes.
void Z3DGlobalParameters::updateCutUiEnabling()
{
  const auto modeX = static_cast<int>(globalXCutMode.associatedData());
  const auto modeY = static_cast<int>(globalYCutMode.associatedData());
  const auto modeZ = static_cast<int>(globalZCutMode.associatedData());

  const auto enableEdge = [](int mode) { return mode == static_cast<int>(CutBindingMode::TrackEdges); };
  const auto enableNorm = [](int mode) { return mode == static_cast<int>(CutBindingMode::Normalized); };

  globalXCutPinLower.setEnabled(enableEdge(modeX));
  globalXCutPinUpper.setEnabled(enableEdge(modeX));
  globalXCutNormalized.setEnabled(enableNorm(modeX));

  globalYCutPinLower.setEnabled(enableEdge(modeY));
  globalYCutPinUpper.setEnabled(enableEdge(modeY));
  globalYCutNormalized.setEnabled(enableNorm(modeY));

  globalZCutPinLower.setEnabled(enableEdge(modeZ));
  globalZCutPinUpper.setEnabled(enableEdge(modeZ));
  globalZCutNormalized.setEnabled(enableNorm(modeZ));
}

// Keep normalized spans in sync if absolute spans are edited while in Normalized mode
void Z3DGlobalParameters::onAbsoluteCutEditedUpdateNormalized()
{
  const auto updateOne = [](const ZFloatSpanParameter& absSpan, ZDoubleSpanParameter& normSpan) {
    const double minB = absSpan.minimum();
    const double maxB = absSpan.maximum();
    const double range = maxB - minB;
    if (range <= 0.0) return; // invalid; nothing to do
    const auto abs = absSpan.get();
    const double lower = (abs[0] - minB) / range;
    const double upper = (abs[1] - minB) / range;
    normSpan.set(glm::dvec2(std::clamp(lower, 0.0, 1.0), std::clamp(upper, 0.0, 1.0)));
  };

  if (static_cast<int>(globalXCutMode.associatedData()) == static_cast<int>(CutBindingMode::Normalized)) {
    updateOne(globalXCut, globalXCutNormalized);
  }
  if (static_cast<int>(globalYCutMode.associatedData()) == static_cast<int>(CutBindingMode::Normalized)) {
    updateOne(globalYCut, globalYCutNormalized);
  }
  if (static_cast<int>(globalZCutMode.associatedData()) == static_cast<int>(CutBindingMode::Normalized)) {
    updateOne(globalZCut, globalZCutNormalized);
  }
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

  const auto clampd = [](double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); };

  // Helper to evaluate one axis
  auto evalAxis = [&](ZFloatSpanParameter& absSpan,
                      const ZDoubleSpanParameter& normSpan,
                      const ZStringIntOptionParameter& modeParam,
                      const ZBoolParameter& trackLower,
                      const ZBoolParameter& trackUpper,
                      double newMin,
                      double newMax) {
    const auto oldAbs = absSpan.get();
    const double oldLower = oldAbs[0];
    const double oldUpper = oldAbs[1];
    const auto mode = static_cast<CutBindingMode>(modeParam.associatedData());
    double lower = oldLower;
    double upper = oldUpper;
    switch (mode) {
      case CutBindingMode::Absolute: {
        lower = clampd(oldLower, newMin, newMax);
        upper = clampd(oldUpper, newMin, newMax);
        break;
      }
      case CutBindingMode::TrackEdges: {
        lower = trackLower.get() ? newMin : clampd(oldLower, newMin, newMax);
        upper = trackUpper.get() ? newMax : clampd(oldUpper, newMin, newMax);
        break;
      }
      case CutBindingMode::Normalized: {
        const auto nf = normSpan.get();
        const double t0 = std::clamp(static_cast<double>(nf[0]), 0.0, 1.0);
        const double t1 = std::clamp(static_cast<double>(nf[1]), 0.0, 1.0);
        lower = newMin + (newMax - newMin) * t0;
        upper = newMin + (newMax - newMin) * t1;
        break;
      }
    }
    absSpan.setRange(newMin, newMax);
    absSpan.set(glm::vec2(static_cast<float>(lower), static_cast<float>(upper)));
  };

  evalAxis(globalXCut, globalXCutNormalized, globalXCutMode, globalXCutPinLower, globalXCutPinUpper, nx, xx);
  evalAxis(globalYCut, globalYCutNormalized, globalYCutMode, globalYCutPinLower, globalYCutPinUpper, ny, xy);
  evalAxis(globalZCut, globalZCutNormalized, globalZCutMode, globalZCutPinLower, globalZCutPinUpper, nz, xz);
}


} // namespace nim
