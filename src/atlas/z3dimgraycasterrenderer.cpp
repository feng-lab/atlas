#include "z3dimgraycasterrenderer.h"

#include "z3dtexture.h"
#include "z3drendertarget.h"
#include "z3dimg.h"
#include "zmesh.h"
#include "zbenchtimer.h"
#include "zimgcache.h"
#include "zimgregioncache.h"
#include "zlog.h"
#include "zcancellation.h"
#include "zstatisticsutils.h"
#include "z3dscratchresourcepool.h"
#include "z3drenderglobalstate.h"
#include <absl/strings/str_cat.h>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>
#include <algorithm>
#include <array>
#include <optional>

DEFINE_uint32(atlas_volume_rendering_maximum_round,
              100,
              "Maximum number of rounds for volume rendering, default is 100");

#if defined(__linux__)
DEFINE_bool(atlas_debug_texture_output, false, "produce debug intermediate texture to /data/testoutput");
#endif

namespace nim {

Z3DImgRaycasterRenderer::Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_textureAndEyeCoordinateRenderer(m_rendererBase)
{
  createResources(m_rendererBase.activeBackend());

  // Render targets (layer, block-id, entry/exit) are acquired from the scratch pool on demand.
}

void Z3DImgRaycasterRenderer::setData(Z3DImg& img)
{
  m_img = &img;

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    m_volumeUniformNames.clear();
    m_volumeDimensionNames.clear();
    m_transferFuncUniformNames.clear();
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      m_volumeUniformNames.push_back(fmt::format("volume_{}", i + 1));
      m_volumeDimensionNames.push_back(fmt::format("volume_dimensions_{}", i + 1));
      m_transferFuncUniformNames.push_back(fmt::format("transfer_function_{}", i + 1));
    }
  }
  setChannelCount(m_img->numChannels());
  compile();
}

void Z3DImgRaycasterRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  if (!m_img || !hasVisibleRendering()) {
    return;
  }

  std::vector<size_t> visibleChannels;
  visibleChannels.reserve(m_img->numChannels());
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    if (i < m_channelVisibilities.size() && m_channelVisibilities[i]) {
      visibleChannels.push_back(i);
    }
  }

  if (visibleChannels.empty()) {
    return;
  }

  CHECK(m_outputSize.x > 0u && m_outputSize.y > 0u) << "Vulkan img raycaster output size is zero.";

  ImgRaycasterPayload payload;
  payload.streamKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  payload.image = m_img;
  payload.outputSize = m_outputSize;
  payload.samplingRate = m_samplingRateValue;
  payload.isoValue = m_isoValue;
  payload.localMIPThreshold = m_localMIPThreshold;
  payload.compositingMode = m_compositingModeValue;
  payload.fastPathOnly = m_fastRendering || !m_img->isVolumeDownsampled();
  // Vulkan path uses the output size for entry/exit
  payload.visibleChannels = visibleChannels;
  payload.activeChannel = visibleChannels.empty() ? std::numeric_limits<size_t>::max() : visibleChannels.front();
  payload.activeChannelIndex = 0u;
  // Progressive init parity with GL: on first entry into progressive rendering,
  // initialize state and advance generation so downstream Vulkan pipelines can
  // clear/prime their per-generation targets before paging kicks in.
  if (!payload.fastPathOnly) {
    if (m_channelIdx[eye] < 0) {
      m_channelIdx[eye] = 0;
      m_round[eye] = 0;
      ++m_progressiveGeneration[eye];
    }
    if (!visibleChannels.empty()) {
      const int clampedIndex = std::clamp(m_channelIdx[eye], 0, static_cast<int>(visibleChannels.size() - 1));
      payload.activeChannelIndex = static_cast<uint32_t>(clampedIndex);
      payload.activeChannel = visibleChannels[static_cast<size_t>(clampedIndex)];
    }
  } else {
    m_channelIdx[eye] = -1;
    m_round[eye] = 0;
  }
  payload.progressiveGeneration = m_progressiveGeneration[eye];
  payload.transferFunctions = &m_transferFunctions;
  payload.roundsCompleted = static_cast<uint32_t>(std::max(0, m_round[eye]));
  payload.roundsRemaining = FLAGS_atlas_volume_rendering_maximum_round;

  auto populateFromEntryExitMesh = [&]() {
    const auto& positions = m_entryExitMesh.vertices();
    const auto& texCoords = m_entryExitMesh.textureCoordinates3D();
    const auto& indices = m_entryExitMesh.indices();

    payload.entryPositions.assign(positions.begin(), positions.end());
    payload.entryTexCoords.assign(texCoords.begin(), texCoords.end());
    payload.entryHasIndices = m_entryExitMesh.hasIndices();
    if (payload.entryHasIndices) {
      payload.entryIndices.assign(indices.begin(), indices.end());
    }
    payload.entryPrimitive = static_cast<uint32_t>(m_entryExitMesh.type());
    payload.entryFlipped = m_entryExitMeshFlipped;
  };

  auto populateFromQuads = [&]() {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> texCoords;
    positions.reserve(m_quads.size() * 6);
    texCoords.reserve(m_quads.size() * 6);

    auto appendVertex = [&](const glm::vec3& pos, const glm::vec3& tex) {
      positions.push_back(pos);
      texCoords.push_back(tex);
    };

    for (const auto& quad : m_quads) {
      const auto& verts = quad.vertices();
      if (verts.empty()) {
        continue;
      }
      const auto& tex3d = quad.textureCoordinates3D();
      const auto& tex2d = quad.textureCoordinates2D();
      const bool has3d = tex3d.size() == verts.size();
      const bool has2d = tex2d.size() == verts.size();

      auto texAt = [&](size_t idx) {
        if (has3d) {
          return tex3d[idx];
        }
        if (has2d) {
          const auto& uv = tex2d[idx];
          return glm::vec3(uv.x, uv.y, 0.0f);
        }
        return glm::vec3(0.0f);
      };

      if (quad.hasIndices()) {
        const auto& quadIndices = quad.indices();
        if (quadIndices.size() >= 3) {
          for (size_t idx = 0; idx + 2 < quadIndices.size(); idx += 3) {
            const uint32_t i0 = quadIndices[idx];
            const uint32_t i1 = quadIndices[idx + 1];
            const uint32_t i2 = quadIndices[idx + 2];
            if (i0 < verts.size() && i1 < verts.size() && i2 < verts.size()) {
              appendVertex(verts[i0], texAt(i0));
              appendVertex(verts[i1], texAt(i1));
              appendVertex(verts[i2], texAt(i2));
            }
          }
        }
        continue;
      }

      if (verts.size() == 4) {
        static constexpr std::array<uint32_t, 6> kQuadTris{0, 1, 2, 0, 2, 3};
        for (auto idx : kQuadTris) {
          if (idx < verts.size()) {
            appendVertex(verts[idx], texAt(idx));
          }
        }
        continue;
      }

      for (size_t idx = 0; idx < verts.size(); ++idx) {
        appendVertex(verts[idx], texAt(idx));
      }
    }

    if (!positions.empty()) {
      payload.entryHasIndices = false;
      payload.entryPrimitive = static_cast<uint32_t>(ZMesh::Type::TRIANGLES);
      payload.entryPositions = std::move(positions);
      payload.entryTexCoords = std::move(texCoords);
      payload.entryFlipped = false;
    }
  };

  if (m_entryExitMeshValid && m_quads.empty() && !m_entryExitMesh.vertices().empty()) {
    populateFromEntryExitMesh();
  } else if (!m_quads.empty()) {
    populateFromQuads();
  }

  CHECK(!payload.entryPositions.empty()) << "Vulkan img raycaster is missing entry geometry for the current draw.";

  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  auto entryLease = pool.acquireEntryExitRenderTarget(m_outputSize,
                                                      2u,
                                                      ScratchFormat::RGBA32F,
                                                      std::optional<RenderBackend>(RenderBackend::Vulkan));
  payload.entryExitLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(std::move(entryLease));

  if (visibleChannels.size() > 1 && payload.fastPathOnly) {
    auto layerLease = pool.acquireLayerArrayRenderTarget(m_outputSize,
                                                         static_cast<uint32_t>(visibleChannels.size()),
                                                         ScratchFormat::RGBA16,
                                                         ScratchFormat::Depth32F,
                                                         std::optional<RenderBackend>(RenderBackend::Vulkan));
    payload.channelLayerLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(std::move(layerLease));
  }

  if (!payload.fastPathOnly) {
    ensureRaycastAccumulators(eye);

    // Create a non-owning "view" lease that snapshots the pointers/descriptor
    // from the persistent member lease. The view has an empty releaser so it
    // will not double-release the underlying slot. This avoids dangling
    // references when the persistent lease is released during backend switches
    // (Vulkan path defers actual slot release until the frame fence signals).
    auto shareLease = [](Z3DScratchResourcePool::RenderTargetLease& src) {
      auto view = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>();
      view->descriptor = src.descriptor;
      view->backend = src.backend;
      view->renderTarget = src.renderTarget;
      view->vulkanImage = src.vulkanImage;
      view->attachments = src.attachments;
      // leave view->releaser empty (no-op) to avoid double release
      return view;
    };

    if (m_lastRaycastAccum[eye]) {
      payload.lastAccumLease = shareLease(m_lastRaycastAccum[eye]);
    }
    if (m_currentRaycastAccum[eye]) {
      payload.currentAccumLease = shareLease(m_currentRaycastAccum[eye]);
    }

    auto blockLease =
      pool.acquireBlockIdRenderTarget(m_outputSize, -1, -1.0, std::optional<RenderBackend>(RenderBackend::Vulkan));
    payload.blockIdAttachmentCount = blockLease.attachments;
    payload.blockIdLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(std::move(blockLease));
  }

  if (!m_blockIDs.empty()) {
    payload.blockIdReadback = m_blockIDs;
  }

  RenderBatch batch;
  batch.eye = eye;
  batch.geometry = std::move(payload);

  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DImgRaycasterRenderer::ensureRaycastAccumulators(Z3DEye eye)
{
  CHECK_GT(m_outputSize.x, 0u);
  CHECK_GT(m_outputSize.y, 0u);

  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    auto ensureLease = [&](Z3DScratchResourcePool::RenderTargetLease& lease) {
      const bool sizeMismatch = lease.descriptor.size != m_outputSize;
      if (!lease.hasVulkanImage() || sizeMismatch) {
        lease.release();
        lease =
          pool.acquireRaycastAccumulatorRenderTarget(m_outputSize, std::optional<RenderBackend>(RenderBackend::Vulkan));
      }
    };
    ensureLease(m_lastRaycastAccum[eye]);
    ensureLease(m_currentRaycastAccum[eye]);
    return;
  }

  auto ensureGlLease = [&](Z3DScratchResourcePool::RenderTargetLease& lease) {
    if (!lease || !lease.hasGLRenderTarget() || lease.renderTarget->size() != m_outputSize) {
      lease.release();
      m_rendererBase.acquirePersistentRaycastAccumulatorRenderTarget(lease, m_outputSize);
    }
  };

  ensureGlLease(m_lastRaycastAccum[eye]);
  ensureGlLease(m_currentRaycastAccum[eye]);
}

void Z3DImgRaycasterRenderer::finalizeProgressiveRound(Z3DEye eye, bool lastRound, size_t channelCount)
{
  std::swap(m_lastRaycastAccum[eye], m_currentRaycastAccum[eye]);

  if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
    return;
  }

  if (lastRound) {
    m_channelIdx[eye] = -1;
    m_round[eye] = 0;
  } else {
    if (m_channelIdx[eye] < 0 && channelCount > 0) {
      // Initialize progressive state; do not count this as a completed full-res
      // round so the first post-render progress reports ~0.5 (GL parity).
      m_channelIdx[eye] = 0;
      // keep m_round[eye] at 0
    } else {
      ++m_round[eye];
    }
  }
}

void Z3DImgRaycasterRenderer::releaseRaycastAccumulators(Z3DEye eye)
{
  m_lastRaycastAccum[eye].release();
  m_currentRaycastAccum[eye].release();
}

void Z3DImgRaycasterRenderer::releaseAllRaycastAccumulators()
{
  for (size_t idx = 0; idx < m_lastRaycastAccum.size(); ++idx) {
    m_lastRaycastAccum[idx].release();
    m_currentRaycastAccum[idx].release();
  }
}

void Z3DImgRaycasterRenderer::setChannelCount(size_t count)
{
  m_channelVisibilities.assign(count, false);
  m_transferFunctions.assign(count, nullptr);
}

void Z3DImgRaycasterRenderer::releaseScratchResources()
{
  if (m_entryExitLease) {
    m_entryExitLease.release();
  }
  if (m_progressiveLayerLease) {
    m_progressiveLayerLease.release();
  }

  releaseAllRaycastAccumulators();
  for (Z3DEye eye : {MonoEye, LeftEye, RightEye}) {
    resetProgress(eye);
  }

  m_entryExitMeshValid = false;
  m_entryExitMeshFlipped = false;
}

void Z3DImgRaycasterRenderer::setChannelVisibility(size_t index, bool visible)
{
  CHECK_LT(index, m_channelVisibilities.size());
  if (m_channelVisibilities[index] == visible) {
    return;
  }
  m_channelVisibilities[index] = visible;
  compile();
}

void Z3DImgRaycasterRenderer::setChannelVisibilities(const std::vector<bool>& visibilities)
{
  CHECK_EQ(m_channelVisibilities.size(), visibilities.size());
  if (m_channelVisibilities == visibilities) {
    return;
  }
  m_channelVisibilities = visibilities;
  compile();
}

void Z3DImgRaycasterRenderer::setTransferFunction(size_t index, Z3DTransferFunction* transferFunction)
{
  CHECK_LT(index, m_transferFunctions.size());
  m_transferFunctions[index] = transferFunction;
}

void Z3DImgRaycasterRenderer::setTransferFunctions(const std::vector<Z3DTransferFunction*>& transferFunctions)
{
  CHECK_EQ(m_transferFunctions.size(), transferFunctions.size());
  m_transferFunctions = transferFunctions;
}

void Z3DImgRaycasterRenderer::addQuad(const ZMesh& quad)
{
  if (quad.empty() || (quad.numVertices() != 4 && quad.numVertices() != 6) ||
      (quad.numVertices() != quad.num2DTextureCoordinates() && quad.numVertices() != quad.num3DTextureCoordinates())) {
    CHECK(false) << "Input quad should be 2D slice with either 2D or 3D texture coordinates";
    return;
  }
  m_quads.push_back(quad);
  m_entryExitLease.release();
}

void Z3DImgRaycasterRenderer::bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const
{
  shader.setLogUniformLocationError(false);

  size_t idx = 0;
  for (size_t i = 0; i < m_img->numChannels(); ++i) {
    if (!m_channelVisibilities[i]) {
      continue;
    }
    CHECK(m_transferFunctions[i] != nullptr);

    // volumes
    auto* texture = m_img->channelTexture(i);
    CHECK(texture != nullptr) << "Missing OpenGL texture for channel " << i;
    shader.bindTexture(m_volumeUniformNames[idx], texture);
    shader.setUniform(m_volumeDimensionNames[idx], glm::vec3(m_img->channelDimensions(i)));

    // transfer functions (GL LUT cache)
    if (auto* tex = transferTextureGL(*m_transferFunctions[i])) {
      shader.bindTexture(m_transferFuncUniformNames[idx++], tex);
    }

    CHECK_GL_ERROR
  }

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const
{
  shader.setLogUniformLocationError(false);

  auto* texture = m_img->channelTexture(idx);
  CHECK(texture != nullptr) << "Missing OpenGL texture for channel " << idx;
  shader.bindTexture(m_volumeUniformNames[0], texture);
  shader.setUniform(m_volumeDimensionNames[0], glm::vec3(m_img->channelDimensions(idx)));

  // transfer functions (GL LUT cache)
  CHECK(idx < m_transferFunctions.size());
  CHECK(m_transferFunctions[idx] != nullptr);
  if (auto* tex = transferTextureGL(*m_transferFunctions[idx])) {
    shader.bindTexture(m_transferFuncUniformNames[0], tex);
  }

  // m_transferFunctions[idx]->texture()->saveAsColorImage("/Users/feng/Downloads/abcd_tf.tif");
  // if (auto* tex = m_img->channelTexture(idx)) {
  //   tex->saveAsColorImage("/Users/feng/Downloads/abcd_v.tif");
  // }

  CHECK_GL_ERROR

  shader.setLogUniformLocationError(true);
}

void Z3DImgRaycasterRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_scRaycasterShader != nullptr);
  DCHECK(m_sc2dImageShader != nullptr);
  DCHECK(m_scVolumeSliceWithTransferfunShader != nullptr);
  DCHECK(m_image3DSliceWithTransferfunBlockIDsShader != nullptr);
  DCHECK(m_image3DSliceWithTransferfunShader != nullptr);
  DCHECK(m_image3DRaycasterBlockIDsShader != nullptr);
  DCHECK(m_image3DRaycasterShader != nullptr);
  DCHECK(m_mergeChannelShader != nullptr);
  DCHECK(m_copyTextureShader != nullptr);
  //  m_raycasterShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_2dImageShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
  //  m_volumeSliceWithTransferfunShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());

  const std::string headerSource = m_rendererBase.generateHeader() + generateHeader();
  m_scRaycasterShader->setHeaderAndRebuild(headerSource);
  m_sc2dImageShader->setHeaderAndRebuild(headerSource);
  m_scVolumeSliceWithTransferfunShader->setHeaderAndRebuild(headerSource);
  m_image3DSliceWithTransferfunBlockIDsShader->setHeaderAndRebuild(headerSource);
  m_image3DSliceWithTransferfunShader->setHeaderAndRebuild(headerSource);
  m_image3DRaycasterBlockIDsShader->setHeaderAndRebuild(headerSource);
  m_image3DRaycasterShader->setHeaderAndRebuild(headerSource);
  m_mergeChannelShader->setHeaderAndRebuild(headerSource);
  m_copyTextureShader->setHeaderAndRebuild(headerSource);
}

void Z3DImgRaycasterRenderer::prepareEntryExit(const ZMesh& clipped, bool flipped, Z3DEye eye, const glm::uvec2& size)
{
  // VLOG(1) << "prepareEntryExit";
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_entryExitMesh = clipped;
    m_entryExitMeshValid = true;
    m_entryExitMeshFlipped = flipped;
    m_quads.clear();
    return;
  }

  // Release any previous entry/exit lease before acquiring a new one to
  // avoid growing the scratch pool unnecessarily.
  if (m_entryExitLease) {
    m_entryExitLease.release();
  }
  m_quads.clear();

  // Acquire entry/exit RT from scratch pool (2-layer RGBA32F array)
  m_rendererBase.acquirePersistentEntryExitRenderTarget(m_entryExitLease, size, 2);

  glEnable(GL_CULL_FACE);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

  // Back faces to slice 1
  m_entryExitLease.renderTarget->attachSlice(1);
  m_entryExitLease.renderTarget->bind();
  glDrawBuffers(1, g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);
  glCullFace(flipped ? GL_BACK : GL_FRONT);
  m_rendererBase.frameState().updateViewportData(size);
  m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
  m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
  m_entryExitLease.renderTarget->release();

  // Front faces to slice 0
  m_entryExitLease.renderTarget->attachSlice(0);
  m_entryExitLease.renderTarget->bind();
  glDrawBuffers(1, g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);
  glCullFace(flipped ? GL_FRONT : GL_BACK);
  m_rendererBase.frameState().updateViewportData(size);
  m_textureAndEyeCoordinateRenderer.setTriangleList(&clipped);
  m_rendererBase.render(eye, m_textureAndEyeCoordinateRenderer);
  m_entryExitLease.renderTarget->release();

  // restore
  glCullFace(GL_BACK);
  glDisable(GL_CULL_FACE);
}

std::string Z3DImgRaycasterRenderer::generateHeader()
{
  std::string header;
  header.reserve(512);

  size_t numVisibleChannels = 0;
  size_t numLevels = 1;
  if (m_img) {
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      if (m_channelVisibilities[i]) {
        ++numVisibleChannels;
      }
    }
    numLevels = m_img->numLevels();
  }

  fmt::format_to(std::back_inserter(header), "#define LEVEL_COUNT {}\n", numLevels);

  if (numVisibleChannels > 0) {
    fmt::format_to(std::back_inserter(header), "#define NUM_VOLUMES {}\n", numVisibleChannels);
  } else {
    absl::StrAppend(&header, "#define NUM_VOLUMES 0\n", "#define DISABLE_TEXTURE_COORD_OUTPUT\n");
  }

  const bool useMIPMerge = m_compositingModeValue == ImgCompositingMode::MaximumIntensityProjection ||
                           m_compositingModeValue == ImgCompositingMode::LocalMIP ||
                           m_compositingModeValue == ImgCompositingMode::MIPOpaque ||
                           m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque;

  switch (m_compositingModeValue) {
    case ImgCompositingMode::DirectVolumeRendering:
      absl::StrAppend(&header,
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeDVR(result, color, currentRayLength, rayDepth);\n");
      break;
    case ImgCompositingMode::IsoSurface:
      absl::StrAppend(&header,
                      "#define ISO\n",
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeISO(result, color, currentRayLength, rayDepth, iso_value);\n");
      break;
    case ImgCompositingMode::MaximumIntensityProjection:
      absl::StrAppend(&header, "#define MIP\n");
      break;
    case ImgCompositingMode::LocalMIP:
      absl::StrAppend(&header, "#define MIP\n", "#define LOCAL_MIP\n");
      break;
    case ImgCompositingMode::XRay:
      absl::StrAppend(&header,
                      "#define COMPOSITING(result, color, currentRayLength, rayDepth) ",
                      "compositeXRay(result, color, currentRayLength, rayDepth);\n");
      break;
    case ImgCompositingMode::MIPOpaque:
      absl::StrAppend(&header, "#define MIP\n", "#define RESULT_OPAQUE\n");
      break;
    case ImgCompositingMode::LocalMIPOpaque:
      absl::StrAppend(&header, "#define MIP\n", "#define LOCAL_MIP\n", "#define RESULT_OPAQUE\n");
      break;
  }

  if (!m_quads.empty() || useMIPMerge) {
    absl::StrAppend(&header, "#define MAX_PROJ_MERGE\n");
  }

  return header;
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
    if (!m_entryExitLease) {
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
    if (m_channelVisibilities[i]) {
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
    if (!m_entryExitLease) {
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
    if (m_channelVisibilities[i]) {
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

bool Z3DImgRaycasterRenderer::hasVisibleRendering() const
{
  if (m_img) {
    for (size_t i = 0; i < m_img->numChannels(); ++i) {
      if (m_channelVisibilities[i]) {
        return true;
      }
    }
  }
  return false;
}

double Z3DImgRaycasterRenderer::progressiveProgress(Z3DEye eye) const
{
  // Count visible channels
  size_t visible = 0;
  for (size_t i = 0; i < m_channelVisibilities.size(); ++i) {
    if (m_channelVisibilities[i]) {
      ++visible;
    }
  }
  if (visible == 0) {
    return 1.0;
  }
  // Derive progress from current channel/round state, matching GL semantics
  const int totalRound = static_cast<int>(visible) * static_cast<int>(FLAGS_atlas_volume_rendering_maximum_round);
  const int chan = m_channelIdx[eye];
  const int round = m_round[eye];
  // If chan < 0, rendering has completed for this eye
  if (chan < 0) {
    return 1.0;
  }
  const int currentRound = chan * static_cast<int>(FLAGS_atlas_volume_rendering_maximum_round) + round;
  if (currentRound >= totalRound) {
    return 1.0;
  }
  return static_cast<double>(currentRound) / static_cast<double>(totalRound) * 0.5 + 0.5;
}

void Z3DImgRaycasterRenderer::render2DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  m_sc2dImageShader->bind();
  m_rendererBase.setGlobalShaderParameters(*m_sc2dImageShader, eye);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(*m_sc2dImageShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(*m_VAO, *m_sc2dImageShader, quad);
    }

  } else {
    layerLease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize, static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      layerLease.renderTarget->attachSlice(j);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();
      bindVolumeAndTransferFunc(*m_sc2dImageShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(*m_VAO, *m_sc2dImageShader, quad);
      }

      layerLease.renderTarget->release();
    }
  }

  m_sc2dImageShader->release();

  if (visibleIdxs.size() > 1) {
    // layerLease already holds the rendered slices
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
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
  if (visibleIdxs.size() > 1) {
    layerLease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize, static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
  }
  size_t idx = 0;
  for (auto c : visibleIdxs) {
    LOG(INFO) << "";
    ZBenchTimer bt(fmt::format("render 2d slice of 3d image ch{}", c));

    processEventsAndMaybeCancel(cancellationToken);

    // Acquire single-attachment Block ID RT for slice-of-3D pass
    auto blockLease = scratchPool.acquireBlockIdRenderTarget(m_outputSize, 1);
    if (blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4 != m_blockIDs.size()) {
      m_blockIDs.resize(blockLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->numPixels() * 4);
      // VLOG(1) << m_blockIDs.size();
    }

    std::vector<uint32_t> missingBlockIDs;
    tbb::concurrent_unordered_set<uint32_t> ccSet;
    { // scope for block id shader
      m_image3DSliceWithTransferfunBlockIDsShader->bind();
      auto guard = folly::makeGuard([=, this]() {
        m_image3DSliceWithTransferfunBlockIDsShader->release();
      });

      m_image3DSliceWithTransferfunBlockIDsShader->setUniform("ze_to_screen_pixel_voxel_size",
                                                              ze_to_screen_pixel_voxel_size);
      m_image3DSliceWithTransferfunBlockIDsShader->setProjectionViewMatrixUniform(eyeState.projectionViewMatrix);
      m_image3DSliceWithTransferfunBlockIDsShader->setViewMatrixUniform(eyeState.viewMatrix);

      // render block ids
      const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0};

      m_img->bindFullResBlockIDsShader(*m_image3DSliceWithTransferfunBlockIDsShader, c);

      for (auto& quad : m_quads) {
        blockLease.renderTarget->bind();
        glDrawBuffers(1, g_drawBuffers);
        glClear(GL_COLOR_BUFFER_BIT);

        renderTriangleList(*m_VAO, *m_image3DSliceWithTransferfunBlockIDsShader, quad);

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
    m_image3DSliceWithTransferfunShader->bind();

    m_image3DSliceWithTransferfunShader->setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);
    m_image3DSliceWithTransferfunShader->setProjectionViewMatrixUniform(eyeState.projectionViewMatrix);
    m_image3DSliceWithTransferfunShader->setViewMatrixUniform(eyeState.viewMatrix);

    if (visibleIdxs.size() == 1) {
      m_img->bindFullResRenderShader(*m_image3DSliceWithTransferfunShader, c);
      CHECK(c < m_transferFunctions.size());
      CHECK(m_transferFunctions[c] != nullptr);
      if (auto* tex = transferTextureGL(*m_transferFunctions[c])) {
        m_image3DSliceWithTransferfunShader->bindTexture("transfer_function", tex);
      }
      for (auto& quad : m_quads) {
        renderTriangleList(*m_VAO, *m_image3DSliceWithTransferfunShader, quad);
      }
    } else {
      layerLease.renderTarget->attachSlice(idx++);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      m_img->bindFullResRenderShader(*m_image3DSliceWithTransferfunShader, c);
      CHECK(c < m_transferFunctions.size());
      CHECK(m_transferFunctions[c] != nullptr);
      if (auto* tex = transferTextureGL(*m_transferFunctions[c])) {
        m_image3DSliceWithTransferfunShader->bindTexture("transfer_function", tex);
      }
      for (auto& quad : m_quads) {
        renderTriangleList(*m_VAO, *m_image3DSliceWithTransferfunShader, quad);
      }

      layerLease.renderTarget->release();
    }

    m_image3DSliceWithTransferfunShader->release();
    // glFinish();
    bt.recordEvent("render image3d slice");
    STOP_AND_VLOG(bt)
  }

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
  }

  CHECK_GL_ERROR

  m_channelIdx[eye] = -1;
  return 1;
}

void Z3DImgRaycasterRenderer::render2DSliceOf3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs)
{
  m_scVolumeSliceWithTransferfunShader->bind();
  m_rendererBase.setGlobalShaderParameters(*m_scVolumeSliceWithTransferfunShader, eye);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(*m_scVolumeSliceWithTransferfunShader, visibleIdxs[0]);

    for (auto& quad : m_quads) {
      renderTriangleList(*m_VAO, *m_scVolumeSliceWithTransferfunShader, quad);
    }
  } else {
    layerLease = Z3DRenderGlobalState::instance().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t j = 0; j < visibleIdxs.size(); ++j) {
      layerLease.renderTarget->attachSlice(j);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      bindVolumeAndTransferFunc(*m_scVolumeSliceWithTransferfunShader, visibleIdxs[j]);

      for (auto& quad : m_quads) {
        renderTriangleList(*m_VAO, *m_scVolumeSliceWithTransferfunShader, quad);
      }

      layerLease.renderTarget->release();
    }
  }

  m_scVolumeSliceWithTransferfunShader->release();

  if (visibleIdxs.size() > 1) {
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
  }

  CHECK_GL_ERROR
}

double Z3DImgRaycasterRenderer::render3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive)
{
  // VLOG(1) << "render3DImage";
  const auto& sceneState = m_rendererBase.sceneState();
  const auto& viewState = m_rendererBase.viewState();
  const auto& monoEyeState = viewState.eyes[MonoEye];

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (progressive && m_channelIdx[eye] < 0) {
    // Acquire and clear the persistent layer array lease used across rounds
    if (m_progressiveLayerLease) {
      // Ensure previous lease is returned to the pool before re-acquiring
      m_progressiveLayerLease.release();
    }
    // Show an initial fast result immediately
    if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
      render3DImageFast(eye, visibleIdxs);
    }
    // Initialize progressive accumulation state
    m_channelIdx[eye] = 0;
    ++m_progressiveGeneration[eye];

    m_rendererBase.acquirePersistentLayerArrayRenderTarget(m_progressiveLayerLease,
                                                           m_outputSize,
                                                           static_cast<uint32_t>(visibleIdxs.size()));
    if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
      // Clear GL-backed slices
      for (size_t idx = 0; idx < visibleIdxs.size(); ++idx) {
        m_progressiveLayerLease.renderTarget->attachSlice(idx);
        m_progressiveLayerLease.renderTarget->bind();
        m_progressiveLayerLease.renderTarget->clear();
        m_progressiveLayerLease.renderTarget->release();
      }
    }
    return 0.5;
  }

  double progress = 1;

  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();

  float n = viewState.nearClip;
  float f = viewState.farClip;
  glm::vec2 pixelEyeSpaceSize = monoEyeState.frustumNearPlaneSize / glm::vec2(m_outputSize);
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float ze_to_zw_a = f * n / (f - n);
  float ze_to_zw_b = 0.5f * (f + n) / (f - n) + 0.5f;
  float ze_to_screen_pixel_voxel_size =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / n * sceneState.devicePixelRatio;
  VLOG(1) << n << " " << f << " " << ze_to_screen_pixel_voxel_size << " " << pixelEyeSpaceSize << " " << ze_to_zw_a
          << " " << ze_to_zw_b;

  processEventsAndMaybeCancel(cancellationToken);

#if defined(__linux__)
  static int dummyidx = -1;
  if (FLAGS_atlas_debug_texture_output) {
    ++dummyidx;
  }
#endif

  if (progressive) {
    ensureRaycastAccumulators(eye);
    CHECK(m_channelIdx[eye] >= 0 && m_channelIdx[eye] < static_cast<int>(visibleIdxs.size()))
      << m_channelIdx[eye] << " " << visibleIdxs.size();
    bool lastRound = render3DImageForOneRound(eye,
                                              visibleIdxs[m_channelIdx[eye]],
                                              m_round[eye],
                                              ze_to_zw_a,
                                              ze_to_zw_b,
                                              ze_to_screen_pixel_voxel_size,
                                              visibleIdxs.size());
    processEventsAndMaybeCancel(cancellationToken);

    auto* lastTarget = m_lastRaycastAccum[eye].renderTarget;
    CHECK(lastTarget);
    CHECK(lastTarget->size() == m_outputSize) << lastTarget->size();

    if (visibleIdxs.size() == 1) {
      m_copyTextureShader->bind();
      m_copyTextureShader->bindTexture("color_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader->bindTexture("depth_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(*m_VAO, *m_copyTextureShader);
      m_copyTextureShader->release();
    } else {
      m_progressiveLayerLease.renderTarget->attachSlice(m_channelIdx[eye]);
      m_progressiveLayerLease.renderTarget->bind();
      m_progressiveLayerLease.renderTarget->clear();

      m_copyTextureShader->bind();
      m_copyTextureShader->bindTexture("color_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_copyTextureShader->bindTexture("depth_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT1));
      renderScreenQuad(*m_VAO, *m_copyTextureShader);
      m_copyTextureShader->release();

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
    ensureRaycastAccumulators(eye);
    layerLease = Z3DRenderGlobalState::instance().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t channelIdx = 0; channelIdx < visibleIdxs.size(); ++channelIdx) {
      auto c = visibleIdxs[channelIdx];
      for (uint32_t round = 0; round < FLAGS_atlas_volume_rendering_maximum_round; ++round) {
        bool lastRound = render3DImageForOneRound(eye,
                                                  c,
                                                  round,
                                                  ze_to_zw_a,
                                                  ze_to_zw_b,
                                                  ze_to_screen_pixel_voxel_size,
                                                  visibleIdxs.size());
#if defined(__linux__)
        if (FLAGS_atlas_debug_texture_output) {
          auto* debugTarget = m_lastRaycastAccum[eye].renderTarget;
          if (debugTarget) {
            auto filen =
              QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att0.tif", dummyidx, c, round));
            debugTarget->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage(filen);
            filen =
              QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_round{}_att1.tif", dummyidx, c, round));
            debugTarget->attachment(GL_COLOR_ATTACHMENT1)->saveAsRGBFloatImage(filen);
            if (round == 0) {
              filen = QString::fromStdString(fmt::format("/data/testoutput/tex_{}_ch{}_entry.tif", dummyidx, c));
              m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage(filen);
            }
          }
        }
#endif
        if (lastRound) {
          break;
        }
      }

      processEventsAndMaybeCancel(cancellationToken);

      auto* lastTarget = m_lastRaycastAccum[eye].renderTarget;
      CHECK(lastTarget);
      CHECK(lastTarget->size() == m_outputSize) << lastTarget->size();

      if (visibleIdxs.size() == 1) {
        m_copyTextureShader->bind();
        m_copyTextureShader->bindTexture("color_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT0));
        m_copyTextureShader->bindTexture("depth_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT1));
        renderScreenQuad(*m_VAO, *m_copyTextureShader);
        m_copyTextureShader->release();
      } else {
        layerLease.renderTarget->attachSlice(channelIdx);
        layerLease.renderTarget->bind();
        layerLease.renderTarget->clear();

        m_copyTextureShader->bind();
        m_copyTextureShader->bindTexture("color_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT0));
        m_copyTextureShader->bindTexture("depth_texture", lastTarget->attachment(GL_COLOR_ATTACHMENT1));
        renderScreenQuad(*m_VAO, *m_copyTextureShader);
        m_copyTextureShader->release();

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
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", mergeColor);
    m_mergeChannelShader->bindTexture("depth_texture", mergeDepth);
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
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
                                                       float ze_to_screen_pixel_voxel_size,
                                                       size_t totalChannels)
{
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  LOG(INFO) << "channel " << c << " round " << round;
  ZBenchTimer bt(fmt::format("render 3D image channel {} round {}", c, round));

  ensureRaycastAccumulators(eye);
  auto* lastTarget = m_lastRaycastAccum[eye].renderTarget;
  auto* currentTarget = m_currentRaycastAccum[eye].renderTarget;
  CHECK(lastTarget);
  CHECK(currentTarget);

  processEventsAndMaybeCancel(cancellationToken);

  m_image3DRaycasterBlockIDsShader->bind();
  //          m_image3DRaycasterBlockIDsShader->setUniform("screen_dim_RCP",
  //                                                      1.f / glm::vec2(m_blockIDsRenderTarget->size()));
  m_image3DRaycasterBlockIDsShader->setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

  // entry exit points
  m_image3DRaycasterBlockIDsShader->bindTexture("ray_entry_exit_tex_coord",
                                                m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));

  m_image3DRaycasterBlockIDsShader->setUniform("sampling_rate", m_samplingRateValue);

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
    lastTarget->bind();
    glDrawBuffers(2, g_drawBuffers);
    lastTarget->clear();
    lastTarget->release();
  }

  // Acquire multi-attachment Block ID RT via scratch pool
  auto blockLease = scratchPool.acquireBlockIdRenderTarget(lastTarget->size());
  blockLease.renderTarget->bind();
  glDrawBuffers(static_cast<GLsizei>(blockLease.attachments), g_drawBuffers);
  glClear(GL_COLOR_BUFFER_BIT);

  m_img->bindFullResBlockIDsShader(*m_image3DRaycasterBlockIDsShader, c);
  m_image3DRaycasterBlockIDsShader->bindTexture("last_ray_depth", lastTarget->attachment(GL_COLOR_ATTACHMENT1));
  renderScreenQuad(*m_VAO, *m_image3DRaycasterBlockIDsShader);

  blockLease.renderTarget->release();
  m_image3DRaycasterBlockIDsShader->release();
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
  m_image3DRaycasterShader->bind();

  m_image3DRaycasterShader->setUniform("ze_to_zw_b", ze_to_zw_b);
  m_image3DRaycasterShader->setUniform("ze_to_zw_a", ze_to_zw_a);
  m_image3DRaycasterShader->setUniform("ze_to_screen_pixel_voxel_size", ze_to_screen_pixel_voxel_size);

  // entry exit points
  m_image3DRaycasterShader->bindTexture("ray_entry_exit_tex_coord",
                                        m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));

  m_image3DRaycasterShader->bindTexture("last_color", lastTarget->attachment(GL_COLOR_ATTACHMENT0));
  m_image3DRaycasterShader->bindTexture("last_ray_depth", lastTarget->attachment(GL_COLOR_ATTACHMENT1));

  if (m_compositingModeValue == ImgCompositingMode::IsoSurface) {
    m_image3DRaycasterShader->setUniform("iso_value", m_isoValue);
  }

  if (m_compositingModeValue == ImgCompositingMode::LocalMIP ||
      m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque) {
    m_image3DRaycasterShader->setUniform("local_MIP_threshold", m_localMIPThreshold);
  }

  m_image3DRaycasterShader->setUniform("sampling_rate", m_samplingRateValue);

  currentTarget->bind();
  glDrawBuffers(2, g_drawBuffers);
  currentTarget->clear();

  m_img->bindFullResRenderShader(*m_image3DRaycasterShader, c);
  CHECK(c < m_transferFunctions.size());
  CHECK(m_transferFunctions[c] != nullptr);
  if (auto* tex = transferTextureGL(*m_transferFunctions[c])) {
    m_image3DRaycasterShader->bindTexture("transfer_function", tex);
  }
  renderScreenQuad(*m_VAO, *m_image3DRaycasterShader);

  currentTarget->release();

  m_image3DRaycasterShader->release();
  // glFinish();
  bt.recordEvent("render image");
  STOP_AND_VLOG(bt)

  finalizeProgressiveRound(eye, lastRound, totalChannels);

  return lastRound;
}

Z3DTexture* Z3DImgRaycasterRenderer::transferTextureGL(const Z3DTransferFunction& tf) const
{
  const uint64_t gen = tf.generation();
  const uint32_t width = static_cast<uint32_t>(tf.dimensions().x);
  auto itMeta = m_transferCache.meta.find(&tf);
  auto itTex = m_transferCache.textures.find(&tf);
  const bool needCreate = itMeta == m_transferCache.meta.end() || itTex == m_transferCache.textures.end() ||
                          itMeta->second.first != gen || itMeta->second.second != width;
  if (needCreate) {
    std::vector<uint8_t> lut;
    tf.buildLUTBGRA8(lut, width);
    if (lut.empty()) {
      return nullptr;
    }
    auto tex =
      std::make_unique<Z3DTexture>(GLint(GL_RGBA8), glm::uvec3(width, 1, 1), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV);
    tex->updateImage(lut.data());
    m_transferCache.textures[&tf] = std::move(tex);
    m_transferCache.meta[&tf] = std::make_pair(gen, width);
  }
  return m_transferCache.textures[&tf].get();
}

bool finalizeImgRaycasterRoundByKey(Z3DRendererBase& rendererBase,
                                    uint64_t streamKey,
                                    Z3DEye eye,
                                    bool lastRound,
                                    uint32_t channelCount)
{
  (void)rendererBase;
  auto* ptr = reinterpret_cast<Z3DPrimitiveRenderer*>(static_cast<uintptr_t>(streamKey));
  if (auto* rc = dynamic_cast<Z3DImgRaycasterRenderer*>(ptr)) {
    rc->finalizeProgressiveRound(eye, lastRound, channelCount);
    return true;
  }
  return false;
}

void Z3DImgRaycasterRenderer::render3DImageFast(Z3DEye /*eye*/, const std::vector<size_t>& visibleIdxs)
{
  m_scRaycasterShader->bind();

  const auto& viewState = m_rendererBase.viewState();
  float n = viewState.nearClip;
  float f = viewState.farClip;
  // http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
  //  zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
  float a = f * n / (f - n);
  float b = 0.5f * (f + n) / (f - n) + 0.5f;
  m_scRaycasterShader->setUniform("ze_to_zw_b", b);
  m_scRaycasterShader->setUniform("ze_to_zw_a", a);

  // entry exit points
  m_scRaycasterShader->bindTexture("ray_entry_exit_tex_coord",
                                   m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
  // m_entryExitLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0)->saveAsRGBAFloatImage("/Users/feng/Downloads/abcd_entryexit.tif");

  if (m_compositingModeValue == ImgCompositingMode::IsoSurface) {
    m_scRaycasterShader->setUniform("iso_value", m_isoValue);
  }

  if (m_compositingModeValue == ImgCompositingMode::LocalMIP ||
      m_compositingModeValue == ImgCompositingMode::LocalMIPOpaque) {
    m_scRaycasterShader->setUniform("local_MIP_threshold", m_localMIPThreshold);
  }

  m_scRaycasterShader->setUniform("sampling_rate", m_samplingRateValue);

  Z3DScratchResourcePool::RenderTargetLease layerLease;
  if (visibleIdxs.size() == 1) {
    bindVolumeAndTransferFunc(*m_scRaycasterShader, visibleIdxs[0]);
    renderScreenQuad(*m_VAO, *m_scRaycasterShader);
  } else {
    CHECK(visibleIdxs.size() > 1);
    layerLease = Z3DRenderGlobalState::instance().scratchPool().acquireLayerArrayRenderTarget(
      m_outputSize,
      static_cast<uint32_t>(visibleIdxs.size()));
    // VLOG(1) << "lease acquired";
    for (size_t i = 0; i < visibleIdxs.size(); ++i) {
      layerLease.renderTarget->attachSlice(i);
      layerLease.renderTarget->bind();
      layerLease.renderTarget->clear();

      bindVolumeAndTransferFunc(*m_scRaycasterShader, visibleIdxs[i]);
      renderScreenQuad(*m_VAO, *m_scRaycasterShader);

      layerLease.renderTarget->release();
    }
    // layerLease.renderTarget->colorTexture()->saveAsColorImage("/Users/feng/Downloads/abcd_b.tif");
  }

  m_scRaycasterShader->release();

  if (visibleIdxs.size() > 1) {
    // Merge after drawing into layerLease
    m_mergeChannelShader->bind();
    m_mergeChannelShader->bindTexture("color_texture", layerLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_mergeChannelShader->bindTexture("depth_texture", layerLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    renderScreenQuad(*m_VAO, *m_mergeChannelShader);
    m_mergeChannelShader->release();
  }

  CHECK_GL_ERROR
}

void Z3DImgRaycasterRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_scRaycasterShader = std::make_unique<Z3DShaderProgram>();
  m_sc2dImageShader = std::make_unique<Z3DShaderProgram>();
  m_scVolumeSliceWithTransferfunShader = std::make_unique<Z3DShaderProgram>();
  m_image3DSliceWithTransferfunBlockIDsShader = std::make_unique<Z3DShaderProgram>();
  m_image3DSliceWithTransferfunShader = std::make_unique<Z3DShaderProgram>();
  m_image3DRaycasterBlockIDsShader = std::make_unique<Z3DShaderProgram>();
  m_image3DRaycasterShader = std::make_unique<Z3DShaderProgram>();
  m_mergeChannelShader = std::make_unique<Z3DShaderProgram>();
  m_copyTextureShader = std::make_unique<Z3DShaderProgram>();
  const std::string headerSource = m_rendererBase.generateHeader() + generateHeader();
  m_scRaycasterShader->loadFromSourceFile("pass.vert", "volume_raycaster_single_channel.frag", headerSource);
  m_sc2dImageShader->loadFromSourceFile("transform_with_2dtexture.vert",
                                        "image2d_with_transfun_single_channel.frag",
                                        headerSource);
  m_scVolumeSliceWithTransferfunShader->loadFromSourceFile("transform_with_3dtexture.vert",
                                                           "volume_slice_with_transfun_single_channel.frag",
                                                           headerSource);
  m_image3DSliceWithTransferfunBlockIDsShader->loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                                  "image3d_slice_with_transfun_blockID.frag",
                                                                  headerSource);
  m_image3DSliceWithTransferfunShader->loadFromSourceFile("transform_with_3dtexture_and_eye_coordinate.vert",
                                                          "image3d_slice_with_transfun.frag",
                                                          headerSource);
  m_image3DRaycasterBlockIDsShader->loadFromSourceFile("pass.vert", "image3d_raycaster_blockID.frag", headerSource);
  m_image3DRaycasterShader->loadFromSourceFile("pass.vert", "image3d_raycaster.frag", headerSource);
  m_mergeChannelShader->loadFromSourceFile("pass.vert", "image2d_array_compositor.frag", headerSource);
  m_copyTextureShader->loadFromSourceFile("pass.vert", "copy_raycaster_image.frag", headerSource);
  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
  CHECK_GL_ERROR
}

void Z3DImgRaycasterRenderer::destroyResources()
{
  m_scRaycasterShader.reset();
  m_sc2dImageShader.reset();
  m_scVolumeSliceWithTransferfunShader.reset();
  m_image3DSliceWithTransferfunBlockIDsShader.reset();
  m_image3DSliceWithTransferfunShader.reset();
  m_image3DRaycasterBlockIDsShader.reset();
  m_image3DRaycasterShader.reset();
  m_mergeChannelShader.reset();
  m_copyTextureShader.reset();
  m_VAO.reset();
}

} // namespace nim
