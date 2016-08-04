#include "z3dimgfilter.h"

#include "z3dgpuinfo.h"
#include "zimg.h"
#include "zeventlistenerparameter.h"
#include "zmesh.h"
#include "zlog.h"
#include "zbenchtimer.h"
#include "zmeshutils.h"

namespace nim {

//const size_t Z3DImgFilter::m_maxNumOfFullResolutionVolumeSlice = 6;

Z3DImgFilter::Z3DImgFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_imgRaycasterRenderer(m_rendererBase)
  , m_imgSliceRenderer(m_rendererBase)
  , m_textureAndEyeCoordinateRenderer(m_rendererBase)
  , m_textureCopyRenderer(m_rendererBase)
  , m_visible("Visible", true)
  , m_stayOnTop("Stay On Top", false)
  , m_isVolumeDownsampled("Volume Is Downsampled", false)
  , m_numParas(0)
  //, m_interactionDownsample("Interaction Downsample", 1, 1, 16)
  , m_smoothInteraction("Smooth Interaction", true)
  , m_entryTarget(glm::uvec2(32, 32))
  , m_exitTarget(glm::uvec2(32, 32))
  , m_layerTarget(glm::uvec2(32, 32))
  , m_layerColorTexture(GL_TEXTURE_2D_ARRAY, GLint(GL_RGBA16), glm::uvec3(32, 32, 3), GL_RGBA, GL_FLOAT)
  , m_layerDepthTexture(GL_TEXTURE_2D_ARRAY, GLint(GL_DEPTH_COMPONENT24), glm::uvec3(32, 32, 3), GL_DEPTH_COMPONENT,
                        GL_FLOAT)
  , m_missBlocksTexture1(GL_TEXTURE_2D, GLint(GL_RGBA32UI), glm::uvec3(32, 32, 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT)
  , m_missBlocksTexture2(GL_TEXTURE_2D, GLint(GL_RGBA32UI), glm::uvec3(32, 32, 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT)
  , m_usedBlocksTexture1(GL_TEXTURE_2D, GLint(GL_RGBA32UI), glm::uvec3(32, 32, 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT)
  , m_usedBlocksTexture2(GL_TEXTURE_2D, GLint(GL_RGBA32UI), glm::uvec3(32, 32, 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT)
  , m_usedBlocksTexture3(GL_TEXTURE_2D, GLint(GL_RGBA32UI), glm::uvec3(32, 32, 1), GL_RGBA_INTEGER, GL_UNSIGNED_INT)
  , m_blockIDsRenderTarget(glm::uvec2(32, 32))
  , m_outport("Image", true, InvalidMonoViewResult)
  , m_leftEyeOutport("LeftEyeImage", true, InvalidLeftEyeResult)
  , m_rightEyeOutport("RightEyeImage", true, InvalidRightEyeResult)
  , m_vPPort("VolumeFilter")
  , m_opaqueOutport("OpaqueImage", true, InvalidMonoViewResult)
  , m_opaqueLeftEyeOutport("OpaqueLeftEyeImage", true, InvalidLeftEyeResult)
  , m_opaqueRightEyeOutport("OpaqueRightEyeImage", true, InvalidRightEyeResult)
  //, m_FRVolumeSlices(m_maxNumOfFullResolutionVolumeSlice)
  //, m_FRVolumeSlicesValidState(m_maxNumOfFullResolutionVolumeSlice, false)
  //, m_useFRVolumeSlice("Use Full Resolution Volume Slice", true)
  , m_showXSlice("Show X Slice", false)
  , m_xSlicePosition("X Slice Position", 0, 0, 1)
  , m_showYSlice("Show Y Slice", false)
  , m_ySlicePosition("Y Slice Position", 0, 0, 1)
  , m_showZSlice("Show Z Slice", false)
  , m_zSlicePosition("Z Slice Position", 0, 0, 1)
  , m_showXSlice2("Show X Slice 2", false)
  , m_xSlice2Position("X Slice 2 Position", 0, 0, 1)
  , m_showYSlice2("Show Y Slice 2", false)
  , m_ySlice2Position("Y Slice 2 Position", 0, 0, 1)
  , m_showZSlice2("Show Z Slice 2", false)
  , m_zSlice2Position("Z Slice 2 Position", 0, 0, 1)
  , m_leftMouseButtonPressEvent("Left Mouse Button Pressed", false)
{
  CHECK_GL_ERROR
  m_baseBoundBoxRenderer.setEnableMultisample(false);
  m_textureCopyRenderer.setDiscardTransparent(true);

  addParameter(m_visible);
  addParameter(m_stayOnTop);
  m_isVolumeDownsampled.setEnabled(false);
  addParameter(m_isVolumeDownsampled);
  connect(&m_rendererBase, &Z3DRendererBase::coordTransformChanged, this, &Z3DImgFilter::changeCoordTransform);
  connect(&m_rendererBase.globalParas().interactionHandler, &Z3DTrackballInteractionHandler::mousePressed,
          this, &Z3DImgFilter::mousePressed);
  connect(&m_rendererBase.globalParas().interactionHandler, &Z3DTrackballInteractionHandler::mouseReleased,
          this, &Z3DImgFilter::mouseReleased);

  //addParameter(m_interactionDownsample);
  addParameter(m_smoothInteraction);

  Z3DTexture* g_TexId[2];
  g_TexId[0] = new Z3DTexture(GLint(GL_RGBA32F), glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[0]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  g_TexId[0]->uploadImage();
  g_TexId[1] = new Z3DTexture(GLint(GL_RGBA32F), glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[1]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  g_TexId[1]->uploadImage();
  m_entryTarget.attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0);
  m_entryTarget.attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1);
  m_entryTarget.isFBOComplete();
  g_TexId[0] = new Z3DTexture(GLint(GL_RGBA32F), glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[0]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  g_TexId[0]->uploadImage();
  g_TexId[1] = new Z3DTexture(GLint(GL_RGBA32F), glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[1]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  g_TexId[1]->uploadImage();
  m_exitTarget.attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0);
  m_exitTarget.attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1);
  m_exitTarget.isFBOComplete();
  m_layerColorTexture.uploadImage();
  m_layerDepthTexture.uploadImage();
  m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
  m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
  m_layerTarget.isFBOComplete();

  m_missBlocksTexture1.setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_missBlocksTexture2.setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_usedBlocksTexture1.setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_usedBlocksTexture2.setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_usedBlocksTexture3.setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_missBlocksTexture1.uploadImage();
  m_missBlocksTexture2.uploadImage();
  m_usedBlocksTexture1.uploadImage();
  m_usedBlocksTexture2.uploadImage();
  m_usedBlocksTexture3.uploadImage();
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture1, GL_COLOR_ATTACHMENT0, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_missBlocksTexture2, GL_COLOR_ATTACHMENT1, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_usedBlocksTexture1, GL_COLOR_ATTACHMENT2, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_usedBlocksTexture2, GL_COLOR_ATTACHMENT3, false);
  m_blockIDsRenderTarget.attachTextureToFBO(&m_usedBlocksTexture3, GL_COLOR_ATTACHMENT4, false);
  m_blockIDsRenderTarget.isFBOComplete();

  // ports
  addPrivateRenderTarget(m_entryTarget);
  addPrivateRenderTarget(m_exitTarget);
  addPrivateRenderTarget(m_layerTarget);
  addPort(m_outport);
  addPort(m_leftEyeOutport);
  addPort(m_rightEyeOutport);
  addPort(m_vPPort);
  addPrivateRenderPort(m_opaqueOutport);
  addPrivateRenderPort(m_opaqueLeftEyeOutport);
  addPrivateRenderPort(m_opaqueRightEyeOutport);

  //addParameter(m_useFRVolumeSlice);
  addParameter(m_showXSlice);
  addParameter(m_xSlicePosition);
  addParameter(m_showYSlice);
  addParameter(m_ySlicePosition);
  addParameter(m_showZSlice);
  addParameter(m_zSlicePosition);
  addParameter(m_showXSlice2);
  addParameter(m_xSlice2Position);
  addParameter(m_showYSlice2);
  addParameter(m_ySlice2Position);
  addParameter(m_showZSlice2);
  addParameter(m_zSlice2Position);

  connect(&m_showXSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showXSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showYSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);
  connect(&m_showZSlice2, &ZBoolParameter::valueChanged, this, &Z3DImgFilter::adjustWidget);

  //connect(&m_xSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice);
  //connect(&m_ySlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeYSlice);
  //connect(&m_zSlicePosition, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeZSlice);
  //connect(&m_xSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  //connect(&m_ySlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);
  //connect(&m_zSlice2Position, &ZIntParameter::valueChanged, this, &Z3DImgFilter::invalidateFRVolumeXSlice2);

  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  connect(&m_leftMouseButtonPressEvent, &ZEventListenerParameter::mouseEventTriggered,
          this, &Z3DImgFilter::leftMouseButtonPressed);
  addEventListener(m_leftMouseButtonPressEvent);

  m_imgRaycasterRenderer.setLayerTarget(m_layerTarget);
  m_imgSliceRenderer.setLayerTarget(m_layerTarget);
  m_imgRaycasterRenderer.setBlockIDsRenderTarget(m_blockIDsRenderTarget);
  m_imgSliceRenderer.setBlockIDsRenderTarget(m_blockIDsRenderTarget);
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

  adjustWidget();
  CHECK_GL_ERROR

  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DImgFilter::objVisibleChanged);

  m_numParas = m_parameters.size();
}

Z3DImgFilter::~Z3DImgFilter()
{
}

void Z3DImgFilter::setOffset(double x, double y, double z)
{
  m_rendererBase.translate(x, y, z);
}

void Z3DImgFilter::setData(const ZImgPack& imgPack)
{
  if (m_widgetsGroup) {
    for (auto it = m_imgRaycasterRenderer.channelVisibleParas().begin();
         it != m_imgRaycasterRenderer.channelVisibleParas().end(); ++it) {
      m_widgetsGroup->removeChild(*it->get());
    }
    for (auto it = m_imgRaycasterRenderer.transferFuncParas().begin();
         it != m_imgRaycasterRenderer.transferFuncParas().end(); ++it) {
      m_widgetsGroup->removeChild(*it->get());
    }
    for (auto it = m_imgRaycasterRenderer.texFilterModeParas().begin();
         it != m_imgRaycasterRenderer.texFilterModeParas().end(); ++it) {
      m_widgetsGroup->removeChild(*it->get());
    }
    for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
      m_widgetsGroup->removeChild(*it->get());
    }
  }
  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }

  m_3dImg.reset(new Z3DImg(imgPack, m_rendererBase.coordTransformPara().scale()));

  updateBlockIDTarget();

  if (m_3dImg->numChannels() > m_layerColorTexture.depth()) {
    m_layerColorTexture.setDimension(
      glm::uvec3(m_layerColorTexture.width(), m_layerColorTexture.height(), m_3dImg->numChannels()));
    m_layerColorTexture.uploadImage();
    m_layerDepthTexture.setDimension(
      glm::uvec3(m_layerDepthTexture.width(), m_layerDepthTexture.height(), m_3dImg->numChannels()));
    m_layerDepthTexture.uploadImage();
    m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
    m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
    m_layerTarget.isFBOComplete();
  }
  m_isVolumeDownsampled.set(m_3dImg->isVolumeDownsampled());

  m_sliceColormaps.clear();
  for (size_t c = 0; c < m_3dImg->numChannels(); ++c) {
    m_sliceColormaps.emplace_back(
      std::make_unique<ZColorMapParameter>(QString("Slice Channel %1 Colormap").arg(c + 1)));
    m_sliceColormaps[c]->get().create1DTexture(256);
    m_sliceColormaps[c]->get().reset(0.0, 1.0, QColor(0, 0, 0), QColor(m_3dImg->channelColor(c).r,
                                                                       m_3dImg->channelColor(c).g,
                                                                       m_3dImg->channelColor(c).b));
  }

  bool is2DImage = m_3dImg->is2DData();
  glm::uvec3 volDim = m_3dImg->dimensions();
  m_xCut.setRange(-1, volDim.x);
  m_xCut.set(m_xCut.range());
  m_yCut.setRange(-1, volDim.y);
  m_yCut.set(m_yCut.range());
  m_zCut.setRange(-1, volDim.z);
  m_zCut.set(m_zCut.range());

  m_rendererBase.setRotationCenter(glm::vec3(volDim.x - 1, volDim.y - 1, volDim.z - 1) / 2.f);

  m_zSlicePosition.setRange(0, volDim.z - 1);
  m_ySlicePosition.setRange(0, volDim.y - 1);
  m_xSlicePosition.setRange(0, volDim.x - 1);
  m_zSlice2Position.setRange(0, volDim.z - 1);
  m_ySlice2Position.setRange(0, volDim.y - 1);
  m_xSlice2Position.setRange(0, volDim.x - 1);
  //invalidateAllFRVolumeSlices();
  //m_useFRVolumeSlice.set(!is2DImage);
  //m_useFRVolumeSlice.setVisible(!is2DImage);
  m_showXSlice.set(false);
  m_showYSlice.set(false);
  m_showZSlice.set(false);
  m_showXSlice2.set(false);
  m_showYSlice2.set(false);
  m_showZSlice2.set(false);
  m_showXSlice.setVisible(!is2DImage);
  m_showYSlice.setVisible(!is2DImage);
  m_showZSlice.setVisible(!is2DImage);
  m_showXSlice2.setVisible(!is2DImage);
  m_showYSlice2.setVisible(!is2DImage);
  m_showZSlice2.setVisible(!is2DImage);

  m_imgRaycasterRenderer.setData(*m_3dImg.get());
  if (!is2DImage) {
    m_imgSliceRenderer.setData(*m_3dImg.get(), m_sliceColormaps);
  }

  updateBoundBox();

  for (auto it = m_imgRaycasterRenderer.channelVisibleParas().begin();
       it != m_imgRaycasterRenderer.channelVisibleParas().end(); ++it) {
    addParameter(*it->get());
  }
  for (auto it = m_imgRaycasterRenderer.transferFuncParas().begin();
       it != m_imgRaycasterRenderer.transferFuncParas().end(); ++it) {
    addParameter(*it->get());
  }
  for (auto it = m_imgRaycasterRenderer.texFilterModeParas().begin();
       it != m_imgRaycasterRenderer.texFilterModeParas().end(); ++it) {
    addParameter(*it->get());
  }
  for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
    addParameter(*it->get());
  }

  if (m_widgetsGroup) {
    for (auto it = m_imgRaycasterRenderer.channelVisibleParas().begin();
         it != m_imgRaycasterRenderer.channelVisibleParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 2);
    }
    for (auto it = m_imgRaycasterRenderer.transferFuncParas().begin();
         it != m_imgRaycasterRenderer.transferFuncParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 3);
    }
    for (auto it = m_imgRaycasterRenderer.texFilterModeParas().begin();
         it != m_imgRaycasterRenderer.texFilterModeParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 15);
    }
    for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 11);
    }
    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }

  invalidateResult();
}

std::shared_ptr<ZWidgetsGroup> Z3DImgFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Img", 1);

    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_isVolumeDownsampled, 2);

    for (auto it = m_imgRaycasterRenderer.channelVisibleParas().begin();
         it != m_imgRaycasterRenderer.channelVisibleParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 2);
    }
    for (auto it = m_imgRaycasterRenderer.transferFuncParas().begin();
         it != m_imgRaycasterRenderer.transferFuncParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 3);
    }
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.compositingModePara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.isoValuePara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.localMIPThresholdPara(), 4);
    m_widgetsGroup->addChild(m_imgRaycasterRenderer.samplingRatePara(), 15);
    for (auto it = m_imgRaycasterRenderer.texFilterModeParas().begin();
         it != m_imgRaycasterRenderer.texFilterModeParas().end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 15);
    }

    m_widgetsGroup->addChild(m_xCut, 12);
    m_widgetsGroup->addChild(m_yCut, 12);
    m_widgetsGroup->addChild(m_zCut, 12);
    m_widgetsGroup->addChild(m_boundBoxMode, 13);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 13);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 13);
    m_widgetsGroup->addChild(m_selectionLineWidth, 17);
    m_widgetsGroup->addChild(m_selectionLineColor, 17);
    m_widgetsGroup->addChild(m_manipulatorSize, 17);
    //m_widgetsGroup->addChild(m_interactionDownsample, 19);
    m_widgetsGroup->addChild(m_smoothInteraction, 19);
    m_widgetsGroup->addChild(m_rendererBase.coordTransformPara(), 1);

    const std::vector<ZParameter*>& paras = parameters();
    for (size_t i = 0; i < paras.size(); i++) {
      ZParameter* para = paras[i];
      if (para->name().contains("Slice") && !para->name().endsWith("2") && !para->name().endsWith("2 Position"))
        m_widgetsGroup->addChild(*para, 11);
      else if (para->name().contains("Slice"))
        m_widgetsGroup->addChild(*para, 19);
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
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono) ?
                                        m_opaqueOutport : (eye == Z3DEye::Left) ? m_opaqueLeftEyeOutport
                                                                                : m_opaqueRightEyeOutport;
  m_textureCopyRenderer.setColorTexture(currentOutport.colorTexture());
  m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  m_rendererBase.render(eye, m_textureCopyRenderer);
  //glFinish();
  //currentOutport.colorTexture()->saveAsColorImage("/Users/feng/Downloads/abc.tif");
}

bool Z3DImgFilter::hasTransparent(Z3DEye eye) const
{
  const Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono) ?
                                              m_outport : (eye == Z3DEye::Left) ? m_leftEyeOutport : m_rightEyeOutport;
  return currentOutport.hasValidData();
}

void Z3DImgFilter::renderTransparent(Z3DEye eye)
{
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono) ?
                                        m_outport : (eye == Z3DEye::Left) ? m_leftEyeOutport : m_rightEyeOutport;
  m_textureCopyRenderer.setColorTexture(currentOutport.colorTexture());
  m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

void Z3DImgFilter::updateSize()
{
  Z3DBoundedFilter::updateSize();
  updateBlockIDTarget();
}

void Z3DImgFilter::changeCoordTransform()
{
  //invalidateAllFRVolumeSlices();
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
}

void Z3DImgFilter::leftMouseButtonPressed(QMouseEvent* e, int w, int h)
{
  Q_UNUSED(e)
  Q_UNUSED(w)
  Q_UNUSED(h)
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

//void Z3DImgFilter::invalidateFRVolumeZSlice()
//{
//  m_FRVolumeSlicesValidState[0] = false;
//}

//void Z3DImgFilter::invalidateFRVolumeYSlice()
//{
//  m_FRVolumeSlicesValidState[1] = false;
//}

//void Z3DImgFilter::invalidateFRVolumeXSlice()
//{
//  m_FRVolumeSlicesValidState[2] = false;
//}

//void Z3DImgFilter::invalidateFRVolumeZSlice2()
//{
//  m_FRVolumeSlicesValidState[3] = false;
//}

//void Z3DImgFilter::invalidateFRVolumeYSlice2()
//{
//  m_FRVolumeSlicesValidState[4] = false;
//}

//void Z3DImgFilter::invalidateFRVolumeXSlice2()
//{
//  m_FRVolumeSlicesValidState[5] = false;
//}

void Z3DImgFilter::mousePressed()
{
  if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled()) {
    m_imgRaycasterRenderer.setFastRendering(true);
    m_imgSliceRenderer.setFastRendering(true);
  }
}

void Z3DImgFilter::mouseReleased()
{
  if (m_smoothInteraction.get() && m_3dImg && m_3dImg->isVolumeDownsampled()) {
    m_imgRaycasterRenderer.setFastRendering(false);
    m_imgSliceRenderer.setFastRendering(false);
    // upstream will invalidate the network, but in case there are no upstream
    // do one more invalidation
    invalidateResult();
  }
}

void Z3DImgFilter::process(Z3DEye eye)
{
  glEnable(GL_DEPTH_TEST);

  if (hasImage()) {
    renderImage(eye);
  }

  if (hasSlices()) {
    renderSlices(eye);
  }

  glDisable(GL_DEPTH_TEST);

  CHECK_GL_ERROR
}

bool Z3DImgFilter::hasSlices() const
{
  return m_showZSlice.get() || m_showXSlice.get() || m_showYSlice.get()
         || m_showXSlice2.get() || m_showYSlice2.get() || m_showZSlice2.get();
}

void Z3DImgFilter::renderSlices(Z3DEye eye)
{
  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono) ?
                                        m_opaqueOutport : (eye == Z3DEye::Left) ? m_opaqueLeftEyeOutport
                                                                                : m_opaqueRightEyeOutport;

  currentOutport.bindTarget();
  currentOutport.clearTarget();
  m_rendererBase.setViewport(currentOutport.size());

  glm::uvec3 volDim = m_3dImg->dimensions();
  glm::vec3 coordLuf = m_3dImg->physicalLUF();
  glm::vec3 coordRdb = m_3dImg->physicalRDB();

  //  if (m_useFRVolumeSlice.get() && m_3dImg->isVolumeDownsampled()) {
  //    std::vector<Z3DPrimitiveRenderer*> renderers;

  //    size_t sliceRendererIdx = 0;
  //    if (m_showZSlice.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeZSliceVolume(m_zSlicePosition.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float zTexCoord = m_zSlicePosition.get() / static_cast<float>(volDim.z-1);
  //        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(zCoord, 2, coordLuf.xy(), coordRdb.xy());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    sliceRendererIdx = 1;
  //    if (m_showYSlice.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeYSliceVolume(m_ySlicePosition.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float yTexCoord = m_ySlicePosition.get() / static_cast<float>(volDim.y-1);
  //        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(yCoord, 1, coordLuf.xz(), coordRdb.xz());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    sliceRendererIdx = 2;
  //    if (m_showXSlice.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeXSliceVolume(m_xSlicePosition.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float xTexCoord = m_xSlicePosition.get() / static_cast<float>(volDim.x-1);
  //        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(xCoord, 0, coordLuf.yz(), coordRdb.yz());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    sliceRendererIdx = 3;
  //    if (m_showZSlice2.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeZSliceVolume(m_zSlice2Position.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float zTexCoord = m_zSlice2Position.get() / static_cast<float>(volDim.z-1);
  //        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(zCoord, 2, coordLuf.xy(), coordRdb.xy());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    sliceRendererIdx = 4;
  //    if (m_showYSlice2.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeYSliceVolume(m_ySlice2Position.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float yTexCoord = m_ySlice2Position.get() / static_cast<float>(volDim.y-1);
  //        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(yCoord, 1, coordLuf.xz(), coordRdb.xz());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    sliceRendererIdx = 5;
  //    if (m_showXSlice2.get()) {
  //      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
  //        m_image2DRenderers[sliceRendererIdx]->clearQuads();

  //        m_FRVolumeSlices[sliceRendererIdx] = m_3dImg->makeXSliceVolume(m_xSlice2Position.get());
  //        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

  //        float xTexCoord = m_xSlice2Position.get() / static_cast<float>(volDim.x-1);
  //        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);
  //        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(xCoord, 0, coordLuf.yz(), coordRdb.yz());
  //        slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
  //        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
  //        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
  //      }
  //      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
  //    }
  //    m_rendererBase.render(eye, renderers);

  //  } else {

  m_imgSliceRenderer.clearQuads();

  if (m_showZSlice.get()) {
    float zTexCoord = m_zSlicePosition.get() / static_cast<float>(volDim.z - 1);
    float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }
  if (m_showYSlice.get()) {
    float yTexCoord = m_ySlicePosition.get() / static_cast<float>(volDim.y - 1);
    float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }
  if (m_showXSlice.get()) {
    float xTexCoord = m_xSlicePosition.get() / static_cast<float>(volDim.x - 1);
    float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }

  if (m_showZSlice2.get()) {
    float zTexCoord = m_zSlice2Position.get() / static_cast<float>(volDim.z - 1);
    float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }
  if (m_showYSlice2.get()) {
    float yTexCoord = m_ySlice2Position.get() / static_cast<float>(volDim.y - 1);
    float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }
  if (m_showXSlice2.get()) {
    float xTexCoord = m_xSlice2Position.get() / static_cast<float>(volDim.x - 1);
    float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

    ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
    slice.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgSliceRenderer.addQuad(slice);
  }
  m_rendererBase.render(eye, m_imgSliceRenderer);
  //}

  currentOutport.releaseTarget();

  //glFinish();
  //currentOutport.colorTexture()->saveAsColorImage("/Users/feng/Downloads/abc.tif");
}

bool Z3DImgFilter::hasImage() const
{
  return m_imgRaycasterRenderer.hasVisibleRendering() &&
         m_xCut.upperValue() > m_xCut.minimum() &&
         m_yCut.upperValue() > m_yCut.minimum() &&
         m_zCut.upperValue() > m_zCut.minimum() &&
         m_xCut.lowerValue() < m_xCut.maximum() &&
         m_yCut.lowerValue() < m_yCut.maximum() &&
         m_zCut.lowerValue() < m_zCut.maximum();
}

void Z3DImgFilter::renderImage(Z3DEye eye)
{
  glm::vec3 coordLuf = m_3dImg->physicalLUF();
  glm::vec3 coordRdb = m_3dImg->physicalRDB();

  float xTexCoordStart = std::max(m_xCut.lowerValue(), m_xCut.minimum() + 1) / (m_xCut.maximum() - 1);
  float xTexCoordEnd = std::min(m_xCut.upperValue(), m_xCut.maximum() - 1) / (m_xCut.maximum() - 1);
  float xCoordStart = glm::mix(coordLuf.x, coordRdb.x, xTexCoordStart);
  float xCoordEnd = glm::mix(coordLuf.x, coordRdb.x, xTexCoordEnd);
  float yTexCoordStart = std::max(m_yCut.lowerValue(), m_yCut.minimum() + 1) / (m_yCut.maximum() - 1);
  float yTexCoordEnd = std::min(m_yCut.upperValue(), m_yCut.maximum() - 1) / (m_yCut.maximum() - 1);
  float yCoordStart = glm::mix(coordLuf.y, coordRdb.y, yTexCoordStart);
  float yCoordEnd = glm::mix(coordLuf.y, coordRdb.y, yTexCoordEnd);
  float zTexCoordStart = std::max(m_zCut.lowerValue(), m_zCut.minimum() + 1) / (m_zCut.maximum() - 1);
  float zTexCoordEnd = std::min(m_zCut.upperValue(), m_zCut.maximum() - 1) / (m_zCut.maximum() - 1);
  float zCoordStart = glm::mix(coordLuf.z, coordRdb.z, zTexCoordStart);
  float zCoordEnd = glm::mix(coordLuf.z, coordRdb.z, zTexCoordEnd);

  if (m_3dImg->is2DData()) { // for 2d image
    ZMesh m_2DImageQuad = ZMesh::createImageSlice(0, glm::vec2(xCoordStart, yCoordStart),
                                                  glm::vec2(xCoordEnd, yCoordEnd),
                                                  glm::vec2(xTexCoordStart, yTexCoordStart),
                                                  glm::vec2(xTexCoordEnd, yTexCoordEnd));
    m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
  } else if (m_zCut.lowerValue() == m_zCut.upperValue()) { // slice of 3d image
    ZMesh m_2DImageQuad = ZMesh::createCubeSlice(zCoordStart, zTexCoordStart, 2, glm::vec2(xCoordStart, yCoordStart),
                                                 glm::vec2(xCoordEnd, yCoordEnd),
                                                 glm::vec2(xTexCoordStart, yTexCoordStart),
                                                 glm::vec2(xTexCoordEnd, yTexCoordEnd));
    m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
  } else if (m_yCut.lowerValue() == m_yCut.upperValue()) { // slice of 3d image
    ZMesh m_2DImageQuad = ZMesh::createCubeSlice(yCoordStart, yTexCoordStart, 1, glm::vec2(xCoordStart, zCoordStart),
                                                 glm::vec2(xCoordEnd, zCoordEnd),
                                                 glm::vec2(xTexCoordStart, zTexCoordStart),
                                                 glm::vec2(xTexCoordEnd, zTexCoordEnd));
    m_2DImageQuad.transformVerticesByMatrix(m_rendererBase.coordTransform());
    m_imgRaycasterRenderer.clearQuads();
    m_imgRaycasterRenderer.addQuad(m_2DImageQuad);
  } else if (m_xCut.lowerValue() == m_xCut.upperValue()) { // slice of 3d image
    ZMesh m_2DImageQuad = ZMesh::createCubeSlice(xCoordStart, xTexCoordStart, 0, glm::vec2(yCoordStart, zCoordStart),
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

    // enable culling
    glEnable(GL_CULL_FACE);

    m_rendererBase.setViewport(m_exitTarget.size());
    CHECK_GL_ERROR

    // render back texture
    const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                    GL_COLOR_ATTACHMENT1
    };
    m_exitTarget.bind();
    glDrawBuffers(2, g_drawBuffers);
    glClear(GL_COLOR_BUFFER_BIT);
    glCullFace(GL_FRONT);

    m_textureAndEyeCoordinateRenderer.setTriangleList(&cube);
    m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
    m_exitTarget.release();
    CHECK_GL_ERROR

    // render front texture
    m_entryTarget.bind();
    glDrawBuffers(2, g_drawBuffers);
    glClear(GL_COLOR_BUFFER_BIT);
    glCullFace(GL_BACK);

    float nearPlaneDistToOrigin =
      glm::dot(globalCamera().eye(), -globalCamera().viewVector()) - globalCamera().nearDist() - 0.01f;
    std::vector<glm::vec4> planes;
    planes.emplace_back(-globalCamera().viewVector(), nearPlaneDistToOrigin);
    ZMesh clipped = ZMeshUtils::clipClosedSurface(cube, planes);
    m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
    m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
    m_entryTarget.release();
    CHECK_GL_ERROR

    // restore OpenGL state
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);

    m_imgRaycasterRenderer.setEntryExitInfo(m_entryTarget.attachment(GL_COLOR_ATTACHMENT0),
                                            m_entryTarget.attachment(GL_COLOR_ATTACHMENT1),
                                            m_exitTarget.attachment(GL_COLOR_ATTACHMENT0),
                                            m_exitTarget.attachment(GL_COLOR_ATTACHMENT1));
  }


  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  Z3DRenderOutputPort& currentOutport = (eye == Z3DEye::Mono) ?
                                        m_outport : (eye == Z3DEye::Left) ? m_leftEyeOutport : m_rightEyeOutport;

  currentOutport.bindTarget();
  currentOutport.clearTarget();
  m_rendererBase.setViewport(currentOutport.size());

  m_rendererBase.render(eye, m_imgRaycasterRenderer);

  renderBoundBox(eye);
  CHECK_GL_ERROR

  currentOutport.releaseTarget();

  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);
}

void Z3DImgFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox[0] = 0;
  m_notTransformedBoundBox[1] = 0;
  m_notTransformedBoundBox[2] = 0;
  m_notTransformedBoundBox[1] = m_3dImg->dimensions().x;
  m_notTransformedBoundBox[3] = m_3dImg->dimensions().y;
  m_notTransformedBoundBox[5] = m_3dImg->dimensions().z;
}

void Z3DImgFilter::updateBlockIDTarget()
{
  if (m_3dImg && m_3dImg->isVolumeDownsampled()) {
    glm::uvec2 size = m_layerTarget.size();
    uint32_t sizeScale =
      std::min(std::min(Z3DImg::imageBlockSize().x, Z3DImg::imageBlockSize().y), Z3DImg::imageBlockSize().z) / 6;
    size.x = (size.x + sizeScale - 1) / sizeScale;
    size.y = (size.y + sizeScale - 1) / sizeScale;
    m_blockIDsRenderTarget.resize(size);
  }
}

//void Z3DImgFilter::invalidateAllFRVolumeSlices()
//{
//  m_FRVolumeSlicesValidState.clear();
//  m_FRVolumeSlicesValidState.resize(m_maxNumOfFullResolutionVolumeSlice, false);
//}

void Z3DImgFilter::volumeChanged()
{

}

} // namespace nim


