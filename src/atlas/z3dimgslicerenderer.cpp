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
#include <folly/OperationCancelled.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <absl/strings/str_cat.h>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>

DECLARE_uint32(atlas_volume_rendering_maximum_round);
DECLARE_string(atlas_vk_blockid_compaction_source);

namespace nim {

namespace {

inline constexpr uint64_t kFnv1aOffsetBasis64 = 14695981039346656037ull;
inline constexpr uint64_t kFnv1aPrime64 = 1099511628211ull;

inline void fnv1aUpdateBytes(uint64_t& hash, const void* data, size_t size)
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<uint64_t>(bytes[i]);
    hash *= kFnv1aPrime64;
  }
}

inline void fnv1aUpdateU64(uint64_t& hash, uint64_t v)
{
  fnv1aUpdateBytes(hash, &v, sizeof(v));
}

inline void fnv1aUpdateU32(uint64_t& hash, uint32_t v)
{
  fnv1aUpdateBytes(hash, &v, sizeof(v));
}

inline void fnv1aUpdateFloat(uint64_t& hash, float v)
{
  static_assert(sizeof(float) == sizeof(uint32_t));
  uint32_t bits = 0u;
  std::memcpy(&bits, &v, sizeof(bits));
  fnv1aUpdateU32(hash, bits);
}

inline void fnv1aUpdateVec3(uint64_t& hash, const glm::vec3& v)
{
  fnv1aUpdateFloat(hash, v.x);
  fnv1aUpdateFloat(hash, v.y);
  fnv1aUpdateFloat(hash, v.z);
}

uint64_t computeSliceMeshesSignature(std::span<const ZMesh> slices)
{
  uint64_t hash = kFnv1aOffsetBasis64;
  fnv1aUpdateU64(hash, static_cast<uint64_t>(slices.size()));

  for (const auto& slice : slices) {
    fnv1aUpdateU32(hash, static_cast<uint32_t>(slice.type()));

    const auto& vertices = slice.vertices();
    fnv1aUpdateU64(hash, static_cast<uint64_t>(vertices.size()));
    for (const auto& v : vertices) {
      fnv1aUpdateVec3(hash, v);
    }

    const auto& texCoords = slice.textureCoordinates3D();
    fnv1aUpdateU64(hash, static_cast<uint64_t>(texCoords.size()));
    for (const auto& t : texCoords) {
      fnv1aUpdateVec3(hash, t);
    }

    const auto& indices = slice.indices();
    fnv1aUpdateU64(hash, static_cast<uint64_t>(indices.size()));
    for (const auto idx : indices) {
      fnv1aUpdateU32(hash, idx);
    }
  }

  return hash;
}

} // namespace

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
  // Build raw ZColorMap pointer cache for Vulkan payloads
  m_colormapsRaw.clear();
  m_colormapsRaw.reserve(colormaps.size());
  for (const auto& p : colormaps) {
    m_colormapsRaw.push_back(&(p->get()));
  }

  if (m_img->numChannels() != m_volumeUniformNames.size()) {
    compile();
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

  auto stages = buildVulkanStagePayloads(eye);
  if (stages.empty()) {
    return;
  }

  for (auto& payload : stages) {
    RenderBatch batch;
    batch.eye = eye;
    batch.geometry = std::move(payload);
    m_rendererBase.appendBatch(std::move(batch));
  }
}

std::vector<ImgSlicePayload> Z3DImgSliceRenderer::buildVulkanStagePayloads(Z3DEye eye)
{
  std::vector<ImgSlicePayload> stages;
  if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
    return stages;
  }
  if (!m_img || m_slices.empty()) {
    return stages;
  }
  CHECK(m_outputSize.x > 0u && m_outputSize.y > 0u) << "Vulkan img slice output size is zero.";

  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  ImgSlicePayload common;
  common.streamKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  common.image = m_img;
  common.colormaps = &m_colormapsRaw;
  common.slices = std::span<const ZMesh>(m_slices.data(), m_slices.size());
  common.slicesSignature = computeSliceMeshesSignature(common.slices);
  common.outputSize = m_outputSize;
  common.fastPathOnly = m_fastRendering || !m_img->isVolumeDownsampled();
  common.maxProjectionMerge = true;

  const bool usePaging = !common.fastPathOnly && m_img->isVolumeDownsampled();
  if (usePaging) {
    // Progressive init parity with raycaster:
    // - channelIdx < 0 indicates the initial fast-preview frame (handled by Vulkan pipeline context)
    // - generation bumps only when starting a new progressive cycle
    if (m_channelIdx[eye] < 0) {
      ++m_progressiveGeneration[eye];
    }
    common.progressiveGeneration = m_progressiveGeneration[eye];
    common.channelIndexRaw = m_channelIdx[eye];
    common.roundIndexRaw = m_round[eye];
  }

  const uint32_t channelCount = static_cast<uint32_t>(m_img->numChannels());
  if (channelCount > 1u) {
    // Helper to create a non-owning shared_ptr "view" of a persistent lease to avoid double-release.
    auto shareLease = [](Z3DScratchResourcePool::RenderTargetLease& src) {
      auto view = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>();
      view->descriptor = src.descriptor;
      view->backend = src.backend;
      view->renderTarget = src.renderTarget;
      view->vulkanImage = src.vulkanImage;
      view->attachments = src.attachments;
      // Leave view->releaser empty (no-op): the owning member lease controls lifetime.
      return view;
    };

    if (usePaging) {
      // Keep the layer array persistent across progressive rounds so we can
      // refine channels incrementally (preview -> full-res).
      auto& lease = m_progressiveLayerLease[eye];
      const bool needsReacquire =
        !lease.hasVulkanImage() || lease.descriptor.size != m_outputSize || lease.descriptor.layers != channelCount;
      if (needsReacquire) {
        lease.release();
        lease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize,
                                                          channelCount,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth32F,
                                                          std::optional<RenderBackend>(RenderBackend::Vulkan));
      }
      common.layerLease = shareLease(lease);
    } else {
      // Fast path: allocate a temporary layer array for this frame only.
      auto lease = scratchPool.acquireLayerArrayRenderTarget(m_outputSize,
                                                             channelCount,
                                                             ScratchFormat::RGBA16,
                                                             ScratchFormat::Depth32F,
                                                             std::optional<RenderBackend>(RenderBackend::Vulkan));
      common.layerLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(std::move(lease));
    }
  }

  auto attachBlockIdLeaseIfNeeded = [&](ImgSlicePayload& payload) {
    const bool usePagingLocal = !payload.fastPathOnly && payload.image && payload.image->isVolumeDownsampled();
    if (!usePagingLocal) {
      return;
    }
    if (payload.stage != ImgSlicePayload::Stage::BlockIdDiscovery) {
      return;
    }
    if (payload.channelIndexRaw < 0 || payload.roundIndexRaw != 0) {
      return;
    }
    auto lease = scratchPool.acquireBlockIdRenderTarget(payload.outputSize, 1, 1.0, RenderBackend::Vulkan);
    payload.blockIdLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(std::move(lease));
  };

  stages.reserve(4);
  if (channelCount <= 1u || !common.layerLease) {
    if (usePaging && common.channelIndexRaw >= 0 && common.roundIndexRaw == 0) {
      // Single-channel paging round 0: split block-ID discovery from the
      // output-writing draw so linear scripts can keep raster() callbacks
      // focused on one logical pass / render target.
      ImgSlicePayload blockId = common;
      blockId.stage = ImgSlicePayload::Stage::BlockIdDiscovery;
      attachBlockIdLeaseIfNeeded(blockId);
      stages.push_back(std::move(blockId));

      ImgSlicePayload draw = common;
      draw.stage = ImgSlicePayload::Stage::DrawLayers;
      stages.push_back(std::move(draw));
      return stages;
    }

    ImgSlicePayload draw = common;
    draw.stage = ImgSlicePayload::Stage::DrawLayers;
    stages.push_back(std::move(draw));
    return stages;
  }

  if (usePaging && common.channelIndexRaw >= 0 && common.roundIndexRaw == 0) {
    ImgSlicePayload blockId = common;
    blockId.stage = ImgSlicePayload::Stage::BlockIdDiscovery;
    attachBlockIdLeaseIfNeeded(blockId);
    stages.push_back(std::move(blockId));

    ImgSlicePayload merge = common;
    merge.stage = ImgSlicePayload::Stage::MergeLayers;
    stages.push_back(std::move(merge));
    return stages;
  }

  ImgSlicePayload draw = common;
  draw.stage = ImgSlicePayload::Stage::DrawLayers;
  stages.push_back(std::move(draw));

  ImgSlicePayload merge = common;
  merge.stage = ImgSlicePayload::Stage::MergeLayers;
  stages.push_back(std::move(merge));

  return stages;
}

ZVulkanLinearScript::SegmentHandle
Z3DImgSliceRenderer::recordVulkanStagesToScript(ZVulkanLinearScript& script,
                                                Z3DEye eye,
                                                Z3DScratchResourcePool::RenderTargetLease& outputLease,
                                                ZVulkanLinearScript::SegmentHandle deps)
{
  if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
    return deps;
  }
  CHECK(outputLease.hasVulkanImage()) << "Slice script recording requires a Vulkan output lease";

  auto stages = buildVulkanStagePayloads(eye);
  if (stages.empty()) {
    return deps;
  }

  auto labelForStage = [&](const ImgSlicePayload& stagePayload) -> std::string_view {
    const bool paging =
      stagePayload.image != nullptr && stagePayload.image->isVolumeDownsampled() && !stagePayload.fastPathOnly;

    using Stage = ImgSlicePayload::Stage;
    switch (stagePayload.stage) {
      case Stage::Unspecified:
        CHECK(false) << "Unspecified ImgSlicePayload stage";
        return "slice_unspecified";
      case Stage::DrawLayers: {
        if (!stagePayload.layerLease) {
          return "slice_draw_direct";
        }
        if (paging && stagePayload.channelIndexRaw < 0) {
          return "slice_preview_layers";
        }
        if (paging && stagePayload.roundIndexRaw > 0) {
          return "slice_paged_layer_update";
        }
        return "slice_draw_layers";
      }
      case Stage::BlockIdDiscovery:
        return "slice_block_id";
      case Stage::MergeLayers:
        return (paging && stagePayload.channelIndexRaw < 0) ? "slice_preview_merge" : "slice_merge";
    }
    CHECK(false) << "Unhandled ImgSlicePayload stage";
    return "slice_unknown";
  };

  auto stageWritesOutput = [](const ImgSlicePayload& stagePayload) -> bool {
    using Stage = ImgSlicePayload::Stage;
    switch (stagePayload.stage) {
      case Stage::Unspecified:
        CHECK(false) << "Unspecified ImgSlicePayload stage";
        return false;
      case Stage::DrawLayers:
        // Single-channel: draws directly into the active surface.
        // Multi-channel: draws into layer array (output merge happens later).
        return !stagePayload.layerLease;
      case Stage::BlockIdDiscovery:
        return false;
      case Stage::MergeLayers:
        return true;
    }
    return false;
  };

  auto surfaceFromLeaseWithLayerIndex = [&](const Z3DScratchResourcePool::RenderTargetLease& lease,
                                            uint32_t layerIndex) -> RendererFrameState::ActiveSurface {
    RendererFrameState::ActiveSurface surface = m_rendererBase.describeSurface(lease);
    for (auto& att : surface.colorAttachments) {
      att.handle.index = layerIndex;
    }
    if (surface.depthAttachment) {
      surface.depthAttachment->handle.index = layerIndex;
    }
    return surface;
  };

  auto addExternalUse = [](RenderBatch& batch,
                           const AttachmentHandle& handle,
                           ExternalImageUseKind kind,
                           ExternalImageAspectHint aspectHint) {
    if (!handle.valid()) {
      return;
    }
    batch.pass.externalImageUses.push_back({handle, kind, aspectHint});
  };

  auto layerArraySurfaceHandles = [&](const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& leasePtr)
    -> std::pair<std::vector<AttachmentHandle>, std::vector<AttachmentHandle>> {
    std::vector<AttachmentHandle> colors;
    std::vector<AttachmentHandle> depths;
    if (!leasePtr) {
      return {colors, depths};
    }
    const auto surface = m_rendererBase.describeSurface(*leasePtr);
    if (!surface.colorAttachments.empty()) {
      colors.push_back(surface.colorAttachments.front().handle);
    }
    if (surface.depthAttachment) {
      depths.push_back(surface.depthAttachment->handle);
    }
    return {std::move(colors), std::move(depths)};
  };

  auto blockIdExternalKind = []() -> ExternalImageUseKind {
    const std::string v = FLAGS_atlas_vk_blockid_compaction_source;
    if (v.empty() || v == "buffer" || v == "Buffer" || v == "BUFFER") {
      return ExternalImageUseKind::TransferSrc;
    }
    if (v == "storage" || v == "Storage" || v == "STORAGE") {
      return ExternalImageUseKind::StorageRead;
    }
    if (v == "sampled" || v == "Sampled" || v == "SAMPLED") {
      return ExternalImageUseKind::SampledRead;
    }
    CHECK(false) << "Unknown --atlas_vk_blockid_compaction_source='" << v
                 << "'. Expected one of: buffer, storage, sampled.";
    return ExternalImageUseKind::TransferSrc;
  };

  auto markOutputSurfaceSampled = [&]() {
    for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
      att.finalUse = AttachmentFinalUse::Sampled;
    }
    if (m_rendererBase.frameState().activeSurface.depthAttachment) {
      m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
    }
  };

  ZVulkanLinearScript::SegmentHandle prevStage = deps;
  for (auto& stagePayload : stages) {
    const std::string_view label = labelForStage(stagePayload);
    const bool writesOutput = stageWritesOutput(stagePayload);

    if (stagePayload.stage == ImgSlicePayload::Stage::BlockIdDiscovery) {
      // Block-ID discovery is paging-only and requires per-slice isolation (clear -> draw -> compact)
      // to avoid slice overlap/occlusion. Express it as a single node that emits an ordered
      // batch list (Raster + Compute pairs) so the backend can own segment boundaries.
      auto recordBlockId = [&]() {
        const auto prevViewport = m_rendererBase.frameState().viewport;
        const auto prevSurface = m_rendererBase.frameState().activeSurface;
        auto guard = folly::makeGuard([&]() {
          m_rendererBase.frameState().updateViewportData(prevViewport);
          m_rendererBase.setActiveSurfaceWithLoadStore(prevSurface, Z3DRendererBase::Preserve);
        });

        CHECK(stagePayload.blockIdLease && stagePayload.blockIdLease->hasVulkanImage())
          << "Slice BlockIdDiscovery stage missing Vulkan blockIdLease";
        const glm::uvec2 blockViewportSize = stagePayload.blockIdLease->descriptor.size;
        CHECK_GT(blockViewportSize.x, 0u) << "Slice BlockIdDiscovery blockIdLease has zero width";
        CHECK_GT(blockViewportSize.y, 0u) << "Slice BlockIdDiscovery blockIdLease has zero height";
        m_rendererBase.frameState().updateViewportData(blockViewportSize);
        const auto blockSurface = m_rendererBase.describeSurface(*stagePayload.blockIdLease);
        CHECK(!blockSurface.colorAttachments.empty()) << "Slice blockIdLease missing color attachment";

        const size_t sliceCount = stagePayload.slices.size();
        CHECK_GT(sliceCount, 0u) << "Slice BlockIdDiscovery stage missing slices span";

        for (size_t sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
          // Raster: render block IDs for a single slice into the scratch blockId surface.
          {
            ImgSlicePayload perSlice = stagePayload;
            perSlice.blockIdSliceIndexRaw = static_cast<int32_t>(sliceIndex);
            m_rendererBase.setActiveSurfaceWithLoadStore(*stagePayload.blockIdLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::DontCare,
                                                         StoreOp::DontCare);
            RenderBatch batch;
            batch.eye = eye;
            batch.geometry = std::move(perSlice);
            m_rendererBase.appendBatch(std::move(batch));
          }

          // Compute: compact IDs into buffers for CPU-side union/upload decisions.
          {
            ImgSlicePayload perSlice = stagePayload;
            perSlice.blockIdSliceIndexRaw = static_cast<int32_t>(sliceIndex);
            RenderBatch batch;
            batch.eye = eye;
            batch.pass.kind = BackendPassDesc::Kind::Compute;
            batch.geometry = std::move(perSlice);
            addExternalUse(batch,
                           blockSurface.colorAttachments.front().handle,
                           blockIdExternalKind(),
                           ExternalImageAspectHint::Color);
            m_rendererBase.appendBatch(std::move(batch));
          }
        }
      };

      prevStage =
        prevStage ? script.raster(label, {prevStage}, recordBlockId) : script.raster(label, {}, recordBlockId);
      continue;
    }

    auto recordStage = [&]() {
      const auto prevViewport = m_rendererBase.frameState().viewport;
      const auto prevSurface = m_rendererBase.frameState().activeSurface;
      auto guard = folly::makeGuard([&]() {
        m_rendererBase.frameState().updateViewportData(prevViewport);
        m_rendererBase.setActiveSurfaceWithLoadStore(prevSurface, Z3DRendererBase::Preserve);
      });

      const bool paging =
        stagePayload.image != nullptr && stagePayload.image->isVolumeDownsampled() && !stagePayload.fastPathOnly;

      if (stagePayload.stage == ImgSlicePayload::Stage::MergeLayers) {
        // MergeLayers is the one stage that both writes the output surface and
        // *samples* the layer-array render target. For Vulkan this needs to be
        // represented in pass.externalImageUses so the linear script can
        // pre-register bindless sampled-image indices before recording begins.
        m_rendererBase.frameState().updateViewportData(outputLease.descriptor.size);
        m_rendererBase.setActiveSurfaceWithLoadStore(outputLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        markOutputSurfaceSampled();

        RenderBatch batch;
        batch.eye = eye;
        batch.geometry = stagePayload;

        auto [layerColors, layerDepths] = layerArraySurfaceHandles(stagePayload.layerLease);
        for (const auto& h : layerColors) {
          addExternalUse(batch, h, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color);
        }
        for (const auto& h : layerDepths) {
          addExternalUse(batch, h, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth);
        }
        m_rendererBase.appendBatch(std::move(batch));
        return;
      }

      if (writesOutput) {
        m_rendererBase.frameState().updateViewportData(outputLease.descriptor.size);
        m_rendererBase.setActiveSurfaceWithLoadStore(outputLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        markOutputSurfaceSampled();

        RenderBatch batch;
        batch.eye = eye;
        batch.geometry = stagePayload;
        m_rendererBase.appendBatch(std::move(batch));
        return;
      }

      CHECK(stagePayload.stage == ImgSlicePayload::Stage::DrawLayers)
        << "Unexpected non-output slice stage (expected DrawLayers/MergeLayers)";
      CHECK(stagePayload.layerLease && stagePayload.layerLease->hasVulkanImage())
        << "Slice DrawLayers stage expected a Vulkan layerLease for multi-channel rendering";
      CHECK(stagePayload.image != nullptr) << "Slice DrawLayers stage missing image pointer";

      m_rendererBase.frameState().updateViewportData(stagePayload.layerLease->descriptor.size);

      const uint32_t channelCount = static_cast<uint32_t>(stagePayload.image->numChannels());
      CHECK(channelCount > 1u) << "Slice layered DrawLayers requires multi-channel image";

      auto recordLayer = [&](uint32_t layerIndex) {
        const auto layerSurface = surfaceFromLeaseWithLayerIndex(*stagePayload.layerLease, layerIndex);
        m_rendererBase.setActiveSurfaceWithLoadStore(layerSurface,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);

        RenderBatch batch;
        batch.eye = eye;
        batch.geometry = stagePayload;
        m_rendererBase.appendBatch(std::move(batch));
      };

      if (paging && stagePayload.channelIndexRaw >= 0 && stagePayload.roundIndexRaw > 0) {
        const uint32_t active = static_cast<uint32_t>(stagePayload.channelIndexRaw);
        CHECK(active < channelCount) << "Slice paging active channel out of range: " << active
                                     << " >= " << channelCount;
        recordLayer(active);
      } else {
        for (uint32_t layer = 0; layer < channelCount; ++layer) {
          recordLayer(layer);
        }
      }
    };

    prevStage = prevStage ? script.raster(label, {prevStage}, recordStage) : script.raster(label, {}, recordStage);
  }

  return prevStage;
}

double Z3DImgSliceRenderer::progressiveProgress(Z3DEye eye) const
{
  if (!m_img) {
    return 1.0;
  }
  const size_t channelCount = m_img->numChannels();
  if (channelCount == 0) {
    return 1.0;
  }

  const int chan = m_channelIdx[eye];
  const int round = m_round[eye];
  if (chan < 0) {
    return 1.0;
  }
  const int totalRound = static_cast<int>(channelCount) * static_cast<int>(FLAGS_atlas_volume_rendering_maximum_round);
  const int currentRound = chan * static_cast<int>(FLAGS_atlas_volume_rendering_maximum_round) + round;
  if (totalRound <= 0 || currentRound >= totalRound) {
    return 1.0;
  }
  return static_cast<double>(currentRound) / static_cast<double>(totalRound) * 0.5 + 0.5;
}

void Z3DImgSliceRenderer::resetProgress(Z3DEye eye)
{
  m_progress[eye] = 0;
  m_channelIdx[eye] = -1;
  m_round[eye] = 0;
  if (m_progressiveLayerLease[eye]) {
    m_progressiveLayerLease[eye].release();
  }
}

void Z3DImgSliceRenderer::finalizeProgressiveRound(Z3DEye eye, bool lastRound, size_t channelCount)
{
  if (m_rendererBase.activeBackend() != RenderBackend::Vulkan) {
    return;
  }

  if (m_channelIdx[eye] < 0 && channelCount > 0) {
    // Preview finalized: start progressive paging at channel 0, round 0.
    m_channelIdx[eye] = 0;
    m_round[eye] = 0;
    return;
  }

  if (lastRound) {
    ++m_channelIdx[eye];
    m_round[eye] = 0;
    if (m_channelIdx[eye] < 0 || static_cast<size_t>(m_channelIdx[eye]) >= channelCount) {
      // All channels complete.
      m_channelIdx[eye] = -1;
      m_round[eye] = 0;
      if (m_progressiveLayerLease[eye]) {
        m_progressiveLayerLease[eye].release();
      }
    }
  } else {
    ++m_round[eye];
  }
}

void Z3DImgSliceRenderer::releaseScratchResources()
{
  for (Z3DEye eye : {MonoEye, LeftEye, RightEye}) {
    if (m_progressiveLayerLease[eye]) {
      m_progressiveLayerLease[eye].release();
    }
    m_channelIdx[eye] = -1;
    m_round[eye] = 0;
  }
}

bool finalizeImgSliceRoundByKey(Z3DRendererBase& rendererBase,
                                uint64_t streamKey,
                                Z3DEye eye,
                                bool lastRound,
                                uint32_t channelCount)
{
  auto* ptr = reinterpret_cast<Z3DPrimitiveRenderer*>(static_cast<uintptr_t>(streamKey));
  if (!rendererBase.isRendererRegistered(ptr)) {
    return false;
  }
  if (auto* sr = dynamic_cast<Z3DImgSliceRenderer*>(ptr)) {
    sr->finalizeProgressiveRound(eye, lastRound, channelCount);
    return true;
  }
  return false;
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
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_scVolumeSliceShader != nullptr);
  DCHECK(m_mergeChannelShader != nullptr);
  DCHECK(m_image3DSliceWithColorMapBlockIDsShader != nullptr);
  DCHECK(m_image3DSliceWithColorMapShader != nullptr);
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
  catch (const folly::OperationCancelled&) {
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

    m_img->updateAndUploadPageDirectoryCaches(missingBlockIDs, i, cancellationToken, bt, /*roundIndex=*/0);

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
      //        m_layerTarget->saveAsColorImage("~/Downloads/abcd_b.tif");
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
      // m_layerTarget->saveAsColorImage("~/Downloads/abcd.tif");
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
