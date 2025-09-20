#include "z3dvolumefilter.h"

#include "z3dgpuinfo.h"
#include "zimg.h"
#include "zeventlistenerparameter.h"
#include "zmesh.h"
#include "zbenchtimer.h"
#include "zmeshutils.h"
#include "zlog.h"
#include <folly/ScopeGuard.h>
#include <QApplication>
#include <QMessageBox>

namespace nim {

const size_t Z3DVolumeFilter::m_maxNumOfFullResolutionVolumeSlice = 6;

Z3DVolumeFilter::Z3DVolumeFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_volumeRaycasterRenderer(m_rendererBase)
  , m_volumeSliceRenderer(m_rendererBase)
  , m_textureAndEyeCoordinateRenderer(m_rendererBase)
  , m_textureCopyRenderer(m_rendererBase)
  , m_raycasterCompositingMode("Compositing")
  , m_raycasterSamplingRate("Sampling Rate", 2.f, 0.01f, 20.f)
  , m_raycasterIsoValue("ISO Value", 0.5f, 0.0f, 1.0f)
  , m_raycasterLocalMIPThreshold("Local MIP Threshold", 0.8f, 0.01f, 1.f)
  , m_imgPack(nullptr)
  , m_stayOnTop("Stay On Top", false)
  , m_isVolumeDownsampled("Volume Is Downsampled", false)
  , m_isSubVolume("Is Subvolume", false)
  , m_zoomInViewSize("Zoom In View Size", 256, 128, 1024)
  , m_numParas(0)
  , m_interactionDownsample("Interaction Downsample", 1, 1, 16)
  , m_entryTarget(glm::uvec2(32, 32))
  , m_exitTarget(glm::uvec2(32, 32))
  , m_layerTarget(glm::uvec2(32, 32))
  , m_layerColorTexture(GL_TEXTURE_2D_ARRAY, (GLint)GL_RGBA16, glm::uvec3(32, 32, 3), GL_RGBA, GL_FLOAT)
  , m_layerDepthTexture(GL_TEXTURE_2D_ARRAY,
                        (GLint)GL_DEPTH_COMPONENT24,
                        glm::uvec3(32, 32, 3),
                        GL_DEPTH_COMPONENT,
                        GL_FLOAT)
  , m_vPPort("VolumeFilter", this)
  , m_FRVolumeSlices(m_maxNumOfFullResolutionVolumeSlice)
  , m_FRVolumeSlicesValidState(m_maxNumOfFullResolutionVolumeSlice, false)
  , m_useFRVolumeSlice("Use Full Resolution Volume Slice", true)
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
  , m_2DImageQuad(ZMesh::Type::TRIANGLE_STRIP)
  , m_nChannels(0)
{
  m_baseBoundBoxRenderer.setEnableMultisample(false);
  m_textureCopyRenderer.setDiscardTransparent(true);

  updateRaycasterSamplingRate();
  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();

  connect(&m_raycasterSamplingRate,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DVolumeFilter::updateRaycasterSamplingRate);
  connect(&m_raycasterIsoValue, &ZFloatParameter::valueChanged, this, &Z3DVolumeFilter::updateRaycasterIsoValue);
  connect(&m_raycasterLocalMIPThreshold,
          &ZFloatParameter::valueChanged,
          this,
          &Z3DVolumeFilter::updateRaycasterLocalMIPThreshold);

  m_raycasterCompositingMode.clearOptions();
  m_raycasterCompositingMode.addOptionsWithData(
    std::make_pair(QStringLiteral("Direct Volume Rendering"),
                   static_cast<int>(VolumeCompositingMode::DirectVolumeRendering)),
    std::make_pair(QStringLiteral("Maximum Intensity Projection"),
                   static_cast<int>(VolumeCompositingMode::MaximumIntensityProjection)),
    std::make_pair(QStringLiteral("MIP Opaque"), static_cast<int>(VolumeCompositingMode::MIPOpaque)),
    std::make_pair(QStringLiteral("Local MIP"), static_cast<int>(VolumeCompositingMode::LocalMIP)),
    std::make_pair(QStringLiteral("Local MIP Opaque"), static_cast<int>(VolumeCompositingMode::LocalMIPOpaque)),
    std::make_pair(QStringLiteral("ISO Surface"), static_cast<int>(VolumeCompositingMode::IsoSurface)),
    std::make_pair(QStringLiteral("X Ray"), static_cast<int>(VolumeCompositingMode::XRay)));
  m_raycasterCompositingMode.select(QStringLiteral("MIP Opaque"));

  updateRaycasterCompositingMode();
  connect(&m_raycasterCompositingMode,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DVolumeFilter::updateRaycasterCompositingMode);

  // directX 10 resource limit
  // 128 MB
  // directX 11 resource limit
  // min(max(128, 0.25f * (amount of dedicated VRAM)), 2048) MB
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM (128)
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_B_TERM (0.25f)
  // D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM (2048)
  size_t currentAvailableTexMem = Z3DGpuInfo::instance().dedicatedVideoMemoryMB();
  m_maxVoxelNumber =
    std::min(std::max(size_t(128), static_cast<size_t>(0.25 * currentAvailableTexMem)), size_t(2048)) * 1024 * 1024;

  addParameter(m_stayOnTop);
  m_isVolumeDownsampled.setEnabled(false);
  addParameter(m_isVolumeDownsampled);
  m_isSubVolume.setEnabled(false);
  addParameter(m_isSubVolume);
  m_zoomInViewSize.setTracking(false);
  m_zoomInViewSize.setSingleStep(32);
  addParameter(m_zoomInViewSize);
  connect(&m_rendererBase, &Z3DRendererBase::coordTransformChanged, this, &Z3DVolumeFilter::changeCoordTransform);
  connect(&m_zoomInViewSize, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::changeZoomInViewSize);

  addParameter(m_interactionDownsample);

  Z3DTexture* g_TexId[2];
  g_TexId[0] = new Z3DTexture((GLint)GL_RGBA32F, glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[0]->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
  g_TexId[0]->uploadImage();
  g_TexId[1] = new Z3DTexture((GLint)GL_RGBA32F, glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[1]->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
  g_TexId[1]->uploadImage();
  m_entryTarget.attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0);
  m_entryTarget.attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1);
  m_entryTarget.isFBOComplete();
  g_TexId[0] = new Z3DTexture((GLint)GL_RGBA32F, glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[0]->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
  g_TexId[0]->uploadImage();
  g_TexId[1] = new Z3DTexture((GLint)GL_RGBA32F, glm::uvec3(32, 32, 1), GL_RGBA, GL_FLOAT);
  g_TexId[1]->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
  g_TexId[1]->uploadImage();
  m_exitTarget.attachTextureToFBO(g_TexId[0], GL_COLOR_ATTACHMENT0);
  m_exitTarget.attachTextureToFBO(g_TexId[1], GL_COLOR_ATTACHMENT1);
  m_exitTarget.isFBOComplete();
  m_layerColorTexture.uploadImage();
  m_layerDepthTexture.uploadImage();
  m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
  m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
  m_layerTarget.isFBOComplete();

  // ports
  addPrivateRenderTarget(m_entryTarget);
  addPrivateRenderTarget(m_exitTarget);
  addPrivateRenderTarget(m_layerTarget);
  addPort(m_vPPort);

  addParameter(m_useFRVolumeSlice);
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

  connect(&m_showXSlice, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);
  connect(&m_showYSlice, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);
  connect(&m_showZSlice, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);
  connect(&m_showXSlice2, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);
  connect(&m_showYSlice2, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);
  connect(&m_showZSlice2, &ZBoolParameter::valueChanged, this, &Z3DVolumeFilter::adjustWidget);

  connect(&m_xSlicePosition, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeXSlice);
  connect(&m_ySlicePosition, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeYSlice);
  connect(&m_zSlicePosition, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeZSlice);
  connect(&m_xSlice2Position, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeXSlice2);
  connect(&m_ySlice2Position, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeYSlice2);
  connect(&m_zSlice2Position, &ZIntParameter::valueChanged, this, &Z3DVolumeFilter::invalidateFRVolumeZSlice2);

  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_leftMouseButtonPressEvent.listenTo("trace", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  connect(&m_leftMouseButtonPressEvent,
          &ZEventListenerParameter::mouseEventTriggered,
          this,
          &Z3DVolumeFilter::leftMouseButtonPressed);
  addEventListener(m_leftMouseButtonPressEvent);

  m_volumeRaycasterRenderer.setLayerTarget(&m_layerTarget);
  m_volumeSliceRenderer.setLayerTarget(&m_layerTarget);
  for (size_t i = 0; i < m_maxNumOfFullResolutionVolumeSlice; ++i) {
    m_image2DRenderers.emplace_back(std::make_unique<Z3DImage2DRenderer>(m_rendererBase));
    m_image2DRenderers[i]->setLayerTarget(&m_layerTarget);
  }
  m_boundBoxLineWidth.set(1);
  m_boundBoxMode.select("Bound Box");

  addParameter(m_raycasterCompositingMode);
  addParameter(m_raycasterIsoValue);
  addParameter(m_raycasterLocalMIPThreshold);
  addParameter(m_raycasterSamplingRate);

  adjustWidget();
  CHECK_GL_ERROR

  m_numParas = m_parameters.size();
}

void Z3DVolumeFilter::setData(const ZImgPack& img)
{
  if (m_widgetsGroup) {
    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (const auto& para : m_texFilterModeParas) {
      m_widgetsGroup->removeChild(*para);
    }
    for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
      m_widgetsGroup->removeChild(*it->get());
    }
  }
  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }

  m_imgPack = &img;

  m_volumes.clear();
  m_zoomInVolumes.clear();
  readVolumes();
  updateBoundBox();

  m_channelVisibleParas.clear();
  m_transferFuncParas.clear();
  m_texFilterModeParas.clear();

  std::vector<bool> channelVisibilities;
  channelVisibilities.reserve(m_volumes.size());
  std::vector<Z3DTransferFunction*> transferFunctions;
  transferFunctions.reserve(m_volumes.size());
  std::vector<GLint> texFilterModes;
  texFilterModes.reserve(m_volumes.size());

  for (size_t i = 0; i < m_volumes.size(); ++i) {
    auto visiblePara = std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(i + 1), true);
    channelVisibilities.push_back(visiblePara->get());
    connect(visiblePara.get(), &ZBoolParameter::boolChanged, this, [this, i](bool value) {
      m_volumeRaycasterRenderer.setChannelVisibility(i, value);
      invalidateResult();
    });
    m_channelVisibleParas.emplace_back(std::move(visiblePara));

    auto transferPara = std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(i + 1));
    transferPara->setVolume(m_volumes[i].get());
    connect(transferPara.get(), &Z3DTransferFunctionParameter::valueChanged, this, [this]() {
      invalidateResult();
    });
    transferFunctions.push_back(&transferPara->get());

    auto& transferFunction = transferPara->get();
    transferFunction.reset(0.0, 1.0, glm::vec4(0.f), glm::vec4(m_volumes[i]->volColor(), 1.f));
    if (false) {
      transferFunction.addKey(ZColorMapKey(0.001, glm::vec4(0.01f, 0.01f, 0.01f, 0.0f)));
      transferFunction.addKey(ZColorMapKey(0.01, glm::vec4(0.01f, 0.01f, 0.01f, 1.0f)));
    }

    m_transferFuncParas.emplace_back(std::move(transferPara));

    auto texFilterPara = std::make_unique<ZStringIntOptionParameter>(QString("Texture Filtering %1").arg(i + 1));
    texFilterPara->addOptionsWithData(std::make_pair(QStringLiteral("Nearest"), static_cast<int>(GL_NEAREST)),
                                      std::make_pair(QStringLiteral("Linear"), static_cast<int>(GL_LINEAR)));
    texFilterPara->select(QStringLiteral("Linear"));
    texFilterModes.push_back(texFilterPara->associatedData());
    connect(texFilterPara.get(),
            &ZStringIntOptionParameter::valueChanged,
            this,
            [this, i, para = texFilterPara.get()]() {
              m_volumeRaycasterRenderer.setTexFilterMode(i, para->associatedData());
              invalidateResult();
            });
    m_texFilterModeParas.emplace_back(std::move(texFilterPara));
  }

  m_volumeRaycasterRenderer.setChannelVisibilities(channelVisibilities);
  m_volumeRaycasterRenderer.setTransferFunctions(transferFunctions);
  m_volumeRaycasterRenderer.setTexFilterModes(texFilterModes);

  for (const auto& para : m_channelVisibleParas) {
    addParameter(*para);
  }
  for (const auto& para : m_transferFuncParas) {
    addParameter(*para);
  }
  for (const auto& para : m_texFilterModeParas) {
    addParameter(*para);
  }
  for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
    addParameter(*it->get());
  }

  if (m_widgetsGroup) {
    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->addChild(*para, 2);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    for (const auto& para : m_texFilterModeParas) {
      m_widgetsGroup->addChild(*para, 15);
    }
    for (auto it = m_sliceColormaps.begin(); it != m_sliceColormaps.end(); ++it) {
      m_widgetsGroup->addChild(*it->get(), 11);
    }
    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }

  invalidateResult();
}

bool Z3DVolumeFilter::openZoomInView(const glm::ivec3& volPos)
{
  if (!m_isVolumeDownsampled.get()) {
    return false;
  }
  if (!volumeNeedDownsample()) {
    return false;
  }
  if (m_volumes.empty()) {
    return false;
  }
  glm::ivec3 voldim = glm::ivec3(m_volumes[0]->cubeSize());
  if (!(volPos[0] >= 0 && volPos[0] < voldim.x && volPos[1] >= 0 && volPos[1] < voldim.y && volPos[2] >= 0 &&
        volPos[2] < voldim.z)) {
    return false;
  }

  m_zoomInPos = volPos;
  if (m_zoomInViewSize.get() % 2 != 0) {
    m_zoomInViewSize.set(m_zoomInViewSize.get() + 1);
  }
  int halfsize = m_zoomInViewSize.get() / 2;
  int left = std::max(volPos[0] - halfsize + 1, 0);
  int right = std::min(volPos[0] + halfsize, int(m_imgPack->imgInfo().width) - 1);
  int up = std::max(volPos[1] - halfsize + 1, 0);
  int down = std::min(volPos[1] + halfsize, int(m_imgPack->imgInfo().height) - 1);
  int front = 0;
  int back = int(m_imgPack->imgInfo().depth) - 1;
  readSubVolumes(left, right, up, down, front, back);

  m_isSubVolume.set(true);
  m_isVolumeDownsampled.set(false);

  volumeChanged();
  invalidateResult();
  return true;
}

void Z3DVolumeFilter::exitZoomInView()
{
  if (m_zoomInVolumes.empty()) {
    return;
  }

  // copy transform matrix from sub volume, in case it is changed
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    m_volumes[i]->setPhysicalToWorldMatrix(m_zoomInVolumes[i]->physicalToWorldMatrix());
  }
  m_zoomInVolumes.clear();
  m_isSubVolume.set(false);
  m_isVolumeDownsampled.set(true);

  volumeChanged();
  invalidateResult();
}

bool Z3DVolumeFilter::volumeNeedDownsample() const
{
  size_t maxTextureSize = 100;
  if (m_imgPack->imgInfo().depth > 1) {
    maxTextureSize = Z3DGpuInfo::instance().max3DTextureSize();
  } else {
    maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  }
  return m_imgPack->imgInfo().timeVoxelNumber() > m_maxVoxelNumber || m_imgPack->imgInfo().width > maxTextureSize ||
         m_imgPack->imgInfo().height > maxTextureSize || m_imgPack->imgInfo().depth > maxTextureSize;
}

bool Z3DVolumeFilter::isVolumeDownsampled() const
{
  return m_isVolumeDownsampled.get();
}

std::shared_ptr<ZWidgetsGroup> Z3DVolumeFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Img", 1);

    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_isVolumeDownsampled, 2);
    m_widgetsGroup->addChild(m_isSubVolume, 2);
    m_widgetsGroup->addChild(m_zoomInViewSize, 2);

    for (const auto& para : m_channelVisibleParas) {
      m_widgetsGroup->addChild(*para, 2);
    }
    for (const auto& para : m_transferFuncParas) {
      m_widgetsGroup->addChild(*para, 3);
    }
    m_widgetsGroup->addChild(m_raycasterCompositingMode, 4);
    m_widgetsGroup->addChild(m_raycasterIsoValue, 4);
    m_widgetsGroup->addChild(m_raycasterLocalMIPThreshold, 4);
    m_widgetsGroup->addChild(m_raycasterSamplingRate, 15);
    for (const auto& para : m_texFilterModeParas) {
      m_widgetsGroup->addChild(*para, 15);
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
    m_widgetsGroup->addChild(m_interactionDownsample, 19);
    m_widgetsGroup->addChild(m_rendererBase.coordTransformPara(), 1);

    const std::vector<ZParameter*>& paras = parameters();
    for (auto para : paras) {
      if (para->name().contains("Slice") && !para->name().endsWith("2") && !para->name().endsWith("2 Position")) {
        m_widgetsGroup->addChild(*para, 11);
      } else if (para->name().contains("Slice")) {
        m_widgetsGroup->addChild(*para, 19);
      }
    }
    m_widgetsGroup->setBasicAdvancedCutoff(14);
  }
  return m_widgetsGroup;
}

void Z3DVolumeFilter::enterInteractionMode()
{
  const uint32_t factor = static_cast<uint32_t>(m_interactionDownsample.get());
  if (factor <= 1 || m_outputSize.x == 0 || m_outputSize.y == 0) {
    return;
  }

  if (!m_interactionDownsampleActive) {
    m_interactionBaseSize = m_outputSize;
  }

  glm::uvec2 newSize = glm::max(m_outputSize / factor, glm::uvec2(1u, 1u));
  if (newSize == m_outputSize) {
    return;
  }

  m_outputSize = newSize;
  m_entryTarget.resize(newSize);
  m_exitTarget.resize(newSize);
  m_layerTarget.resize(newSize);
  releaseAllRenderTargets();
  markTargetsInvalid();

  for (auto port : inputPorts()) {
    port->setExpectedSize(newSize);
  }

  m_interactionDownsampleActive = true;
  Q_EMIT requestUpstreamSizeChange(this);
  invalidateResult();
}

void Z3DVolumeFilter::exitInteractionMode()
{
  if (!m_interactionDownsampleActive) {
    return;
  }

  glm::uvec2 restoreSize = m_interactionBaseSize;
  if (restoreSize.x == 0 || restoreSize.y == 0) {
    restoreSize = m_outputSize;
  }

  m_outputSize = restoreSize;
  m_entryTarget.resize(restoreSize);
  m_exitTarget.resize(restoreSize);
  m_layerTarget.resize(restoreSize);
  releaseAllRenderTargets();
  markTargetsInvalid();

  for (auto port : inputPorts()) {
    port->setExpectedSize(restoreSize);
  }

  m_interactionDownsampleActive = false;
  m_interactionBaseSize = glm::uvec2(0u, 0u);
  Q_EMIT requestUpstreamSizeChange(this);
  invalidateResult();
}

void Z3DVolumeFilter::updateRaycasterSamplingRate()
{
  m_volumeRaycasterRenderer.setSamplingRate(m_raycasterSamplingRate.get());
}

void Z3DVolumeFilter::updateRaycasterIsoValue()
{
  m_volumeRaycasterRenderer.setIsoValue(m_raycasterIsoValue.get());
}

void Z3DVolumeFilter::updateRaycasterLocalMIPThreshold()
{
  m_volumeRaycasterRenderer.setLocalMIPThreshold(m_raycasterLocalMIPThreshold.get());
}

void Z3DVolumeFilter::updateRaycasterCompositingMode()
{
  const auto mode = static_cast<VolumeCompositingMode>(m_raycasterCompositingMode.associatedData());
  m_volumeRaycasterRenderer.setCompositingMode(mode);

  const bool showIso = mode == VolumeCompositingMode::IsoSurface;
  const bool showLocal = mode == VolumeCompositingMode::LocalMIP || mode == VolumeCompositingMode::LocalMIPOpaque;
  m_raycasterIsoValue.setVisible(showIso);
  m_raycasterLocalMIPThreshold.setVisible(showLocal);

  updateRaycasterIsoValue();
  updateRaycasterLocalMIPThreshold();
}

bool Z3DVolumeFilter::isReady(Z3DEye eye) const
{
  return Z3DBoundedFilter::isReady(eye) && m_visible.get() && m_imgPack;
}

glm::vec3 Z3DVolumeFilter::get3DPosition(int x, int y, int width, int height, bool& success)
{
  const auto mode = static_cast<VolumeCompositingMode>(m_raycasterCompositingMode.associatedData());

  if (mode == VolumeCompositingMode::DirectVolumeRendering) {
    return getMaxInten3DPositionUnderScreenPoint(x, y, width, height, success);
  } else {
    return getFirstHit3DPosition(x, y, width, height, success);
  }
}

bool Z3DVolumeFilter::hasOpaque(Z3DEye eye) const
{
  return m_opaqueValid[eyeIndex(eye)];
}

void Z3DVolumeFilter::renderOpaque(Z3DEye eye)
{
  const size_t idx = eyeIndex(eye);
  if (!m_opaqueValid[idx]) {
    return;
  }
  const auto& target = opaqueTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

bool Z3DVolumeFilter::hasTransparent(Z3DEye eye) const
{
  return m_transparentValid[eyeIndex(eye)];
}

void Z3DVolumeFilter::renderTransparent(Z3DEye eye)
{
  const size_t idx = eyeIndex(eye);
  if (!m_transparentValid[idx]) {
    return;
  }
  const auto& target = transparentTarget(eye);
  m_textureCopyRenderer.setColorTexture(target.attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(target.attachment(GL_DEPTH_ATTACHMENT));
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

void Z3DVolumeFilter::changeCoordTransform()
{
  if (m_volumes.empty()) {
    return;
  }
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    m_volumes[i]->setPhysicalToWorldMatrix(m_rendererBase.coordTransform());
  }
  for (size_t i = 0; i < m_zoomInVolumes.size(); ++i) {
    m_zoomInVolumes[i]->setPhysicalToWorldMatrix(m_rendererBase.coordTransform());
  }
  invalidateAllFRVolumeSlices();
}

void Z3DVolumeFilter::changeZoomInViewSize()
{
  if (m_zoomInVolumes.empty()) {
    return;
  }
  exitZoomInView();
  openZoomInView(m_zoomInPos);
}

void Z3DVolumeFilter::adjustWidget()
{
  m_zSlicePosition.setVisible(m_showZSlice.get());
  m_ySlicePosition.setVisible(m_showYSlice.get());
  m_xSlicePosition.setVisible(m_showXSlice.get());
  m_zSlice2Position.setVisible(m_showZSlice2.get());
  m_ySlice2Position.setVisible(m_showYSlice2.get());
  m_xSlice2Position.setVisible(m_showXSlice2.get());
}

void Z3DVolumeFilter::updateSize()
{
  Z3DBoundedFilter::updateSize();

  const glm::uvec2 requestedSize = m_vPPort.size();
  if (requestedSize.x == 0 || requestedSize.y == 0) {
    return;
  }

  if (m_interactionDownsampleActive && m_interactionBaseSize.x != 0 && m_interactionBaseSize.y != 0) {
    m_interactionBaseSize = requestedSize;
  }

  if (m_outputSize != requestedSize) {
    m_outputSize = requestedSize;
    m_entryTarget.resize(m_outputSize);
    m_exitTarget.resize(m_outputSize);
    m_layerTarget.resize(m_outputSize);
    releaseAllRenderTargets();
  }
}
void Z3DVolumeFilter::leftMouseButtonPressed(QMouseEvent* e, int w, int h)
{
  e->ignore();
  if (!m_volumeRaycasterRenderer.hasVisibleRendering()) {
    return;
  }
  // Mouse button pressed
  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = e->position().x();
    m_startCoord.y = e->position().y();
    toggleInteractionMode(true, this);
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      bool success;
#ifndef _QT4_
      glm::vec3 pos3D = getFirstHit3DPosition(e->position().x() * m_rendererBase.globalParas().devicePixelRatio.get(),
                                              e->position().y() * m_rendererBase.globalParas().devicePixelRatio.get(),
                                              w * m_rendererBase.globalParas().devicePixelRatio.get(),
                                              h * m_rendererBase.globalParas().devicePixelRatio.get(),
                                              success);
#else
      glm::vec3 pos3D = getFirstHit3DPosition(e->x(), e->y(), w, h, success);
#endif
      if (success) {
        Q_EMIT pointInVolumeLeftClicked(e->pos(), glm::ivec3(pos3D));
        // e->accept();
      }
    }
    toggleInteractionMode(false, this);
  }
}

void Z3DVolumeFilter::invalidateFRVolumeZSlice()
{
  m_FRVolumeSlicesValidState[0] = false;
}

void Z3DVolumeFilter::invalidateFRVolumeYSlice()
{
  m_FRVolumeSlicesValidState[1] = false;
}

void Z3DVolumeFilter::invalidateFRVolumeXSlice()
{
  m_FRVolumeSlicesValidState[2] = false;
}

void Z3DVolumeFilter::invalidateFRVolumeZSlice2()
{
  m_FRVolumeSlicesValidState[3] = false;
}

void Z3DVolumeFilter::invalidateFRVolumeYSlice2()
{
  m_FRVolumeSlicesValidState[4] = false;
}

void Z3DVolumeFilter::invalidateFRVolumeXSlice2()
{
  m_FRVolumeSlicesValidState[5] = false;
}

[[maybe_unused]] void Z3DVolumeFilter::updateCubeSerieSlices()
{
  m_cubeSerieSlices.clear();
  Z3DVolume* volume = getVolumes()[0].get();

  glm::vec3 coordLuf = volume->physicalLUF();
  glm::vec3 coordRdb = volume->physicalRDB();
  glm::uvec3 volDim = volume->originalDimensions();
  glm::uvec3 dim = volume->dimensions();

  float xTexCoordStart = std::max(m_xCut.lowerValue(), m_xCut.minimum() + 1) / (m_xCut.maximum() - 1);
  float xTexCoordEnd = std::min(m_xCut.upperValue(), m_xCut.maximum() - 1) / (m_xCut.maximum() - 1);
  float xCoordStart = glm::mix(coordLuf.x, coordRdb.x, xTexCoordStart);
  float xCoordEnd = glm::mix(coordLuf.x, coordRdb.x, xTexCoordEnd);
  float yTexCoordStart = std::max(m_yCut.lowerValue(), m_yCut.minimum() + 1) / (m_yCut.maximum() - 1);
  float yTexCoordEnd = std::min(m_yCut.upperValue(), m_yCut.maximum() - 1) / (m_yCut.maximum() - 1.f);
  float yCoordStart = glm::mix(coordLuf.y, coordRdb.y, yTexCoordStart);
  float yCoordEnd = glm::mix(coordLuf.y, coordRdb.y, yTexCoordEnd);
  float zTexCoordStart = std::max(m_zCut.lowerValue(), m_zCut.minimum() + 1) / (m_zCut.maximum() - 1);
  float zTexCoordEnd = std::min(m_zCut.upperValue(), m_zCut.maximum() - 1) / (m_zCut.maximum() - 1);
  float zCoordStart = glm::mix(coordLuf.z, coordRdb.z, zTexCoordStart);
  float zCoordEnd = glm::mix(coordLuf.z, coordRdb.z, zTexCoordEnd);

  // it is no point to make more slices than actual texture dimension
  int numZSlice = std::ceil((m_zCut.upperValue() - m_zCut.lowerValue() - 1.0) / dim.z * volDim.z) * 2;
  int numYSlice = std::ceil((m_yCut.upperValue() - m_yCut.lowerValue() - 1.0) / dim.y * volDim.y) * 2;
  int numXSlice = std::ceil((m_xCut.upperValue() - m_xCut.lowerValue() - 1.0) / dim.x * volDim.x) * 2;
  // Z front to back
  m_cubeSerieSlices["ZF2B"] = ZMesh::createCubeSerieSlices(numZSlice,
                                                           2,
                                                           glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                                           glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                                           glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                                           glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
  // Z back to front
  m_cubeSerieSlices["ZB2F"] = ZMesh::createCubeSerieSlices(numZSlice,
                                                           2,
                                                           glm::vec3(xCoordStart, yCoordStart, zCoordEnd),
                                                           glm::vec3(xCoordEnd, yCoordEnd, zCoordStart),
                                                           glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordEnd),
                                                           glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordStart));
  // Y front to back
  m_cubeSerieSlices["YF2B"] = ZMesh::createCubeSerieSlices(numYSlice,
                                                           1,
                                                           glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                                           glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                                           glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                                           glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
  // Y back to front
  m_cubeSerieSlices["YB2F"] = ZMesh::createCubeSerieSlices(numYSlice,
                                                           1,
                                                           glm::vec3(xCoordStart, yCoordEnd, zCoordStart),
                                                           glm::vec3(xCoordEnd, yCoordStart, zCoordEnd),
                                                           glm::vec3(xTexCoordStart, yTexCoordEnd, zTexCoordStart),
                                                           glm::vec3(xTexCoordEnd, yTexCoordStart, zTexCoordEnd));
  // X front to back
  m_cubeSerieSlices["XF2B"] = ZMesh::createCubeSerieSlices(numXSlice,
                                                           0,
                                                           glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                                           glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                                           glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                                           glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
  // X back to front
  m_cubeSerieSlices["XB2F"] = ZMesh::createCubeSerieSlices(numXSlice,
                                                           0,
                                                           glm::vec3(xCoordEnd, yCoordStart, zCoordStart),
                                                           glm::vec3(xCoordStart, yCoordEnd, zCoordEnd),
                                                           glm::vec3(xTexCoordEnd, yTexCoordStart, zTexCoordStart),
                                                           glm::vec3(xTexCoordStart, yTexCoordEnd, zTexCoordEnd));
}

void Z3DVolumeFilter::invalidate(State inv)
{
  Z3DBoundedFilter::invalidate(inv);
  markTargetsInvalid();
}

void Z3DVolumeFilter::process(Z3DEye eye)
{
  glEnable(GL_DEPTH_TEST);

  Z3DVolume* volume = getVolumes()[0].get();
  if (volume->is1DData()) {
    return;
  }

  bool allCliped = m_xCut.upperValue() < m_xCut.minimum() + 1 || m_yCut.upperValue() < m_yCut.minimum() + 1 ||
                   m_zCut.upperValue() < m_zCut.minimum() + 1 || m_xCut.lowerValue() > m_xCut.maximum() - 1 ||
                   m_yCut.lowerValue() > m_yCut.maximum() - 1 || m_zCut.lowerValue() > m_zCut.maximum() - 1;

  Z3DRenderTarget& currentTarget = transparentTarget(eye);
  const size_t idx = eyeIndex(eye);

  currentTarget.bind();
  currentTarget.clear();
  m_rendererBase.setViewport(currentTarget.size());

  m_transparentValid[idx] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, idx]() {
    currentTarget.release();
    m_transparentValid[idx] = true;
  });

  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  if (m_volumeRaycasterRenderer.hasVisibleRendering() && !allCliped) {
    prepareDataForRaycaster(volume, eye);
    m_rendererBase.render(eye, m_volumeRaycasterRenderer);
  }

  renderBoundBox(eye);
  CHECK_GL_ERROR

  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  if (hasSlices()) {
    renderSlices(eye);
  }

  glDisable(GL_DEPTH_TEST);

  CHECK_GL_ERROR
}

bool Z3DVolumeFilter::hasSlices() const
{
  return m_showZSlice.get() || m_showXSlice.get() || m_showYSlice.get() || m_showXSlice2.get() || m_showYSlice2.get() ||
         m_showZSlice2.get();
}

void Z3DVolumeFilter::renderSlices(Z3DEye eye)
{
  Z3DRenderTarget& currentTarget = opaqueTarget(eye);
  const size_t idx = eyeIndex(eye);

  m_layerTarget.resize(currentTarget.size());

  currentTarget.bind();
  currentTarget.clear();
  m_rendererBase.setViewport(currentTarget.size());

  m_opaqueValid[idx] = false;
  auto targetGuard = folly::makeGuard([&currentTarget, this, idx]() {
    currentTarget.release();
    m_opaqueValid[idx] = true;
  });

  Z3DVolume* volume = getVolumes()[0].get();
  glm::uvec3 volDim = volume->originalDimensions();
  glm::vec3 coordLuf = volume->physicalLUF();
  glm::vec3 coordRdb = volume->physicalRDB();

  if (m_useFRVolumeSlice.get() && volume->isDownsampledVolume()) {
    std::vector<Z3DPrimitiveRenderer*> renderers;

    size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();

    size_t sliceRendererIdx = 0;
    if (m_showZSlice.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float zTexCoord = m_zSlicePosition.get() / static_cast<float>(volDim.z - 1);
        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(0, -1, 0, -1, m_zSlicePosition.get(), m_zSlicePosition.get() + 1, c, c + 1, 0, 1));
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(zCoord, 2, coordLuf.xy(), coordRdb.xy());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    sliceRendererIdx = 1;
    if (m_showYSlice.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float yTexCoord = m_ySlicePosition.get() / static_cast<float>(volDim.y - 1);
        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(0, -1, m_ySlicePosition.get(), m_ySlicePosition.get() + 1, 0, -1, c, c + 1, 0, 1));
          croped.infoRef().height = m_imgPack->imgInfo().depth;
          croped.infoRef().depth = 1;
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(yCoord, 1, coordLuf.xz(), coordRdb.xz());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    sliceRendererIdx = 2;
    if (m_showXSlice.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float xTexCoord = m_xSlicePosition.get() / static_cast<float>(volDim.x - 1);
        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(m_xSlicePosition.get(), m_xSlicePosition.get() + 1, 0, -1, 0, -1, c, c + 1, 0, 1));
          croped.infoRef().width = m_imgPack->imgInfo().height;
          croped.infoRef().height = m_imgPack->imgInfo().depth;
          croped.infoRef().depth = 1;
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(xCoord, 0, coordLuf.yz(), coordRdb.yz());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    sliceRendererIdx = 3;
    if (m_showZSlice2.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float zTexCoord = m_zSlice2Position.get() / static_cast<float>(volDim.z - 1);
        float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(0, -1, 0, -1, m_zSlice2Position.get(), m_zSlice2Position.get() + 1, c, c + 1, 0, 1));
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(zCoord, 2, coordLuf.xy(), coordRdb.xy());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    sliceRendererIdx = 4;
    if (m_showYSlice2.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float yTexCoord = m_ySlice2Position.get() / static_cast<float>(volDim.y - 1);
        float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(0, -1, m_ySlice2Position.get(), m_ySlice2Position.get() + 1, 0, -1, c, c + 1, 0, 1));
          croped.infoRef().height = m_imgPack->imgInfo().depth;
          croped.infoRef().depth = 1;
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(yCoord, 1, coordLuf.xz(), coordRdb.xz());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    sliceRendererIdx = 5;
    if (m_showXSlice2.get()) {
      if (!m_FRVolumeSlicesValidState[sliceRendererIdx]) {
        m_image2DRenderers[sliceRendererIdx]->clearQuads();
        m_FRVolumeSlices[sliceRendererIdx].clear();

        float xTexCoord = m_xSlice2Position.get() / static_cast<float>(volDim.x - 1);
        float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

        for (size_t c = 0; c < m_nChannels; ++c) {
          ZImg croped = m_imgPack->crop(
            ZImgRegion(m_xSlice2Position.get(), m_xSlice2Position.get() + 1, 0, -1, 0, -1, c, c + 1, 0, 1));
          croped.infoRef().width = m_imgPack->imgInfo().height;
          croped.infoRef().height = m_imgPack->imgInfo().depth;
          croped.infoRef().depth = 1;
          if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
            croped =
              croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
          }
          if (!croped.isType<uint8_t>()) {
            croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
          } else {
            croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
          }
          auto vh = new Z3DVolume(croped);
          vh->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                    m_imgPack->imgInfo().channelColors[c].g / 255.,
                                    m_imgPack->imgInfo().channelColors[c].b / 255.));
          m_FRVolumeSlices[sliceRendererIdx].emplace_back(vh);
        }
        m_image2DRenderers[sliceRendererIdx]->setChannels(m_FRVolumeSlices[sliceRendererIdx], m_sliceColormaps);

        ZMesh slice = ZMesh::createCubeSliceWith2DTexture(xCoord, 0, coordLuf.yz(), coordRdb.yz());
        slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
        m_image2DRenderers[sliceRendererIdx]->addQuad(slice);
        m_FRVolumeSlicesValidState[sliceRendererIdx] = true;
      }
      renderers.push_back(m_image2DRenderers[sliceRendererIdx].get());
    }
    m_rendererBase.render(eye, renderers);

  } else {
    m_volumeSliceRenderer.clearQuads();

    if (m_showZSlice.get()) {
      float zTexCoord = m_zSlicePosition.get() / static_cast<float>(volDim.z - 1);
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }
    if (m_showYSlice.get()) {
      float yTexCoord = m_ySlicePosition.get() / static_cast<float>(volDim.y - 1);
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }
    if (m_showXSlice.get()) {
      float xTexCoord = m_xSlicePosition.get() / static_cast<float>(volDim.x - 1);
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }

    if (m_showZSlice2.get()) {
      float zTexCoord = m_zSlice2Position.get() / static_cast<float>(volDim.z - 1);
      float zCoord = glm::mix(coordLuf.z, coordRdb.z, zTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(zCoord, zTexCoord, 2, coordLuf.xy(), coordRdb.xy());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }
    if (m_showYSlice2.get()) {
      float yTexCoord = m_ySlice2Position.get() / static_cast<float>(volDim.y - 1);
      float yCoord = glm::mix(coordLuf.y, coordRdb.y, yTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(yCoord, yTexCoord, 1, coordLuf.xz(), coordRdb.xz());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }
    if (m_showXSlice2.get()) {
      float xTexCoord = m_xSlice2Position.get() / static_cast<float>(volDim.x - 1);
      float xCoord = glm::mix(coordLuf.x, coordRdb.x, xTexCoord);

      ZMesh slice = ZMesh::createCubeSlice(xCoord, xTexCoord, 0, coordLuf.yz(), coordRdb.yz());
      slice.transformVerticesByMatrix(volume->physicalToWorldMatrix());
      m_volumeSliceRenderer.addQuad(slice);
    }
    m_rendererBase.render(eye, m_volumeSliceRenderer);
  }
}

const std::vector<std::unique_ptr<Z3DVolume>>& Z3DVolumeFilter::getVolumes() const
{
  if (m_isSubVolume.get()) {
    return m_zoomInVolumes;
  } else {
    return m_volumes;
  }
}

void Z3DVolumeFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.setMinCorner(glm::dvec3(m_volumes[0]->parentVolPhysicalLUF()));
  m_notTransformedBoundBox.setMaxCorner(glm::dvec3(m_volumes[0]->parentVolPhysicalRDB()));
}

void Z3DVolumeFilter::readVolumes()
{
  m_volumes.clear();
  m_nChannels = m_imgPack->imgInfo().numChannels;

#if 0
  // shader limit is 20 channels
  // limited by Max FS Texture Image Units
  // see https://www.opengl.org/wiki/Shader#Resource_limitations
  size_t maxPossibleChannels = std::min(20, (Z3DGpuInfoInstance.maxTextureImageUnits() - 4) / 2);
#else
  size_t maxPossibleChannels = Z3DGpuInfo::instance().maxArrayTextureLayers();
#endif
  if (m_nChannels > maxPossibleChannels) {
    QMessageBox::warning(
      QApplication::activeWindow(),
      "Too many channels",
      QString("Due to hardware limit, only first %1 channels of this image will be shown").arg(maxPossibleChannels));
    m_nChannels = maxPossibleChannels;
  }

  if (m_nChannels > m_layerColorTexture.depth()) {
    m_layerColorTexture.setDimension(
      glm::uvec3(m_layerColorTexture.width(), m_layerColorTexture.height(), m_nChannels));
    m_layerColorTexture.uploadImage();
    m_layerDepthTexture.setDimension(
      glm::uvec3(m_layerDepthTexture.width(), m_layerDepthTexture.height(), m_nChannels));
    m_layerDepthTexture.uploadImage();
    m_layerTarget.attachTextureToFBO(&m_layerColorTexture, GL_COLOR_ATTACHMENT0, false);
    m_layerTarget.attachTextureToFBO(&m_layerDepthTexture, GL_DEPTH_ATTACHMENT, false);
    m_layerTarget.isFBOComplete();
  }

  bool scaleZ = m_imgPack->imgInfo().depth > std::pow(m_maxVoxelNumber, 1 / 3.0);
  double scale = 1.0;
  if (m_imgPack->imgInfo().timeVoxelNumber() > m_maxVoxelNumber) {
    if (scaleZ) {
      scale = std::pow((m_maxVoxelNumber * 1.0) / m_imgPack->imgInfo().timeVoxelNumber(), 1 / 3.0);
    } else {
      scale = std::sqrt((m_maxVoxelNumber * 1.0) / m_imgPack->imgInfo().timeVoxelNumber());
    }
  }
  int height = static_cast<int>(m_imgPack->imgInfo().height * scale);
  int width = static_cast<int>(m_imgPack->imgInfo().width * scale);
  int depth =
    scaleZ ? static_cast<int>(m_imgPack->imgInfo().depth * scale) : static_cast<int>(m_imgPack->imgInfo().depth);
  double widthScale = 1.0;
  double heightScale = 1.0;
  double depthScale = 1.0;
  int maxTextureSize = 100;
  if (m_imgPack->imgInfo().depth > 1) {
    maxTextureSize = Z3DGpuInfo::instance().max3DTextureSize();
  } else {
    maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  }

  if (height > maxTextureSize) {
    heightScale = static_cast<double>(maxTextureSize) / height;
    height = std::floor(height * heightScale);
  }
  if (width > maxTextureSize) {
    widthScale = static_cast<double>(maxTextureSize) / width;
    width = std::floor(width * widthScale);
  }
  if (depth > maxTextureSize) {
    depthScale = static_cast<double>(maxTextureSize) / depth;
    depth = std::floor(depth * depthScale);
  }

  widthScale *= scale;
  heightScale *= scale;
  if (scaleZ) {
    depthScale *= scale;
  }

  if (widthScale != 1.0 || heightScale != 1.0 || depthScale != 1.0) {
    m_isVolumeDownsampled.set(true);
  }

  ZImg img = m_imgPack->resizedImg(width, height, depth, 0);
  img.computeMinMax(m_imgMinIntensity, m_imgMaxIntensity);
  if (!img.isType<uint8_t>()) {
    img = img.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
  } else /* if (img.validBitCount() != 0 && img.validBitCount() < 8) */ {
    img.normalize(m_imgMinIntensity, m_imgMaxIntensity);
  }
  if (m_nChannels == 1) {
    Z3DVolume* vh = new Z3DVolume(img,
                                  glm::vec3(1.f / widthScale, 1.f / heightScale, 1.f / depthScale),
                                  glm::vec3(.0),
                                  m_rendererBase.coordTransform());

    m_volumes.emplace_back(vh);
  } else {
    for (size_t i = 0; i < m_nChannels; ++i) {
      ZImg cImg = img.crop(ZImgRegion(0, -1, 0, -1, 0, -1, i, i + 1));
      Z3DVolume* vh = new Z3DVolume(cImg,
                                    glm::vec3(1.f / widthScale, 1.f / heightScale, 1.f / depthScale),
                                    glm::vec3(.0),
                                    m_rendererBase.coordTransform());

      m_volumes.emplace_back(vh);
    } // for each cannel
  }

  for (size_t i = 0; i < m_nChannels; ++i) {
    m_volumes[i]->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[i].r / 255.,
                                        m_imgPack->imgInfo().channelColors[i].g / 255.,
                                        m_imgPack->imgInfo().channelColors[i].b / 255.));
  }

  m_sliceColormaps.clear();
  for (size_t i = 0; i < m_volumes.size(); ++i) {
    m_sliceColormaps.emplace_back(
      std::make_unique<ZColorMapParameter>(QString("Slice Channel %1 Colormap").arg(i + 1)));
    m_sliceColormaps[i]->get().create1DTexture(256);
    m_sliceColormaps[i]->get().reset(0.0,
                                     1.0,
                                     QColor(0, 0, 0),
                                     QColor(m_imgPack->imgInfo().channelColors[i].r,
                                            m_imgPack->imgInfo().channelColors[i].g,
                                            m_imgPack->imgInfo().channelColors[i].b));
  }

  volumeChanged();
}

void Z3DVolumeFilter::readSubVolumes(int left, int right, int up, int down, int front, int back)
{
  m_zoomInVolumes.clear();

  glm::vec3 downsampleSpacing = glm::vec3(1.f, 1.f, 1.f);
  glm::vec3 offset = glm::vec3(left, right, front) + m_volumes[0]->offset();
  for (size_t i = 0; i < m_nChannels; ++i) {
    ZImg img = m_imgPack->crop(ZImgRegion(left, right + 1, up, down + 1, front, back + 1, i, i + 1, 0, 1));
    if (!img.isType<uint8_t>()) {
      img = img.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
    } else {
      img.normalize(m_imgMinIntensity, m_imgMaxIntensity);
    }

    auto vh = new Z3DVolume(img, downsampleSpacing, offset, m_volumes[0]->physicalToWorldMatrix());
    vh->setParentVolumeDimensions(
      glm::uvec3(m_imgPack->imgInfo().width, m_imgPack->imgInfo().height, m_imgPack->imgInfo().depth));
    vh->setParentVolumeOffset(m_volumes[0]->offset());
    m_zoomInVolumes.emplace_back(vh);
  }

  for (size_t i = 0; i < m_nChannels; ++i) {
    m_zoomInVolumes[i]->setVolColor(glm::vec3(m_imgPack->imgInfo().channelColors[i].r / 255.,
                                              m_imgPack->imgInfo().channelColors[i].g / 255.,
                                              m_imgPack->imgInfo().channelColors[i].b / 255.));
  }

  m_zoomInBound = m_zoomInVolumes[0]->worldBoundBox();
}

glm::vec3 Z3DVolumeFilter::getFirstHit3DPosition(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  success = false;
  const bool monoValid = m_transparentValid[eyeIndex(Mono)];
  const bool rightValid = m_transparentValid[eyeIndex(Right)];
  if (m_volumeRaycasterRenderer.hasVisibleRendering() && (monoValid || rightValid)) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderTarget& target = monoValid ? transparentTarget(Mono) : transparentTarget(Right);
    const uint32_t factor = static_cast<uint32_t>(m_interactionDownsample.get());
    if (m_interactionDownsampleActive && factor > 1) {
      pos2D /= factor;
      width /= static_cast<int>(factor);
      height /= static_cast<int>(factor);
    }
    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, target);
    res = glm::round(glm::applyMatrix(getVolumes()[0]->worldToPhysicalMatrix(), fpos3D));
    if (res.x >= 0 && res.x < m_imgPack->imgInfo().width && res.y >= 0 && res.y < m_imgPack->imgInfo().height &&
        res.z >= 0 && res.z < m_imgPack->imgInfo().depth) {
      success = true;
    }
  }
  return res;
}

glm::vec3 Z3DVolumeFilter::getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool& success)
{
  glm::vec3 res(-1);
  glm::vec3 des(-1);
  success = false;
  const bool monoValid = m_transparentValid[eyeIndex(Mono)];
  const bool rightValid = m_transparentValid[eyeIndex(Right)];
  if (m_volumeRaycasterRenderer.hasVisibleRendering() && (monoValid || rightValid)) {
    glm::ivec2 pos2D = glm::ivec2(x, height - y);
    Z3DRenderTarget& target = monoValid ? transparentTarget(Mono) : transparentTarget(Right);
    const uint32_t factor = static_cast<uint32_t>(m_interactionDownsample.get());
    if (m_interactionDownsampleActive && factor > 1) {
      pos2D /= factor;
      width /= static_cast<int>(factor);
      height /= static_cast<int>(factor);
    }
    glm::vec3 fpos3D = get3DPosition(pos2D, width, height, target);
    res = glm::round(glm::applyMatrix(getVolumes()[0]->worldToPhysicalMatrix(), fpos3D));
    if (res.x >= 0 && res.x < m_imgPack->imgInfo().width && res.y >= 0 && res.y < m_imgPack->imgInfo().height &&
        res.z >= 0 && res.z < m_imgPack->imgInfo().depth) {
      success = true;
    }

    if (success) {
      fpos3D = get3DPosition(pos2D, 1.0, width, height);
      des = glm::round(glm::applyMatrix(getVolumes()[0]->worldToPhysicalMatrix(), fpos3D));
      // LWARN() << "start" << res << "to" << des;
      if (glm::length(des - res) <= 1.f) { // res is last pixel along current ray direction
        return res;
      }
    }
  }

  // find maximum intensity voxel start from res along des direction
  if (success) {
    double maxInten = m_imgPack->value(res.x, res.y, res.z);
    glm::vec3 p = res;
    glm::vec3 d = des - res;
    float N = std::max(std::max(std::abs(d.x), std::abs(d.y)), std::abs(d.z));
    glm::vec3 stepSize = d / N;
    while (true) {
      p = p + stepSize;
      glm::vec3 roundP = glm::round(p);
      if (roundP.x < 0 || roundP.x >= m_imgPack->imgInfo().width || roundP.y < 0 ||
          roundP.y >= m_imgPack->imgInfo().height || roundP.z < 0 || roundP.z >= m_imgPack->imgInfo().depth) {
        break;
      }
      double inten = m_imgPack->value(roundP.x, roundP.y, roundP.z);
      if (inten > maxInten) {
        maxInten = inten;
        res = roundP;
      }
    }
    // LWARN() << "res" << res << "maxInten" << maxInten;
  }
  return res;
}

glm::vec3 Z3DVolumeFilter::get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderTarget& target)
{
  glm::mat4 projection = globalCamera().projectionMatrix(Mono);
  glm::mat4 modelview = globalCamera().viewMatrix(Mono);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  GLfloat WindowPosZ = target.depthAtPos(pos2D);

  CHECK_GL_ERROR
  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, WindowPosZ), modelview, projection, viewport);

  return pos;
}

glm::vec3 Z3DVolumeFilter::get3DPosition(glm::ivec2 pos2D, double depth, int width, int height)
{
  glm::mat4 projection = globalCamera().projectionMatrix(Mono);
  glm::mat4 modelview = globalCamera().viewMatrix(Mono);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  glm::vec3 pos = glm::unProject(glm::vec3(pos2D.x, pos2D.y, depth), modelview, projection, viewport);

  return pos;
}

void Z3DVolumeFilter::prepareDataForRaycaster(Z3DVolume* volume, Z3DEye eye)
{
  if (!m_volumeRaycasterRenderer.hasVisibleRendering()) {
    return;
  }

  glm::vec3 coordLuf = volume->physicalLUF();
  glm::vec3 coordRdb = volume->physicalRDB();

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

  m_2DImageQuad.clear();

  if (volume->is2DData()) { // for 2d image
    m_2DImageQuad = ZMesh::createImageSlice(volume->offset().z,
                                            glm::vec2(xCoordStart, yCoordStart),
                                            glm::vec2(xCoordEnd, yCoordEnd),
                                            glm::vec2(xTexCoordStart, yTexCoordStart),
                                            glm::vec2(xTexCoordEnd, yTexCoordEnd));
    m_2DImageQuad.transformVerticesByMatrix(volume->physicalToWorldMatrix());
  } else { // 3d volume but 2d slice
    if (m_zCut.lowerValue() == m_zCut.upperValue()) {
      m_2DImageQuad = ZMesh::createCubeSlice(zCoordStart,
                                             zTexCoordStart,
                                             2,
                                             glm::vec2(xCoordStart, yCoordStart),
                                             glm::vec2(xCoordEnd, yCoordEnd),
                                             glm::vec2(xTexCoordStart, yTexCoordStart),
                                             glm::vec2(xTexCoordEnd, yTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(volume->physicalToWorldMatrix());
    } else if (m_yCut.lowerValue() == m_yCut.upperValue()) {
      m_2DImageQuad = ZMesh::createCubeSlice(yCoordStart,
                                             yTexCoordStart,
                                             1,
                                             glm::vec2(xCoordStart, zCoordStart),
                                             glm::vec2(xCoordEnd, zCoordEnd),
                                             glm::vec2(xTexCoordStart, zTexCoordStart),
                                             glm::vec2(xTexCoordEnd, zTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(volume->physicalToWorldMatrix());
    } else if (m_xCut.lowerValue() == m_xCut.upperValue()) {
      m_2DImageQuad = ZMesh::createCubeSlice(xCoordStart,
                                             xTexCoordStart,
                                             0,
                                             glm::vec2(yCoordStart, zCoordStart),
                                             glm::vec2(yCoordEnd, zCoordEnd),
                                             glm::vec2(yTexCoordStart, zTexCoordStart),
                                             glm::vec2(yTexCoordEnd, zTexCoordEnd));
      m_2DImageQuad.transformVerticesByMatrix(volume->physicalToWorldMatrix());
    }
  }

  if (!m_2DImageQuad.empty()) {
    m_volumeRaycasterRenderer.clearQuads();
    m_volumeRaycasterRenderer.addQuad(m_2DImageQuad);
    return;
  }

  //  // 3d volume MIP
  //  if (m_volumeRaycasterRenderer->isMIPRendering()) {
  //    m_volumeRaycasterRenderer->clearSlices();
  //    float thre = 0.5;
  //    if (glm::dot(m_camera.getViewVector(), glm::vec3(0,0,1)) > thre)
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["ZB2F"]);
  //    else if (glm::dot(m_camera.getViewVector(), glm::vec3(0,0,-1)) > thre)
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["ZF2B"]);
  //    else if (glm::dot(m_camera.getViewVector(), glm::vec3(0,1,0)) > thre)
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["YB2F"]);
  //    else if (glm::dot(m_camera.getViewVector(), glm::vec3(0,-1,0)) > thre)
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["YF2B"]);
  //    else if (glm::dot(m_camera.getViewVector(), glm::vec3(1,0,0)) > thre)
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["XB2F"]);
  //    else
  //      m_volumeRaycasterRenderer->addSlice(m_cubeSerieSlices["XF2B"]);
  //    return;
  //  }

  // 3d volume Raycasting
  ZMesh cube = ZMesh::createCube(glm::vec3(xCoordStart, yCoordStart, zCoordStart),
                                 glm::vec3(xCoordEnd, yCoordEnd, zCoordEnd),
                                 glm::vec3(xTexCoordStart, yTexCoordStart, zTexCoordStart),
                                 glm::vec3(xTexCoordEnd, yTexCoordEnd, zTexCoordEnd));
  cube.transformVerticesByMatrix(volume->physicalToWorldMatrix());

  // enable culling
  glEnable(GL_CULL_FACE);

  m_exitTarget.resize(m_outputSize);
  m_entryTarget.resize(m_outputSize);

  m_rendererBase.setViewport(m_exitTarget.size());
  CHECK_GL_ERROR

  // render back texture
  GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
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

  m_volumeRaycasterRenderer.setEntryExitInfo(m_entryTarget.attachment(GL_COLOR_ATTACHMENT0),
                                             m_entryTarget.attachment(GL_COLOR_ATTACHMENT1),
                                             m_exitTarget.attachment(GL_COLOR_ATTACHMENT0),
                                             m_exitTarget.attachment(GL_COLOR_ATTACHMENT1));
}

void Z3DVolumeFilter::invalidateAllFRVolumeSlices()
{
  m_FRVolumeSlicesValidState.clear();
  m_FRVolumeSlicesValidState.resize(m_maxNumOfFullResolutionVolumeSlice, false);
}

void Z3DVolumeFilter::volumeChanged()
{
  Z3DVolume* volume = getVolumes()[0].get();
  bool is2DImage = (volume->is2DData());
  glm::uvec3 volDim = volume->originalDimensions();
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
  invalidateAllFRVolumeSlices();
  if (is2DImage) {
    m_useFRVolumeSlice.set(false);
    m_useFRVolumeSlice.setVisible(false);
    m_showXSlice.set(false);
    m_showYSlice.set(false);
    m_showZSlice.set(false);
    m_showXSlice2.set(false);
    m_showYSlice2.set(false);
    m_showZSlice2.set(false);
    m_showXSlice.setVisible(false);
    m_showYSlice.setVisible(false);
    m_showZSlice.setVisible(false);
    m_showXSlice2.setVisible(false);
    m_showYSlice2.setVisible(false);
    m_showZSlice2.setVisible(false);
  }

  m_volumeRaycasterRenderer.setChannels(getVolumes());
  for (size_t i = 0; i < m_transferFuncParas.size() && i < m_volumes.size(); ++i) {
    m_transferFuncParas[i]->setVolume(m_volumes[i].get());
  }
  // todo
  if (!is2DImage) {
    m_volumeSliceRenderer.setData(getVolumes(), m_sliceColormaps);
  }
}

size_t Z3DVolumeFilter::eyeIndex(Z3DEye eye)
{
  switch (eye) {
    case Mono:
      return 0;
    case Left:
      return 1;
    case Right:
      return 2;
  }
  return 0;
}

Z3DRenderTarget& Z3DVolumeFilter::transparentTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_transparentTargets[eyeIndex(eye)]);
}

const Z3DRenderTarget& Z3DVolumeFilter::transparentTarget(Z3DEye eye) const
{
  const auto& lease = m_transparentTargets[eyeIndex(eye)];
  CHECK(lease.renderTarget) << "transparent target requested before rendering";
  return *lease.renderTarget;
}

Z3DRenderTarget& Z3DVolumeFilter::opaqueTarget(Z3DEye eye)
{
  return ensureRenderTarget(m_opaqueTargets[eyeIndex(eye)]);
}

const Z3DRenderTarget& Z3DVolumeFilter::opaqueTarget(Z3DEye eye) const
{
  const auto& lease = m_opaqueTargets[eyeIndex(eye)];
  CHECK(lease.renderTarget) << "opaque target requested before rendering";
  return *lease.renderTarget;
}

Z3DRenderTarget& Z3DVolumeFilter::ensureRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (!lease.renderTarget || lease.renderTarget->size() != m_outputSize) {
    lease.release();
    CHECK_GT(m_outputSize.x, 0u);
    CHECK_GT(m_outputSize.y, 0u);
    lease = m_rendererBase.globalParas().scratchPool().acquireTempRenderTarget2D(m_outputSize);
  }
  return *lease.renderTarget;
}

void Z3DVolumeFilter::releaseAllRenderTargets()
{
  for (auto& lease : m_transparentTargets) {
    lease.release();
  }
  for (auto& lease : m_opaqueTargets) {
    lease.release();
  }
}

void Z3DVolumeFilter::markTargetsInvalid()
{
  m_transparentValid.fill(false);
  m_opaqueValid.fill(false);
}

} // namespace nim
