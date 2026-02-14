#include "zvulkanspherepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dsphererenderer.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zsysteminfo.h"
#include "zlog.h"
#include "zexception.h"
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>
#include <cstring>
#include <cstdint>

#include <folly/coro/Task.h>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

namespace nim {

namespace {

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

bool clipPlanesEqual(const ClipPlanesState& a, const ClipPlanesState& b)
{
  if (a.enabled != b.enabled || a.planeCount != b.planeCount) {
    return false;
  }
  auto vec4Equal = [](const glm::vec4& x, const glm::vec4& y) {
    return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
  };
  const size_t count = std::min(static_cast<size_t>(a.planeCount), kRenderBatchMaxClipPlanes);
  for (size_t i = 0; i < count; ++i) {
    if (!vec4Equal(a.planes[i], b.planes[i])) {
      return false;
    }
  }
  return true;
}

} // namespace

ZVulkanSpherePipelineContext::ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanSpherePipelineContext::~ZVulkanSpherePipelineContext() = default;

ZVulkanSpherePipelineContext::FormatsKey
ZVulkanSpherePipelineContext::FormatsKey::from(const vulkan::AttachmentFormats& formats)
{
  FormatsKey key{};
  const size_t count = std::min(formats.colorFormats.size(), key.colorFormats.size());
  key.colorCount = static_cast<uint32_t>(count);
  for (size_t i = 0; i < count; ++i) {
    key.colorFormats[i] = formats.colorFormats[i];
  }
  if (formats.depthFormat.has_value()) {
    key.depthFormat = *formats.depthFormat;
  }
  return key;
}

void ZVulkanSpherePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_centerRadiusBuffer = nullptr;
  m_colorBuffer = nullptr;
  m_specularBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_centerRadiusOffset = 0;
  m_colorOffset = 0;
  m_specularOffset = 0;
  m_flagsOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Per-stream UBO offsets are cached per frame-slot in the backend's persistent
  // uniform arena; keep them across submissions to stabilize dynamic offsets.
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
  m_staticCopyPendingKeys.clear();
}

void ZVulkanSpherePipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  for (auto it = m_staticCopyPendingKeys.begin(); it != m_staticCopyPendingKeys.end();) {
    if (it->streamKey == streamKey) {
      it = m_staticCopyPendingKeys.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = m_staticCache.begin(); it != m_staticCache.end();) {
    if (it->first.streamKey != streamKey) {
      ++it;
      continue;
    }
    auto& entry = it->second;
    m_backend.releaseStaticSlice(entry.vbCenterRadius);
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.vbSpecular);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
    it = m_staticCache.erase(it);
  }

  for (auto it = m_secondaryCache.begin(); it != m_secondaryCache.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_secondaryCache.erase(it);
    } else {
      ++it;
    }
  }

  for (auto& [frameKey, cache] : m_uboCacheByFrameKey) {
    (void)frameKey;
    cache.byStream.erase(streamKey);
  }
}

void ZVulkanSpherePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK sphere retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK sphere retained UBO lifetime");
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(
      keepAlive,
      [fence, keep = sp]() mutable -> folly::coro::Task<void> {
        co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
        co_return;
      }(),
      "VK sphere retained UBO lifetime");
  }
  m_retainedUbos.clear();
}

void ZVulkanSpherePipelineContext::resetDescriptors()
{
  m_descriptorSetsByFrameKey.clear();
  m_secondaryCache.clear();
}

void ZVulkanSpherePipelineContext::record(Z3DRendererBase& renderer,
                                          const RenderBatch& batch,
                                          const SpherePayload& payload,
                                          const vk::Viewport& viewport,
                                          const vk::Rect2D& scissor,
                                          vk::raii::CommandBuffer& cmd)
{
  // Ensure last frame's UBOs remain alive until this submission completes.
  flushRetainedUbos();
  if (payload.pointsAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.pointsAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

  // Per-draw diagnostic; keep behind VLOG(2) so --v=1 remains usable for perf runs.
  VLOG(2) << fmt::format(
    "VK sphere draw: picking={} verts={} idx={} buf(center={}, color={}, spec={}, flags={}) offs(cr={}, c={}, s={}, f={})",
    payload.pickingPass,
    m_vertexCount,
    m_indexCount,
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_centerRadiusBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_colorBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_specularBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_flagsBuffer))),
    m_centerRadiusOffset,
    m_colorOffset,
    m_specularOffset,
    m_flagsOffset);

  CHECK(batch.shaderHook.captured) << "Sphere batch missing shader hook snapshot";
  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = batch.shaderHook.type;

  // Match OpenGL: lighting is controlled per-renderer (Z3DPrimitiveRenderer::m_needLighting),
  // not purely by whether the scene has lights. Use the "no lighting" UBO slice whenever the
  // payload opts out of lighting, so unlit overlays (e.g. transform gizmos) render with flat
  // premultiplied colors like the GL backend.
  m_dynLightingOffset = (payload.pickingPass || !payload.wantsLighting) ? m_backend.framePickingLightingOffset()
                                                                        : m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // Descriptor sets are primed in beginRender(); avoid record-time allocation/updates.
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Sphere record called without an active Vulkan frame-slot key";
  const auto itSets = m_descriptorSetsByFrameKey.find(frameKey);
  CHECK(itSets != m_descriptorSetsByFrameKey.end())
    << "Sphere pipeline descriptor sets missing for active frame-slot (expected beginRender priming)";
  FrameDescriptorSets& sets = itSets->second;
  CHECK(sets.placeholder && sets.lighting && sets.transforms)
    << "Sphere pipeline descriptor sets missing (placeholder/lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  const auto& hookPara = batch.shaderHook.para;
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "Sphere DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "sphere dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "sphere dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const auto& formatsOpt = m_backend.currentSegmentFormats();
  CHECK(formatsOpt.has_value()) << "Sphere pipeline context requires an active Vulkan rendering segment";
  const auto& formats = *formatsOpt;

  m_backend.validateFormatsOrCrash(formats, "sphere");

  PipelineKey key;
  key.dynamicMaterial = payload.useDynamicMaterial && !pickingPass;
  key.shaderHookType = shaderHook;
  key.formats = FormatsKey::from(formats);

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Draw-only recording under backend-managed segment: no attachment handling here

  const vk::DescriptorSet ds0 =
    dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet() : sets.placeholder->descriptorSet();
  const std::array<vk::DescriptorSet, 3> boundSets{
    ds0,
    sets.lighting->descriptorSet(),
    sets.transforms->descriptorSet(),
  };

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  drawSpec.descriptorSets = boundSets;

  uint32_t expectedSets = static_cast<uint32_t>(boundSets.size());
  std::array<vk::DescriptorSet, 1> oitSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> extraDescriptorBinds{};
  std::span<const ZVulkanDescriptorBindInfo> extraDescriptorBindsSpan{};
  if (sets.oit) {
    oitSets = {sets.oit->descriptorSet()};
    extraDescriptorBinds[0].firstSet = vkbind::kSetOITParams;
    extraDescriptorBinds[0].sets = oitSets;
    extraDescriptorBinds[0].dynamicOffsets = {};
    extraDescriptorBindsSpan = extraDescriptorBinds;
    expectedSets = std::max(expectedSets, vkbind::kSetOITParams + 1);
  }
  drawSpec.extraDescriptorBinds = extraDescriptorBindsSpan;
  drawSpec.expectedDescriptorSetCount = expectedSets;

  const std::array<vk::Buffer, 4> vertexBuffers{m_centerRadiusBuffer, m_colorBuffer, m_flagsBuffer, m_specularBuffer};
  const std::array<vk::DeviceSize, 4> vertexOffsets{m_centerRadiusOffset,
                                                    m_colorOffset,
                                                    m_flagsOffset,
                                                    m_specularOffset};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    drawSpec.indexBuffer = m_indexUploadBuffer;
    drawSpec.indexOffset = m_indexUploadOffset;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(m_indexCount);
  } else {
    drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  }
  drawSpec.instanceCount = 1;

  const std::array<uint64_t, 4> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[3]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments = (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) &&
                                 (vertexBufferSegmentIds[2] != 0) && (vertexBufferSegmentIds[3] != 0) &&
                                 ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));
  // Dynamic offsets order must match set/binding order:
  // - lighting (set1,b0)
  // - frame transforms (set2,b0)
  // - object transforms (set2,b1)
  // - material (set2,b2)
  const std::array<uint32_t, 4> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset),
                                               static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                               static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                               static_cast<uint32_t>(m_dynMaterialOffset)};
  drawSpec.dynamicOffsets = dynamicOffsets;

  const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                        shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (FLAGS_atlas_vk_cache_draw_secondaries &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedStaticVBThisFrame && allStaticSegments && dsPlaceholderOverride == nullptr && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();
    SecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.picking = pickingPass;
    cacheKey.dynamicMaterial = key.dynamicMaterial;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;

    SecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = boundSets;
    signature.baseDescriptorGenerations = {sets.placeholder->generation(),
                                           sets.lighting->generation(),
                                           sets.transforms->generation()};
    signature.hasOit = static_cast<bool>(sets.oit);
    signature.oitDescriptorSet = sets.oit ? sets.oit->descriptorSet() : vk::DescriptorSet{};
    signature.oitResourcesRevision = sets.oit ? m_backend.oitResourcesRevision() : 0;
    signature.dynamicOffsets = dynamicOffsets;
    signature.vertexBuffers = vertexBuffers;
    signature.vertexOffsets = vertexOffsets;
    signature.vertexBufferSegmentIds = vertexBufferSegmentIds;
    signature.indexBuffer = drawSpec.indexBuffer;
    signature.indexOffset = drawSpec.indexOffset;
    signature.indexBufferSegmentId = indexBufferSegmentId;
    signature.indexType = drawSpec.indexType;
    signature.indexCount = drawSpec.indexCount;
    signature.vertexCount = drawSpec.vertexCount;
    signature.viewport = viewport;
    signature.scissor = scissor;

    auto itCache = m_secondaryCache.find(cacheKey);
    if (itCache != m_secondaryCache.end()) {
      m_backend.notifyDrawSecondaryCacheKeyFound();
      SecondaryCacheEntry& entry = itCache->second;
      const vk::CommandBuffer rawSecondary = entry.commandBuffer;
      if (entry.recorded && rawSecondary != vk::CommandBuffer{}) {
        if (entry.signature == signature) {
          m_backend.notifyDrawSecondaryCacheHit();
          m_backend.notifyDrawSecondaryCacheExecute();
          cmd.executeCommands({rawSecondary});
          m_backend.notifyDrawSubmitted();
          return;
        }
      }
      uint32_t mask = 0;
      const auto& prev = entry.signature;
      if (prev.pipeline != signature.pipeline) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kPipeline;
      }
      if (prev.layout != signature.layout) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kLayout;
      }
      if (prev.baseDescriptorSets != signature.baseDescriptorSets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorSets;
      }
      if (prev.baseDescriptorGenerations != signature.baseDescriptorGenerations) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorGenerations;
      }
      if (prev.hasOit != signature.hasOit) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorPresence;
      }
      if (prev.oitDescriptorSet != signature.oitDescriptorSet) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorSet;
      }
      if (prev.oitResourcesRevision != signature.oitResourcesRevision) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitResourcesRevision;
      }
      if (prev.dynamicOffsets != signature.dynamicOffsets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kDynamicOffsets;
      }
      if (prev.vertexBuffers != signature.vertexBuffers) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexBuffers;
      }
      if (prev.vertexOffsets != signature.vertexOffsets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexOffsets;
      }
      if (prev.vertexBufferSegmentIds != signature.vertexBufferSegmentIds) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexBufferLifetime;
      }
      if (prev.indexBuffer != signature.indexBuffer || prev.indexOffset != signature.indexOffset ||
          prev.indexType != signature.indexType) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexState;
      }
      if (prev.indexBufferSegmentId != signature.indexBufferSegmentId) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexBufferLifetime;
      }
      if (prev.indexCount != signature.indexCount || prev.vertexCount != signature.vertexCount) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kCounts;
      }
      if (prev.viewport.x != signature.viewport.x || prev.viewport.y != signature.viewport.y ||
          prev.viewport.width != signature.viewport.width || prev.viewport.height != signature.viewport.height ||
          prev.viewport.minDepth != signature.viewport.minDepth ||
          prev.viewport.maxDepth != signature.viewport.maxDepth) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kViewport;
      }
      if (prev.scissor.offset.x != signature.scissor.offset.x || prev.scissor.offset.y != signature.scissor.offset.y ||
          prev.scissor.extent.width != signature.scissor.extent.width ||
          prev.scissor.extent.height != signature.scissor.extent.height) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kScissor;
      }
      m_backend.notifyDrawSecondaryCacheSignatureMismatchMask(mask);
    }

    SecondaryCacheEntry& entry = m_secondaryCache[cacheKey];
    entry.signature = signature;
    m_backend.notifyDrawSecondaryCacheBuild();

    vk::SampleCountFlagBits rasterSamples = vk::SampleCountFlagBits::e1;
    bool samplesCaptured = false;
    for (const auto& attachment : batch.pass.colorAttachments) {
      if (!attachment.handle.valid()) {
        continue;
      }
      const auto& tex =
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "sphere secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Sphere secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "sphere secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Sphere secondary: mismatched MSAA sample counts in attachments";
      }
    }

    const bool hasColors = !formats.colorFormats.empty();
    const vk::Format* colorFormatsPtr = hasColors ? formats.colorFormats.data() : nullptr;
    const vk::Format dsFormat = formats.depthFormat.value_or(vk::Format::eUndefined);
    vk::CommandBufferInheritanceRenderingInfo renderingInheritance{};
    renderingInheritance.rasterizationSamples = rasterSamples;
    renderingInheritance.colorAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
    renderingInheritance.pColorAttachmentFormats = colorFormatsPtr;
    renderingInheritance.depthAttachmentFormat = dsFormat;
    // Atlas uses depth-only dynamic rendering segments (pStencilAttachment == nullptr).
    // Keep stencilAttachmentFormat UNDEFINED to match vkCmdBeginRendering.
    renderingInheritance.stencilAttachmentFormat = vk::Format::eUndefined;

    vk::CommandBufferInheritanceInfo inheritance{};
    inheritance.pNext = &renderingInheritance;

    ZVulkanSecondaryBuildInfo secondaryInfo{};
    secondaryInfo.device = &m_backend.device().context().device();
    secondaryInfo.commandPool = m_backend.device().context().commandPool();
    secondaryInfo.inheritance = inheritance;

    entry.commandBuffer = buildStaticSecondary(secondaryInfo, [&](vk::raii::CommandBuffer& secondaryCmd) {
      ZVulkanPipelineCommandRecorder secondaryRecorder(secondaryCmd);
      secondaryRecorder.recordGraphicsDraw(drawSpec);
    });
    entry.recorded = true;

    m_backend.notifyDrawSecondaryCacheExecute();
    cmd.executeCommands({static_cast<vk::CommandBuffer>(entry.commandBuffer)});
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    // Prepare device-local indirect args during init; copies are flushed after init pass ends.
    const bool indexed = (drawSpec.indexCount > 0);
    if (static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE) {
      m_ddpArgsOffset = m_backend.ddpAllocDeviceArgsSlot(indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                                                 : sizeof(VkDrawIndirectCommand));
      if (indexed) {
        struct Cmd
        {
          uint32_t indexCount, instanceCount, firstIndex;
          int32_t vertexOffset;
          uint32_t firstInstance;
        } cmdPayload{drawSpec.indexCount,
                     drawSpec.instanceCount,
                     drawSpec.firstIndex,
                     drawSpec.vertexOffset,
                     drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } cmdPayload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      }
      m_ddpArgsPrepared = true;
    }
    recorder.recordGraphicsDraw(drawSpec);
  } else if (m_backend.ddpIndirectCountEnabled() &&
             shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    // Use the prebuilt device-local args for indirect count draw
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(m_ddpArgsPrepared) << "Sphere DDP peel: args not prepared in init";
      if (indexed) {
        c.drawIndexedIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        c.drawIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

void ZVulkanSpherePipelineContext::ensureDescriptorLayouts()
{
  if (!m_setPlaceholder) {
    m_setPlaceholder = m_backend.dualTexturePlaceholderDescriptorSetLayout();
  }
  if (!m_setLighting) {
    m_setLighting = m_backend.lightingDescriptorSetLayout();
  }
  if (!m_setTransforms) {
    m_setTransforms = m_backend.transformDescriptorSetLayout();
  }
  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

void ZVulkanSpherePipelineContext::ensureDescriptorSets()
{
  CHECK(!m_backend.isRecording()) << "Sphere ensureDescriptorSets called while recording";
  ensureDescriptorLayouts();
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Sphere ensureDescriptorSets called without an active Vulkan frame-slot key";

  FrameDescriptorSets& sets = m_descriptorSetsByFrameKey[frameKey];
  if (!sets.placeholder && m_setPlaceholder) {
    sets.placeholder = m_backend.allocatePersistentDescriptorSet(m_setPlaceholder);
  }
  if (!sets.lighting && m_setLighting) {
    sets.lighting = m_backend.allocatePersistentDescriptorSet(m_setLighting);
  }
  if (!sets.transforms && m_setTransforms) {
    sets.transforms = m_backend.allocatePersistentDescriptorSet(m_setTransforms);
  }
  if (!sets.oit && m_setOIT) {
    sets.oit = m_backend.allocatePersistentDescriptorSet(m_setOIT);
  }

  ensurePlaceholderTexture();
  if (sets.placeholder) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    sets.placeholder->writeTextureOnce(0, tex, m_backend.defaultSampler());
    sets.placeholder->writeTextureOnce(1, tex, m_backend.defaultSampler());
  }

  // Bind dynamic UBOs to the per-frame uniform arena for this frame-slot.
  // NOTE: This is intentionally an update (not write-once) so we rebind if the
  // uniform arena grows and a new VkBuffer is allocated.
  if (sets.lighting) {
    sets.lighting->updateUniformBufferDynamic(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (sets.transforms) {
    sets.transforms->updateUniformBufferDynamic(0, m_backend.uniformArenaBuffer(), sizeof(FrameTransformsUBOStd140));
    sets.transforms->updateUniformBufferDynamic(1,
                                                m_backend.persistentUniformArenaBuffer(),
                                                sizeof(ObjectTransformsUBOStd140));
    sets.transforms->updateUniformBufferDynamic(2, m_backend.persistentUniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }
  if (sets.oit) {
    m_backend.primeOITDescriptorSet(*sets.oit);
  }
}

void ZVulkanSpherePipelineContext::ensureOITResources()
{
  ensureDescriptorSets();
}

 

void ZVulkanSpherePipelineContext::ensurePlaceholderTexture() {}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanSpherePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                      const RenderBatch& batch,
                                                      const SpherePayload& payload,
                                                      bool pickingPass)
{
  CHECK(batch.shaderHook.captured) << "Sphere batch missing shader hook snapshot";
  CHECK(payload.paramsCaptured) << "Sphere payload missing params";
  (void)renderer;

  // Frame transforms are shared per eye and updated once per submission in beginRender().
  m_dynFrameTransformsOffset = m_backend.frameTransformsOffset(batch.eye);

  ObjectTransformsUBOStd140 objectTransforms{};
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  objectTransforms.pos_transform = model;
  // Sphere shaders do not consume the normal matrix; keep identity to avoid
  // redundant inverse/transpose work on static geometry.
  objectTransforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  objectTransforms.parameters = glm::vec4(sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, objectTransforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  // Match OpenGL path: when dynamic-material is disabled, spheres should still
  // use the renderer's uniform material properties (specular/shininess).
  // When dynamic-material is enabled, the shader reads per-vertex material
  // values, but keeping the uniform values consistent is harmless and avoids
  // surprising toggling behaviour.
  material.material_specular = pickingPass ? glm::vec4(0.0f) : payload.params.materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : payload.params.materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Sphere updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];

    for (auto& cached : entries) {
      if (cached.pickingPass != pickingPass) {
        continue;
      }

      m_dynObjectTransformsOffset = cached.objectTransformsOffset;
      m_dynMaterialOffset = cached.materialOffset;

      if (cached.params == payload.params && cached.followCoordTransform == payload.followCoordTransform &&
          cached.followSizeScale == payload.followSizeScale && cached.followOpacity == payload.followOpacity &&
          clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        return;
      }

      std::memcpy(m_backend.persistentUniformMappedAt(cached.objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
                  &objectTransforms,
                  sizeof(objectTransforms));
      std::memcpy(m_backend.persistentUniformMappedAt(cached.materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      cached.params = payload.params;
      cached.followCoordTransform = payload.followCoordTransform;
      cached.followSizeScale = payload.followSizeScale;
      cached.followOpacity = payload.followOpacity;
      cached.clipPlanes = batch.clipPlanes;
      return;
    }

    auto objectSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
    std::memcpy(objectSlice.mapped, &objectTransforms, sizeof(objectTransforms));
    m_dynObjectTransformsOffset = objectSlice.offset;

    auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(materialSlice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = materialSlice.offset;

    UboCacheEntry entry{};
    entry.pickingPass = pickingPass;
    entry.params = payload.params;
    entry.followCoordTransform = payload.followCoordTransform;
    entry.followSizeScale = payload.followSizeScale;
    entry.followOpacity = payload.followOpacity;
    entry.clipPlanes = batch.clipPlanes;
    entry.objectTransformsOffset = objectSlice.offset;
    entry.materialOffset = materialSlice.offset;
    entries.push_back(std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto objectSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
  std::memcpy(objectSlice.mapped, &objectTransforms, sizeof(objectTransforms));
  m_dynObjectTransformsOffset = objectSlice.offset;

  auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(materialSlice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = materialSlice.offset;
}

ZVulkanSpherePipelineContext::PipelineInstance&
ZVulkanSpherePipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  ensureDescriptorLayouts();

  PipelineInstance instance;

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> std::string {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return "dual_peeling_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return "ppll_count_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return "ppll_store_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "sphere.frag.spv";
    }
  };

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "sphere.vert.spv",
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  // Dynamic material specialization (match GL's DYNAMIC_MATERIAL_PROPERTY)
  {
    const uint32_t useDynamic = key.dynamicMaterial ? 1u : 0u;
    std::array<vk::SpecializationMapEntry, 1> entries{
      vk::SpecializationMapEntry{.constantID = 60, .offset = 0, .size = sizeof(uint32_t)}
    };
    std::array<uint32_t, 1> data{useDynamic};
    auto entriesVec = std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end());
    auto dataVec = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(data.data()),
                                        reinterpret_cast<const uint8_t*>(data.data()) + sizeof(data));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, entriesVec, dataVec);
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entriesVec, dataVec);
  }

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{m_setPlaceholder, m_setLighting, m_setTransforms, m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = false;

  auto makeAttachments =
    [&](vk::BlendFactor srcColor, vk::BlendFactor dstColor, vk::BlendFactor srcAlpha, vk::BlendFactor dstAlpha) {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = srcColor;
        attachment.dstColorBlendFactor = dstColor;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = srcAlpha;
        attachment.dstAlphaBlendFactor = dstAlpha;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
    };

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
      makeAttachments(vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendFactor::eOne);
      // Match GL WA init: depth-test against the (loaded) opaque depth buffer
      // when a depth attachment is present, but do not write depth.
      instance.pipeline->setDepthTestEnable(formats.depthFormat.has_value());
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      if (!formats.colorFormats.empty()) {
        std::vector<vk::PipelineColorBlendAttachmentState> attachments;
        attachments.reserve(formats.colorFormats.size());
        for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
          auto state = baseBlend;
          state.blendEnable = true;
          if (i == 0) {
            state.srcColorBlendFactor = vk::BlendFactor::eOne;
            state.dstColorBlendFactor = vk::BlendFactor::eOne;
            state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          } else {
            state.srcColorBlendFactor = vk::BlendFactor::eZero;
            state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
            state.srcAlphaBlendFactor = vk::BlendFactor::eZero;
            state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
          }
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.alphaBlendOp = vk::BlendOp::eAdd;
          attachments.push_back(state);
        }
        instance.pipeline->setColorBlendAttachments(std::move(attachments));
      }
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments;
      attachments.reserve(formats.colorFormats.size());
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = baseBlend;
        if (i == 0 || i == 3) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          // Match GL DDP peel: back-temp uses MAX blending (one fragment per pixel).
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else {
          state.blendEnable = false;
        }
        attachments.push_back(state);
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    default:
      break;
  }

  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanSpherePipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 4> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // centerRadius
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // color
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // specular/shininess
  };
  static std::array<vk::VertexInputAttributeDescription, 4> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 2, .binding = 2, .format = vk::Format::eR32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 3,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               }
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanSpherePipelineContext::uploadGeometry(const SpherePayload& payload)
{
  m_usedStaticVBThisFrame = false;
  m_vertexCount = payload.pointsAndRadius.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.flags.size() == m_vertexCount) << "Vulkan sphere backend skipping batch: flag buffer is incomplete.";

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    const bool pickingPass = payload.pickingPass;
    CacheKey key{payload.streamKey, pickingPass, payload.useDynamicMaterial};
    if (!m_staticCopyPendingKeys.contains(key)) {
      auto it = m_staticCache.find(key);
      if (it != m_staticCache.end()) {
        const CacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
        const uint32_t expectedColorGen = pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        const bool specularSame =
          (!payload.useDynamicMaterial || pickingPass) ? true : (entry.specularGen == payload.specularGen);
        const bool gensSame = (entry.centersGen == payload.centersGen) && (entry.flagsGen == payload.flagsGen) &&
                              (entry.indexGen == payload.indexGen) && (entry.colorsGen == expectedColorGen) &&
                              specularSame;
        if (entry.promoted && sizeSame && gensSame && entry.vbCenterRadius && entry.vbColor && entry.vbSpecular &&
            entry.vbFlags && (m_indexCount == 0 || entry.ib)) {
          m_centerRadiusBuffer = entry.vbCenterRadius.buffer;
          m_colorBuffer = entry.vbColor.buffer;
          m_specularBuffer = entry.vbSpecular.buffer;
          m_flagsBuffer = entry.vbFlags.buffer;
          m_centerRadiusOffset = entry.vbCenterRadius.offset;
          m_colorOffset = entry.vbColor.offset;
          m_specularOffset = entry.vbSpecular.offset;
          m_flagsOffset = entry.vbFlags.offset;
          if (m_indexCount > 0) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          m_backend.pinStaticSliceForActiveSubmission(entry.vbCenterRadius);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbColor);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbSpecular);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbFlags);
          if (m_indexCount > 0) {
            m_backend.pinStaticSliceForActiveSubmission(entry.ib);
          }
          m_usedStaticVBThisFrame = true;
          return;
        }
      }
    }
  }

  const size_t crBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t colBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t spBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t flBytes = m_vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {crBytes,                         alignof(glm::vec4)},
    {colBytes,                        alignof(glm::vec4)},
    {spBytes,                         alignof(glm::vec4)},
    {flBytes,                         alignof(float)    },
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });

  auto crSlice = m_backend.suballocateUpload(crBytes, alignof(glm::vec4));
  auto colSlice = m_backend.suballocateUpload(colBytes, alignof(glm::vec4));
  auto spSlice = m_backend.suballocateUpload(spBytes, alignof(glm::vec4));
  auto flSlice = m_backend.suballocateUpload(flBytes, alignof(float));
  if (!crSlice.buffer || !crSlice.mapped || !colSlice.buffer || !colSlice.mapped || !spSlice.buffer ||
      !spSlice.mapped || !flSlice.buffer || !flSlice.mapped) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }
  std::memcpy(crSlice.mapped, payload.pointsAndRadius.data(), crBytes);
  if (payload.pickingPass) {
    auto* cOut = static_cast<glm::vec4*>(colSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      cOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
    }
  } else {
    auto* cOut = static_cast<glm::vec4*>(colSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      cOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(1.0f);
    }
  }
  // Specular/shininess: per-vertex when using dynamic material; otherwise zeros
  {
    auto* spOut = static_cast<glm::vec4*>(spSlice.mapped);
    if (!payload.pickingPass && payload.useDynamicMaterial) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        spOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
      }
    } else {
      std::memset(spOut, 0, spBytes);
    }
  }
  auto* fOut = static_cast<float*>(flSlice.mapped);
  for (size_t i = 0; i < m_vertexCount; ++i) {
    fOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
  }

  if (m_indexCount > 0) {
    auto iSlice = m_backend.suballocateUpload(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
    if (iSlice.buffer && iSlice.mapped) {
      std::memcpy(iSlice.mapped, payload.indices.data(), m_indexCount * sizeof(uint32_t));
      m_indexUploadBuffer = iSlice.buffer;
      m_indexUploadOffset = iSlice.offset;
    } else {
      m_indexCount = 0;
    }
  } else {
    m_indexUploadBuffer = nullptr;
    m_indexUploadOffset = 0;
  }
  m_centerRadiusBuffer = crSlice.buffer;
  m_colorBuffer = colSlice.buffer;
  m_specularBuffer = spSlice.buffer;
  m_flagsBuffer = flSlice.buffer;
  m_centerRadiusOffset = crSlice.offset;
  m_colorOffset = colSlice.offset;
  m_specularOffset = spSlice.offset;
  m_flagsOffset = flSlice.offset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Sphere payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass, payload.useDynamicMaterial};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.specularGen = payload.specularGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool gensSame = entry.centersGen == payload.centersGen && entry.flagsGen == payload.flagsGen &&
                            entry.indexGen == payload.indexGen &&
                            entry.colorsGen == (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen) &&
                            entry.specularGen == payload.specularGen;
      entry.unchangedFrames = (sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_staticCopyPendingKeys.contains(key)) {
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.specularGen = payload.specularGen;
        return;
      }

      if (entry.promoted && !sizeSame) {
        m_backend.releaseStaticSlice(entry.vbCenterRadius);
        m_backend.releaseStaticSlice(entry.vbColor);
        m_backend.releaseStaticSlice(entry.vbSpecular);
        m_backend.releaseStaticSlice(entry.vbFlags);
        m_backend.releaseStaticSlice(entry.ib);
        entry.promoted = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.specularGen = payload.specularGen;
        return;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = false;
        if (entry.centersGen != payload.centersGen) {
          m_backend.scheduleStaticCopy(entry.vbCenterRadius.buffer, entry.vbCenterRadius.offset, crSlice, false);
          anyChanged = true;
        }
        if (entry.colorsGen != (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen)) {
          m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colSlice, false);
          anyChanged = true;
        }
        if (entry.specularGen != payload.specularGen) {
          m_backend.scheduleStaticCopy(entry.vbSpecular.buffer, entry.vbSpecular.offset, spSlice, false);
          anyChanged = true;
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flSlice, false);
          anyChanged = true;
        }
        if (entry.indexGen != payload.indexGen && m_indexUploadBuffer && m_indexCount > 0) {
          Z3DRendererVulkanBackend::UploadSlice idx{m_indexUploadBuffer,
                                                    m_indexUploadOffset,
                                                    nullptr,
                                                    m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, idx, true);
          anyChanged = true;
        }

        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.specularGen = payload.specularGen;

        // If anything changed, defer restaging to the next frame to avoid
        // hazards. This frame binds upload slices; at the next steady frame
        // promotion/restage will occur.
        if (anyChanged) {
          m_staticCopyPendingKeys.insert(key);
          return;
        }
        // Safety: if we restaged any stream this frame, bind the upload slices
        // for this draw and let the static buffers take effect on the next
        // frame. This avoids driver-dependent hazards on buffer copies.
        if (!anyChanged) {
          m_centerRadiusBuffer = entry.vbCenterRadius.buffer;
          m_colorBuffer = entry.vbColor.buffer;
          m_specularBuffer = entry.vbSpecular.buffer;
          m_flagsBuffer = entry.vbFlags.buffer;
          m_centerRadiusOffset = entry.vbCenterRadius.offset;
          m_colorOffset = entry.vbColor.offset;
          m_specularOffset = entry.vbSpecular.offset;
          m_flagsOffset = entry.vbFlags.offset;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          m_backend.pinStaticSliceForActiveSubmission(entry.vbCenterRadius);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbColor);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbSpecular);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbFlags);
          if (entry.indexCount > 0) {
            m_backend.pinStaticSliceForActiveSubmission(entry.ib);
          }
        }
        return;
      }

      // Not promoted: keep observed state up to date so stability tracking can
      // reach the promotion threshold after a data change.
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.specularGen = payload.specularGen;

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto crDst = m_backend.allocateStaticVB(crBytes, alignof(glm::vec4));
        auto colDst = m_backend.allocateStaticVB(colBytes, alignof(glm::vec4));
        auto flDst = m_backend.allocateStaticVB(flBytes, alignof(float));
        auto spDst = m_backend.allocateStaticVB(spBytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (m_indexCount > 0) {
          ibDst = m_backend.allocateStaticIB(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
        }
        if (crDst && colDst && flDst && spDst && (m_indexCount == 0 || ibDst)) {
          size_t staged = 0;
          m_backend.scheduleStaticCopy(crDst.buffer, crDst.offset, crSlice, false);
          staged += crBytes;
          m_backend.scheduleStaticCopy(colDst.buffer, colDst.offset, colSlice, false);
          staged += colBytes;
          m_backend.scheduleStaticCopy(spDst.buffer, spDst.offset, spSlice, false);
          staged += spBytes;
          m_backend.scheduleStaticCopy(flDst.buffer, flDst.offset, flSlice, false);
          staged += flBytes;
          if (m_indexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice idx{m_indexUploadBuffer,
                                                      m_indexUploadOffset,
                                                      nullptr,
                                                      m_indexCount * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, idx, true);
            staged += m_indexCount * sizeof(uint32_t);
          }
          if (staged > 0) {
            m_backend.addSphereBytesStaged(staged);
          }
          entry.vbCenterRadius = crDst;
          entry.vbColor = colDst;
          entry.vbSpecular = spDst;
          entry.vbFlags = flDst;
          entry.ib = ibDst;
          entry.centersGen = payload.centersGen;
          entry.flagsGen = payload.flagsGen;
          entry.indexGen = payload.indexGen;
          entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
          entry.specularGen = payload.specularGen;
          entry.promoted = true;
          VLOG(1) << fmt::format("VK sphere promote: centerRadius={}B color={}B flags={}B idx={}B",
                                 crBytes,
                                 colBytes,
                                 flBytes,
                                 m_indexCount * sizeof(uint32_t));
          m_staticCopyPendingKeys.insert(key);
          return;
        }
      }
    }
  }
}

} // namespace nim
