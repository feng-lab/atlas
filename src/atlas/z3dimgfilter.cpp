#include "z3dimgfilter.h"

#include "z3dgpuinfo.h"
#include "zbenchtimer.h"
#include "zeventlistenerparameter.h"
#include "zlog.h"
#include "zmesh.h"
#include <QMenu>
#include <memory>

namespace nim {

// const size_t Z3DImgFilter::m_maxNumOfFullResolutionVolumeSlice = 6;

Z3DImgFilter::Z3DImgFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_imgRaycasterRenderer(m_rendererBase)
  , m_imgSliceRenderer(m_rendererBase)
  , m_textureAndEyeCoordinateRenderer(m_rendererBase)
  , m_textureCopyRenderer(m_rendererBase)
  , m_stayOnTop("Stay On Top", false)
  , m_fullResolutionRendering("Full Resolution Rendering", false)
  , m_numParas(0)
  //, m_interactionDownsample("Interaction Downsample", 1, 1, 16)
  //, m_smoothInteraction("Smooth Interaction", true)
  , m_entryExitTarget(glm::uvec2(32, 32))
  , m_layerTarget(glm::uvec2(32, 32))
  , m_layerColorTexture(GL_TEXTURE_2D_ARRAY, GLint(GL_RGBA16), glm::uvec3(32, 32, 3), GL_RGBA, GL_FLOAT)
  , m_layerDepthTexture(GL_TEXTURE_2D_ARRAY,
                        GLint(GL_DEPTH_COMPONENT24),
                        glm::uvec3(32, 32, 3),
                        GL_DEPTH_COMPONENT,
                        GL_FLOAT)
  , m_missBlocksTexture0(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture1(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture2(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture3(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture4(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture5(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture6(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_missBlocksTexture7(GL_TEXTURE_2D,
                         GLint(GL_RGBA32UI),
                         glm::uvec3(32, 32, 1),
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         nullptr,
                         GLint(GL_NEAREST),
                         GLint(GL_NEAREST))
  , m_blockIDsRenderTarget(glm::uvec2(32, 32))
  , m_imageRenderTarget1(glm::uvec2(32, 32))
  , m_imageRenderTarget2(glm::uvec2(32, 32))
  , m_imageRenderTarget1Left(glm::uvec2(32, 32))
  , m_imageRenderTarget2Left(glm::uvec2(32, 32))
  , m_imageRenderTarget1Right(glm::uvec2(32, 32))
  , m_imageRenderTarget2Right(glm::uvec2(32, 32))
  , m_outport("Image", this)
  , m_leftEyeOutport("LeftEyeImage", this)
  , m_rightEyeOutport("RightEyeImage", this)
  , m_vPPort("VolumeFilter", this)
  , m_opaqueOutport("OpaqueImage", this)
  , m_opaqueLeftEyeOutport("OpaqueLeftEyeImage", this)
  , m_opaqueRightEyeOutport("OpaqueRightEyeImage", this)
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

  addParameter(m_stayOnTop);
  addParameter(m_fullResolutionRendering);
  connect(&m_rendererBase, &Z3DRendererBase::coordTransformChanged, this, &Z3DImgFilter::changeCoordTransform);
  //  connect(&m_rendererBase.globalParas().interactionHandler,
  //          &Z3DTrackballInteractionHandler::enterInteractionMode,
  //          this,
  //          &Z3DImgFilter::enterFastMode);
  //  connect(&m_rendererBase.globalParas().interactionHandler,
  //          &Z3DTrackballInteractionHandler::exitInteractionMode,
  //          this,
  //          &Z3DImgFilter::exitFastMode);

  // addParameter(m_interactionDownsample);
  // addParameter(m_smoothInteraction);

  Z3DTexture* g_TexId[2];

  g_TexId[0] = new Z3DTexture(GL_TEXTURE_2D_ARRAY,
                              GLint(GL_RGBA32F),
                              glm::uvec3(32, 32, 2),
                              GL_RGBA,
                              GL_FLOAT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
  m_entryExitTarget.attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0);
  m_entryExitTarget.isFBOComplete();

  m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
  m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
  m_layerTarget.isFBOComplete();

  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture0, GL_COLOR_ATTACHMENT0, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture1, GL_COLOR_ATTACHMENT1, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture2, GL_COLOR_ATTACHMENT2, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture3, GL_COLOR_ATTACHMENT3, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture4, GL_COLOR_ATTACHMENT4, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture5, GL_COLOR_ATTACHMENT5, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture6, GL_COLOR_ATTACHMENT6, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture7, GL_COLOR_ATTACHMENT7, false);
  m_blockIDsRenderTarget.isFBOComplete();

  m_imageRenderTarget1s[0] = &m_imageRenderTarget1Left;
  m_imageRenderTarget1s[1] = &m_imageRenderTarget1;
  m_imageRenderTarget1s[2] = &m_imageRenderTarget1Right;
  m_imageRenderTarget2s[0] = &m_imageRenderTarget2Left;
  m_imageRenderTarget2s[1] = &m_imageRenderTarget2;
  m_imageRenderTarget2s[2] = &m_imageRenderTarget2Right;
  for (int i = 0; i < 3; ++i) {
    g_TexId[0] = new Z3DTexture(GLint(GL_RGBA16),
                                glm::uvec3(32, 32, 1),
                                GL_RGBA,
                                GL_FLOAT,
                                nullptr,
                                GLint(GL_NEAREST),
                                GLint(GL_NEAREST));
    g_TexId[1] = new Z3DTexture(GLint(GL_RG32F),
                                glm::uvec3(32, 32, 1),
                                GL_RG,
                                GL_FLOAT,
                                nullptr,
                                GLint(GL_NEAREST),
                                GLint(GL_NEAREST));
    m_imageRenderTarget1s[i]->attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0, true);
    m_imageRenderTarget1s[i]->attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1, true);
    m_imageRenderTarget1s[i]->isFBOComplete();
    g_TexId[0] = new Z3DTexture(GLint(GL_RGBA16),
                                glm::uvec3(32, 32, 1),
                                GL_RGBA,
                                GL_FLOAT,
                                nullptr,
                                GLint(GL_NEAREST),
                                GLint(GL_NEAREST));
    g_TexId[1] = new Z3DTexture(GLint(GL_RG32F),
                                glm::uvec3(32, 32, 1),
                                GL_RG,
                                GL_FLOAT,
                                nullptr,
                                GLint(GL_NEAREST),
                                GLint(GL_NEAREST));
    m_imageRenderTarget2s[i]->attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0, true);
    m_imageRenderTarget2s[i]->attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1, true);
    m_imageRenderTarget2s[i]->isFBOComplete();
  }

  // ports
  addPort(m_outport);
  addPort(m_leftEyeOutport);
  addPort(m_rightEyeOutport);
  addPort(m_vPPort);

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

  m_imgRaycasterRenderer.setLayerTarget(m_layerTarget);
  m_imgSliceRenderer.setLayerTarget(m_layerTarget);
  m_imgRaycasterRenderer.setBlockIDsRenderTarget(m_blockIDsRenderTarget);
  m_imgSliceRenderer.setBlockIDsRenderTarget(m_blockIDsRenderTarget);
  m_imgRaycasterRenderer.setImageRenderTargetWithRayDepthLayer(m_imageRenderTarget1s, m_imageRenderTarget2s);
  //  for (size_t i=0; i<m_maxNumOfFullResolutionVolumeSlice; ++i) {
  //    m_image2DRenderers.emplace_back(std::make_unique<Z3DImage2DRenderer>(m_rendererBase));
  //    m_image2DRenderers[i]->setLayerTarget(&m_layerTarget);
  //  }
  m_boundBoxLineWidth.set(1);
  m_boundBoxMode.select("Bound Box");

  addParameter(m_imgRaycasterRenderer.compositingModePara());
  addParameter(m_imgRaycasterRenderer.isoValuePara());
  addParameter(m_imgRaycasterRenderer.localMIPThresholdPara());
  addParameter(m_imgRaycasterRenderer.samplingRatePara());

  m_imgRaycasterRenderer.setFastRendering(!m_fullResolutionRendering.get());
  m_imgSliceRenderer.setFastRendering(!m_fullResolutionRendering.get());
  connect(&m_fullResolutionRendering,
          &ZBoolParameter::valueChanged,
          this,
          &Z3DImgFilter::fullResolutionRenderingToggled);

  connect(&m_rendererBase.globalXCutPara(), &ZFloatSpanParameter::valueChanged, this, &Z3DImgFilter::invalidateResult);
  connect(&m_rendererBase.globalYCutPara(), &ZFloatSpanParameter::valueChanged, this, &Z3DImgFilter::invalidateResult);
  connect(&m_rendererBase.globalZCutPara(), &ZFloatSpanParameter::valueChanged, this, &Z3DImgFilter::invalidateResult);

  adjustWidget();
  CHECK_GL_ERROR

  m_numParas = m_parameters.size();
}

void Z3DImgFilter::setData(const ZImgPack& imgPack)
{
  if (m_widgetsGroup) {
    for (const auto& para : m_imgRaycasterRenderer.channelVisibleParas()) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_imgRaycasterRenderer.transferFuncParas()) {
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
    std::vector<glm::dvec2> drs;
    if (imgPack.hasMinMax() && imgPack.maxIntensity() > imgPack.minIntensity()) {
      drs = std::vector<glm::dvec2>(
        imgPack.imgInfo().numChannels,
        glm::dvec2(imgPack.minIntensity() + (imgPack.maxIntensity() - imgPack.minIntensity()) * 0.02,
                   imgPack.maxIntensity()));
    } else {
      drs = std::vector<glm::dvec2>(
        imgPack.imgInfo().numChannels,
        glm::dvec2(imgPack.rangeMin() + (imgPack.rangeMax() - imgPack.rangeMin()) * 0.02, imgPack.rangeMax()));
    }

    m_3dImg = std::make_unique<Z3DImg>(imgPack, m_rendererBase.coordTransformPara().scale(), drs);
    connect(m_3dImg.get(), &Z3DImg::renderingError, this, &Z3DImgFilter::renderingError);

    updateBlockIDTarget();

    if (m_3dImg->numChannels() > m_layerColorTexture.depth()) {
      m_layerColorTexture.setDimension(
        glm::uvec3(m_layerColorTexture.width(), m_layerColorTexture.height(), m_3dImg->numChannels()));
      m_layerDepthTexture.setDimension(
        glm::uvec3(m_layerDepthTexture.width(), m_layerDepthTexture.height(), m_3dImg->numChannels()));
      m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
      m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
      m_layerTarget.isFBOComplete();
    }
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

    m_rendererBase.setRotationCenter(glm::vec3(volDim.x, volDim.y, volDim.z) / 2.f);

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
    if (!is2DImage) {
      m_imgSliceRenderer.setData(*m_3dImg, m_sliceColormaps);
    }

    updateBoundBox();

    for (const auto& para : m_imgRaycasterRenderer.channelVisibleParas()) {
      addParameter(*para);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      addParameter(*para);
    }
    for (const auto& para : m_imgRaycasterRenderer.transferFuncParas()) {
      addParameter(*para);
    }
    //    for (const auto& para : m_imgRaycasterRenderer.texFilterModeParas()) {
    //      addParameter(*para);
    //    }
    for (const auto& cm : m_sliceColormaps) {
      addParameter(*cm);
    }

    if (m_widgetsGroup) {
      for (const auto& para : m_imgRaycasterRenderer.channelVisibleParas()) {
        m_widgetsGroup->addChild(*para, 2);
      }
      for (const auto& para : m_doubleChannelRangeParas) {
        m_widgetsGroup->addChild(*para, 3);
      }
      for (const auto& para : m_imgRaycasterRenderer.transferFuncParas()) {
        m_widgetsGroup->addChild(*para, 3);
      }
      //      for (const auto& para : m_imgRaycasterRenderer.texFilterModeParas()) {
      //        m_widgetsGroup->addChild(*para, 15);
      //      }
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

    for (const auto& para : m_imgRaycasterRenderer.channelVisibleParas()) {
      m_widgetsGroup->addChild(*para, 2);
    }
    for (const auto& para : m_doubleChannelRangeParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    for (const auto& para : m_imgRaycasterRenderer.transferFuncParas()) {
      m_widgetsGroup->addChild(*para, 3);
    }
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.compositingModePara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.isoValuePara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.localMIPThresholdPara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.samplingRatePara(), 15);
    //    for (const auto& para : m_imgRaycasterRenderer.texFilterModeParas()) {
    //      m_widgetsGroup->addChild(*para, 15);
    //    }

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
    m_widgetsGroup->addChild(m_rendererBase.coordTransformPara(), 1);

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

bool Z3DImgFilter::hasOpaque(Z3DEye /*unused*/) const
{
  return hasSlices();
}

void Z3DImgFilter::renderOpaque(Z3DEye eye)
{
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_opaqueOutport
                                        : (eye == Z3DEye::Left) ? m_opaqueLeftEyeOutport
                                                                : m_opaqueRightEyeOutport;
  m_textureCopyRenderer.setColorTexture(currentOutport.colorTexture());
  m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  m_rendererBase.render(eye, m_textureCopyRenderer);
  // glFinish();
  // currentOutport.colorTexture()->saveAsColorImage("/Users/feng/Downloads/abc.tif");
}

bool Z3DImgFilter::hasTransparent(Z3DEye eye) const
{
  const Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_outport
                                              : (eye == Z3DEye::Left) ? m_leftEyeOutport
                                                                      : m_rightEyeOutport;
  return currentOutport.hasValidData();
}

void Z3DImgFilter::renderTransparent(Z3DEye eye)
{
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_outport
                                        : (eye == Z3DEye::Left) ? m_leftEyeOutport
                                                                : m_rightEyeOutport;
  m_textureCopyRenderer.setColorTexture(currentOutport.colorTexture());
  m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

glm::vec3 Z3DImgFilter::get3DPosition(int x, int y, int width, int height, bool& success)
{
  if (m_imgRaycasterRenderer.compositeMode() == "Direct Volume Rendering") {
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
  LOG(INFO) << "open subregion at image coord " << pos3D;
  auto minCoord = pos3D - 64.f;
  auto maxCoord = pos3D + 64.f;
  m_xCut.set(glm::vec2(minCoord.x, maxCoord.x));
  m_yCut.set(glm::vec2(minCoord.y, maxCoord.y));
  m_zCut.set(glm::vec2(minCoord.z, maxCoord.z));
  m_rendererBase.globalParas().cameraFocusesOn(axisAlignedBoundBoxAfterClipping());
  m_fullResolutionRendering.set(true);
}

void Z3DImgFilter::exitSubregionView()
{
  m_fullResolutionRendering.set(false);
  m_xCut.set(m_xCut.range());
  m_yCut.set(m_yCut.range());
  m_zCut.set(m_zCut.range());
  m_rendererBase.globalParas().cameraFocusesOn(axisAlignedBoundBox());
}

void Z3DImgFilter::invalidate(State inv)
{
  Z3DBoundedFilter::invalidate(inv);
  m_imgRaycasterRenderer.resetProgress();
  m_imgSliceRenderer.resetProgress();
}

void Z3DImgFilter::updateSize()
{
  Z3DBoundedFilter::updateSize();
  updateBlockIDTarget();
}

void Z3DImgFilter::changeCoordTransform()
{
  // invalidateAllFRVolumeSlices();
  if (m_3dImg) {
    m_3dImg->setScale(m_rendererBase.coordTransformPara().scale());
  }
  m_imgRaycasterRenderer.compile();
  m_imgSliceRenderer.compile();
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
    auto pos3D = get3DPosition(event->x() * m_rendererBase.globalParas().devicePixelRatio.get(),
                               event->y() * m_rendererBase.globalParas().devicePixelRatio.get(),
                               w * m_rendererBase.globalParas().devicePixelRatio.get(),
                               h * m_rendererBase.globalParas().devicePixelRatio.get(),
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
  if (m_channelRangeChanged) {
    if (m_3dImg) {
      std::vector<glm::dvec2> channelDisplayRanges;
      for (const auto& para : m_doubleChannelRangeParas) {
        channelDisplayRanges.push_back(para->get());
      }
      m_3dImg->setChannelDisplayRanges(channelDisplayRanges);
      m_imgRaycasterRenderer.updateDisplayRanges();
    }

    m_channelRangeChanged = false;
  }

  glEnable(GL_DEPTH_TEST);
  auto openGLStateGuard = folly::makeGuard([]() {
    glDisable(GL_DEPTH_TEST);
  });

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
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_opaqueOutport
                                        : (eye == Z3DEye::Left) ? m_opaqueLeftEyeOutport
                                                                : m_opaqueRightEyeOutport;

  if (!(m_progressiveRendering && m_imgSliceRenderer.renderingStarted(eye))) {
    currentOutport.resize(m_outport.size());
    m_layerTarget.resize(currentOutport.size());

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
        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
        m_imgSliceRenderer.addSlice(slice);
      }
    }
    if (m_showZSlice.get()) {
      float zTexCoord = (m_zSlicePosition.get() + .5f) / volDim.z;
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showYSlice.get()) {
      float yTexCoord = (m_ySlicePosition.get() + .5f) / volDim.y;
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showXSlice.get()) {
      float xTexCoord = (m_xSlicePosition.get() + .5f) / volDim.x;
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
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
        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
        m_imgSliceRenderer.addSlice(slice);
      }
    }
    if (m_showZSlice2.get()) {
      float zTexCoord = (m_zSlice2Position.get() + .5f) / volDim.z;
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showYSlice2.get()) {
      float yTexCoord = (m_ySlice2Position.get() + .5f) / volDim.y;
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgSliceRenderer.addSlice(slice);
    }
    if (m_showXSlice2.get()) {
      float xTexCoord = (m_xSlice2Position.get() + .5f) / volDim.x;
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgSliceRenderer.addSlice(slice);
    }
  }

  currentOutport.bindTarget();
  currentOutport.clearTarget();
  m_rendererBase.setViewport(currentOutport.size());

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgSliceRenderer);
  } else {
    progress = m_imgSliceRenderer.renderProgressively(eye);
  }

  currentOutport.releaseTarget();

  // glFinish();
  // currentOutport.colorTexture()->saveAsColorImage("/Users/feng/Downloads/abc.tif");

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
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_outport
                                        : (eye == Z3DEye::Left) ? m_leftEyeOutport
                                                                : m_rightEyeOutport;

  if (!(m_progressiveRendering && m_imgRaycasterRenderer.renderingStarted(eye))) {
    m_layerTarget.resize(currentOutport.size());
    m_blockIDsRenderTarget.resize(currentOutport.size());
    m_imageRenderTarget1s[to_underlying(eye)]->resize(currentOutport.size());
    m_imageRenderTarget2s[to_underlying(eye)]->resize(currentOutport.size());

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
      m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
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
      m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
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
      m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
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
      m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
      m_imgRaycasterRenderer.clearQuads();
      m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
    } else { // 3d volume Raycasting
      ZMesh cube = ZMesh::createCube(glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                     glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                     glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                     glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
      cube.transformVerticesByMatrix(m_rendererBase.coordTransform());
      bool flipped = glm::determinant(glm::mat3(m_rendererBase.coordTransform())) < 0.0;

      // enable culling
      glEnable(GL_CULL_FACE);

      m_rendererBase.setViewport(currentOutport.size());
      CHECK_GL_ERROR

      std::vector<glm::vec3> planeNormals;
      std::vector<glm::vec3> planeOrigins;
      planeNormals.push_back(globalCamera().viewVector());
      planeOrigins.push_back(globalCamera().eye() + globalCamera().viewVector() * (globalCamera().nearDist() + 0.01f));
      if (m_rendererBase.globalParas().globalXCut.lowerValue() != m_rendererBase.globalParas().globalXCut.minimum()) {
        planeNormals.emplace_back(1., 0., 0.);
        planeOrigins.emplace_back(m_rendererBase.globalParas().globalXCut.lowerValue(), 0, 0);
      }
      if (m_rendererBase.globalParas().globalXCut.upperValue() != m_rendererBase.globalParas().globalXCut.maximum()) {
        planeNormals.emplace_back(-1., 0., 0.);
        planeOrigins.emplace_back(m_rendererBase.globalParas().globalXCut.upperValue(), 0, 0);
      }
      if (m_rendererBase.globalParas().globalYCut.lowerValue() != m_rendererBase.globalParas().globalYCut.minimum()) {
        planeNormals.emplace_back(0., 1., 0.);
        planeOrigins.emplace_back(0, m_rendererBase.globalParas().globalYCut.lowerValue(), 0);
      }
      if (m_rendererBase.globalParas().globalYCut.upperValue() != m_rendererBase.globalParas().globalYCut.maximum()) {
        planeNormals.emplace_back(0., -1., 0.);
        planeOrigins.emplace_back(0, m_rendererBase.globalParas().globalYCut.upperValue(), 0);
      }
      if (m_rendererBase.globalParas().globalZCut.lowerValue() != m_rendererBase.globalParas().globalZCut.minimum()) {
        planeNormals.emplace_back(0., 0., 1.);
        planeOrigins.emplace_back(0, 0, m_rendererBase.globalParas().globalZCut.lowerValue());
      }
      if (m_rendererBase.globalParas().globalZCut.upperValue() != m_rendererBase.globalParas().globalZCut.maximum()) {
        planeNormals.emplace_back(0., 0., -1.);
        planeOrigins.emplace_back(0, 0, m_rendererBase.globalParas().globalZCut.upperValue());
      }
      // LOG(INFO) << planeNormals.size();
      ZMesh clipped = ZMesh::clipClosedSurface(cube, planeNormals, planeOrigins);
#if 0
    float nearPlaneDistToOrigin =
      glm::dot(globalCamera().eye(), -globalCamera().viewVector()) - globalCamera().nearDist() - 0.01f;
    std::vector<glm::vec4> planes;
    planes.emplace_back(-globalCamera().viewVector(), nearPlaneDistToOrigin);
    if (m_rendererBase.globalParas().xCut.lowerValue() != m_rendererBase.globalParas().xCut.minimum()) {
      planes.emplace_back(-1., 0., 0., -m_rendererBase.globalParas().xCut.lowerValue());
    }
    if (m_rendererBase.globalParas().xCut.upperValue() != m_rendererBase.globalParas().xCut.maximum()) {
      planes.emplace_back(1., 0., 0., m_rendererBase.globalParas().xCut.upperValue());
    }
    if (m_rendererBase.globalParas().globalYCut.lowerValue() != m_rendererBase.globalParas().globalYCut.minimum()) {
      planes.emplace_back(0., -1., 0., -m_rendererBase.globalParas().globalYCut.lowerValue());
    }
    if (m_rendererBase.globalParas().globalYCut.upperValue() != m_rendererBase.globalParas().globalYCut.maximum()) {
      planes.emplace_back(0., 1., 0., m_rendererBase.globalParas().globalYCut.upperValue());
    }
    if (m_rendererBase.globalParas().globalZCut.lowerValue() != m_rendererBase.globalParas().globalZCut.minimum()) {
      planes.emplace_back(0., 0., -1., -m_rendererBase.globalParas().globalZCut.lowerValue());
    }
    if (m_rendererBase.globalParas().globalZCut.upperValue() != m_rendererBase.globalParas().globalZCut.maximum()) {
      planes.emplace_back(0., 0., 1., m_rendererBase.globalParas().globalZCut.upperValue());
    }
    LOG(INFO) << planes.size();
    auto clipped = ZMeshUtils::clipClosedSurface(cube, planes);
#endif

      // render back texture
      const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};
      m_entryExitTarget.resize(currentOutport.size());

      m_entryExitTarget.attachSlice(1);
      m_entryExitTarget.bind();
      glDrawBuffers(1, g_drawBuffers);
      glClear(GL_COLOR_BUFFER_BIT);
      glCullFace(flipped ? GL_BACK : GL_FRONT);

      m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
      m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
      m_entryExitTarget.release();
      CHECK_GL_ERROR

      // render front texture
      m_entryExitTarget.attachSlice(0);
      m_entryExitTarget.bind();
      glDrawBuffers(1, g_drawBuffers);
      glClear(GL_COLOR_BUFFER_BIT);
      glCullFace(flipped ? GL_FRONT : GL_BACK);

      m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
      m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
      m_entryExitTarget.release();
      CHECK_GL_ERROR

      // restore OpenGL state
      glCullFace(GL_BACK);
      glDisable(GL_CULL_FACE);

      // m_entryExitTarget.attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage("/Users/feng/Downloads/test1rayeye.tif");

      m_imgRaycasterRenderer.setEntryExitInfo(m_entryExitTarget.attachment(GL_COLOR_ATTACHMENT0));
    }
  }

  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  currentOutport.bindTarget();
  currentOutport.clearTarget();
  m_rendererBase.setViewport(currentOutport.size());

  auto openGLStateGuard = folly::makeGuard([&currentOutport]() {
    currentOutport.releaseTarget();

    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
  });

  double progress = 1.0;
  if (!m_progressiveRendering) {
    m_rendererBase.render(eye, m_imgRaycasterRenderer);
  } else {
    progress = m_imgRaycasterRenderer.renderProgressively(eye);
  }

  renderBoundBox(eye);
  CHECK_GL_ERROR

  return progress;
}

bool Z3DImgFilter::onlyBoundBox() const
{
  return !hasImage() && !m_boundBoxMode.isSelected("No Bound Box");
}

void Z3DImgFilter::renderOnlyBoundBox(nim::Z3DEye eye)
{
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono)   ? m_outport
                                        : (eye == Z3DEye::Left) ? m_leftEyeOutport
                                                                : m_rightEyeOutport;

  currentOutport.bindTarget();
  currentOutport.clearTarget();
  m_rendererBase.setViewport(currentOutport.size());

  renderBoundBox(eye);
  CHECK_GL_ERROR

  currentOutport.releaseTarget();

  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);
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
  if (m_imgRaycasterRenderer.hasVisibleRendering() && (m_outport.hasValidData() || m_rightEyeOutport.hasValidData())) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderOutputPort& port = m_outport.hasValidData() ? m_outport : m_rightEyeOutport;

    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, port);
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
  if (m_imgRaycasterRenderer.hasVisibleRendering() && m_3dImg &&
      (m_outport.hasValidData() || m_rightEyeOutport.hasValidData())) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderOutputPort& port = m_outport.hasValidData() ? m_outport : m_rightEyeOutport;

    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, port);
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

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderOutputPort& port)
{
  glm::mat4 projection = globalCamera().projectionMatrix(Z3DEye::Mono);
  glm::mat4 modelview = globalCamera().viewMatrix(Z3DEye::Mono);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  GLfloat WindowPosZ;
  port.bindTarget();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(pos2D.x, pos2D.y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &WindowPosZ);
  port.releaseTarget();

  CHECK_GL_ERROR
  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, WindowPosZ), modelview, projection, viewport);

  return pos;
}

glm::vec3 Z3DImgFilter::get3DPosition(glm::ivec2 pos2D, double depth, int width, int height)
{
  glm::mat4 projection = globalCamera().projectionMatrix(Z3DEye::Mono);
  glm::mat4 modelview = globalCamera().viewMatrix(Z3DEye::Mono);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, depth), modelview, projection, viewport);

  return pos;
}

} // namespace nim
