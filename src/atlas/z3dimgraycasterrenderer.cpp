#include "z3dimgraycasterrenderer.h"

#include "z3dtexture.h"
#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zimgcache.h"
#include "zimgregioncache.h"
#include "zlog.h"
#include "zcancellation.h"
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>
#include <QCoreApplication>

DEFINE_bool(atlas_clear_image_cache_after_rendering,
            false,
            "Clear image cache after rendering, for test, default is false");

DEFINE_bool(atlas_squeeze_image_cache_after_rendering, false, "Squeeze image cache after rendering, default is false");

DEFINE_uint32(atlas_volume_rendering_maximum_round,
              100,
              "Maximum number of rounds for volume rendering, default is 100");

namespace nim {

Z3DImgRaycasterRenderer::Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_samplingRate("Sampling Rate", 2.f, 0.01f, 20.f)
  , m_isoValue("ISO Value", 0.5f, 0.0f, 1.0f)
  , m_localMIPThreshold("Local MIP Threshold", 0.8f, 0.01f, 1.f)
  , m_compositingMode("Compositing")
  , m_entryTexCoordAndZeTexture(nullptr)
  , m_exitTexCoordAndZeTexture(nullptr)
  , m_opaque(false)
  // , m_alpha(1.0)
  , m_VAO(1)
{
  // m_gradientMode.addOptions("None", "Forward Differences", "Central Differences", "Filtered");
  // m_gradientMode.select("None");
  //  todo: add gradient
  // addParameter(m_gradientMode);

  // compositing modes
  m_compositingMode.addOptions("Direct Volume Rendering",
                               "Maximum Intensity Projection",
                               "MIP Opaque",
                               "Local MIP",
                               "Local MIP Opaque",
                               "ISO Surface",
                               "X Ray");
  m_compositingMode.select("MIP Opaque");

  connect(&m_compositingMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DImgRaycasterRenderer::adjustWidgets);
  connect(&m_compositingMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DImgRaycasterRenderer::compile);
  // connect(&m_gradientMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DImgRaycasterRenderer::compile);

  adjustWidgets();

  //  m_raycasterShader.bindFragDataLocation(0, "FragData0");
  //  m_raycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster.frag",
  //                                       m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.bindFragDataLocation(0, "FragData0");
  //  m_2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_transfun.frag",
  //                                     m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.bindFragDataLocation(0, "FragData0");
  //  m_volumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
  //  "volume_slice_with_transfun.frag",
  //                                                        m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.bindFragDataLocation(0, "FragData0");
  m_scRaycasterShader.loadFromSourceFile("pass.vert",
                                         "volume_raycaster_single_channel.frag",
                                         m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.bindFragDataLocation(0, "FragData0");
  m_sc2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert",
                                       "image2d_with_transfun_single_channel.frag",
                                       m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.bindFragDataLocation(0, "FragData0");
  m_scVolumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
                                                          "volume_slice_with_transfun_single_channel.frag",
                                                          m_rendererBase.generateHeader() + generateHeader());

  m_image3DSliceWithTransferfunBlockIDsShader.bindFragDataLocationForce(0, "FragData0");
  // m_image3DSliceWithTransferfunBlockIDsShader.bindFragDataLocationForce(1, "FragData1");
  m_image3DSliceWithTransferfunBlockIDsShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                                 "image3d_slice_with_transfun_blockID.frag",
                                                                 m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithTransferfunShader.bindFragDataLocation(0, "FragData0");
  m_image3DSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                         "image3d_slice_with_transfun.frag",
                                                         m_rendererBase.generateHeader() + generateHeader());

  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(0, "FragData0");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(1, "FragData1");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(2, "FragData2");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(3, "FragData3");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(4, "FragData4");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(5, "FragData5");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(6, "FragData6");
  m_image3DRaycasterBlockIDsShader.bindFragDataLocationForce(7, "FragData7");
  m_image3DRaycasterBlockIDsShader.loadFromSourceFile("pass.vert",
                                                      "image3d_raycaster_blockID.frag",
                                                      m_rendererBase.generateHeader() + generateHeader());
  m_image3DRaycasterShader.bindFragDataLocation(0, "FragData0");
  m_image3DRaycasterShader.loadFromSourceFile("pass.vert",
                                              "image3d_raycaster.frag",
                                              m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.bindFragDataLocation(0, "FragData0");
  m_mergeChannelShader.loadFromSourceFile("pass.vert",
                                          "image2d_array_compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
  m_copyTextureShader.bindFragDataLocation(0, "FragData0");
  m_copyTextureShader.loadFromSourceFile("pass.vert",
                                         "copy_raycaster_image.frag",
                                         m_rendererBase.generateHeader() + generateHeader());
  CHECK_GL_ERROR
}

QString Z3DImgRaycasterRenderer::compositeMode() const
{
  return m_compositingMode.get();
}

void Z3DImgRaycasterRenderer::setData(Z3DImg& img)
{
  m_img = &img;

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    m_volumeUniformNames.clear();
    m_volumeDimensionNames.clear();
    m_transferFuncUniformNames.clear();
    m_channelVisibleParas.clear();
    m_transferFuncParas.clear();
    m_texFilterModeParas.clear();
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      m_volumeUniformNames.push_back(QString("volume_%1").arg(i + 1));
      m_volumeDimensionNames.push_back(QString("volume_dimensions_%1").arg(i + 1));
      m_transferFuncUniformNames.push_back(QString("transfer_function_%1").arg(i + 1));
      m_channelVisibleParas.emplace_back(std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(i + 1), true));
      connect(m_channelVisibleParas[i].get(), &ZBoolParameter::valueChanged, this, &Z3DImgRaycasterRenderer::compile);
      m_transferFuncParas.emplace_back(
        std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(i + 1)));
      // m_transferFuncParas[i]->setVolume(m_img->volumes()[i].get());
      m_texFilterModeParas.emplace_back(
        std::make_unique<ZStringIntOptionParameter>(QString("Texture Filtering %1").arg(i + 1)));
      m_texFilterModeParas[i]->addOptionsWithData(std::make_pair(QString("Nearest"), static_cast<int>(GL_NEAREST)),
                                                  std::make_pair(QString("Linear"), static_cast<int>(GL_LINEAR)));
      m_texFilterModeParas[i]->select("Linear");
    }
  }
  compile();
  resetTransferFunctions();
  updateDisplayRanges();
}

void Z3DImgRaycasterRenderer::addQuad(const ZMesh& quad)
{
  if (quad.empty() || (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      (quad.numVertices() != quad.num2DTextureCoordinates() && quad.numVertices() != quad.num3DTextureCoordinates())) {
    LOG(ERROR) << "Input quad should be 2D slice with either 2D or 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
  m_entryTexCoordAndZeTexture = nullptr;
  m_exitTexCoordAndZeTexture = nullptr;
}

void Z3DImgRaycasterRenderer::setEntryExitInfo(const Z3DTexture* entryTexCoordAndZeTexture,
                                               const Z3DTexture* exitTexCoordAndZeTexture)
{
  m_entryTexCoordAndZeTexture = entryTexCoordAndZeTexture;
  m_exitTexCoordAndZeTexture = exitTexCoordAndZeTexture;
  m_quads.clear();
}

void Z3DImgRaycasterRenderer::adjustWidgets()
{
  m_isoValue.setVisible(m_compositingMode.isSelected("ISO Surface"));
  m_localMIPThreshold.setVisible(m_compositingMode.isSelected("Local MIP") ||
                                 m_compositingMode.isSelected("Local MIP Opaque"));
}

void Z3DImgRaycasterRenderer::bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const
{
  shader.setLogUniformLocationError(false);

  size_t idx = 0;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    if (!m_channelVisibleParas[i]->get()) {
      continue;
    }

    // volumes
    shader.bindTexture(m_volumeUniformNames[idx],
                       m_img->volumes()[i]->texture(),
                       m_texFilterModeParas[i]->associatedData(),
                       m_texFilterModeParas[i]->associatedData());
    shader.setUniform(m_volumeDimensionNames[idx], glm::vec3(m_img->volumes()[i]->dimensions()));

    // transfer functions
    shader.bindTexture(m_transferFuncUniformNames[idx++], m_transferFuncParas[i]->get().texture());

    CHECK_GL_ERROR
  }

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const
{
  shader.setLogUniformLocationError(false);

  shader.bindTexture(m_volumeUniformNames[0],
                     m_img->volumes()[idx]->texture(),
                     m_texFilterModeParas[idx]->associatedData(),
                     m_texFilterModeParas[idx]->associatedData());
  shader.setUniform(m_volumeDimensionNames[0], glm::vec3(m_img->volumes()[idx]->dimensions()));

  // transfer functions
  shader.bindTexture(m_transferFuncUniformNames[0], m_transferFuncParas[idx]->get().texture());

  CHECK_GL_ERROR

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::compile()
{
  //  m_raycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scRaycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_sc2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_scVolumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithTransferfunBlockIDsShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DRaycasterBlockIDsShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DRaycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_copyTextureShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DImgRaycasterRenderer::generateHeader()
{
  QString headerSource;

  size_t numVisibleChannels = 0;
  size_t numLevels = 1;
  if (m_img) {
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      if (m_channelVisibleParas[i]->get()) {
        ++numVisibleChannels;
      }
    }
    numLevels = m_img->numLevels();
  }
  headerSource += QString("#define LEVEL_COUNT %1\n").arg(numLevels);

  if (numVisibleChannels > 0) {
    headerSource += QString("#define NUM_VOLUMES %1\n").arg(numVisibleChannels);
  } else {
    headerSource += QString("#define NUM_VOLUMES 0\n");
    headerSource += "#define DISABLE_TEXTURE_COORD_OUTPUT\n";
  }

  //  if (!m_gradientMode.isSelected("None"))
  //    headerSource += "#define USE_GRADIENTS\n";

  if (m_compositingMode.isSelected("Direct Volume Rendering")) {
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeDVR(result, color, currentRayLength, rayDepth);\n";
  } else if (m_compositingMode.isSelected("ISO Surface")) {
    headerSource += "#define ISO\n";
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeISO(result, color, currentRayLength, rayDepth, iso_value);\n";
  } else if (m_compositingMode.isSelected("Maximum Intensity Projection")) {
    headerSource += "#define MIP\n";
  } else if (m_compositingMode.isSelected("Local MIP")) {
    headerSource += "#define MIP\n";
    headerSource += "#define LOCAL_MIP\n";
  } else if (m_compositingMode.isSelected("X Ray")) {
    headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
    headerSource += "compositeXRay(result, color, currentRayLength, rayDepth);\n";
  } else if (m_compositingMode.isSelected("MIP Opaque")) {
    headerSource += "#define MIP\n";
    headerSource += "#define RESULT_OPAQUE\n";
  } else if (m_compositingMode.isSelected("Local MIP Opaque")) {
    headerSource += "#define MIP\n";
    headerSource += "#define LOCAL_MIP\n";
    headerSource += "#define RESULT_OPAQUE\n";
  }

  if (!m_quads.empty() || m_compositingMode.isSelected("Maximum Intensity Projection") ||
      m_compositingMode.isSelected("Local MIP") || m_compositingMode.isSelected("MIP Opaque") ||
      m_compositingMode.isSelected("Local MIP Opaque")) {
    headerSource += "#define MAX_PROJ_MERGE\n";
  }

  return headerSource;
}

void Z3DImgRaycasterRenderer::render(Z3DEye eye)
{
  // m_lastRenderingIsFastRendering = false;

  if (!hasVisibleRendering()) {
    return;
  }

  if (m_quads.empty()) {
    if (m_entryTexCoordAndZeTexture == nullptr || m_exitTexCoordAndZeTexture == nullptr) {
      return;
    }
  } else {
    for (auto& quad : m_quads) {
      if (m_img->is2DData() && quad.numVertices() != quad.num2DTextureCoordinates()) {
        return;
      }
      if (m_img->is3DData() && quad.numVertices() != quad.num3DTextureCoordinates()) {
        return;
      }
    }
  }

  std::vector<size_t> visibleIdxs;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    if (m_channelVisibleParas[i]->get()) {
      visibleIdxs.push_back(i);
    }
  }

  if (!m_quads.empty()) { // 2d image or slice from 3d volume
    if (m_img->is2DData()) { // image is 2D
      render2DImage(eye, visibleIdxs);
    } else { // image is 3D, but a 2D slice will be shown
      if (!m_fastRendering && m_img->isVolumeDownsampled()) {
        render2DSliceOf3DImage(eye, visibleIdxs);
      } else {
        // m_lastRenderingIsFastRendering = true;

        render2DSliceOf3DImageFast(eye, visibleIdxs);
      }
    }
  } else { // 3d volume raycasting
    ZBenchTimer bta("all");
    if (!m_fastRendering && m_img->isVolumeDownsampled()) {
      render3DImage(eye, visibleIdxs);
    } else {
      // m_lastRenderingIsFastRendering = true;

      render3DImageFast(eye, visibleIdxs);
    }
    STOP_AND_LOG(bta)
  }

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", m_layerTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", m_layerTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

void Z3DImgRaycasterRenderer::renderPicking(Z3DEye /*unused*/) {}

void Z3DImgRaycasterRenderer::resetTransferFunctions()
{
  for (size_t i = 0; i < m_transferFuncParas.size(); ++i) {
    if (m_opaque) {
      m_transferFuncParas[i]->get().reset(0.0,
                                          1.0,
                                          glm::vec4(0.f),
                                          glm::vec4(m_img->channelColor(i).r / 255.,
                                                    m_img->channelColor(i).g / 255.,
                                                    m_img->channelColor(i).b / 255.,
                                                    1.f));
      m_transferFuncParas[i]->get().addKey(ZColorMapKey(0.001, glm::vec4(0.01f, 0.01f, 0.01f, 0.0f)));
      m_transferFuncParas[i]->get().addKey(ZColorMapKey(0.01, glm::vec4(0.01f, 0.01f, 0.01f, 1.0f)));
    } else {
      m_transferFuncParas[i]->get().reset(0.0,
                                          1.0,
                                          glm::vec4(0.f),
                                          glm::vec4(m_img->channelColor(i).r / 255.,
                                                    m_img->channelColor(i).g / 255.,
                                                    m_img->channelColor(i).b / 255.,
                                                    1.f));
      // m_transferFuncParas[i]->get().addKey(ZColorMapKey(0.1, glm::vec4(m_volumes[i]->volColor(), 1.f) *
      //                                                   glm::vec4(.1f,.1f,.1f,0.f)));
    }
  }
}

void Z3DImgRaycasterRenderer::updateDisplayRanges()
{
  for (size_t i = 0; i < m_transferFuncParas.size(); ++i) {
    m_transferFuncParas[i]->setMinMaxIntensity(m_img->displayRange(i).x, m_img->displayRange(i).y);
  }
}

bool Z3DImgRaycasterRenderer::hasVisibleRendering() const
{
  if (m_img) {
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      if (m_channelVisibleParas[i]->get()) {
        return true;
      }
    }
  }
  return false;
}

void Z3DImgRaycasterRenderer::render2DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  m_sc2dImageShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_sc2dImageShader, eye);

  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(m_VAO, m_sc2dImageShader, quad);
    }

  } else {
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      m_layerTarget->attachSlice(j);
      m_layerTarget->bind();
      m_layerTarget->clear();
      bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_sc2dImageShader, quad);
      }

      m_layerTarget->release();
    }
  }

  m_sc2dImageShader.release();
}

void Z3DImgRaycasterRenderer::render2DSliceOf3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  auto cancellationToken = m_rendererBase.globalParas().cancellationSource
                             ? m_rendererBase.globalParas().cancellationSource->getToken()
                             : folly::CancellationToken();

  float n = m_rendererBase.camera().nearDist();
  glm::vec2 pixelEyeSpaceSize = m_rendererBase.camera().frustumNearPlaneSize() / glm::vec2(m_layerTarget->size());
  float ze_to_screen_pixel_voxel_size =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * m_rendererBase.globalParas().devicePixelRatio.get();

  size_t idx = 0;
  for (auto c : visibleIdxs) {
    LOG(INFO) << "";
    ZBenchTimer bt("render and collect blockids");

    if (m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4 != m_blockIDs.size()) {
      m_blockIDs.resize(m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4);
      LOG(INFO) << m_blockIDs.size();
    }

    m_image3DSliceWithTransferfunBlockIDsShader.bind();
    m_image3DSliceWithTransferfunBlockIDsShader.setUniform("ze_to_screen_pixel_voxel_size",
                                                           ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithTransferfunBlockIDsShader.setProjectionViewMatrixUniform(
      m_rendererBase.camera().projectionViewMatrix(eye));
    m_image3DSliceWithTransferfunBlockIDsShader.setViewMatrixUniform(m_rendererBase.camera().viewMatrix(eye));

    // render block ids
    std::vector<uint32_t> missingBlockIDs;
    tbb::concurrent_unordered_set<uint32_t> ccSet;

    const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

    m_img->bindFullResBlockIDsShader(m_image3DSliceWithTransferfunBlockIDsShader, c);

    for (auto& quad : m_quads) {
      m_blockIDsRenderTarget->bind();
      glDrawBuffers(1, g_drawBuffers);
      glClear(GL_COLOR_BUFFER_BIT);

      renderTriangleList(m_VAO, m_image3DSliceWithTransferfunBlockIDsShader, quad);

      m_blockIDsRenderTarget->release();

      m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0)
        ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
      tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                        [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                          ccSet.insert(range.begin(), range.end()); // inserts a sequence
                        });

      ccSet.unsafe_erase(0_u32);
      ccSet.unsafe_erase(std::numeric_limits<uint32_t>::max());
      missingBlockIDs.insert(missingBlockIDs.end(), ccSet.begin(), ccSet.end());
      ccSet.clear();
    }

    m_image3DSliceWithTransferfunBlockIDsShader.release();
    // glFinish();
    STOP_AND_LOG(bt)

    m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, c, cancellationToken);

    bt.resetAndStart("render image3d slice");
    // render channels one by one
    m_image3DSliceWithTransferfunShader.bind();

    m_image3DSliceWithTransferfunShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithTransferfunShader.setProjectionViewMatrixUniform(
      m_rendererBase.camera().projectionViewMatrix(eye));
    m_image3DSliceWithTransferfunShader.setViewMatrixUniform(m_rendererBase.camera().viewMatrix(eye));

    if (visibleIdxs.size() == 1) {
      m_img->bindFullResRenderShader(m_image3DSliceWithTransferfunShader, c);
      m_image3DSliceWithTransferfunShader.bindTexture("transfer_function", m_transferFuncParas[c]->get().texture());
      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_image3DSliceWithTransferfunShader, quad);
      }
    } else {
      m_layerTarget->attachSlice(idx++);
      m_layerTarget->bind();
      m_layerTarget->clear();

      m_img->bindFullResRenderShader(m_image3DSliceWithTransferfunShader, c);
      m_image3DSliceWithTransferfunShader.bindTexture("transfer_function", m_transferFuncParas[c]->get().texture());
      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_image3DSliceWithTransferfunShader, quad);
      }

      m_layerTarget->release();
    }

    m_image3DSliceWithTransferfunShader.release();
    // glFinish();
    STOP_AND_LOG(bt)
  }
}

void Z3DImgRaycasterRenderer::render2DSliceOf3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  m_scVolumeSliceWithTransferfunShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceWithTransferfunShader, eye);

  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, quad);
    }
  } else {
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      m_layerTarget->attachSlice(j);
      m_layerTarget->bind();
      m_layerTarget->clear();

      bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, quad);
      }

      m_layerTarget->release();
    }
  }

  m_scVolumeSliceWithTransferfunShader.release();
}

void Z3DImgRaycasterRenderer::render3DImage(Z3DEye /*eye*/, const std::vector<size_t>& visibleIdxs)
{
  auto cancellationToken = m_rendererBase.globalParas().cancellationSource
                             ? m_rendererBase.globalParas().cancellationSource->getToken()
                             : folly::CancellationToken();

  float n = m_rendererBase.camera().nearDist();
  float f = m_rendererBase.camera().farDist();
  glm::vec2 pixelEyeSpaceSize = m_rendererBase.camera().frustumNearPlaneSize() / glm::vec2(m_layerTarget->size());
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float ze_to_zw_a = f * n / (f - n);
  float ze_to_zw_b = 0.5f * (f + n) / (f - n) + 0.5f;
  float ze_to_screen_pixel_voxel_size =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * m_rendererBase.globalParas().devicePixelRatio.get();
  VLOG(1) << n << " " << f << " " << ze_to_screen_pixel_voxel_size << " " << pixelEyeSpaceSize;

  CHECK(m_lastImageRenderTarget->size() == m_layerTarget->size()) << m_lastImageRenderTarget->size();
  CHECK(m_lastImageRenderTarget->size() == m_blockIDsRenderTarget->size())
    << m_lastImageRenderTarget->size() << " " << m_blockIDsRenderTarget->size();

  static int dummyidx = 0;
  ++dummyidx;

  size_t idx = 0;
  for (auto c : visibleIdxs) {
    for (uint32_t round = 0; round < FLAGS_atlas_volume_rendering_maximum_round; ++round) {
      LOG(INFO) << "round " << round;
      ZBenchTimer btrb("render blockids");

      processEventsAndMaybeCancel(cancellationToken);

      m_image3DRaycasterBlockIDsShader.bind();
      //          m_image3DRaycasterBlockIDsShader.setUniform("screen_dim_RCP",
      //                                                      1.f / glm::vec2(m_blockIDsRenderTarget->size()));
      m_image3DRaycasterBlockIDsShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

      // entry exit points
      m_image3DRaycasterBlockIDsShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordAndZeTexture);
      m_image3DRaycasterBlockIDsShader.bindTexture("ray_exit_tex_coord", m_exitTexCoordAndZeTexture);

      m_image3DRaycasterBlockIDsShader.setUniform("sampling_rate", m_samplingRate.get());

      // render block ids
      const GLenum g_drawBuffers[] = {
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3,
        GL_COLOR_ATTACHMENT4,
        GL_COLOR_ATTACHMENT5,
        GL_COLOR_ATTACHMENT6,
        GL_COLOR_ATTACHMENT7,
      };

      if (round == 0) {
        m_lastImageRenderTarget->bind();
        glDrawBuffers(2, g_drawBuffers);
        m_lastImageRenderTarget->clear();
        m_lastImageRenderTarget->release();
      }

      m_blockIDsRenderTarget->bind();
      glDrawBuffers(8, g_drawBuffers);
      glClear(GL_COLOR_BUFFER_BIT);

      m_img->bindFullResBlockIDsShader(m_image3DRaycasterBlockIDsShader, c);
      m_image3DRaycasterBlockIDsShader.bindTexture("last_ray_depth",
                                                   m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(m_VAO, m_image3DRaycasterBlockIDsShader);

      m_blockIDsRenderTarget->release();
      m_image3DRaycasterBlockIDsShader.release();
      // glFinish();
      STOP_AND_LOG(btrb)

      processEventsAndMaybeCancel(cancellationToken);

      ZBenchTimer btcb("collect blockids");
      // check missed blocks and upload
      const Z3DTexture* missingBlockIDsTexture = m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT0);
      if (missingBlockIDsTexture->numPixels() * 4 != m_blockIDs.size()) {
        m_blockIDs.resize(missingBlockIDsTexture->numPixels() * 4);
        LOG(INFO) << m_blockIDs.size();
      }
      missingBlockIDsTexture->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

#if 0
          if (!QFileInfo("/Users/feng/Downloads/test_missid.tif").exists()) {
            ZImg img;
            img.wrapData(m_blockIDs.data(), missingBlockIDsTexture->width(), missingBlockIDsTexture->height(), 1, 4);
            ZImg outImg = img;
            ZImgFormat::CXYZtoXYZC(img, outImg);
            outImg.flip(nim::Dimension::Y);
            outImg.save("/Users/feng/Downloads/test_missid.tif");
          }
#endif

      tbb::concurrent_unordered_set<uint32_t> ccSet;

      tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                        [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                          ccSet.insert(range.begin(), range.end()); // inserts a sequence
                        });

      processEventsAndMaybeCancel(cancellationToken);

      bool attachment0ContainsValidMissingBlocks = ccSet.size() > 2;
      bool attachment0ContainsLastBlock = false;
      if (!attachment0ContainsValidMissingBlocks) {
        attachment0ContainsLastBlock = ccSet.find(std::numeric_limits<uint32_t>::max()) != ccSet.end();
        ccSet.unsafe_erase(0_u32);
        ccSet.unsafe_erase(std::numeric_limits<uint32_t>::max());
        attachment0ContainsValidMissingBlocks = !ccSet.empty();
      }

      size_t numberBlock = ccSet.size();
      bool hasEnoughMissingIDs = false;

      if (attachment0ContainsValidMissingBlocks) {
        hasEnoughMissingIDs = numberBlock > m_img->numCachedImages(c);
        if (!hasEnoughMissingIDs) {
          m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT1)
            ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

          tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                            [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                              ccSet.insert(range.begin(), range.end()); // inserts a sequence
                            });

          hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
        }
      } else if (!attachment0ContainsLastBlock) {
        STOP_AND_LOG(btcb)
        LOG(INFO) << "no blocks to render";
        break;
      }

      processEventsAndMaybeCancel(cancellationToken);

      bool lastRound = false;
      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT2)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT3)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT4)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT5)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT6)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock != ccSet.size()) {
        numberBlock = ccSet.size();
        m_blockIDsRenderTarget->attachment(GL_COLOR_ATTACHMENT7)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);
        // LOG(INFO) << m_img->numCachedImages() << " " << numberBlock << " " << ccSet.size();
      } else {
        lastRound = !hasEnoughMissingIDs;
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (!hasEnoughMissingIDs && numberBlock == ccSet.size()) {
        lastRound = true;
      }

      std::vector<uint32_t> missingBlockIDs;

      ccSet.unsafe_erase(0_u32);
      if (ccSet.find(std::numeric_limits<uint32_t>::max()) != ccSet.end() || attachment0ContainsLastBlock) {
        VLOG(1) << "use last block";
      }
      ccSet.unsafe_erase(std::numeric_limits<uint32_t>::max());
      if (!ccSet.empty()) {
        CHECK(ccSet.size() < m_img->numCachedImages(c) * 10);
        missingBlockIDs.reserve(ccSet.size());
        missingBlockIDs.insert(missingBlockIDs.end(), ccSet.begin(), ccSet.end());
        if ((round % 2 == 1) && hasEnoughMissingIDs) {
          std::sort(missingBlockIDs.begin(), missingBlockIDs.end(), std::greater<>());
        } else {
          std::sort(missingBlockIDs.begin(), missingBlockIDs.end());
        }
      }
      // LOG(INFO) << missingBlockIDs.size() << " " << usedBlockIDs.size();
      STOP_AND_LOG(btcb)

      processEventsAndMaybeCancel(cancellationToken);

      lastRound = m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, c, cancellationToken) && lastRound;

      processEventsAndMaybeCancel(cancellationToken);

      ZBenchTimer btri("render image");
      // render channels one by one
      m_image3DRaycasterShader.bind();

      m_image3DRaycasterShader.setUniform("ze_to_zw_b", ze_to_zw_b);
      m_image3DRaycasterShader.setUniform("ze_to_zw_a", ze_to_zw_a);
      m_image3DRaycasterShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

      // entry exit points
      m_image3DRaycasterShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordAndZeTexture);
      m_image3DRaycasterShader.bindTexture("ray_exit_tex_coord", m_exitTexCoordAndZeTexture);

      m_image3DRaycasterShader.bindTexture("last_color", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_image3DRaycasterShader.bindTexture("last_ray_depth", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1));

      if (m_compositingMode.get() == "ISO Surface") {
        m_image3DRaycasterShader.setUniform("iso_value", m_isoValue.get());
      }

      if (m_compositingMode.get() == "Local MIP" || m_compositingMode.get() == "Local MIP Opaque") {
        m_image3DRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold.get());
      }

      m_image3DRaycasterShader.setUniform("sampling_rate", m_samplingRate.get());

      m_currentImageRenderTarget->bind();
      glDrawBuffers(2, g_drawBuffers);
      m_currentImageRenderTarget->clear();

      m_img->bindFullResRenderShader(m_image3DRaycasterShader, c);
      m_image3DRaycasterShader.bindTexture("transfer_function", m_transferFuncParas[c]->get().texture());
      renderScreenQuad(m_VAO, m_image3DRaycasterShader);

      m_currentImageRenderTarget->release();

      m_image3DRaycasterShader.release();
      // glFinish();
      STOP_AND_LOG(btri)

      auto filen = QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att0.tif", dummyidx, c, round));
      m_currentImageRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage(filen);
      filen = QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att1.tif", dummyidx, c, round));
      m_currentImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1)->saveAsRGBAFloatImage(filen);

      std::swap(m_lastImageRenderTarget, m_currentImageRenderTarget);

      // m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage(QString("/Users/feng/Downloads/att1_repeat%1.nim").arg(round));
      // m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1)->saveAsRGBFloatImage(QString("/Users/feng/Downloads/att2_repeat%1.nim").arg(round));

      if (lastRound) {
        break;
      }
    }

    processEventsAndMaybeCancel(cancellationToken);

    if (visibleIdxs.size() == 1) {
      m_copyTextureShader.bind();
      m_copyTextureShader.bindTexture("color_texture", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader.bindTexture("depth_texture", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(m_VAO, m_copyTextureShader);
      m_copyTextureShader.release();
    } else {
      m_layerTarget->attachSlice(idx++);
      m_layerTarget->bind();
      m_layerTarget->clear();

      m_copyTextureShader.bind();
      m_copyTextureShader.bindTexture("color_texture", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader.bindTexture("depth_texture", m_lastImageRenderTarget->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(m_VAO, m_copyTextureShader);
      m_copyTextureShader.release();

      m_layerTarget->release();
    }
  }

  processEventsAndMaybeCancel(cancellationToken);

  if (FLAGS_atlas_clear_image_cache_after_rendering) {
    ZImgCache::instance().clear();
    ZImgRegionCache::instance().clear();
  } else if (FLAGS_atlas_squeeze_image_cache_after_rendering) {
    ZImgCache::instance().squeeze();
    ZImgRegionCache::instance().squeeze();
  }
  LOG(INFO) << "image cache size: " << ZImgCache::instance().size();
  LOG(INFO) << "image block cache size: " << ZImgRegionCache::instance().size();
}

void Z3DImgRaycasterRenderer::render3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  m_scRaycasterShader.bind();

  if (!GLVersionGE(3, 0)) {
    m_rendererBase.setGlobalShaderParameters(m_scRaycasterShader, eye);
  }

  float n = m_rendererBase.camera().nearDist();
  float f = m_rendererBase.camera().farDist();
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float a = f * n / (f - n);
  float b = 0.5f * (f + n) / (f - n) + 0.5f;
  m_scRaycasterShader.setUniform("ze_to_zw_b", b);
  m_scRaycasterShader.setUniform("ze_to_zw_a", a);

  // entry exit points
  m_scRaycasterShader.bindTexture("ray_entry_tex_coord", m_entryTexCoordAndZeTexture);
  m_scRaycasterShader.bindTexture("ray_exit_tex_coord", m_exitTexCoordAndZeTexture);

  if (m_compositingMode.get() == "ISO Surface") {
    m_scRaycasterShader.setUniform("iso_value", m_isoValue.get());
  }

  if (m_compositingMode.get() == "Local MIP" || m_compositingMode.get() == "Local MIP Opaque") {
    m_scRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold.get());
  }

  m_scRaycasterShader.setUniform("sampling_rate", m_samplingRate.get());

  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[0]);
    renderScreenQuad(m_VAO, m_scRaycasterShader);
  } else {
    for (size_t i = 0; i < visibleIdxs.size(); ++i) {
      m_layerTarget->attachSlice(i);
      m_layerTarget->bind();
      m_layerTarget->clear();

      bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[i]);
      renderScreenQuad(m_VAO, m_scRaycasterShader);

      m_layerTarget->release();
    }
  }

  m_scRaycasterShader.release();
}

} // namespace nim
