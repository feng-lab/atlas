#include "z3dimgraycasterrenderer.h"

#include "z3dtexture.h"
#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zimgcache.h"
#include "zimgregioncache.h"
#include "zlog.h"
#include "zcancellation.h"
#include "zstatisticsutils.h"
#include "z3dscratchresourcepool.h"
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>

DEFINE_uint32(atlas_volume_rendering_maximum_round,
              100,
              "Maximum number of rounds for volume rendering, default is 100");

#if defined(__linux__)
DEFINE_bool(atlas_debug_texture_output, false, "produce debug intermediate texture to /data/testoutput");
#endif

namespace nim {

Z3DImgRaycasterRenderer::Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_opaque(false)
  // , m_alpha(1.0)
  , m_VAO(1)
  , m_textureAndEyeCoordinateRenderer(m_rendererBase)
{
  // m_gradientMode.addOptions("None", "Forward Differences", "Central Differences", "Filtered");
  // m_gradientMode.select("None");
  //  todo: add gradient
  // addParameter(m_gradientMode);

  //  m_raycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster.frag",
  //                                       m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert", "image2d_with_transfun.frag",
  //                                     m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
  //  "volume_slice_with_transfun.frag",
  //                                                        m_rendererBase.generateHeader() + generateHeader());

  QString headerSource = m_rendererBase.generateHeader() + generateHeader();
  m_scRaycasterShader.loadFromSourceFile("pass.vert", "volume_raycaster_single_channel.frag", headerSource);
  m_sc2dImageShader.loadFromSourceFile("transform_with_2dtexture.vert",
                                       "image2d_with_transfun_single_channel.frag",
                                       headerSource);
  m_scVolumeSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture.vert",
                                                          "volume_slice_with_transfun_single_channel.frag",
                                                          headerSource);

  m_image3DSliceWithTransferfunBlockIDsShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                                 "image3d_slice_with_transfun_blockID.frag",
                                                                 headerSource);
  m_image3DSliceWithTransferfunShader.loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                         "image3d_slice_with_transfun.frag",
                                                         headerSource);

  m_image3DRaycasterBlockIDsShader.loadFromSourceFile("pass.vert", "image3d_raycaster_blockID.frag", headerSource);
  m_image3DRaycasterShader.loadFromSourceFile("pass.vert", "image3d_raycaster.frag", headerSource);
  m_mergeChannelShader.loadFromSourceFile("pass.vert", "image2d_array_compositor.frag", headerSource);
  m_copyTextureShader.loadFromSourceFile("pass.vert", "copy_raycaster_image.frag", headerSource);
  CHECK_GL_ERROR

  // Internal targets; allocate and initialize per-eye ping-pong
  for (int e = 0; e < 3; ++e) {
    m_imageRenderTarget1s[e] = std::make_unique<Z3DRenderTarget>(glm::uvec2(32, 32));
    m_imageRenderTarget2s[e] = std::make_unique<Z3DRenderTarget>(glm::uvec2(32, 32));
    // last
    auto* c0 = new Z3DTexture(GLint(GL_RGBA16),
                              glm::uvec3(m_imageRenderTarget1s[e]->size().x, m_imageRenderTarget1s[e]->size().y, 1),
                              GL_RGBA,
                              GL_FLOAT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    auto* d0 = new Z3DTexture(GLint(GL_RG32F),
                              glm::uvec3(m_imageRenderTarget1s[e]->size().x, m_imageRenderTarget1s[e]->size().y, 1),
                              GL_RG,
                              GL_FLOAT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    m_imageRenderTarget1s[e]->attachTextureToFBO(c0, GL_COLOR_ATTACHMENT0, true);
    m_imageRenderTarget1s[e]->attachTextureToFBO(d0, GL_COLOR_ATTACHMENT1, true);
    m_imageRenderTarget1s[e]->isFBOComplete();

    // current
    auto* c1 = new Z3DTexture(GLint(GL_RGBA16),
                              glm::uvec3(m_imageRenderTarget2s[e]->size().x, m_imageRenderTarget2s[e]->size().y, 1),
                              GL_RGBA,
                              GL_FLOAT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    auto* d1 = new Z3DTexture(GLint(GL_RG32F),
                              glm::uvec3(m_imageRenderTarget2s[e]->size().x, m_imageRenderTarget2s[e]->size().y, 1),
                              GL_RG,
                              GL_FLOAT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    m_imageRenderTarget2s[e]->attachTextureToFBO(c1, GL_COLOR_ATTACHMENT0, true);
    m_imageRenderTarget2s[e]->attachTextureToFBO(d1, GL_COLOR_ATTACHMENT1, true);
    m_imageRenderTarget2s[e]->isFBOComplete();
  }
  m_lastImageRenderTargets[0] = m_imageRenderTarget1s[0].get();
  m_lastImageRenderTargets[1] = m_imageRenderTarget1s[1].get();
  m_lastImageRenderTargets[2] = m_imageRenderTarget1s[2].get();
  m_currentImageRenderTargets[0] = m_imageRenderTarget2s[0].get();
  m_currentImageRenderTargets[1] = m_imageRenderTarget2s[1].get();
  m_currentImageRenderTargets[2] = m_imageRenderTarget2s[2].get();

  // Render targets (layer, block-id, entry/exit) are acquired from the scratch pool on demand.
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
    // m_texFilterModeParas.clear();
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      m_volumeUniformNames.push_back(QString("volume_%1").arg(i + 1));
      m_volumeDimensionNames.push_back(QString("volume_dimensions_%1").arg(i + 1));
      m_transferFuncUniformNames.push_back(QString("transfer_function_%1").arg(i + 1));
      m_channelVisibleParas.emplace_back(std::make_unique<ZBoolParameter>(QString("Show Channel %1").arg(i + 1), true));
      connect(m_channelVisibleParas[i].get(), &ZBoolParameter::valueChanged, this, &Z3DImgRaycasterRenderer::compile);
      m_transferFuncParas.emplace_back(
        std::make_unique<Z3DTransferFunctionParameter>(QString("Transfer Function %1").arg(i + 1)));
      // m_transferFuncParas[i]->setVolume(m_img->volumes()[i].get());

      //      m_texFilterModeParas.emplace_back(
      //        std::make_unique<ZStringIntOptionParameter>(QString("Texture Filtering %1").arg(i + 1)));
      //      m_texFilterModeParas[i]->addOptionsWithData(std::make_pair(QString("Nearest"),
      //      static_cast<int>(GL_NEAREST)),
      //                                                  std::make_pair(QString("Linear"),
      //                                                  static_cast<int>(GL_LINEAR)));
      //      m_texFilterModeParas[i]->select("Linear");
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
  m_entryExitTexCoordAndZeTexture = nullptr;
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
    shader.bindTexture(m_volumeUniformNames[idx], m_img->volumes()[i]->texture());
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

  shader.bindTexture(m_volumeUniformNames[0], m_img->volumes()[idx]->texture());
  shader.setUniform(m_volumeDimensionNames[0], glm::vec3(m_img->volumes()[idx]->dimensions()));

  // transfer functions
  shader.bindTexture(m_transferFuncUniformNames[0], m_transferFuncParas[idx]->get().texture());

  // m_transferFuncParas[idx]->get().texture()->saveAsColorImage("/Users/feng/Downloads/abcd_tf.tif");
  // m_img->volumes()[idx]->texture()->saveAsColorImage("/Users/feng/Downloads/abcd_v.tif");

  CHECK_GL_ERROR

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::compile()
{
  //  m_raycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  QString headerSource = m_rendererBase.generateHeader() + generateHeader();
  m_scRaycasterShader.setHeaderAndRebuild(headerSource);
  m_sc2dImageShader.setHeaderAndRebuild(headerSource);
  m_scVolumeSliceWithTransferfunShader.setHeaderAndRebuild(headerSource);
  m_image3DSliceWithTransferfunBlockIDsShader.setHeaderAndRebuild(headerSource);
  m_image3DSliceWithTransferfunShader.setHeaderAndRebuild(headerSource);
  m_image3DRaycasterBlockIDsShader.setHeaderAndRebuild(headerSource);
  m_image3DRaycasterShader.setHeaderAndRebuild(headerSource);
  m_mergeChannelShader.setHeaderAndRebuild(headerSource);
  m_copyTextureShader.setHeaderAndRebuild(headerSource);
}

void Z3DImgRaycasterRenderer::prepareEntryExit(const ZMesh& clipped, bool flipped, Z3DEye eye, const glm::uvec2& size)
{
  // VLOG(1) << "prepareEntryExit";
  // Release any previous entry/exit lease before acquiring a new one to
  // avoid growing the scratch pool unnecessarily.
  if (m_entryExitLease) {
    m_entryExitLease.release();
  }
  m_quads.clear();

  // Acquire entry/exit RT from scratch pool (2-layer RGBA32F array)
  m_entryExitLease = m_rendererBase.globalParas().scratchPool().acquireEntryExitRenderTarget(size, 2);

  glEnable(GL_CULL_FACE);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

  // Back faces to slice 1
  m_entryExitLease.renderTarget->attachSlice(1);
  m_entryExitLease.renderTarget->bind();
  glDrawBuffers(1, g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);
  glCullFace(flipped ? GL_BACK : GL_FRONT);
  m_rendererBase.setViewport(size);
  m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
  m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
  m_entryExitLease.renderTarget->release();

  // Front faces to slice 0
  m_entryExitLease.renderTarget->attachSlice(0);
  m_entryExitLease.renderTarget->bind();
  glDrawBuffers(1, g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);
  glCullFace(flipped ? GL_FRONT : GL_BACK);
  m_rendererBase.setViewport(size);
  m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
  m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
  m_entryExitLease.renderTarget->release();

  // restore
  glCullFace(GL_BACK);
  glDisable(GL_CULL_FACE);

  // Hand off to raycaster
  m_entryExitTexCoordAndZeTexture = m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
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
  // VLOG(1) << numVisibleChannels << " generate header";

  //  if (!m_gradientMode.isSelected("None"))
  //    headerSource += "#define USE_GRADIENTS\n";

  const bool useMIPMerge = m_compositingModeValue == ImgCompositingMode::MaximumIntensityProjection ||
                           m_compositingModeValue == ImgCompositingMode::LocalMIP ||
                           m_compositingModeValue == ImgCompositingMode::MIPOpaque ||
                           m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque;

  switch (m_compositingModeValue) {
    case ImgCompositingMode::DirectVolumeRendering:
      headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
      headerSource += "compositeDVR(result, color, currentRayLength, rayDepth);\n";
      break;
    case ImgCompositingMode::IsoSurface:
      headerSource += "#define ISO\n";
      headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
      headerSource += "compositeISO(result, color, currentRayLength, rayDepth, iso_value);\n";
      break;
    case ImgCompositingMode::MaximumIntensityProjection:
      headerSource += "#define MIP\n";
      break;
    case ImgCompositingMode::LocalMIP:
      headerSource += "#define MIP\n";
      headerSource += "#define LOCAL_MIP\n";
      break;
    case ImgCompositingMode::XRay:
      headerSource += "#define COMPOSITING(result, color, currentRayLength, rayDepth) ";
      headerSource += "compositeXRay(result, color, currentRayLength, rayDepth);\n";
      break;
    case ImgCompositingMode::MIPOpaque:
      headerSource += "#define MIP\n";
      headerSource += "#define RESULT_OPAQUE\n";
      break;
    case ImgCompositingMode::LocalMIPOpaque:
      headerSource += "#define MIP\n";
      headerSource += "#define LOCAL_MIP\n";
      headerSource += "#define RESULT_OPAQUE\n";
      break;
  }

  if (!m_quads.empty() || useMIPMerge) {
    headerSource += "#define MAX_PROJ_MERGE\n";
  }

  return headerSource;
}

double Z3DImgRaycasterRenderer::renderProgressively(Z3DEye eye)
{
  double progress = 1;

  // // Manage blending for raycaster output
  // glEnable(GL_BLEND);
  // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  // auto blendGuard = folly::makeGuard([]() {
  //   glBlendFunc(GL_ONE, GL_ZERO);
  //   glDisable(GL_BLEND);
  // });
  // Ensure depth testing during raycaster rendering
  glEnable(GL_DEPTH_TEST);
  auto depthGuard = folly::makeGuard([]() {
    glDisable(GL_DEPTH_TEST);
  });

  if (!hasVisibleRendering()) {
    VLOG(1) << "no visible rendering";
    return progress;
  }

  if (m_quads.empty()) {
    if (m_entryExitTexCoordAndZeTexture == nullptr) {
      VLOG(1) << "no entry exit texture";
      return progress;
    }
  } else {
    for (auto& quad : m_quads) {
      if (m_img->is2DData() && quad.numVertices() != quad.num2DTextureCoordinates()) {
        VLOG(1) << "2d quad not correct";
        return progress;
      }
      if (m_img->is3DData() && quad.numVertices() != quad.num3DTextureCoordinates()) {
        VLOG(1) << "3d quad not correct";
        return progress;
      }
    }
  }

  std::vector<size_t> visibleIdxs;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    if (m_channelVisibleParas[i]->get()) {
      // VLOG(1) << "show channel " << i;
      visibleIdxs.push_back(i);
    }
  }

  try {
    if (!m_quads.empty()) { // 2d image or slice from 3d volume
      if (m_img->is2DData()) { // image is 2D
        render2DImage(eye, visibleIdxs);
      } else { // image is 3D, but a 2D slice will be shown
        if (!m_fastRendering && m_img->isVolumeDownsampled()) {
          progress = render2DSliceOf3DImage(eye, visibleIdxs, true);
        } else {
          render2DSliceOf3DImageFast(eye, visibleIdxs);
        }
      }
    } else { // 3d volume raycasting
      ZBenchTimer bta("all");
      if (!m_fastRendering && m_img->isVolumeDownsampled()) {
        progress = render3DImage(eye, visibleIdxs, true);
      } else {
        render3DImageFast(eye, visibleIdxs);
      }
      STOP_AND_VLOG(bta)
    }
    return progress;
  }
  catch (const ZCancellationException&) {
    // VLOG(1) << "cancel renderProgressively";
    resetProgress(eye);
    throw;
  }
}

void Z3DImgRaycasterRenderer::render(Z3DEye eye)
{
  // m_lastRenderingIsFastRendering = false;

  if (!hasVisibleRendering()) {
    return;
  }

  // // Manage blending for raycaster output
  // glEnable(GL_BLEND);
  // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  // auto blendGuard = folly::makeGuard([]() {
  //   glBlendFunc(GL_ONE, GL_ZERO);
  //   glDisable(GL_BLEND);
  // });
  // Ensure depth testing during raycaster rendering
  glEnable(GL_DEPTH_TEST);
  auto depthGuard = folly::makeGuard([]() {
    glDisable(GL_DEPTH_TEST);
  });

  if (m_quads.empty()) {
    if (m_entryExitTexCoordAndZeTexture == nullptr) {
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
    STOP_AND_VLOG(bta)
  }
}

void Z3DImgRaycasterRenderer::renderPicking(Z3DEye) {}

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

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(m_VAO, m_sc2dImageShader, quad);
    }

  } else {
    layerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      layerLease.renderTarget->attachSlice(j);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();
      bindVolumeAndTransferFunc(m_sc2dImageShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_sc2dImageShader, quad);
      }

      layerLease.renderTarget->release();
    }
  }

  m_sc2dImageShader.release();

  if (visibleIdxs.size() > 1) {
    // layerLease already holds the rendered slices
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

double
Z3DImgRaycasterRenderer::render2DSliceOf3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive)
{
  if (progressive && m_channelIdx[eye] < 0) {
    render2DSliceOf3DImageFast(eye, visibleIdxs);
    m_channelIdx[eye] = 0;
    return 0.5;
  }

  auto cancellationToken = m_rendererBase.globalParas().cancellationSource
                             ? m_rendererBase.globalParas().cancellationSource->getToken()
                             : folly::CancellationToken();

  float n = m_rendererBase.camera().nearDist();
  glm::vec2 pixelEyeSpaceSize = m_rendererBase.camera().frustumNearPlaneSize() / glm::vec2(m_outputSize);
  float ze_to_screen_pixel_voxel_size =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * m_rendererBase.globalParas().devicePixelRatio.get();

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() > 1) {
    layerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
  }
  size_t idx = 0;
  for (auto c : visibleIdxs) {
    LOG(INFO) << "";
    ZBenchTimer bt(fmt::format("render 2d slice of 3d image ch{}", c));

    processEventsAndMaybeCancel(cancellationToken);

    // Acquire single-attachment Block ID RT for slice-of-3D pass
    auto blockLease = m_rendererBase.globalParas().scratchPool().acquireBlockIdRenderTarget(m_outputSize, 1);
    if (blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4 != m_blockIDs.size()) {
      m_blockIDs.resize(blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4);
      // VLOG(1) << m_blockIDs.size();
    }

    std::vector<uint32_t> missingBlockIDs;
    tbb::concurrent_unordered_set<uint32_t> ccSet;
    { // scope for block id shader
      m_image3DSliceWithTransferfunBlockIDsShader.bind();
      auto guard = folly::makeGuard([=, this]() {
        m_image3DSliceWithTransferfunBlockIDsShader.release();
      });

      m_image3DSliceWithTransferfunBlockIDsShader.setUniform("ze_to_screen_pixel_voxel_size",
                                                             ze_to_screen_pixel_voxel_size);
      m_image3DSliceWithTransferfunBlockIDsShader.setProjectionViewMatrixUniform(
        m_rendererBase.camera().projectionViewMatrix(eye));
      m_image3DSliceWithTransferfunBlockIDsShader.setViewMatrixUniform(m_rendererBase.camera().viewMatrix(eye));

      // render block ids
      const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

      m_img->bindFullResBlockIDsShader(m_image3DSliceWithTransferfunBlockIDsShader, c);

      for (auto& quad : m_quads) {
        blockLease.renderTarget->bind();
        glDrawBuffers(1, g_drawBuffers);
        glClear(GL_COLOR_BUFFER_BIT);

        renderTriangleList(m_VAO, m_image3DSliceWithTransferfunBlockIDsShader, quad);

        blockLease.renderTarget->release();

        processEventsAndMaybeCancel(cancellationToken);

        blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)
          ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());
        tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                          [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                            ccSet.insert(range.begin(), range.end()); // inserts a sequence
                          });

        processEventsAndMaybeCancel(cancellationToken);
      }
      // glFinish();
    }
    ccSet.unsafe_erase(0_u32);
    ccSet.unsafe_erase(std::numeric_limits<uint32_t>::max());
    missingBlockIDs.insert(missingBlockIDs.end(), ccSet.begin(), ccSet.end());
    bt.recordEvent("render and collect blockids");

    processEventsAndMaybeCancel(cancellationToken);

    m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, c, cancellationToken, bt);

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
      layerLease.renderTarget->attachSlice(idx++);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      m_img->bindFullResRenderShader(m_image3DSliceWithTransferfunShader, c);
      m_image3DSliceWithTransferfunShader.bindTexture("transfer_function", m_transferFuncParas[c]->get().texture());
      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_image3DSliceWithTransferfunShader, quad);
      }

      layerLease.renderTarget->release();
    }

    m_image3DSliceWithTransferfunShader.release();
    // glFinish();
    bt.recordEvent("render image3d slice");
    STOP_AND_VLOG(bt)
  }

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR

  m_channelIdx[eye] = -1;
  return 1;
}

void Z3DImgRaycasterRenderer::render2DSliceOf3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  m_scVolumeSliceWithTransferfunShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_scVolumeSliceWithTransferfunShader, eye);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, quad);
    }
  } else {
    layerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      layerLease.renderTarget->attachSlice(j);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      bindVolumeAndTransferFunc(m_scVolumeSliceWithTransferfunShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(m_VAO, m_scVolumeSliceWithTransferfunShader, quad);
      }

      layerLease.renderTarget->release();
    }
  }

  m_scVolumeSliceWithTransferfunShader.release();

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

double Z3DImgRaycasterRenderer::render3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive)
{
  // VLOG(1) << "render3DImage";
  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (progressive && m_channelIdx[eye] < 0) {
    // Acquire and clear the persistent layer array lease used across rounds
    if (m_progressiveLayerLease) {
      // Ensure previous lease is returned to the pool before re-acquiring
      m_progressiveLayerLease.release();
    }
    // Show an initial fast result immediately
    render3DImageFast(eye, visibleIdxs);
    // Initialize progressive accumulation state
    m_channelIdx[eye] = 0;

    m_progressiveLayerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t idx = 0; idx < visibleIdxs.size(); ++idx) {
      m_progressiveLayerLease.renderTarget->attachSlice(idx);
      m_progressiveLayerLease.renderTarget->bind();
      m_progressiveLayerLease.renderTarget->clear();
      m_progressiveLayerLease.renderTarget->release();
    }
    return 0.5;
  }

  double progress = 1;

  auto cancellationToken = m_rendererBase.globalParas().cancellationSource
                             ? m_rendererBase.globalParas().cancellationSource->getToken()
                             : folly::CancellationToken();

  float n = m_rendererBase.camera().nearDist();
  float f = m_rendererBase.camera().farDist();
  glm::vec2 pixelEyeSpaceSize = m_rendererBase.camera().frustumNearPlaneSize() / glm::vec2(m_outputSize);
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float ze_to_zw_a = f * n / (f - n);
  float ze_to_zw_b = 0.5f * (f + n) / (f - n) + 0.5f;
  float ze_to_screen_pixel_voxel_size =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * m_rendererBase.globalParas().devicePixelRatio.get();
  VLOG(1) << n << " " << f << " " << ze_to_screen_pixel_voxel_size << " " << pixelEyeSpaceSize << " " << ze_to_zw_a
          << " " << ze_to_zw_b;

  CHECK(m_lastImageRenderTargets[eye]->size() == m_outputSize) << m_lastImageRenderTargets[eye]->size();

  processEventsAndMaybeCancel(cancellationToken);

#if defined(__linux__)
  static int dummyidx = -1;
  if (FLAGS_atlas_debug_texture_output) {
    ++dummyidx;
  }
#endif

  if (progressive) {
    CHECK(m_channelIdx[eye] >= 0 && m_channelIdx[eye] < static_cast<int>(visibleIdxs.size()))
      << m_channelIdx[eye] << " " << visibleIdxs.size();
    bool lastRound = render3DImageForOneRound(eye,
                                              visibleIdxs[m_channelIdx[eye]],
                                              m_round[eye],
                                              ze_to_zw_a,
                                              ze_to_zw_b,
                                              ze_to_screen_pixel_voxel_size);
    processEventsAndMaybeCancel(cancellationToken);

    if (visibleIdxs.size() == 1) {
      m_copyTextureShader.bind();
      m_copyTextureShader.bindTexture("color_texture", m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader.bindTexture("depth_texture", m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(m_VAO, m_copyTextureShader);
      m_copyTextureShader.release();
    } else {
      m_progressiveLayerLease.renderTarget->attachSlice(m_channelIdx[eye]);
      m_progressiveLayerLease.renderTarget->bind();
      m_progressiveLayerLease.renderTarget->clear();

      m_copyTextureShader.bind();
      m_copyTextureShader.bindTexture("color_texture", m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader.bindTexture("depth_texture", m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(m_VAO, m_copyTextureShader);
      m_copyTextureShader.release();

      m_progressiveLayerLease.renderTarget->release();
    }

    if (lastRound) {
      ++m_channelIdx[eye];
      m_round[eye] = 0;
    } else {
      ++m_round[eye];
    }
    int totalRound = visibleIdxs.size() * FLAGS_atlas_volume_rendering_maximum_round;
    int currentRound = m_channelIdx[eye] * FLAGS_atlas_volume_rendering_maximum_round + m_round[eye];
    progress = currentRound >= totalRound ? 1 : (static_cast<double>(currentRound) / totalRound * 0.5 + 0.5);
    if (progress == 1) {
      m_channelIdx[eye] = -1;
      m_round[eye] = 0;
      // Do not release m_progressiveLayerLease here; we still need it for the
      // final merge step below. It will be released after merging.
    }
  } else {
    layerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t channelIdx = 0; channelIdx < visibleIdxs.size(); ++channelIdx) {
      auto c = visibleIdxs[channelIdx];
      for (uint32_t round = 0; round < FLAGS_atlas_volume_rendering_maximum_round; ++round) {
        bool lastRound = render3DImageForOneRound(eye, c, round, ze_to_zw_a, ze_to_zw_b, ze_to_screen_pixel_voxel_size);
#if defined(__linux__)
        if (FLAGS_atlas_debug_texture_output) {
          auto filen =
            QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att0.tif", dummyidx, c, round));
          m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage(filen);
          filen =
            QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att1.tif", dummyidx, c, round));
          m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1)->saveAsRGBFloatImage(filen);
          if (round == 0) {
            filen = QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_entry.tif", dummyidx, c));
            m_entryExitTexCoordAndZeTexture->saveAsRGBAFloatImage(filen);
          }
        }
#endif
        if (lastRound) {
          break;
        }
      }

      processEventsAndMaybeCancel(cancellationToken);

      if (visibleIdxs.size() == 1) {
        m_copyTextureShader.bind();
        m_copyTextureShader.bindTexture("color_texture",
                                        m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0));
        m_copyTextureShader.bindTexture("depth_texture",
                                        m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));
        renderScreenQuad(m_VAO, m_copyTextureShader);
        m_copyTextureShader.release();
      } else {
        layerLease.renderTarget->attachSlice(channelIdx);
        layerLease.renderTarget->bind();
        layerLease.renderTarget->clear();

        m_copyTextureShader.bind();
        m_copyTextureShader.bindTexture("color_texture",
                                        m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0));
        m_copyTextureShader.bindTexture("depth_texture",
                                        m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));
        renderScreenQuad(m_VAO, m_copyTextureShader);
        m_copyTextureShader.release();

        layerLease.renderTarget->release();
      }
    }
  }

  processEventsAndMaybeCancel(cancellationToken);

  if (visibleIdxs.size() > 1) {
    const Z3DTexture* mergeColor = nullptr;
    const Z3DTexture* mergeDepth = nullptr;
    if (progressive) {
      mergeColor = m_progressiveLayerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
      mergeDepth = m_progressiveLayerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT);
    } else {
      mergeColor = layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
      mergeDepth = layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT);
    }
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", mergeColor);
    m_mergeChannelShader.bindTexture("depth_texture", mergeDepth);
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  // Now that we've displayed the merged result, release the progressive lease
  // at the very end of the frame when progressive is complete.
  if (progressive && progress >= 1 && m_progressiveLayerLease) {
    m_progressiveLayerLease.release();
  }

  CHECK_GL_ERROR

  if (progress == 1.) {
    LOG(INFO) << "image cache size: " << ZImgCache::instance().size();
    LOG(INFO) << "image block cache size: " << ZImgRegionCache::instance().size();
  }
  return progress;
}

bool Z3DImgRaycasterRenderer::render3DImageForOneRound(Z3DEye eye,
                                                       size_t c,
                                                       uint32_t round,
                                                       float ze_to_zw_a,
                                                       float ze_to_zw_b,
                                                       float ze_to_screen_pixel_voxel_size)
{
  auto cancellationToken = m_rendererBase.globalParas().cancellationSource
                             ? m_rendererBase.globalParas().cancellationSource->getToken()
                             : folly::CancellationToken();

  LOG(INFO) << "channel " << c << " round " << round;
  ZBenchTimer bt(fmt::format("render 3D image channel {} round {}", c, round));

  processEventsAndMaybeCancel(cancellationToken);

  m_image3DRaycasterBlockIDsShader.bind();
  //          m_image3DRaycasterBlockIDsShader.setUniform("screen_dim_RCP",
  //                                                      1.f / glm::vec2(m_blockIDsRenderTarget->size()));
  m_image3DRaycasterBlockIDsShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

  // entry exit points
  m_image3DRaycasterBlockIDsShader.bindTexture("ray_entry_exit_tex_coord", m_entryExitTexCoordAndZeTexture);

  m_image3DRaycasterBlockIDsShader.setUniform("sampling_rate", m_samplingRateValue);

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
    m_lastImageRenderTargets[eye]->bind();
    glDrawBuffers(2, g_drawBuffers);
    m_lastImageRenderTargets[eye]->clear();
    m_lastImageRenderTargets[eye]->release();
  }

  // Acquire multi-attachment Block ID RT via scratch pool
  auto blockLease =
    m_rendererBase.globalParas().scratchPool().acquireBlockIdRenderTarget(m_lastImageRenderTargets[eye]->size());
  blockLease.renderTarget->bind();
  glDrawBuffers(static_cast<GLsizei>(blockLease.attachments), g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);

  m_img->bindFullResBlockIDsShader(m_image3DRaycasterBlockIDsShader, c);
  m_image3DRaycasterBlockIDsShader.bindTexture("last_ray_depth",
                                               m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));
  renderScreenQuad(m_VAO, m_image3DRaycasterBlockIDsShader);

  blockLease.renderTarget->release();
  m_image3DRaycasterBlockIDsShader.release();
  // glFinish();
  bt.recordEvent("render blockids");

  processEventsAndMaybeCancel(cancellationToken);

  // check missed blocks and upload
  const Z3DTexture* missingBlockIDsTexture = blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  if (missingBlockIDsTexture->numPixels() * 4 != m_blockIDs.size()) {
    m_blockIDs.resize(missingBlockIDsTexture->numPixels() * 4);
    VLOG(1) << m_blockIDs.size();
  }
  missingBlockIDsTexture->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

#if 0
          if (!QFileInfo("/Users/feng/Downloads/test_missid.tif").exists()) {
            ZImg img;
            img.wrapData(m_blockIDs.data(), missingBlockIDsTexture->width(), missingBlockIDsTexture->height(), 1, 4);
            ZImg outImg = img;
            ZImgFormat::CXYZtoXYZC(img, outImg);
            outImg.flip(Dimension::Y);
            outImg.save("/Users/feng/Downloads/test_missid.tif");
          }
#endif

  tbb::concurrent_unordered_set<uint32_t> ccSet;

  tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                    [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                      ccSet.insert(range.begin(), range.end()); // inserts a sequence
                    });

  processEventsAndMaybeCancel(cancellationToken);

  CHECK(!ccSet.empty());
  bool lastRound = ccSet.size() == 1 && ccSet.contains(0_u32); // ccSet contains only 0
  if (lastRound) {
    LOG(INFO) << "no (non-empty) blocks to render";
    if (round > 0) {
      // otherwise still need to render empty blocks
      return lastRound;
    }
    bt.recordEvent("collect blockids");
  } else {
    // need to upload some image blocks to GPU
    bool hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);

    for (auto att = 1u; !hasEnoughMissingIDs && !lastRound && att < blockLease.attachments; ++att) {
      auto numberBlock = ccSet.size();
      blockLease.renderTarget->attachment(g_drawBuffers[att])
        ->downloadTextureToBuffer(GL_RGBA_INTEGER, GL_UNSIGNED_INT, m_blockIDs.data());

      tbb::parallel_for(tbb::blocked_range<std::vector<uint32_t>::iterator>(m_blockIDs.begin(), m_blockIDs.end()),
                        [&](const tbb::blocked_range<std::vector<uint32_t>::iterator>& range) {
                          ccSet.insert(range.begin(), range.end()); // inserts a sequence
                        });

      hasEnoughMissingIDs = ccSet.size() > m_img->numCachedImages(c);

      lastRound = !hasEnoughMissingIDs && numberBlock == ccSet.size();
      if (lastRound) { // confirm
        lastRound = *parallel_max_element(m_blockIDs) == 0;
      }
      if (lastRound) {
        VLOG(1) << "last att: " << att;
      }

      processEventsAndMaybeCancel(cancellationToken);
    }

    std::vector<uint32_t> missingBlockIDs;

    ccSet.unsafe_erase(0_u32);
    if (ccSet.contains(std::numeric_limits<uint32_t>::max())) {
      VLOG(1) << "use last block";
    }
    ccSet.unsafe_erase(std::numeric_limits<uint32_t>::max());
    if (!ccSet.empty()) {
      CHECK(ccSet.size() < m_img->numCachedImages(c) * 10);
      missingBlockIDs.reserve(ccSet.size());
      missingBlockIDs.insert(missingBlockIDs.end(), ccSet.begin(), ccSet.end());
      if ((round % 2 == 1) && hasEnoughMissingIDs) {
        std::ranges::sort(missingBlockIDs, std::ranges::greater{});
      } else {
        std::ranges::sort(missingBlockIDs);
      }
    }
    // VLOG(1) << missingBlockIDs.size() << " " << usedBlockIDs.size();
    bt.recordEvent("collect blockids");

    processEventsAndMaybeCancel(cancellationToken);

    lastRound = m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, c, cancellationToken, bt) && lastRound;

    processEventsAndMaybeCancel(cancellationToken);
  }

  // render channels one by one
  m_image3DRaycasterShader.bind();

  m_image3DRaycasterShader.setUniform("ze_to_zw_b", ze_to_zw_b);
  m_image3DRaycasterShader.setUniform("ze_to_zw_a", ze_to_zw_a);
  m_image3DRaycasterShader.setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

  // entry exit points
  m_image3DRaycasterShader.bindTexture("ray_entry_exit_tex_coord", m_entryExitTexCoordAndZeTexture);

  m_image3DRaycasterShader.bindTexture("last_color", m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT0));
  m_image3DRaycasterShader.bindTexture("last_ray_depth",
                                       m_lastImageRenderTargets[eye]->attachment(GL_COLOR_ATTACHMENT1));

  if (m_compositingModeValue == ImgCompositingMode::IsoSurface) {
    m_image3DRaycasterShader.setUniform("iso_value", m_isoValue);
  }

  if (m_compositingModeValue == ImgCompositingMode::LocalMIP ||
      m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque) {
    m_image3DRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold);
  }

  m_image3DRaycasterShader.setUniform("sampling_rate", m_samplingRateValue);

  m_currentImageRenderTargets[eye]->bind();
  glDrawBuffers(2, g_drawBuffers);
  m_currentImageRenderTargets[eye]->clear();

  m_img->bindFullResRenderShader(m_image3DRaycasterShader, c);
  m_image3DRaycasterShader.bindTexture("transfer_function", m_transferFuncParas[c]->get().texture());
  renderScreenQuad(m_VAO, m_image3DRaycasterShader);

  m_currentImageRenderTargets[eye]->release();

  m_image3DRaycasterShader.release();
  // glFinish();
  bt.recordEvent("render image");
  STOP_AND_VLOG(bt)

  std::swap(m_lastImageRenderTargets[eye], m_currentImageRenderTargets[eye]);

  return lastRound;
}

void Z3DImgRaycasterRenderer::render3DImageFast(Z3DEye /*eye*/, const std::vector<size_t>& visibleIdxs)
{
  m_scRaycasterShader.bind();

  float n = m_rendererBase.camera().nearDist();
  float f = m_rendererBase.camera().farDist();
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float a = f * n / (f - n);
  float b = 0.5f * (f + n) / (f - n) + 0.5f;
  m_scRaycasterShader.setUniform("ze_to_zw_b", b);
  m_scRaycasterShader.setUniform("ze_to_zw_a", a);

  // entry exit points
  m_scRaycasterShader.bindTexture("ray_entry_exit_tex_coord", m_entryExitTexCoordAndZeTexture);
  // m_entryExitTexCoordAndZeTexture->saveAsRGBAFloatImage("/Users/feng/Downloads/abcd_entryexit.tif");

  if (m_compositingModeValue == ImgCompositingMode::IsoSurface) {
    m_scRaycasterShader.setUniform("iso_value", m_isoValue);
  }

  if (m_compositingModeValue == ImgCompositingMode::LocalMIP ||
      m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque) {
    m_scRaycasterShader.setUniform("local_MIP_threshold", m_localMIPThreshold);
  }

  m_scRaycasterShader.setUniform("sampling_rate", m_samplingRateValue);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[0]);
    renderScreenQuad(m_VAO, m_scRaycasterShader);
  } else {
    CHECK(visibleIdxs.size() > 1);
    layerLease = m_rendererBase.globalParas().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t i = 0; i < visibleIdxs.size(); ++i) {
      layerLease.renderTarget->attachSlice(i);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      bindVolumeAndTransferFunc(m_scRaycasterShader, visibleIdxs[i]);
      renderScreenQuad(m_VAO, m_scRaycasterShader);

      layerLease.renderTarget->release();
    }
    // layerLease.renderTarget->colorTexture()->saveAsColorImage("/Users/feng/Downloads/abcd_b.tif");
  }

  m_scRaycasterShader.release();

  if (visibleIdxs.size() > 1) {
    // Merge after drawing into layerLease
    m_mergeChannelShader.bind();
    m_mergeChannelShader.bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader.bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(m_VAO, m_mergeChannelShader);
    m_mergeChannelShader.release();
  }

  CHECK_GL_ERROR
}

} // namespace nim
