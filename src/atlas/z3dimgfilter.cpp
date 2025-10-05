#include "z3dimgfilter.h"

#include "z3dgpuinfo.h"
#include "zbenchtimer.h"
#include "zeventlistenerparameter.h"
#include "zlog.h"
#include "z3drendertarget.h"
#include "z3drenderglobalstate.h"
#include "zmesh.h"
#include <folly/ScopeGuard.h>
#include <QMenu>
#include <memory>
#include <utility>

namespace nim {

// const size_t Z3DImgFilter::m_maxNumOfFullResolutionVolumeSlice = 6;

Z3DImgFilter::Z3DImgFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_imgRaycasterRenderer(m_rendererBase)
  , m_imgSliceRenderer(m_rendererBase)
  , m_textureCopyRenderer(m_rendererBase)
  , m_stayOnTop("Stay On Top", false)
  , m_fullResolutionRendering("Full Resolution Rendering", false)
  , m_raycasterCompositingMode("Compositing")
  , m_raycasterSamplingRate("Sampling Rate", 2.f, 0.01f, 20.f)
  , m_raycasterIsoValue("ISO Value", 0.5f, 0.0f, 1.0f)
  , m_raycasterLocalMIPThreshold("Local MIP Threshold", 0.8f, 0.01f, 1.f)
  , m_numParas(0)
  //, m_interactionDownsample("Interaction Downsample", 1, 1, 16)
  //, m_smoothInteraction("Smooth Interaction", true)
  , m_vPPort("VolumeFilter", this)
  //, m_FRVolumeSlices(m_maxNumOfFullResolutionVolumeSlice)
  //, m_FRVolumeSlicesValidState(m_maxNumOfFullResolutionVolumeSlice, false)
  //, m_useFRVolumeSlice("Use Full Resolution Volume Slice", true)
  , m_showXSlice("Show X Slice", false)
  , m_xSlicePosition("X Slice Position", 0, 0, 1)
  , m_showYSlice("Show Y Slice", false)
  , m_ySlicePosition("Y Slice Position", 0, 0, 1)
  , m_showZSlice("Show Z Slice", false)
  , m_zSlicePosition("Z Slice Position", 0, 0, 1)
  , m_showObliqueSlice("Show Oblique Slice", false)
  , m_obliqueSliceNormal("Oblique Slice Normal", glm::vec3(1, 1, 0), glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1))
  , m_obliqueSliceDistanceToOrigin("Oblique Slice Distance to Origin", 0, 0, 0)
  , m_showObliqueSlice2("Show Oblique Slice 2", false)
  , m_obliqueSlice2Normal("Oblique Slice 2 Normal", glm::vec3(1, 1, 0), glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1))
  , m_obliqueSlice2DistanceToOrigin("Oblique Slice 2 Distance to Origin", 0, 0, 0)
  , m_showXSlice2("Show X Slice 2", false)
  , m_xSlice2Position("X Slice 2 Position", 0, 0, 1)
  , m_showYSlice2("Show Y Slice 2", false)
  , m_ySlice2Position("Y Slice 2 Position", 0, 0, 1)
  , m_showZSlice2("Show Z Slice 2", false)
  , m_zSlice2Position("Z Slice 2 Position", 0, 0, 1)
  , m_leftMouseButtonPressEvent("Left Mouse Button Pressed", false)
  , m_contextMenuEvent("Context Menu", false)
{
  m_baseBoundBoxRenderer.setEnableMultisample(false);
  m_textureCopyRenderer.setDiscardTransparent(true);

  updateRaycasterSamplingRate();
  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();

  connect(&m_raycasterSamplingRate, &ZFloatParameter::valueChanged, this, &Z3DImgFilter::updateRaycasterSamplingRate);
  connect(&m_raycasterIsoValue, &ZFloatParameter::valueChanged, this, &Z3DImgFilter::updateRaycasterIsoValue);
  connect(&m_raycasterLocalMIPThreshold,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DImgFilter::updateRaycasterLocalMIPThreshold);

  m_raycasterCompositingMode.clearOptions();
  m_raycasterCompositingMode.addOptionsWithData(
    std::make_pair(QStringLiteral("Direct Volume Rendering"),
                   static_cast<int>(ImgCompositingMode::DirectVolumeRendering)),
    std::make_pair(QStringLiteral("Maximum Intensity Projection"),
                   static_cast<int>(ImgCompositingMode::MaximumIntensityProjection)),
    std::make_pair(QStringLiteral("MIP Opaque"), static_cast<int>(ImgCompositingMode::MIPOpaque)),
    std::make_pair(QStringLiteral("Local MIP"), static_cast<int>(ImgCompositingMode::LocalMIP)),
    std::make_pair(QStringLiteral("Local MIP Opaque"), static_cast<int>(ImgCompositingMode::LocalMIPOpaque)),
    std::make_pair(QStringLiteral("ISO Surface"), static_cast<int>(ImgCompositingMode::IsoSurface)),
    std::make_pair(QStringLiteral("X Ray"), static_cast<int>(ImgCompositingMode::XRay)));
  m_raycasterCompositingMode.select(QStringLiteral("MIP Opaque"));

  updateRaycasterCompositingMode();
  connect(&m_raycasterCompositingMode,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DImgFilter::updateRaycasterCompositingMode);

  addParameter(m_stayOnTop);
  addParameter(m_fullResolutionRendering);
  connect(this, &Z3DBoundedFilter::rendererCoordTransformChanged, this, &Z3DImgFilter::changeCoordTransform);
  //  connect(&m_globalParameters.interactionHandler,
  //          &Z3DTrackballInteractionHandler::enterInteractionMode,
  //          this,
  //          &Z3DImgFilter::enterFastMode);
  //  connect(&m_globalParameters.interactionHandler,
  //          &Z3DTrackballInteractionHandler::exitInteractionMode,
  //          this,
  //          &Z3DImgFilter::exitFastMode);

  // addParameter(m_interactionDownsample);
  // addParameter(m_smoothInteraction);

  // renderer-owned targets will be sized on demand

  // layer, block-id, and progressive render targets are now owned by renderers

  // port exposing this filter to the compositor network
  addPort(m_vPPort);
  markTargetsInvalid();
  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DImgFilter::onVisibilityChanged);

  m_obliqueSliceNormal.setNameForEachValue({"x", "y", "z"});
  m_obliqueSlice2Normal.setNameForEachValue({"x", "y", "z"});

  // addParameter(m_useFRVolumeSlice);
  addParameter(m_showXSlice);
  addParameter(m_xSlicePosition);
  addParameter(m_showYSlice);
  addParameter(m_ySlicePosition);
  addParameter(m_showZSlice);
  addParameter(m_zSlicePosition);
  addParameter(m_showObliqueSlice);
  addParameter(m_obliqueSliceNormal);
  addParameter(m_obliqueSliceDistanceToOrigin);
  addParameter(m_showObliqueSlice2);
  addParameter(m_obliqueSlice2Normal);
  addParameter(m_obliqueSlice2DistanceToOrigin);
  addParameter(m_showXSlice2);
  addParameter(m_xSlice2Position);
  addParameter(m_showYSlice2);
  addParameter(m_ySlice2Position);
  addParameter(m_showZSlice2);
  addParameter(m_zSlice2Position);

  connect(&m_showXSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showObliqueSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showObliqueSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showXSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);

  // connect(&m_xSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice);
  // connect(&m_ySlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeYSlice);
  // connect(&m_zSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeZSlice);
  // connect(&m_xSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  // connect(&m_ySlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  // connect(&m_zSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);

  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  connect(&m_leftMouseButtonPressEvent,
          &ZEventListenerParameter::mouseEventTriggered,
          this,
          &Z3DImgFilter::leftMouseButtonPressed);
  addEventListener(m_leftMouseButtonPressEvent);

  m_contextMenuEvent.listenToContextMenuEvent();
  connect(&m_contextMenuEvent,
          &ZEventListenerParameter::contextMenuEventTriggered,
          this,
          &Z3DImgFilter::contextMenuEvent);
  addEventListener(m_contextMenuEvent);

  m_boundBoxLineWidth.set(1);
  m_boundBoxMode.select("Bound Box");

  addParameter(m_raycasterCompositingMode);
  addParameter(m_raycasterIsoValue);
  addParameter(m_raycasterLocalMIPThreshold);
  addParameter(m_raycasterSamplingRate);

  m_imgRaycasterRenderer.setFastRendering(!m_fullResolutionRendering.get());
  m_imgSliceRenderer.setFastRendering(!m_fullResolutionRendering.get());
  connect(&m_fullResolutionRendering,
          &ZBoolParameter::valueChanged,
          this,
          &Z3DImgFilter::fullResolutionRenderingToggled);

  adjustWidget();
  CHECK_GL_ERROR

  m_numParas = m_parameters.size();

  for (auto* para : m_globalParameters.parameters()) {
    connect(para, &ZParameter::valueChanged, this, &Z3DBoundedFilter::invalidateResult);
  }
}

void Z3DImgFilter::setData(const ZImgPack& imgPack)
{
  if (m_widgetsGroup) {
    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->removeChild(*para);
    }
    //    for (const auto& para : m_imgRaycasterRenderer.texFilterModeParas()) {
    //      m_widgetsGroup->removeChild(*para);
    //    }
    for (const auto& cm : m_sliceColormaps) {
      m_widgetsGroup->removeChild(*cm);
    }
  }
  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }

  try {
    m_channelVisibleParas.clear();
    m_transferFuncParas.clear();
    std::vector<glm::dvec2> drs;
    if (imgPack.imgInfo().isType<uint8_t>()) {
      drs = std::vector<glm::dvec2>(imgPack.imgInfo().numChannels, glm::dvec2(0, 255));
    } else if (imgPack.hasMinMax() && imgPack.maxIntensity() > imgPack.minIntensity()) {
      drs = std::vector<glm::dvec2>(
        imgPack.imgInfo().numChannels,
        glm::dvec2(imgPack.minIntensity() + (imgPack.maxIntensity() - imgPack.minIntensity()) * 0.02,
                   imgPack.maxIntensity()));
    } else {
      drs = std::vector<glm::dvec2>(
        imgPack.imgInfo().numChannels,
        glm::dvec2(imgPack.rangeMin() + (imgPack.rangeMax() - imgPack.rangeMin()) * 0.02, imgPack.rangeMax()));
    }

    m_3dImg = std::make_unique<Z3DImg>(imgPack, m_rendererParameters.coordTransform.scale(), drs);
    updateBlockIDTarget();

    // Layer target channel depth managed inside renderers now
    m_fullResolutionRendering.set(!m_3dImg->isVolumeDownsampled());
    m_fullResolutionRendering.setEnabled(m_3dImg->isVolumeDownsampled());
    // m_smoothInteraction.setVisible(m_3dImg->isVolumeDownsampled());

    m_sliceColormaps.clear();
    m_doubleChannelRangeParas.clear();
    for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
      m_sliceColormaps.emplace_back(
        std::make_unique<ZColorMapParameter>(QString("Slice Channel %1 Colormap").arg(c + 1)));
      m_sliceColormaps[c]->get().reset(
        0.0,
        1.0,
        QColor(0, 0, 0),
        QColor(m_3dImg->channelColor(c).r, m_3dImg->channelColor(c).g, m_3dImg->channelColor(c).b));
      m_doubleChannelRangeParas.emplace_back(
        std::make_unique<ZDoubleSpanParameter>(QString("Channel %1 Display Range").arg(c + 1),
                                               drs[c],
                                               imgPack.rangeMin(),
                                               imgPack.rangeMax()));
      m_doubleChannelRangeParas.back()->setStyle("SPINBOX");
      if (imgPack.imgInfo().voxelFormat != VoxelFormat::Float) {
        m_doubleChannelRangeParas.back()->setDecimal(0);
        m_doubleChannelRangeParas.back()->setSingleStep(1);
      }
      connect(m_doubleChannelRangeParas[c].get(),
              &ZDoubleSpanParameter::valueChanged,
              this,
              &Z3DImgFilter::channelRangeChanged);
    }
    channelRangeChanged();

    bool is2DImage = m_3dImg->is2DData();
    glm::uvec3 volDim = m_3dImg->dimensions();
    m_xCut.setRange(0, volDim.x);
    m_xCut.set(m_xCut.range());
    m_yCut.setRange(0, volDim.y);
    m_yCut.set(m_yCut.range());
    m_zCut.setRange(0, volDim.z);
    m_zCut.set(m_zCut.range());

    m_obliqueSliceDistanceToOrigin.setRange(-glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)),
                                            glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)));
    m_obliqueSlice2DistanceToOrigin.setRange(-glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)),
                                             glm::length(glm::vec3(volDim.x, volDim.y, volDim.z)));

    m_rendererParameters.coordTransform.setRotationCenter(glm::vec3(volDim.x, volDim.y, volDim.z) / 2.f);

    m_zSlicePosition.setRange(0, volDim.z - 1);
    m_ySlicePosition.setRange(0, volDim.y - 1);
    m_xSlicePosition.setRange(0, volDim.x - 1);
    m_zSlice2Position.setRange(0, volDim.z - 1);
    m_ySlice2Position.setRange(0, volDim.y - 1);
    m_xSlice2Position.setRange(0, volDim.x - 1);
    // invalidateAllFRVolumeSlices();
    // m_useFRVolumeSlice.set(!is2DImage);
    // m_useFRVolumeSlice.setVisible(!is2DImage);
    m_showXSlice.set(false);
    m_showYSlice.set(false);
    m_showZSlice.set(false);
    m_showObliqueSlice.set(false);
    m_showObliqueSlice2.set(false);
    m_showXSlice2.set(false);
    m_showYSlice2.set(false);
    m_showZSlice2.set(false);
    m_showXSlice.setVisible(!is2DImage);
    m_showYSlice.setVisible(!is2DImage);
    m_showZSlice.setVisible(!is2DImage);
    m_showObliqueSlice.setVisible(!is2DImage);
    m_showObliqueSlice2.setVisible(!is2DImage);
    m_showXSlice2.setVisible(!is2DImage);
    m_showYSlice2.setVisible(!is2DImage);
    m_showZSlice2.setVisible(!is2DImage);

    m_imgRaycasterRenderer.setData(*m_3dImg);

    std::vector<bool> channelVisibilities;
    channelVisibilities.reserve(m_3dImg->numChannels());
    std::vector<Z3DTransferFunction*> transferFunctions;
    transferFunctions.reserve(m_3dImg->numChannels());

    for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
      auto visiblePara = std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(c + 1), true);
      channelVisibilities.push_back(visiblePara->get());
      connect(visiblePara.get(), &ZBoolParameter::boolChanged, this, [this, c](bool value) {
        m_imgRaycasterRenderer.setChannelVisibility(c, value);
        invalidateResult();
      });
      m_channelVisibleParas.emplace_back(std::move(visiblePara));

      auto transferPara = std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(c + 1));
      transferPara->setMinMaxIntensity(m_3dImg->displayRange(c).x, m_3dImg->displayRange(c).y);
      connect(transferPara.get(), &Z3DTransferFunctionParameter::valueChanged, this, [this]() {
        invalidateResult();
      });
      transferFunctions.push_back(&transferPara->get());

      auto& transferFunction = transferPara->get();
      transferFunction.reset(0.0,
                             1.0,
                             glm::vec4(0.f),
                             glm::vec4(m_3dImg->channelColor(c).r / 255.,
                                       m_3dImg->channelColor(c).g / 255.,
                                       m_3dImg->channelColor(c).b / 255.,
                                       1.f));
      if (false) {
        transferFunction.addKey(ZColorMapKey(0.001, glm::vec4(0.01f, 0.01f, 0.01f, 0.0f)));
        transferFunction.addKey(ZColorMapKey(0.01, glm::vec4(0.01f, 0.01f, 0.01f, 1.0f)));
      }

      m_transferFuncParas.emplace_back(std::move(transferPara));
    }

    m_imgRaycasterRenderer.setChannelVisibilities(channelVisibilities);
    m_imgRaycasterRenderer.setTransferFunctions(transferFunctions);

    if (!is2DImage) {
      m_imgSliceRenderer.setData(*m_3dImg, m_sliceColormaps);
    }

    updateBoundBox();

    for (const auto& para : m_channelVisibleParas) {
      addParameter(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      addParameter(*para);
    }
    for (const auto& para : m_transferFuncParas) {
      addParameter(*para);
    }
    for (const auto& cm : m_sliceColormaps) {
      addParameter(*cm);
    }

    if (m_widgetsGroup) {
      for (const auto& para : m_channelVisibleParas) {
        m_widgetsGroup->addChild(*para, 2);
      }
      for (const auto& para : m_doubleChannelRangeParas) {
        m_widgetsGroup->addChild(*para, 3);
      }
      for (const auto& para : m_transferFuncParas) {
        m_widgetsGroup->addChild(*para, 3);
      }
      for (const auto& cm : m_sliceColormaps) {
        m_widgetsGroup->addChild(*cm, 11);
      }
      m_widgetsGroup->emitWidgetsGroupChangedSignal();
    }

    connect(this, &Z3DImgFilter::showImgContextMenu, &imgPack, &ZImgPack::show3DImgContextMenu);
    connect(&imgPack, &ZImgPack::enterSubregionView, this, &Z3DImgFilter::enterSubregionView);
    connect(&imgPack, &ZImgPack::exitSubregionView, this, &Z3DImgFilter::exitSubregionView);
  }
  catch (const ZException& e) {
    m_3dImg.reset();
    LOG(ERROR) << e.what();
    Q_EMIT renderingError(QString("import 3d img error: %1").arg(e.what()));
  }

#ifdef NO // ATLAS_DEBUG_VERSION
  // Reset cached global cuts since our bounds may have changed with new data
  m_cachedGlobalCutsInitialized = false;
  debugSetInvalidateReason("setData");
#endif
  invalidateResult();
}

std::shared_ptr<ZWidgetsGroup> Z3DImgFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Img", 1);

    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_fullResolutionRendering, 1);
    // m_widgetsGroup->addChild(m_smoothInteraction, 1);

    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->addChild(*para, 2);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    m_widgetsGroup->addChild(m_raycasterCompositingMode, 4);
    m_widgetsGroup->addChild(m_raycasterIsoValue, 4);
    m_widgetsGroup->addChild(m_raycasterLocalMIPThreshold, 4);
    m_widgetsGroup->addChild(m_raycasterSamplingRate, 15);

    m_widgetsGroup->addChild(m_xCut, 12);
    m_widgetsGroup->addChild(m_yCut, 12);
    m_widgetsGroup->addChild(m_zCut, 12);
    m_widgetsGroup->addChild(m_boundBoxMode, 13);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 13);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 13);
    m_widgetsGroup->addChild(m_selectionLineWidth, 17);
    m_widgetsGroup->addChild(m_selectionLineColor, 17);
    m_widgetsGroup->addChild(m_manipulatorSize, 17);
    // m_widgetsGroup->addChild(m_interactionDownsample, 19);
    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 1);

    const std::vector<ZParameter*>& paras = parameters();
    for (auto para : paras) {
      if (para->name().contains("Slice") && !para->name().endsWith("2") && !para->name().endsWith("2 Position") &&
          !para->name().endsWith("2 Normal") && !para->name().endsWith("2 Distance to Origin")) {
        m_widgetsGroup->addChild(*para, 11);
      } else if (para->name().contains("Slice")) {
        m_widgetsGroup->addChild(*para, 19);
      }
    }
    m_widgetsGroup->setBasicAdvancedCutoff(14);
  }
  return m_widgetsGroup;
}

bool Z3DImgFilter::isReady(Z3DEye eye) const
{
  return Z3DBoundedFilter::isReady(eye) && m_visible.get() && m_3dImg;
}

bool Z3DImgFilter::hasOpaque(Z3DEye) const
{
  return hasSlices();
}

void Z3DImgFilter::renderOpaque(Z3DEye eye)
{
  if (!m_opaqueValid[eye]) {
    return;
  }

  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const auto& lease = m_opaqueTargets[eye];
    if (!lease || !lease.hasVulkanImage()) {
      return;
    }
    AttachmentHandle colorHandle;
    colorHandle.backend = AttachmentBackend::Vulkan;
    colorHandle.index = 0;
    colorHandle.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));

    AttachmentHandle depthHandle;
    depthHandle.backend = AttachmentBackend::Vulkan;
    depthHandle.index = 0;
    depthHandle.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());

    m_textureCopyRenderer.setSourceAttachments(colorHandle, depthHandle);
    m_rendererBase.render(eye, m_textureCopyRenderer);
    return;
  }

  const auto& target = opaqueTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

bool Z3DImgFilter::hasTransparent(Z3DEye eye) const
{
  return m_transparentValid[eye];
}

void Z3DImgFilter::renderTransparent(Z3DEye eye)
{
  if (!m_transparentValid[eye]) {
    return;
  }

  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const auto& lease = m_transparentTargets[eye];
    if (!lease || !lease.hasVulkanImage()) {
      return;
    }
    AttachmentHandle colorHandle;
    colorHandle.backend = AttachmentBackend::Vulkan;
    colorHandle.index = 0;
    colorHandle.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));

    AttachmentHandle depthHandle;
    depthHandle.backend = AttachmentBackend::Vulkan;
    depthHandle.index = 0;
    depthHandle.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());

    m_textureCopyRenderer.setSourceAttachments(colorHandle, depthHandle);
    m_rendererBase.render(eye, m_textureCopyRenderer);
    return;
  }

  const auto& target = transparentTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

glm::vec3 Z3DImgFilter::get3DPosition(int x, int y, int width, int height, bool& success)
{
  const auto mode = static_cast<ImgCompositingMode>(m_raycasterCompositingMode.associatedData());

  if (mode == ImgCompositingMode::DirectVolumeRendering) {
    return getMaxInten3DPositionUnderScreenPoint(x, y, width, height, success);
  } else {
    return getFirstHit3DPosition(x, y, width, height, success);
  }
}

void Z3DImgFilter::setProgressiveRenderingMode(bool v)
{
  m_progressiveRendering = v;
}

void Z3DImgFilter::enterSubregionView(float x, float y, float z)
{
  glm::vec3 pos3D(x, y, z);
  VLOG(1) << "open subregion at image coord " << pos3D;
  auto minCoord = pos3D - 64.f;
  auto maxCoord = pos3D + 64.f;
  m_xCut.set(glm::vec2(minCoord.x, maxCoord.x));
  m_yCut.set(glm::vec2(minCoord.y, maxCoord.y));
  m_zCut.set(glm::vec2(minCoord.z, maxCoord.z));
  m_globalParameters.cameraFocusesOn(axisAlignedBoundBoxAfterClipping());
  m_fullResolutionRendering.set(true);
}

void Z3DImgFilter::exitSubregionView()
{
  m_fullResolutionRendering.set(false);
  m_xCut.set(m_xCut.range());
  m_yCut.set(m_yCut.range());
  m_zCut.set(m_zCut.range());
  m_globalParameters.cameraFocusesOn(axisAlignedBoundBox());
}

void Z3DImgFilter::updateRaycasterSamplingRate()
{
  m_imgRaycasterRenderer.setSamplingRate(m_raycasterSamplingRate.get());
}

void Z3DImgFilter::updateRaycasterIsoValue()
{
  m_imgRaycasterRenderer.setIsoValue(m_raycasterIsoValue.get());
}

void Z3DImgFilter::updateRaycasterLocalMIPThreshold()
{
  m_imgRaycasterRenderer.setLocalMIPThreshold(m_raycasterLocalMIPThreshold.get());
}

void Z3DImgFilter::onVisibilityChanged(bool visible)
{
  if (visible) {
    return;
  }

  m_imgRaycasterRenderer.releaseScratchResources();
  for (Z3DEye eye : {MonoEye, LeftEye, RightEye}) {
    m_imgSliceRenderer.resetProgress(eye);
  }
  releaseAllRenderTargets();
}

void Z3DImgFilter::updateRaycasterCompositingMode()
{
  const auto mode = static_cast<ImgCompositingMode>(m_raycasterCompositingMode.associatedData());
  m_imgRaycasterRenderer.setCompositingMode(mode);

  const bool showIso = mode == ImgCompositingMode::IsoSurface;
  const bool showLocal = mode == ImgCompositingMode::LocalMIP || mode == ImgCompositingMode::LocalMIPOpaque;
  m_raycasterIsoValue.setVisible(showIso);
  m_raycasterLocalMIPThreshold.setVisible(showLocal);

  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();
}

void Z3DImgFilter::invalidate(State inv)
{
  // Check for global cut churn that doesn't affect this image and skip invalidation if so.
#if 0 // ATLAS_DEBUG_VERSION
  QString reason = debugTakeInvalidateReason();
  if (!reason.isEmpty()) {
    if (reason.startsWith("global ")) {
      bool isCut =
        reason.contains("Global X Cut") || reason.contains("Global Y Cut") || reason.contains("Global Z Cut");
      if (isCut) {
        const auto& worldAABB = axisAlignedBoundBox();
        if (!worldAABB.empty()) {
          auto gx = m_globalParameters.globalXCut.get();
          auto gy = m_globalParameters.globalYCut.get();
          auto gz = m_globalParameters.globalZCut.get();
          glm::vec2 effX{std::clamp(gx[0], float(worldAABB.minCorner.x), float(worldAABB.maxCorner.x)),
                         std::clamp(gx[1], float(worldAABB.minCorner.x), float(worldAABB.maxCorner.x))};
          glm::vec2 effY{std::clamp(gy[0], float(worldAABB.minCorner.y), float(worldAABB.maxCorner.y)),
                         std::clamp(gy[1], float(worldAABB.minCorner.y), float(worldAABB.maxCorner.y))};
          glm::vec2 effZ{std::clamp(gz[0], float(worldAABB.minCorner.z), float(worldAABB.maxCorner.z)),
                         std::clamp(gz[1], float(worldAABB.minCorner.z), float(worldAABB.maxCorner.z))};
          if (effX.x > effX.y) {
            std::swap(effX.x, effX.y);
          }
          if (effY.x > effY.y) {
            std::swap(effY.x, effY.y);
          }
          if (effZ.x > effZ.y) {
            std::swap(effZ.x, effZ.y);
          }
          auto diffGt = [](const glm::vec2& a, const glm::vec2& b, float eps) {
            return std::abs(a.x - b.x) > eps || std::abs(a.y - b.y) > eps;
          };
          constexpr float eps = 1e-4f;
          bool changed = !m_cachedGlobalCutsInitialized || diffGt(effX, m_cachedEffXCut, eps) ||
                         diffGt(effY, m_cachedEffYCut, eps) || diffGt(effZ, m_cachedEffZCut, eps);
          if (!changed) {
            VLOG(1) << "skip invalidate: global cut changed but no effect on image AABB";
            return;
          }
          m_cachedGlobalCutsInitialized = true;
          m_cachedEffXCut = effX;
          m_cachedEffYCut = effY;
          m_cachedEffZCut = effZ;
        }
      }
    }
  }
#endif

  Z3DBoundedFilter::invalidate(inv);
  markTargetsInvalid();
  // If rendering is in progress, request cancellation; renderers will
  // catch cancellation and perform a safe reset of progressive state.
#if 0 // ATLAS_DEBUG_VERSION
  auto invStr = flagsToString(inv);
  auto stateStr = flagsToString(m_state);
  auto reason2 = reason;
  if (!reason2.isEmpty()) {
    VLOG(1) << "image filter invalidate: " << reason2 << ", inv=" << invStr << ", state=" << stateStr;
  } else {
    VLOG(1) << "image filter invalidate, inv=" << invStr << ", state=" << stateStr;
  }
#endif

  if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
    Z3DRenderGlobalState::instance().requestCancellation();
#ifdef ATLAS_DEBUG_VERSION
    VLOG(1) << "requested cancellation on invalidate";
#endif
  }
  // Mark for safe reset at the beginning of next process
  m_resetProgressPending = true;
}

void Z3DImgFilter::updateSize()
{
  Z3DBoundedFilter::updateSize();
  updateBlockIDTarget();

  const glm::uvec2 requestedSize = m_vPPort.size();
  if (requestedSize.x == 0 || requestedSize.y == 0) {
    return;
  }

  if (m_outputSize != requestedSize) {
    m_outputSize = requestedSize;
    releaseAllRenderTargets();
  }
}

void Z3DImgFilter::changeCoordTransform()
{
  VLOG(1) << "image coord changed";
  // invalidateAllFRVolumeSlices();
  if (m_3dImg) {
    m_3dImg->setScale(m_rendererParameters.coordTransform.scale());
  }
  m_imgRaycasterRenderer.compile();
  m_imgSliceRenderer.compile();

#ifdef ATLAS_DEBUG_VERSION
  // World AABB changed; invalidate cached effective cuts
  m_cachedGlobalCutsInitialized = false;
#endif
}

void Z3DImgFilter::adjustWidget()
{
  m_zSlicePosition.setVisible(m_showZSlice.get());
  m_ySlicePosition.setVisible(m_showYSlice.get());
  m_xSlicePosition.setVisible(m_showXSlice.get());
  m_zSlice2Position.setVisible(m_showZSlice2.get());
  m_ySlice2Position.setVisible(m_showYSlice2.get());
  m_xSlice2Position.setVisible(m_showXSlice2.get());
  m_obliqueSliceNormal.setVisible(m_showObliqueSlice.get());
  m_obliqueSliceDistanceToOrigin.setVisible(m_showObliqueSlice.get());
  m_obliqueSlice2Normal.setVisible(m_showObliqueSlice2.get());
  m_obliqueSlice2DistanceToOrigin.setVisible(m_showObliqueSlice2.get());
}

void Z3DImgFilter::fullResolutionRenderingToggled()
{
  m_imgRaycasterRenderer.setFastRendering(!m_fullResolutionRendering.get());
  m_imgSliceRenderer.setFastRendering(!m_fullResolutionRendering.get());
  // m_smoothInteraction.setVisible(m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get());
}

void Z3DImgFilter::leftMouseButtonPressed(QMouseEvent* /*e*/, int /*w*/, int /*h*/)
{
  //  e->ignore();
  //  if (!m_imgRaycasterRenderer.hasVisibleRendering())
  //    return;
  //  // Mouse button pressed
  //  if (e->type() == QEvent::MouseButtonPress) {
  //    m_startCoord.x = e->x();
  //    m_startCoord.y = e->y();
  //    toggleInteractionMode(true, this);
  //    return;
  //  }

  //  if (e->type() == QEvent::MouseButtonRelease) {
  //    toggleInteractionMode(false, this);
  //  }
}

void Z3DImgFilter::contextMenuEvent(QContextMenuEvent* event, int w, int h)
{
  if (isVisible() && isSelected() && m_3dImg) {
    bool success = false;
    auto pos3D = get3DPosition(event->x() * m_globalParameters.devicePixelRatio.get(),
                               event->y() * m_globalParameters.devicePixelRatio.get(),
                               w * m_globalParameters.devicePixelRatio.get(),
                               h * m_globalParameters.devicePixelRatio.get(),
                               success);

    bool enter = success;
    bool exit = m_xCut.get() != m_xCut.range() || m_yCut.get() != m_yCut.range() || m_zCut.get() != m_zCut.range();
    if (!enter && !exit) {
      return;
    }

    Q_EMIT showImgContextMenu(event->globalPos(), pos3D.x, pos3D.y, pos3D.z, enter, exit);
  }
}

// void Z3DImgFilter::invalidateFRVolumeZSlice()
//{
//   m_FRVolumeSlicesValidState[0] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeYSlice()
//{
//   m_FRVolumeSlicesValidState[1] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeXSlice()
//{
//   m_FRVolumeSlicesValidState[2] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeZSlice2()
//{
//   m_FRVolumeSlicesValidState[3] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeYSlice2()
//{
//   m_FRVolumeSlicesValidState[4] = false;
// }

// void Z3DImgFilter::invalidateFRVolumeXSlice2()
//{
//   m_FRVolumeSlicesValidState[5] = false;
// }

// void Z3DImgFilter::enterFastMode()
//{
//   if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get()) {
//     m_imgRaycasterRenderer.setFastRendering(true);
//     m_imgSliceRenderer.setFastRendering(true);
//   }
// }
//
// void Z3DImgFilter::exitFastMode()
//{
//   if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled() && m_fullResolutionRendering.get()) {
//     m_imgRaycasterRenderer.setFastRendering(false);
//     m_imgSliceRenderer.setFastRendering(false);
//     // upstream will invalidate the network, but in case there are no upstream
//     // do one more invalidation
//     if (m_imgRaycasterRenderer.lastRenderingIsFastRendering() || m_imgSliceRenderer.lastRenderingIsFastRendering()) {
//       invalidateResult();
//     }
//   }
// }

double Z3DImgFilter::process(Z3DEye eye)
{
  syncRendererState();

  // Apply any deferred progressive reset at a safe point
  if (m_resetProgressPending) {
    m_imgRaycasterRenderer.resetProgress(MonoEye);
    m_imgRaycasterRenderer.resetProgress(LeftEye);
    m_imgRaycasterRenderer.resetProgress(RightEye);
    m_imgSliceRenderer.resetProgress(MonoEye);
    m_imgSliceRenderer.resetProgress(LeftEye);
    m_imgSliceRenderer.resetProgress(RightEye);
    m_resetProgressPending = false;
  }

  if (m_channelRangeChanged) {
    if (m_3dImg) {
      std::vector<glm::dvec2> channelDisplayRanges;
      for (const auto& para : m_doubleChannelRangeParas) {
        channelDisplayRanges.push_back(para->get());
      }
      m_3dImg->setChannelDisplayRanges(channelDisplayRanges);
      for (size_t i = 0; i < m_transferFuncParas.size() && i < m_3dImg->numChannels(); ++i) {
        auto channelImage = m_3dImg->channelImageShared(i);
        m_transferFuncParas[i]->setImage(std::move(channelImage));
        m_transferFuncParas[i]->setMinMaxIntensity(m_3dImg->displayRange(i).x, m_3dImg->displayRange(i).y);
      }
    }

    m_channelRangeChanged = false;
  }

  // Depth testing is managed by renderers/compositor now

  double currentProgress = 0.0;
  double totalProgress = 0.0;

  if (hasImage()) {
    double progress = renderImage(eye);
    currentProgress += progress;
    totalProgress += 1.0;
  } else if (onlyBoundBox()) {
    renderOnlyBoundBox(eye);
  }

  if (hasSlices()) {
    double progress = renderSlices(eye);
    currentProgress += progress;
    totalProgress += 1.0;
  }

  CHECK_GL_ERROR

  if (!m_progressiveRendering) {
    CHECK(currentProgress == totalProgress) << currentProgress << " " << totalProgress;
  }
  return totalProgress > 0 ? currentProgress / totalProgress : 1.0;
}

bool Z3DImgFilter::hasSlices() const
{
  return m_showZSlice.get() || m_showXSlice.get() || m_showYSlice.get() || m_showXSlice2.get() || m_showYSlice2.get() ||
         m_showZSlice2.get() || m_showObliqueSlice.get() || m_showObliqueSlice2.get();
}

double Z3DImgFilter::renderSlices(Z3DEye eye)
{
  // Vulkan path: render slices into this filter's own per-eye opaque lease
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const bool progressiveStep = m_progressiveRendering && m_imgSliceRenderer.renderingStarted(eye);
    if (!progressiveStep) {
      m_imgSliceRenderer.setOutputSize(m_outputSize);

      glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
      glm::vec3 coordLuf = m_3dImg->physicalLUF();
      glm::vec3 coordRdb = m_3dImg->physicalRDB();

      m_imgSliceRenderer.clearSlices();

      ZMesh cube = ZMesh::createCube(coordLuf, coordRdb, glm::vec3(0, 0, 0), glm::vec3(1, 1, 1));
      if (m_showObliqueSlice.get()) {
        glm::vec3 normal = m_obliqueSliceNormal.get();
        if (glm::length(normal) == 0) {
          normal = glm::vec3(1, 1, 0);
        } else {
          normal = glm::normalize(normal);
        }
        ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSliceDistanceToOrigin.get() * normal);
        if (!slice.empty()) {
          slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
          m_imgSliceRenderer.addSlice(slice);
        }
      }
      if (m_showZSlice.get()) {
        float zTexCoord = (m_zSlicePosition.get() + .5f) / volDim.z;
        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
      if (m_showYSlice.get()) {
        float yTexCoord = (m_ySlicePosition.get() + .5f) / volDim.y;
        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
      if (m_showXSlice.get()) {
        float xTexCoord = (m_xSlicePosition.get() + .5f) / volDim.x;
        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
      if (m_showObliqueSlice2.get()) {
        glm::vec3 normal = m_obliqueSlice2Normal.get();
        if (glm::length(normal) == 0) {
          normal = glm::vec3(1, 1, 0);
        } else {
          normal = glm::normalize(normal);
        }
        ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSlice2DistanceToOrigin.get() * normal);
        if (!slice.empty()) {
          slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
          m_imgSliceRenderer.addSlice(slice);
        }
      }
      if (m_showZSlice2.get()) {
        float zTexCoord = (m_zSlice2Position.get() + .5f) / volDim.z;
        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
      if (m_showYSlice2.get()) {
        float yTexCoord = (m_ySlice2Position.get() + .5f) / volDim.y;
        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
      if (m_showXSlice2.get()) {
        float xTexCoord = (m_xSlice2Position.get() + .5f) / volDim.x;
        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
        ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
    }

    // Ensure/prepare opaque Vulkan lease and clear
    auto& lease = m_opaqueTargets[eye];
    if (!lease || lease.descriptor.size != m_outputSize || !lease.hasVulkanImage()) {
      lease.release();
      m_rendererBase.acquirePersistentTempRenderTarget2D(lease, m_outputSize, ScratchFormat::RGBA16, ScratchFormat::Depth24);
    }

    auto surface = m_rendererBase.describeSurface(lease);
    for (auto& att : surface.colorAttachments) {
      att.loadOp = LoadOp::Clear;
      att.storeOp = StoreOp::Store;
      att.clearValue.color = glm::vec4(0.f);
    }
    if (surface.depthAttachment) {
      surface.depthAttachment->loadOp = LoadOp::Clear;
      surface.depthAttachment->storeOp = StoreOp::Store;
      surface.depthAttachment->clearValue.depth = 1.0f;
      surface.depthAttachment->clearValue.stencil = 0u;
    }
    auto prevSurface = m_rendererBase.frameState().activeSurface;
    m_rendererBase.frameState().setActiveSurface(surface);
    m_rendererBase.render(eye, m_imgSliceRenderer);
    m_rendererBase.frameState().setActiveSurface(prevSurface);
    m_opaqueValid[eye] = true;
    return 1.0;
  }

  Z3DRenderTarget& currentTarget = opaqueTarget(eye);

  const bool progressiveStep = m_progressiveRendering && m_imgSliceRenderer.renderingStarted(eye);
  if (!progressiveStep) {
    m_imgSliceRenderer.setOutputSize(currentTarget.size());

    glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
    glm::vec3 coordLuf = m_3dImg->physicalLUF();
    glm::vec3 coordRdb = m_3dImg->physicalRDB();

    m_imgSliceRenderer.clearSlices();

    ZMesh cube = ZMesh::createCube(coordLuf, coordRdb, glm::vec3(0, 0, 0), glm::vec3(1, 1, 1));
    if (m_showObliqueSlice.get()) {
      glm::vec3 normal = m_obliqueSliceNormal.get();
      if (glm::length(normal) == 0) {
        normal = glm::vec3(1, 1, 0);
      } else {
        normal = glm::normalize(normal);
      }
      ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSliceDistanceToOrigin.get() * normal);
      if (!slice.empty()) {
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
    }
    if (m_showZSlice.get()) {
      float zTexCoord = (m_zSlicePosition.get() + .5f) / volDim.z;
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showYSlice.get()) {
      float yTexCoord = (m_ySlicePosition.get() + .5f) / volDim.y;
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showXSlice.get()) {
      float xTexCoord = (m_xSlicePosition.get() + .5f) / volDim.x;
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }

    if (m_showObliqueSlice2.get()) {
      glm::vec3 normal = m_obliqueSlice2Normal.get();
      if (glm::length(normal) == 0) {
        normal = glm::vec3(1, 1, 0);
      } else {
        normal = glm::normalize(normal);
      }
      ZMesh slice = ZMesh::planeClosedSurfaceIntersection(cube, normal, m_obliqueSlice2DistanceToOrigin.get() * normal);
      if (!slice.empty()) {
        slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgSliceRenderer.addSlice(slice);
      }
    }
    if (m_showZSlice2.get()) {
      float zTexCoord = (m_zSlice2Position.get() + .5f) / volDim.z;
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showYSlice2.get()) {
      float yTexCoord = (m_ySlice2Position.get() + .5f) / volDim.y;
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showXSlice2.get()) {
      float xTexCoord = (m_xSlice2Position.get() + .5f) / volDim.x;
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgSliceRenderer.addSlice(slice);
    }
  }

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_opaqueValid[eye] = false;

  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_opaqueValid[eye] = true;
  });

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgSliceRenderer);
  } else {
    progress = m_imgSliceRenderer.renderProgressively(eye);
  }

  return progress;
}

bool Z3DImgFilter::hasImage() const
{
  return m_imgRaycasterRenderer.hasVisibleRendering() && m_xCut.upperValue() > m_xCut.minimum() &&
         m_yCut.upperValue() > m_yCut.minimum() && m_zCut.upperValue() > m_zCut.minimum() &&
         m_xCut.lowerValue() < m_xCut.maximum() && m_yCut.lowerValue() < m_yCut.maximum() &&
         m_zCut.lowerValue() < m_zCut.maximum();
}

double Z3DImgFilter::renderImage(Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    if (!(m_progressiveRendering && m_imgRaycasterRenderer.renderingStarted(eye))) {
      m_imgRaycasterRenderer.setOutputSize(m_outputSize);

      glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
      glm::vec3 coordLuf = m_3dImg->physicalLUF();
      glm::vec3 coordRdb = m_3dImg->physicalRDB();

      float xTexCoordStart = m_xCut.lowerValue() / volDim.x;
      float xTexCoordEnd = m_xCut.upperValue() / volDim.x;
      float xCoordStart = glm::mix(coordLuf.x, coordRdb.x, xTexCoordStart);
      float xCoordEnd = glm::mix(coordLuf.x, coordRdb.x, xTexCoordEnd);
      float yTexCoordStart = m_yCut.lowerValue() / volDim.y;
      float yTexCoordEnd = m_yCut.upperValue() / volDim.y;
      float yCoordStart = glm::mix(coordLuf.y, coordRdb.y, yTexCoordStart);
      float yCoordEnd = glm::mix(coordLuf.y, coordRdb.y, yTexCoordEnd);
      float zTexCoordStart = m_zCut.lowerValue() / volDim.z;
      float zTexCoordEnd = m_zCut.upperValue() / volDim.z;
      float zCoordStart = glm::mix(coordLuf.z, coordRdb.z, zTexCoordStart);
      float zCoordEnd = glm::mix(coordLuf.z, coordRdb.z, zTexCoordEnd);

      if (m_3dImg->is2DData()) {
        ZMesh m_2DImageQuad = ZMesh::createImageSlice(0,
                                                      glm::vec2(xCoordStart, yCoordStart),
                                                      glm::vec2(xCoordEnd, yCoordEnd),
                                                      glm::vec2(xTexCoordStart, yTexCoordStart),
                                                      glm::vec2(xTexCoordEnd, yTexCoordEnd));
        m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgRaycasterRenderer.clearQuads();
        m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
      } else if (m_zCut.lowerValue() == m_zCut.upperValue()) {
        ZMesh m_2DImageQuad = ZMesh::createCubeSlice(zCoordStart,
                                                     zTexCoordStart,
                                                     2,
                                                     glm::vec2(xCoordStart, yCoordStart),
                                                     glm::vec2(xCoordEnd, yCoordEnd),
                                                     glm::vec2(xTexCoordStart, yTexCoordStart),
                                                     glm::vec2(xTexCoordEnd, yTexCoordEnd));
        m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgRaycasterRenderer.clearQuads();
        m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
      } else if (m_yCut.lowerValue() == m_yCut.upperValue()) {
        ZMesh m_2DImageQuad = ZMesh::createCubeSlice(yCoordStart,
                                                     yTexCoordStart,
                                                     1,
                                                     glm::vec2(xCoordStart, zCoordStart),
                                                     glm::vec2(xCoordEnd, zCoordEnd),
                                                     glm::vec2(xTexCoordStart, zTexCoordStart),
                                                     glm::vec2(xTexCoordEnd, zTexCoordEnd));
        m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgRaycasterRenderer.clearQuads();
        m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
      } else if (m_xCut.lowerValue() == m_xCut.upperValue()) {
        ZMesh m_2DImageQuad = ZMesh::createCubeSlice(xCoordStart,
                                                     xTexCoordStart,
                                                     0,
                                                     glm::vec2(yCoordStart, zCoordStart),
                                                     glm::vec2(yCoordEnd, zCoordEnd),
                                                     glm::vec2(yTexCoordStart, zTexCoordStart),
                                                     glm::vec2(yTexCoordEnd, zTexCoordEnd));
        m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        m_imgRaycasterRenderer.clearQuads();
        m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
      } else { // 3d volume Raycasting
        ZMesh cube = ZMesh::createCube(glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                       glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                       glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                       glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
        cube.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
        bool flipped = glm::determinant(glm::mat3(m_rendererParameters.coordTransform.get())) < 0.0;

        std::vector<glm::vec3> planeNormals;
        std::vector<glm::vec3> planeOrigins;
        planeNormals.push_back(m_globalParameters.camera.get().viewVector());
        planeOrigins.push_back(m_globalParameters.camera.get().eye() +
                               m_globalParameters.camera.get().viewVector() *
                                 (m_globalParameters.camera.get().nearDist() + 0.01f));
        if (m_globalParameters.globalXCut.lowerValue() != m_globalParameters.globalXCut.minimum()) {
          planeNormals.emplace_back(1., 0., 0.);
          planeOrigins.emplace_back(m_globalParameters.globalXCut.lowerValue(), 0, 0);
        }
        if (m_globalParameters.globalXCut.upperValue() != m_globalParameters.globalXCut.maximum()) {
          planeNormals.emplace_back(-1., 0., 0.);
          planeOrigins.emplace_back(m_globalParameters.globalXCut.upperValue(), 0, 0);
        }
        if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
          planeNormals.emplace_back(0., 1., 0.);
          planeOrigins.emplace_back(0, m_globalParameters.globalYCut.lowerValue(), 0);
        }
        if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
          planeNormals.emplace_back(0., -1., 0.);
          planeOrigins.emplace_back(0, m_globalParameters.globalYCut.upperValue(), 0);
        }
        if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
          planeNormals.emplace_back(0., 0., 1.);
          planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.lowerValue());
        }
        if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
          planeNormals.emplace_back(0., 0., -1.);
          planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.upperValue());
        }
        ZMesh clipped = ZMesh::clipClosedSurface(cube, planeNormals, planeOrigins);
        m_imgRaycasterRenderer.prepareEntryExit(clipped, flipped, eye, m_outputSize);
      }
    }

    // Render into this filter's own per-eye transparent lease (Vulkan)
    auto& lease = m_transparentTargets[eye];
    if (!lease || lease.descriptor.size != m_outputSize || !lease.hasVulkanImage()) {
      lease.release();
      m_rendererBase.acquirePersistentTempRenderTarget2D(lease, m_outputSize, ScratchFormat::RGBA16, ScratchFormat::Depth24);
    }

    // Clear target, set active surface, and record batches for the raycaster
    m_rendererBase.setCollectOnly(true);
    m_rendererBase.setActiveSurfaceForNextPass(lease);
    ClearValue clear{};
    clear.color = glm::vec4(0.0f);
    clear.depth = 1.0f;
    clear.stencil = 0u;
    m_rendererBase.setPendingColorAttachmentsLoadStore(LoadOp::Clear, StoreOp::Store, clear);
    m_rendererBase.setPendingDepthAttachmentLoadStore(LoadOp::Clear, StoreOp::Store, clear);
    m_rendererBase.executeVulkanBatches([&]() {
      m_rendererBase.render(eye, m_imgRaycasterRenderer);
    });
    // Draw bound box using the same active surface; no clears
    m_rendererBase.setActiveSurfaceForNextPass(lease);
    m_rendererBase.executeVulkanBatches([&]() {
      renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
    });
    m_rendererBase.setCollectOnly(false);

    m_transparentValid[eye] = true;
    return 1.0;
  }

  Z3DRenderTarget& currentTarget = transparentTarget(eye);

  // VLOG(1) << m_progressiveRendering << " " << m_imgRaycasterRenderer.renderingStarted(eye);
  if (!(m_progressiveRendering && m_imgRaycasterRenderer.renderingStarted(eye))) {
    m_imgRaycasterRenderer.setOutputSize(currentTarget.size());

    glm::uvec3 volDim = glm::max(glm::uvec3(2, 2, 2), m_3dImg->dimensions());
    glm::vec3 coordLuf = m_3dImg->physicalLUF();
    glm::vec3 coordRdb = m_3dImg->physicalRDB();

    float xTexCoordStart = m_xCut.lowerValue() / volDim.x;
    float xTexCoordEnd = m_xCut.upperValue() / volDim.x;
    float xCoordStart = glm::mix(coordLuf.x, coordRdb.x, xTexCoordStart);
    float xCoordEnd = glm::mix(coordLuf.x, coordRdb.x, xTexCoordEnd);
    float yTexCoordStart = m_yCut.lowerValue() / volDim.y;
    float yTexCoordEnd = m_yCut.upperValue() / volDim.y;
    float yCoordStart = glm::mix(coordLuf.y, coordRdb.y, yTexCoordStart);
    float yCoordEnd = glm::mix(coordLuf.y, coordRdb.y, yTexCoordEnd);
    float zTexCoordStart = m_zCut.lowerValue() / volDim.z;
    float zTexCoordEnd = m_zCut.upperValue() / volDim.z;
    float zCoordStart = glm::mix(coordLuf.z, coordRdb.z, zTexCoordStart);
    float zCoordEnd = glm::mix(coordLuf.z, coordRdb.z, zTexCoordEnd);

    if (m_3dImg->is2DData()) { // for 2d image
      ZMesh m_2DImageQuad = ZMesh::createImageSlice(0,
                                                    glm::vec2(xCoordStart, yCoordStart),
                                                    glm::vec2(xCoordEnd, yCoordEnd),
                                                    glm::vec2(xTexCoordStart, yTexCoordStart),
                                                    glm::vec2(xTexCoordEnd, yTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgRaycasterRenderer.clearQuads();
      m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
    } else if (m_zCut.lowerValue() == m_zCut.upperValue()) { // slice of 3d image
      ZMesh m_2DImageQuad = ZMesh::createCubeSlice(zCoordStart,
                                                   zTexCoordStart,
                                                   2,
                                                   glm::vec2(xCoordStart, yCoordStart),
                                                   glm::vec2(xCoordEnd, yCoordEnd),
                                                   glm::vec2(xTexCoordStart, yTexCoordStart),
                                                   glm::vec2(xTexCoordEnd, yTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgRaycasterRenderer.clearQuads();
      m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
    } else if (m_yCut.lowerValue() == m_yCut.upperValue()) { // slice of 3d image
      ZMesh m_2DImageQuad = ZMesh::createCubeSlice(yCoordStart,
                                                   yTexCoordStart,
                                                   1,
                                                   glm::vec2(xCoordStart, zCoordStart),
                                                   glm::vec2(xCoordEnd, zCoordEnd),
                                                   glm::vec2(xTexCoordStart, zTexCoordStart),
                                                   glm::vec2(xTexCoordEnd, zTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgRaycasterRenderer.clearQuads();
      m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
    } else if (m_xCut.lowerValue() == m_xCut.upperValue()) { // slice of 3d image
      ZMesh m_2DImageQuad = ZMesh::createCubeSlice(xCoordStart,
                                                   xTexCoordStart,
                                                   0,
                                                   glm::vec2(yCoordStart, zCoordStart),
                                                   glm::vec2(yCoordEnd, zCoordEnd),
                                                   glm::vec2(yTexCoordStart, zTexCoordStart),
                                                   glm::vec2(yTexCoordEnd, zTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      m_imgRaycasterRenderer.clearQuads();
      m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
    } else { // 3d volume Raycasting
      ZMesh cube = ZMesh::createCube(glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                     glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                     glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                     glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
      cube.transformVerticesByMatrix(m_rendererParameters.coordTransform.get());
      bool flipped = glm::determinant(glm::mat3(m_rendererParameters.coordTransform.get())) < 0.0;

      setViewport(currentTarget.size());
      CHECK_GL_ERROR

      std::vector<glm::vec3> planeNormals;
      std::vector<glm::vec3> planeOrigins;
      planeNormals.push_back(m_globalParameters.camera.get().viewVector());
      planeOrigins.push_back(m_globalParameters.camera.get().eye() +
                             m_globalParameters.camera.get().viewVector() *
                               (m_globalParameters.camera.get().nearDist() + 0.01f));
      if (m_globalParameters.globalXCut.lowerValue() != m_globalParameters.globalXCut.minimum()) {
        planeNormals.emplace_back(1., 0., 0.);
        planeOrigins.emplace_back(m_globalParameters.globalXCut.lowerValue(), 0, 0);
      }
      if (m_globalParameters.globalXCut.upperValue() != m_globalParameters.globalXCut.maximum()) {
        planeNormals.emplace_back(-1., 0., 0.);
        planeOrigins.emplace_back(m_globalParameters.globalXCut.upperValue(), 0, 0);
      }
      if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
        planeNormals.emplace_back(0., 1., 0.);
        planeOrigins.emplace_back(0, m_globalParameters.globalYCut.lowerValue(), 0);
      }
      if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
        planeNormals.emplace_back(0., -1., 0.);
        planeOrigins.emplace_back(0, m_globalParameters.globalYCut.upperValue(), 0);
      }
      if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
        planeNormals.emplace_back(0., 0., 1.);
        planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.lowerValue());
      }
      if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
        planeNormals.emplace_back(0., 0., -1.);
        planeOrigins.emplace_back(0, 0, m_globalParameters.globalZCut.upperValue());
      }
      // VLOG(1) << planeNormals.size();
      ZMesh clipped = ZMesh::clipClosedSurface(cube, planeNormals, planeOrigins);
#if 0
    float nearPlaneDistToOrigin =
      glm::dot(globalCamera().eye(), -globalCamera().viewVector()) - globalCamera().nearDist() - 0.01f;
    std::vector<glm::vec4> planes;
    planes.emplace_back(-globalCamera().viewVector(), nearPlaneDistToOrigin);
    if (m_globalParameters.xCut.lowerValue() != m_globalParameters.xCut.minimum()) {
      planes.emplace_back(-1., 0., 0., -m_globalParameters.xCut.lowerValue());
    }
    if (m_globalParameters.xCut.upperValue() != m_globalParameters.xCut.maximum()) {
      planes.emplace_back(1., 0., 0., m_globalParameters.xCut.upperValue());
    }
    if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
      planes.emplace_back(0., -1., 0., -m_globalParameters.globalYCut.lowerValue());
    }
    if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
      planes.emplace_back(0., 1., 0., m_globalParameters.globalYCut.upperValue());
    }
    if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
      planes.emplace_back(0., 0., -1., -m_globalParameters.globalZCut.lowerValue());
    }
    if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
      planes.emplace_back(0., 0., 1., m_globalParameters.globalZCut.upperValue());
    }
    VLOG(1) << planes.size();
    auto clipped = ZMeshUtils::clipClosedSurface(cube, planes);
#endif

      // prepare entry/exit in renderer
      m_imgRaycasterRenderer.prepareEntryExit(clipped, flipped, eye, currentTarget.size());
    }
  }

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_transparentValid[eye] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_transparentValid[eye] = true;
  });

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgRaycasterRenderer);
  } else {
    progress = m_imgRaycasterRenderer.renderProgressively(eye);
  }

  // Draw bound box with local overlay state
  renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
  CHECK_GL_ERROR

  return progress;
}

bool Z3DImgFilter::onlyBoundBox() const
{
  return !hasImage() && !m_boundBoxMode.isSelected("No Bound Box");
}

void Z3DImgFilter::renderOnlyBoundBox(Z3DEye eye)
{
  Z3DRenderTarget& currentTarget = transparentTarget(eye);

  currentTarget.bind();
  currentTarget.clear();
  setViewport(currentTarget.size());

  m_transparentValid[eye] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, eye]() {
    currentTarget.release();
    m_transparentValid[eye] = true;
  });

  // Draw bound box with local overlay state
  renderBoundBox(eye, Z3DBoundedFilter::BoundBoxRenderStyle::OverlayAlphaDepth);
  CHECK_GL_ERROR
}

void Z3DImgFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.setMinCorner(glm::dvec3(0.0));
  m_notTransformedBoundBox.setMaxCorner(glm::dvec3(m_3dImg->dimensions()));
}

void Z3DImgFilter::updateBlockIDTarget()
{
  //  if (m_3dImg && m_3dImg->isVolumeDownsampled()) {
  //    glm::uvec2 size = m_layerTarget.size();
  ////    uint32_t sizeScale =
  ////      std::min(std::min(Z3DImg::imageBlockSize().x, Z3DImg::imageBlockSize().y), Z3DImg::imageBlockSize().z) / 10;
  //    uint32_t sizeScale = 1;
  //    size.x = (size.x + sizeScale - 1) / sizeScale;
  //    size.y = (size.y + sizeScale - 1) / sizeScale;
  //    m_blockIDsRenderTarget.resize(size);
  //  }
}

// void Z3DImgFilter::invalidateAllFRVolumeSlices()
//{
//   m_FRVolumeSlicesValidState.clear();
//   m_FRVolumeSlicesValidState.resize(m_maxNumOfFullResolutionVolumeSlice, false);
// }

void Z3DImgFilter::volumeChanged() {}

void Z3DImgFilter::channelRangeChanged()
{
  m_channelRangeChanged = true;
}

glm::vec3 Z3DImgFilter::getFirstHit3DPosition(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  success = false;
  const bool monoValid = m_transparentValid[MonoEye];
  const bool rightValid = m_transparentValid[RightEye];
  if (m_imgRaycasterRenderer.hasVisibleRendering() && (monoValid || rightValid)) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderTarget& target = monoValid ? transparentTarget(MonoEye) : transparentTarget(RightEye);

    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, target);
    res = glm::round(glm::applyMatrix(inverseCoordTransform(), fpos3D));
    if (glm::all(glm::greaterThanEqual(res, glm::vec3(0.f))) &&
        glm::all(glm::lessThan(res, glm::vec3(m_3dImg->dimensions())))) {
      success = true;
    }
  }
  return res;
}

glm::vec3 Z3DImgFilter::getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  glm::vec3 des(-1);
  success = false;
  const bool monoValid = m_transparentValid[MonoEye];
  const bool rightValid = m_transparentValid[RightEye];
  if (m_imgRaycasterRenderer.hasVisibleRendering() && m_3dImg && (monoValid || rightValid)) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderTarget& target = monoValid ? transparentTarget(MonoEye) : transparentTarget(RightEye);

    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, target);
    res = glm::round(glm::applyMatrix(inverseCoordTransform(), fpos3D));
    if (glm::all(glm::greaterThanEqual(res, glm::vec3(0.f))) &&
        glm::all(glm::lessThan(res, glm::vec3(m_3dImg->dimensions())))) {
      success = true;
    }

    if (success) {
      fpos3D = get3DPosition(pos2D, 1.0, width, height);
      des = glm::round(glm::applyMatrix(inverseCoordTransform(), fpos3D));
      if (glm::length(des - res) <= 1.f) { // res is last pixel along current ray direction
        return res;
      }
    }
  }

  // find maximum intensity voxel start from res along des direction
  if (success) {
    double maxInten = m_3dImg->imgPack().value(res.x, res.y, res.z);
    glm::vec3 p = res;
    glm::vec3 d = des - res;
    float N = std::max(std::max(std::abs(d.x), std::abs(d.y)), std::abs(d.z));
    glm::vec3 stepSize = d / N;
    while (true) {
      p = p + stepSize;
      glm::vec3 roundP = glm::round(p);
      if (roundP.x < 0 || roundP.x >= m_3dImg->imgPack().imgInfo().width || roundP.y < 0 ||
          roundP.y >= m_3dImg->imgPack().imgInfo().height || roundP.z < 0 ||
          roundP.z >= m_3dImg->imgPack().imgInfo().depth) {
        break;
      }
      double inten = m_3dImg->imgPack().value(roundP.x, roundP.y, roundP.z);
      if (inten > maxInten) {
        maxInten = inten;
        res = roundP;
      }
    }
  }
  return res;
}

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderTarget& target)
{
  glm::mat4 projection = m_globalParameters.camera.get().projectionMatrix(MonoEye);
  glm::mat4 modelview = m_globalParameters.camera.get().viewMatrix(MonoEye);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  GLfloat WindowPosZ = target.depthAtPos(pos2D);
  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, WindowPosZ), modelview, projection, viewport);

  return pos;
}

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, double depth, int width, int height)
{
  glm::mat4 projection = m_globalParameters.camera.get().projectionMatrix(MonoEye);
  glm::mat4 modelview = m_globalParameters.camera.get().viewMatrix(MonoEye);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, depth), modelview, projection, viewport);

  return pos;
}

Z3DRenderTarget& Z3DImgFilter::transparentTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_transparentTargets[eye]);
}

const Z3DRenderTarget& Z3DImgFilter::transparentTarget(Z3DEye eye) const
{
  const auto& lease = m_transparentTargets[eye];
  CHECK(lease.renderTarget) << "transparent target requested before rendering";
  return *lease.renderTarget;
}

Z3DRenderTarget& Z3DImgFilter::opaqueTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_opaqueTargets[eye]);
}

const Z3DRenderTarget& Z3DImgFilter::opaqueTarget(Z3DEye eye) const
{
  const auto& lease = m_opaqueTargets[eye];
  CHECK(lease.renderTarget) << "opaque target requested before rendering";
  return *lease.renderTarget;
}

Z3DRenderTarget& Z3DImgFilter::ensureRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (!lease.renderTarget || lease.renderTarget->size() != m_outputSize) {
    lease.release();
    CHECK_GT(m_outputSize.x, 0u);
    CHECK_GT(m_outputSize.y, 0u);
    m_rendererBase.acquirePersistentTempRenderTarget2D(lease, m_outputSize);
  }
  return *lease.renderTarget;
}

void Z3DImgFilter::releaseAllRenderTargets()
{
  for (auto& lease : m_transparentTargets) {
    lease.release();
  }
  for (auto& lease : m_opaqueTargets) {
    lease.release();
  }
}

void Z3DImgFilter::markTargetsInvalid()
{
  m_transparentValid.fill(false);
  m_opaqueValid.fill(false);
}

} // namespace nim
