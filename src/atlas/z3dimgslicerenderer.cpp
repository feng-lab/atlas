#include "z3dimgslicerenderer.h"

#include "z3dtexture.h"
#include "z3drendertarget.h"
#include "z3dimg.h"
#include "z3drendercommands.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "zcancellation.h"
#include "z3dscratchresourcepool.h"
#include "z3drenderglobalstate.h"
#include <memory>
#include <absl/strings/str_cat.h>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>

namespace nim {

Z3DImgSliceRenderer::Z3DImgSliceRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
{
  createResources(m_rendererBase.activeBackend());
  // Render targets (layer and block-id) are acquired from the scratch pool on demand.
}

void Z3DImgSliceRenderer::setData(Z3DImg& img, const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps)
{
  CHECK(colormaps.size() >= img.numChannels() && img.is3DData());

  m_img = &img;
  m_colormaps = &colormaps;

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
      compile();
    }
    m_volumeUniformNames.resize(m_img->numChannels());
    m_colormapUniformNames.resize(m_img->numChannels());
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      m_volumeUniformNames[i] = fmt::format("volume_{}", i + 1);
      m_colormapUniformNames[i] = fmt::format("colormap_{}", i + 1);
    }
  }
}

void Z3DImgSliceRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  if (!m_img || m_slices.empty()) {
    return;
  }

  ImgSlicePayload payload;
  payload.renderer = this;
  payload.image = m_img;
  payload.colormaps = m_colormaps;
  payload.slices = std::span<const ZMesh>(m_slices.data(), m_slices.size());
  payload.outputSize = m_outputSize;
  payload.fastPathOnly = m_fastRendering;
  payload.maxProjectionMerge = true;

  const uint32_t channelCount = static_cast<uint32_t>(m_img->numChannels());
  if (channelCount > 1u) {
    auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();
    auto lease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize,
                                                          channelCount,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth24,
                                                          std::optional<RenderBackend>(RenderBackend::Vulkan));
    auto leaseHolder = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>();
    *leaseHolder = std::move(lease);
    payload.layerLease = std::move(leaseHolder);
  }

  RenderBatch batch;
  batch.eye = eye;
  batch.geometry = std::move(payload);

  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DImgSliceRenderer::addSlice(const ZMesh& slice)
{
  if (slice.empty() || slice.numVertices() != slice.num3DTextureCoordinates()) {
    LOG(FATAL) << "Input slice should be plane triangles with 3D texture coordinates";
    return;
  }
  m_slices.push_back(slice);
}

void Z3DImgSliceRenderer::bindVolumes(Z3DShaderProgram& shader) const
{
  size_t idx = 0;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    // volumes
    auto* texture = m_img->channelTexture(i);
    CHECK(texture != nullptr) << "Missing OpenGL texture for channel " << i;
    shader.bindTexture(m_volumeUniformNames[idx], texture, GLint(GL_NEAREST), GLint(GL_NEAREST));

    // colormap (GL LUT cache)
    if (auto* tex = colormapTextureGL((*m_colormaps)[i]->get())) {
      shader.bindTexture(m_colormapUniformNames[idx++], tex);
    }

    CHECK_GL_ERROR
  }
}

void Z3DImgSliceRenderer::bindVolume(Z3DShaderProgram& shader, size_t idx) const
{
  // volumes
  auto* texture = m_img->channelTexture(idx);
  CHECK(texture != nullptr) << "Missing OpenGL texture for channel " << idx;
  shader.bindTexture(m_volumeUniformNames[0], texture, GLint(GL_NEAREST), GLint(GL_NEAREST));

  // colormap (GL LUT cache)
  if (auto* tex = colormapTextureGL((*m_colormaps)[idx]->get())) {
    shader.bindTexture(m_colormapUniformNames[0], tex);
  }

  CHECK_GL_ERROR
}

Z3DTexture* Z3DImgSliceRenderer::colormapTextureGL(const ZColorMap& cm, uint32_t width) const
{
  const uint64_t gen = cm.generation();
  auto itMeta = m_colormapCache.meta.find(&cm);
  auto itTex = m_colormapCache.textures.find(&cm);
  const bool needCreate = itMeta == m_colormapCache.meta.end() ||
                          itTex == m_colormapCache.textures.end() ||
                          itMeta->second.first != gen || itMeta->second.second != width;
  if (needCreate) {
    std::vector<uint8_t> lut;
    cm.buildLUTBGRA8(lut, width);
    if (lut.empty()) {
      return nullptr;
    }
    auto tex = std::make_unique<Z3DTexture>(GLint(GL_RGBA8), glm::uvec3(width, 1, 1), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV);
    tex->updateImage(lut.data());
    m_colormapCache.textures[&cm] = std::move(tex);
    m_colormapCache.meta[&cm] = std::make_pair(gen, width);
  }
  return m_colormapCache.textures[&cm].get();
}

void Z3DImgSliceRenderer::compile()
{
  // m_volumeSliceShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_scVolumeSliceShader->setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_mergeChannelShader->setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  m_image3DSliceWithColorMapBlockIDsShader->setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  m_image3DSliceWithColorMapShader->setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DImgSliceRenderer::generateHeader()
{
  std::string header;
  header.reserve(192);

  const size_t numLevels = m_img ? m_img->numLevels() : 1;
  fmt::format_to(std::back_inserter(header), "#define LEVEL_COUNT {}\n", numLevels);

  if (m_img && m_img->numChannels() > 0) {
    fmt::format_to(std::back_inserter(header), "#define NUM_VOLUMES {}\n", m_img->numChannels());
  } else {
    absl::StrAppend(&header, "#define NUM_VOLUMES 0\n", "#define DISABLE_TEXTURE_COORD_OUTPUT\n");
  }

  absl::StrAppend(&header, "#define MAX_PROJ_MERGE\n");
  return header;
}

double Z3DImgSliceRenderer::renderProgressively(Z3DEye eye)
{
  double progress = 1.0;
  bool needRender = m_img && !m_slices.empty();
  if (!needRender) {
    return progress;
  }

  // Depth test only; compositor handles blending later
  glEnable(GL_DEPTH_TEST);
  auto depthGuardProgress = folly::makeGuard([]() {
    glDisable(GL_DEPTH_TEST);
  });

  try {
    if (!m_fastRendering && m_img->isVolumeDownsampled()) {
      progress = renderSlice(eye, true);
    } else {
      renderSliceFast(eye);
    }

    return progress;
  }
  catch (const ZCancellationException&) {
    resetProgress(eye);
    throw;
  }
}

void Z3DImgSliceRenderer::render(Z3DEye eye)
{
  bool needRender = m_img && !m_slices.empty();
  if (!needRender) {
    return;
  }

  // Ensure depth testing during slice rendering
  glEnable(GL_DEPTH_TEST);
  auto depthGuard = folly::makeGuard([]() {
    glDisable(GL_DEPTH_TEST);
  });

  if (!m_fastRendering && m_img->isVolumeDownsampled()) {
    renderSlice(eye);
  } else {
    renderSliceFast(eye);
  }
}

double Z3DImgSliceRenderer::renderSlice(Z3DEye eye, bool progressive)
{
  if (progressive && m_progress[eye] == 0) {
    renderSliceFast(eye);
    m_progress[eye] = 0.5;
    return m_progress[eye];
  }

  const auto& sceneState = m_rendererBase.sceneState();
  const auto& viewState = m_rendererBase.viewState();
  const auto& eyeState = viewState.eyes[eye];
  const auto& monoEyeState = viewState.eyes[MonoEye];
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  const float devicePixelRatio = sceneState.devicePixelRatio;
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  float n = viewState.nearClip;
  glm::vec2 pixelEyeSpaceSize = monoEyeState.frustumNearPlaneSize / glm::vec2(m_outputSize);
  float ze_to_screen_pixel_voxel_size = -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * devicePixelRatio;

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (m_img->numChannels() > 1) {
    layerLease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize, static_cast<uint32_t>(m_img->numChannels()));
    // VLOG(1) << "lease acquired";
  }
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    LOG(INFO) << "";
    ZBenchTimer bt(fmt::format("render slice ch{}", i));

    processEventsAndMaybeCancel(cancellationToken);

    // Acquire a scratch Block ID RT with a single color attachment
    auto blockLease = scratchPool.acquireBlockIdRenderTarget(m_outputSize, 1);

    if (blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4 != m_blockIDs.size()) {
      m_blockIDs.resize(blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4);
    }

    std::vector<uint32_t> missingBlockIDs;
    tbb::concurrent_unordered_set<uint32_t> ccSet;
    { // scope for block id shader
      m_image3DSliceWithColorMapBlockIDsShader->bind();
      auto guard = folly::makeGuard([=, this]() {
        m_image3DSliceWithColorMapBlockIDsShader->release();
      });

      m_image3DSliceWithColorMapBlockIDsShader->setUniform("ze_to_screen_pixel_voxel_size",
                                                          ze_to_screen_pixel_voxel_size);
      m_image3DSliceWithColorMapBlockIDsShader->setProjectionViewMatrixUniform(eyeState.projectionViewMatrix);
      m_image3DSliceWithColorMapBlockIDsShader->setViewMatrixUniform(eyeState.viewMatrix);

      // render block ids
      const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

      m_img->bindFullResBlockIDsShader(*m_image3DSliceWithColorMapBlockIDsShader, i);

      for (auto& slice : m_slices) {
        blockLease.renderTarget->bind();
        glDrawBuffers(1, g_drawBuffers);
        glClear(GL_COLOR_BUFFER_BIT);

        renderTriangleList(*m_VAO, *m_image3DSliceWithColorMapBlockIDsShader, slice);

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

    m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, i, cancellationToken, bt);

    // render channels one by one
    m_image3DSliceWithColorMapShader->bind();

    m_image3DSliceWithColorMapShader->setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithColorMapShader->setProjectionViewMatrixUniform(eyeState.projectionViewMatrix);
    m_image3DSliceWithColorMapShader->setViewMatrixUniform(eyeState.viewMatrix);

    // macOS: if sets here, then the following rendering uses old page directory caches. no idea why
    // m_img->bindFullResRenderShader(*m_image3DSliceWithColorMapShader);

    if (m_img->numChannels() == 1) {
      m_img->bindFullResRenderShader(*m_image3DSliceWithColorMapShader, 0);
      if (auto* tex = colormapTextureGL((*m_colormaps)[0]->get())) {
        m_image3DSliceWithColorMapShader->bindTexture("colormap", tex);
      }
      for (auto& slice : m_slices) {
        renderTriangleList(*m_VAO, *m_image3DSliceWithColorMapShader, slice);
      }
    } else {
      layerLease.renderTarget->attachSlice(i);

      //        if (i == 1) {
      //        m_layerTarget->saveAsColorImage("/Users/feng/Downloads/abcd_b.tif");
      //        }

      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      m_img->bindFullResRenderShader(*m_image3DSliceWithColorMapShader, i);
      if (auto* tex = colormapTextureGL((*m_colormaps)[i]->get())) {
        m_image3DSliceWithColorMapShader->bindTexture("colormap", tex);
      }
      for (auto& slice : m_slices) {
        renderTriangleList(*m_VAO, *m_image3DSliceWithColorMapShader, slice);
      }

      layerLease.renderTarget->release();

      // if (i == 1) {
      // m_layerTarget->saveAsColorImage("/Users/feng/Downloads/abcd.tif");
      // }
    }

    m_image3DSliceWithColorMapShader->release();
    // glFinish();
    bt.recordEvent("render image3d slice");
  }

  if (m_img->numChannels() > 1) {
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
  }

  CHECK_GL_ERROR

  m_progress[eye] = 0;
  return 1;
}

void Z3DImgSliceRenderer::renderSliceFast(Z3DEye eye)
{
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  m_scVolumeSliceShader->bind();
  m_rendererBase.setGlobalShaderParameters(*m_scVolumeSliceShader, eye);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (m_img->numChannels() == 1) {
    bindVolume(*m_scVolumeSliceShader, 0);
    for (auto& slice : m_slices) {
      renderTriangleList(*m_VAO, *m_scVolumeSliceShader, slice);
    }
  } else {
    layerLease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize, static_cast<uint32_t>(m_img->numChannels()));
    // VLOG(1) << "lease acquired";
    for (size_t j = 0; j < m_img->numChannels(); ++j) {
      layerLease.renderTarget->attachSlice(j);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      bindVolume(*m_scVolumeSliceShader, j);
      for (auto& slice : m_slices) {
        renderTriangleList(*m_VAO, *m_scVolumeSliceShader, slice);
      }

      layerLease.renderTarget->release();
    }
  }

  m_scVolumeSliceShader->release();

  if (m_img->numChannels() > 1) {
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
  }

  CHECK_GL_ERROR
}

void Z3DImgSliceRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_scVolumeSliceShader = std::make_unique<Z3DShaderProgram>();
  m_mergeChannelShader = std::make_unique<Z3DShaderProgram>();
  m_image3DSliceWithColorMapBlockIDsShader = std::make_unique<Z3DShaderProgram>();
  m_image3DSliceWithColorMapShader = std::make_unique<Z3DShaderProgram>();

  const std::string header = m_rendererBase.generateHeader() + generateHeader();
  m_scVolumeSliceShader->loadFromSourceFile("transform_with_3dtexture.vert",
                                            "volume_slice_with_colormap_single_channel.frag",
                                            header);
  m_mergeChannelShader->loadFromSourceFile("pass.vert",
                                           "image2d_array_compositor.frag",
                                           header);
  m_image3DSliceWithColorMapBlockIDsShader->loadFromSourceFile(
    "transform_with_3dtexture_and_eye_coordinate.vert",
    "image3d_slice_with_transfun_blockID.frag",
    header);
  m_image3DSliceWithColorMapShader->loadFromSourceFile(
    "transform_with_3dtexture_and_eye_coordinate.vert",
    "image3d_slice_with_colormap.frag",
    header);
  CHECK_GL_ERROR;

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
}

void Z3DImgSliceRenderer::destroyResources()
{
  m_scVolumeSliceShader.reset();
  m_mergeChannelShader.reset();
  m_image3DSliceWithColorMapBlockIDsShader.reset();
  m_image3DSliceWithColorMapShader.reset();
  m_VAO.reset();
}

} // namespace nim
