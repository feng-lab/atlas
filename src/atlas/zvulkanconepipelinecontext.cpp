#include "zvulkanconepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zexception.h"
#include "zlog.h"
#include "z3dconerenderer.h"
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdint>

#include <folly/coro/Task.h>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

namespace nim {

namespace {

int toCapsMode(ConePayload::CapStyle capStyle)
{
  switch (capStyle) {
    case ConePayload::CapStyle::NoCaps:
      return 0;
    case ConePayload::CapStyle::FlatCaps:
      return 1;
    case ConePayload::CapStyle::RoundCaps:
      return 2;
    case ConePayload::CapStyle::FlatBaseRoundTop:
      return 3;
    case ConePayload::CapStyle::RoundBaseFlatTop:
      return 4;
  }
  return 1;
}

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

const RendererViewState& viewStateForBatch(const Z3DRendererBase& renderer, const RenderBatch& batch)
{
  if (batch.viewStateOverride) {
    return *batch.viewStateOverride;
  }
  return renderer.viewState();
}

} // namespace

ZVulkanConePipelineContext::ZVulkanConePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanConePipelineContext::~ZVulkanConePipelineContext() = default;

ZVulkanConePipelineContext::FormatsKey
ZVulkanConePipelineContext::FormatsKey::from(const vulkan::AttachmentFormats& formats)
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

void ZVulkanConePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_originBuffer = nullptr;
  m_axisBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_baseColorBuffer = nullptr;
  m_topColorBuffer = nullptr;
  m_originOffset = 0;
  m_axisOffset = 0;
  m_flagsOffset = 0;
  m_baseColorOffset = 0;
  m_topColorOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Per-stream UBO offsets are cached per frame-slot in the backend's persistent
  // uniform arena; keep them across submissions to stabilize dynamic offsets.
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
  m_staticCopyPendingKeys.clear();
}

void ZVulkanConePipelineContext::evictStream(uint64_t streamKey)
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
    m_backend.releaseStaticSlice(entry.vbOrigin);
    m_backend.releaseStaticSlice(entry.vbAxis);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.vbBaseColor);
    m_backend.releaseStaticSlice(entry.vbTopColor);
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

void ZVulkanConePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK cone retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK cone retained UBO lifetime");
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(
      keepAlive,
      [fence, keep = sp]() mutable -> folly::coro::Task<void> {
        co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
        co_return;
      }(),
      "VK cone retained UBO lifetime");
  }
  m_retainedUbos.clear();
}

void ZVulkanConePipelineContext::resetDescriptors()
{
  m_descriptorSetsByFrameKey.clear();
  m_secondaryCache.clear();
}

void ZVulkanConePipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const ConePayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Ensure last frame's UBOs remain alive until this submission completes.
  flushRetainedUbos();
  VLOG(2) << "record begin: payload sizes base=" << payload.baseAndRadius.size()
          << " axis=" << payload.axisAndTopRadius.size() << " flags=" << payload.flags.size()
          << " baseColors=" << payload.baseColors.size() << " topColors=" << payload.topColors.size()
          << " pickingColors=" << payload.pickingColors.size() << " indices=" << payload.indices.size()
          << " pickingPass=" << payload.pickingPass << " useConeShader2=" << payload.useConeShader2;
  if (payload.baseAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.baseAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  VLOG(2) << "uploadGeometry -> vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount;
  if (m_vertexCount == 0) {
    return;
  }

  // Per-draw diagnostic; keep behind VLOG(2) so --v=1 remains usable for perf runs.
  VLOG(2) << fmt::format(
    "VK cone draw: picking={} useShader2={} verts={} idx={} buf(origin={}, axis={}, flags={}, base={}, top={}) offs(o={}, a={}, f={}, b={}, t={})",
    payload.pickingPass,
    payload.useConeShader2,
    m_vertexCount,
    m_indexCount,
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_originBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_axisBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_flagsBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_baseColorBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_topColorBuffer))),
    m_originOffset,
    m_axisOffset,
    m_flagsOffset,
    m_baseColorOffset,
    m_topColorOffset);

  CHECK(batch.shaderHook.captured) << "Cone batch missing shader hook snapshot";
  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = batch.shaderHook.type;

  // Match OpenGL: lighting is controlled per-renderer (Z3DPrimitiveRenderer::m_needLighting),
  // not purely by whether the scene has lights. Use the "no lighting" UBO slice whenever the
  // payload opts out of lighting, so unlit overlays (e.g. transform gizmos) render with flat
  // premultiplied colors like the GL backend.
  m_dynLightingOffset = (payload.pickingPass || !payload.wantsLighting) ? m_backend.framePickingLightingOffset()
                                                                        : m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // Descriptor sets are primed in beginRender(); avoid record-time allocation/rewrites.
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Cone record called without an active Vulkan frame-slot key";
  const auto itSets = m_descriptorSetsByFrameKey.find(frameKey);
  CHECK(itSets != m_descriptorSetsByFrameKey.end())
    << "Cone pipeline descriptor sets missing for active frame-slot (expected beginRender priming)";
  FrameDescriptorSets& frameSets = itSets->second;
  CHECK(frameSets.placeholder && frameSets.lighting && frameSets.transforms)
    << "Cone pipeline descriptor sets missing (placeholder/lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  ensurePlaceholderTexture();
  const auto& hookPara = batch.shaderHook.para;
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const auto& formatsOpt = m_backend.currentSegmentFormats();
  CHECK(formatsOpt.has_value()) << "Cone pipeline context requires an active Vulkan rendering segment";
  const auto& formats = *formatsOpt;

  m_backend.validateFormatsOrCrash(formats, "cone");

  PipelineKey key;
  key.dynamicMaterial = !pickingPass;
  key.useConeShader2 = payload.useConeShader2;
  key.capsMode = toCapsMode(payload.capStyle);
  key.shaderHookType = shaderHook;
  key.formats = FormatsKey::from(formats);

  PipelineInstance& pipeline = ensurePipeline(key, formats);
  const vk::DescriptorSet ds0 =
    dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet() : frameSets.placeholder->descriptorSet();
  const std::array<vk::DescriptorSet, 3> boundSets{
    ds0,
    frameSets.lighting->descriptorSet(),
    frameSets.transforms->descriptorSet(),
  };

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  drawSpec.descriptorSets = boundSets;
  // Dynamic offsets order: lighting (set1,b0), transforms (set2,b0), material (set2,b1)
  const std::array<uint32_t, 3> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset),
                                               static_cast<uint32_t>(m_dynTransformsOffset),
                                               static_cast<uint32_t>(m_dynMaterialOffset)};
  drawSpec.dynamicOffsets = dynamicOffsets;

  uint32_t expectedSets = static_cast<uint32_t>(boundSets.size());
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> extraBinds{};
  uint32_t extraBindCount = 0;
  if (frameSets.oit) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitDescriptorSets[0] = frameSets.oit->descriptorSet();
    oitBind.sets = oitDescriptorSets;
    extraBinds[0] = oitBind;
    extraBindCount = 1;
    expectedSets = std::max(expectedSets, vkbind::kSetOITParams + 1);
  }
  drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);
  drawSpec.expectedDescriptorSetCount = expectedSets;

  const std::array<vk::Buffer, 5> vertexBuffers{m_originBuffer,
                                                m_axisBuffer,
                                                m_flagsBuffer,
                                                m_baseColorBuffer,
                                                m_topColorBuffer};
  const std::array<vk::DeviceSize, 5> vertexOffsets{m_originOffset,
                                                    m_axisOffset,
                                                    m_flagsOffset,
                                                    m_baseColorOffset,
                                                    m_topColorOffset};
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

  const std::array<uint64_t, 5> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[3]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[4]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments = (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) &&
                                 (vertexBufferSegmentIds[2] != 0) && (vertexBufferSegmentIds[3] != 0) &&
                                 (vertexBufferSegmentIds[4] != 0) &&
                                 ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));

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
    cacheKey.useConeShader2 = key.useConeShader2;
    cacheKey.capsMode = key.capsMode;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;

    SecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = boundSets;
    signature.hasOit = static_cast<bool>(frameSets.oit);
    signature.oitDescriptorSet = frameSets.oit ? frameSets.oit->descriptorSet() : vk::DescriptorSet{};
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
      if (prev.hasOit != signature.hasOit) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorPresence;
      }
      if (prev.oitDescriptorSet != signature.oitDescriptorSet) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorSet;
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
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "cone secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Cone secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "cone secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Cone secondary: mismatched MSAA sample counts in attachments";
      }
    }

    const vk::Format* colorFormatsPtr = formats.colorFormats.empty() ? nullptr : formats.colorFormats.data();
    const vk::Format depthFormat = formats.depthFormat.value_or(vk::Format::eUndefined);
    vk::CommandBufferInheritanceRenderingInfo renderingInheritance{};
    renderingInheritance.rasterizationSamples = rasterSamples;
    renderingInheritance.colorAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
    renderingInheritance.pColorAttachmentFormats = colorFormatsPtr;
    renderingInheritance.depthAttachmentFormat = depthFormat;
    // Atlas uses depth-only dynamic rendering segments (pStencilAttachment == nullptr).
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
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(m_ddpArgsPrepared) << "Cone DDP peel: args not prepared in init";
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

void ZVulkanConePipelineContext::ensureDescriptorLayouts()
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

void ZVulkanConePipelineContext::ensureDescriptorSets()
{
  CHECK(!m_backend.isRecording()) << "Cone ensureDescriptorSets called while recording";
  ensureDescriptorLayouts();
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Cone ensureDescriptorSets called without an active Vulkan frame-slot key";

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

  // Prime dynamic UBO bindings to the per-frame uniform arena for this frame-slot.
  // This is safe to call each beginRender() because descriptor updates are idempotent.
  if (sets.lighting) {
    sets.lighting->updateUniformBufferDynamic(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (sets.transforms) {
    sets.transforms->updateUniformBufferDynamic(0,
                                                m_backend.persistentUniformArenaBuffer(),
                                                sizeof(TransformsUBOStd140));
    sets.transforms->updateUniformBufferDynamic(1, m_backend.persistentUniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }

  if (sets.oit) {
    m_backend.primeOITDescriptorSet(*sets.oit);
  }
}

void ZVulkanConePipelineContext::ensureOITResources()
{
  ensureDescriptorSets();
}

 

void ZVulkanConePipelineContext::ensurePlaceholderTexture() {}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanConePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const ConePayload& payload,
                                                    bool pickingPass)
{
  CHECK(batch.shaderHook.captured) << "Cone batch missing shader hook snapshot";
  CHECK(payload.paramsCaptured) << "Cone payload missing params";

  const auto& viewState = viewStateForBatch(renderer, batch);
  const auto& eyeState = viewState.eyes[static_cast<size_t>(batch.eye)];

  TransformsUBOStd140 transforms{};
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.view_matrix = eyeState.viewMatrix;
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  const glm::mat4 combined = eyeState.viewMatrix * model;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, transforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  material.material_specular = pickingPass ? glm::vec4(0.0f) : payload.params.materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : payload.params.materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Cone updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];

    auto writeAt = [&](vk::DeviceSize transformsOffset, vk::DeviceSize materialOffset) {
      std::memcpy(m_backend.persistentUniformMappedAt(transformsOffset, sizeof(TransformsUBOStd140)),
                  &transforms,
                  sizeof(transforms));
      std::memcpy(m_backend.persistentUniformMappedAt(materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));
    };

    for (const auto& cached : entries) {
      if (cached.pickingPass != pickingPass || cached.eye != batch.eye) {
        continue;
      }
      m_dynTransformsOffset = cached.transformsOffset;
      m_dynMaterialOffset = cached.materialOffset;
      writeAt(cached.transformsOffset, cached.materialOffset);
      return;
    }

    auto transformsSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(TransformsUBOStd140));
    std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
    m_dynTransformsOffset = transformsSlice.offset;

    auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(materialSlice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = materialSlice.offset;

    UboCacheEntry entry{};
    entry.pickingPass = pickingPass;
    entry.eye = batch.eye;
    entry.transformsOffset = transformsSlice.offset;
    entry.materialOffset = materialSlice.offset;
    entries.push_back(std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto transformsSlice = m_backend.suballocateUniformFor(payload, sizeof(TransformsUBOStd140));
  std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
  m_dynTransformsOffset = transformsSlice.offset;

  auto materialSlice = m_backend.suballocateUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(materialSlice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = materialSlice.offset;

  VLOG(2) << fmt::format("VK cone params: sizeScale={:.3f} alpha={:.3f} picking={} ortho={}",
                         payload.params.sizeScale,
                         material.alpha,
                         pickingPass,
                         (eyeState.isPerspective ? 0 : 1));
}

ZVulkanConePipelineContext::PipelineInstance&
ZVulkanConePipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  ensureDescriptorLayouts();

  PipelineInstance instance;

  auto selectFragmentShader = [&](Z3DRendererBase::ShaderHookType hook) -> std::string {
    const char* suffix = key.useConeShader2 ? "_2" : "";
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return std::string("dual_peeling_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return std::string("dual_peeling_peel_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return std::string("ppll_count_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return std::string("ppll_store_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return std::string("wavg_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return std::string("wblended_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return std::string("cone") + suffix + ".frag.spv";
    }
  };

  const std::string vertName = key.useConeShader2 ? "cone_2.vert.spv" : "cone.vert.spv";
  VLOG(1) << "selecting shaders vert='" << (shaderBase + vertName) << "' frag='"
          << (shaderBase + selectFragmentShader(key.shaderHookType)) << "' capsMode=" << key.capsMode
          << " dynamicMaterial=" << key.dynamicMaterial << " shaderHook=" << static_cast<int>(key.shaderHookType);
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + vertName,
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  std::array<vk::SpecializationMapEntry, 1> specEntries{
    vk::SpecializationMapEntry{.constantID = 90, .offset = 0, .size = sizeof(int)}
  };
  std::array<int, 1> specData{key.capsMode};
  // Apply CAPS_MODE to both vertex and fragment (cone_2.vert depends on it)
  {
    auto entries = std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end());
    auto dataVec = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                        reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, entries, dataVec);
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entries, dataVec);
  }

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{m_setPlaceholder, m_setLighting, m_setTransforms, m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  // Screen-space cone quads (cone_2.vert) end up with CW winding under Vulkan
  // due to clip-space conventions. Treat CW as front to match GL behavior.
  instance.pipeline->setFrontFace(vk::FrontFace::eClockwise);

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = false;

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      // Match GL WA init: depth-test against the (loaded) opaque depth buffer
      // when a depth attachment is present, but do not write depth.
      instance.pipeline->setDepthTestEnable(formats.depthFormat.has_value());
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
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

vk::PipelineVertexInputStateCreateInfo ZVulkanConePipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 5> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // origin
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // base color
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // top color
  };
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
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
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 4,
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

void ZVulkanConePipelineContext::uploadGeometry(const ConePayload& payload)
{
  m_usedStaticVBThisFrame = false;
  m_vertexCount = payload.baseAndRadius.size();
  m_indexCount = payload.indices.size();

  VLOG(2) << "uploadGeometry begin: vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount
          << " axisCount=" << payload.axisAndTopRadius.size() << " flagsCount=" << payload.flags.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axisAndTopRadius.size() == m_vertexCount && payload.flags.size() == m_vertexCount)
    << "Vulkan cone backend skipping batch: attribute buffers are incomplete.";

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    CacheKey key{payload.streamKey, payload.pickingPass};
    if (!m_staticCopyPendingKeys.contains(key)) {
      auto it = m_staticCache.find(key);
      if (it != m_staticCache.end()) {
        const CacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
        const bool shapeSame = (entry.baseGen == payload.baseGen) && (entry.axisGen == payload.axisGen) &&
                               (entry.flagsGen == payload.flagsGen) && (entry.indexGen == payload.indexGen);
        const bool colorsSame = payload.pickingPass ? (entry.pickingColorsGen == payload.pickingColorsGen)
                                                    : ((entry.baseColorGen == payload.baseColorGen) &&
                                                       (entry.topColorGen == payload.topColorGen));
        if (entry.promoted && sizeSame && shapeSame && colorsSame && entry.vbOrigin && entry.vbAxis && entry.vbFlags &&
            entry.vbBaseColor && entry.vbTopColor && (m_indexCount == 0 || entry.ib)) {
          m_originBuffer = entry.vbOrigin.buffer;
          m_axisBuffer = entry.vbAxis.buffer;
          m_flagsBuffer = entry.vbFlags.buffer;
          m_baseColorBuffer = entry.vbBaseColor.buffer;
          m_topColorBuffer = entry.vbTopColor.buffer;
          m_originOffset = entry.vbOrigin.offset;
          m_axisOffset = entry.vbAxis.offset;
          m_flagsOffset = entry.vbFlags.offset;
          m_baseColorOffset = entry.vbBaseColor.offset;
          m_topColorOffset = entry.vbTopColor.offset;
          if (m_indexCount > 0) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          m_backend.pinStaticSliceForActiveSubmission(entry.vbOrigin);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbAxis);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbFlags);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbBaseColor);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbTopColor);
          if (m_indexCount > 0) {
            m_backend.pinStaticSliceForActiveSubmission(entry.ib);
          }
          m_usedStaticVBThisFrame = true;
          return;
        }
      }
    }
  }

  // Allocate SoA slices
  const size_t v4Bytes = m_vertexCount * sizeof(glm::vec4);
  const size_t fBytes = m_vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {v4Bytes,                         alignof(glm::vec4)},
    {v4Bytes,                         alignof(glm::vec4)},
    {fBytes,                          alignof(float)    },
    {v4Bytes,                         alignof(glm::vec4)},
    {v4Bytes,                         alignof(glm::vec4)},
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });
  VLOG(2) << "uploadGeometry allocating: v4Bytes=" << v4Bytes << " fBytes=" << fBytes
          << " idxBytes=" << (m_indexCount * sizeof(uint32_t));
  auto originSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto axisSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto flagsSlice = m_backend.suballocateUpload(fBytes, alignof(float));
  auto baseColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto topColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  VLOG(2) << "Slices: origin(buf=" << static_cast<bool>(originSlice.buffer)
          << ", mapped=" << (originSlice.mapped != nullptr) << ") axis(buf=" << static_cast<bool>(axisSlice.buffer)
          << ", mapped=" << (axisSlice.mapped != nullptr) << ") flags(buf=" << static_cast<bool>(flagsSlice.buffer)
          << ", mapped=" << (flagsSlice.mapped != nullptr)
          << ") baseColor(buf=" << static_cast<bool>(baseColorSlice.buffer)
          << ", mapped=" << (baseColorSlice.mapped != nullptr)
          << ") topColor(buf=" << static_cast<bool>(topColorSlice.buffer)
          << ", mapped=" << (topColorSlice.mapped != nullptr) << ")";
  if (!originSlice.buffer || !originSlice.mapped || !axisSlice.buffer || !axisSlice.mapped || !flagsSlice.buffer ||
      !flagsSlice.mapped || !baseColorSlice.buffer || !baseColorSlice.mapped || !topColorSlice.buffer ||
      !topColorSlice.mapped) {
    VLOG(2) << "uploadGeometry: one or more upload slices are invalid (null buffer or mapping). Aborting batch.";
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }
  VLOG(2) << "uploadGeometry: memcpy origins/axis (" << v4Bytes << " bytes each)";
  auto* originOut = static_cast<glm::vec4*>(originSlice.mapped);
  auto* axisOut = static_cast<glm::vec4*>(axisSlice.mapped);
  auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
  auto* baseColorOut = static_cast<glm::vec4*>(baseColorSlice.mapped);
  auto* topColorOut = static_cast<glm::vec4*>(topColorSlice.mapped);
  std::memcpy(originOut, payload.baseAndRadius.data(), v4Bytes);
  std::memcpy(axisOut, payload.axisAndTopRadius.data(), v4Bytes);
  VLOG(2) << "uploadGeometry: filled origin/axis, writing flags/colors";
  for (size_t i = 0; i < m_vertexCount; ++i) {
    float flagsValue = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    // GL parity: keep flags encoding as produced by the GL path.
    // - For cone.vert (8-vertex path), flags are encoded as (right<<8 | up<<4 | out)
    // - For cone_2.vert (quad path), flags are encoded as (right<<4 | up)
    flagsOut[i] = flagsValue;
  }
  if (payload.pickingPass) {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      glm::vec4 c = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
      baseColorOut[i] = c;
      topColorOut[i] = c;
    }
  } else {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      glm::vec4 base = (i < payload.baseColors.size()) ? payload.baseColors[i] : glm::vec4(1.0f);
      baseColorOut[i] = base;
      glm::vec4 top = base;
      if (!payload.sameColorForBaseAndTop && i < payload.topColors.size()) {
        top = payload.topColors[i];
      }
      topColorOut[i] = top;
    }
  }

  if (m_indexCount > 0) {
    VLOG(2) << "uploadGeometry: allocating/staging indices bytes=" << (m_indexCount * sizeof(uint32_t));
    auto iSlice = m_backend.suballocateUpload(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
    if (iSlice.buffer && iSlice.mapped) {
      std::memcpy(iSlice.mapped, payload.indices.data(), m_indexCount * sizeof(uint32_t));
      m_indexUploadBuffer = iSlice.buffer;
      m_indexUploadOffset = iSlice.offset;
      VLOG(2) << "uploadGeometry: indices staged at offset=" << m_indexUploadOffset;
    } else {
      VLOG(2) << "uploadGeometry: index upload slice invalid; drawing non-indexed";
      m_indexCount = 0;
    }
  } else {
    m_indexUploadBuffer = nullptr;
    m_indexUploadOffset = 0;
  }
  m_originBuffer = originSlice.buffer;
  m_axisBuffer = axisSlice.buffer;
  m_flagsBuffer = flagsSlice.buffer;
  m_baseColorBuffer = baseColorSlice.buffer;
  m_topColorBuffer = topColorSlice.buffer;
  m_originOffset = originSlice.offset;
  m_axisOffset = axisSlice.offset;
  m_flagsOffset = flagsSlice.offset;
  m_baseColorOffset = baseColorSlice.offset;
  m_topColorOffset = topColorSlice.offset;
  auto bufToU64 = [](vk::Buffer b) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(b)));
  };
  VLOG(2) << "uploadGeometry: VB buffers origin=" << bufToU64(m_originBuffer) << " axis=" << bufToU64(m_axisBuffer)
          << " flags=" << bufToU64(m_flagsBuffer) << " baseCol=" << bufToU64(m_baseColorBuffer)
          << " topCol=" << bufToU64(m_topColorBuffer);
  VLOG(2) << "uploadGeometry: VB offsets origin=" << m_originOffset << " axis=" << m_axisOffset
          << " flags=" << m_flagsOffset << " baseCol=" << m_baseColorOffset << " topCol=" << m_topColorOffset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Cone payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.baseGen = payload.baseGen;
      entry.axisGen = payload.axisGen;
      entry.baseColorGen = payload.baseColorGen;
      entry.topColorGen = payload.topColorGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool shapeSame = entry.baseGen == payload.baseGen && entry.axisGen == payload.axisGen &&
                             entry.flagsGen == payload.flagsGen && entry.indexGen == payload.indexGen;
      if (sizeSame && shapeSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_staticCopyPendingKeys.contains(key)) {
        // Still update observed state so stability tracking stays correct.
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.baseGen = payload.baseGen;
        entry.axisGen = payload.axisGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.baseColorGen = payload.baseColorGen;
        entry.topColorGen = payload.topColorGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        return;
      }

      // If the stream was previously promoted but the geometry sizes changed,
      // drop the old static slices so this stream can promote again later.
      if (entry.promoted && !sizeSame) {
        m_backend.releaseStaticSlice(entry.vbOrigin);
        m_backend.releaseStaticSlice(entry.vbAxis);
        m_backend.releaseStaticSlice(entry.vbFlags);
        m_backend.releaseStaticSlice(entry.vbBaseColor);
        m_backend.releaseStaticSlice(entry.vbTopColor);
        m_backend.releaseStaticSlice(entry.ib);
        entry.promoted = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.baseGen = payload.baseGen;
        entry.axisGen = payload.axisGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.baseColorGen = payload.baseColorGen;
        entry.topColorGen = payload.topColorGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        return;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = false;
        const bool baseColorChanged =
          (entry.baseColorGen != payload.baseColorGen) || (entry.pickingColorsGen != payload.pickingColorsGen);
        if (baseColorChanged) {
          m_backend.scheduleStaticCopy(entry.vbBaseColor.buffer, entry.vbBaseColor.offset, baseColorSlice, false);
          anyChanged = true;
        }
        const bool topColorChanged = (!payload.sameColorForBaseAndTop && entry.topColorGen != payload.topColorGen) ||
                                     (payload.sameColorForBaseAndTop && baseColorChanged);
        if (topColorChanged && entry.vbTopColor) {
          m_backend.scheduleStaticCopy(entry.vbTopColor.buffer, entry.vbTopColor.offset, topColorSlice, false);
          anyChanged = true;
        }
        if (entry.axisGen != payload.axisGen) {
          m_backend.scheduleStaticCopy(entry.vbAxis.buffer, entry.vbAxis.offset, axisSlice, false);
          anyChanged = true;
        }
        if (entry.baseGen != payload.baseGen) {
          m_backend.scheduleStaticCopy(entry.vbOrigin.buffer, entry.vbOrigin.offset, originSlice, false);
          anyChanged = true;
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
          anyChanged = true;
        }
        if (entry.indexGen != payload.indexGen && m_indexCount > 0 && m_indexUploadBuffer) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                        m_indexUploadOffset,
                                                        nullptr,
                                                        m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, iUpload, true);
          anyChanged = true;
        }

        // Cache the new observed gens immediately. Restaged buffers take effect
        // on the next submission; this frame binds upload slices when anyChanged.
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.baseGen = payload.baseGen;
        entry.axisGen = payload.axisGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        entry.baseColorGen = payload.baseColorGen;
        entry.topColorGen = payload.topColorGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        if (!anyChanged) {
          m_originBuffer = entry.vbOrigin.buffer;
          m_axisBuffer = entry.vbAxis.buffer;
          m_flagsBuffer = entry.vbFlags.buffer;
          m_baseColorBuffer = entry.vbBaseColor.buffer;
          m_topColorBuffer = entry.vbTopColor.buffer;
          m_originOffset = entry.vbOrigin.offset;
          m_axisOffset = entry.vbAxis.offset;
          m_flagsOffset = entry.vbFlags.offset;
          m_baseColorOffset = entry.vbBaseColor.offset;
          m_topColorOffset = entry.vbTopColor.offset;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          m_backend.pinStaticSliceForActiveSubmission(entry.vbOrigin);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbAxis);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbFlags);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbBaseColor);
          m_backend.pinStaticSliceForActiveSubmission(entry.vbTopColor);
          if (entry.indexCount > 0) {
            m_backend.pinStaticSliceForActiveSubmission(entry.ib);
          }
          m_usedStaticVBThisFrame = true;
        } else {
          // Defer promotion copies to the next frame; use upload slices now.
          m_staticCopyPendingKeys.insert(key);
          return;
        }
        return;
      }

      // Not promoted: keep observed counts/gens up-to-date so unchangedFrames
      // can reach the promotion threshold after a data change.
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.baseGen = payload.baseGen;
      entry.axisGen = payload.axisGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      entry.baseColorGen = payload.baseColorGen;
      entry.topColorGen = payload.topColorGen;
      entry.pickingColorsGen = payload.pickingColorsGen;

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto originDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto axisDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto flagsDst = m_backend.allocateStaticVB(fBytes, alignof(float));
        auto baseColorDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto topColorDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (m_indexCount > 0) {
          ibDst = m_backend.allocateStaticIB(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
        }
        if (originDst && axisDst && flagsDst && baseColorDst && topColorDst && (m_indexCount == 0 || ibDst)) {
          m_backend.scheduleStaticCopy(originDst.buffer, originDst.offset, originSlice, false);
          m_backend.scheduleStaticCopy(axisDst.buffer, axisDst.offset, axisSlice, false);
          m_backend.scheduleStaticCopy(flagsDst.buffer, flagsDst.offset, flagsSlice, false);
          m_backend.scheduleStaticCopy(baseColorDst.buffer, baseColorDst.offset, baseColorSlice, false);
          m_backend.scheduleStaticCopy(topColorDst.buffer, topColorDst.offset, topColorSlice, false);
          if (m_indexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          m_indexCount * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, iUpload, /*isIndexBuffer=*/true);
          }
          VLOG(1) << fmt::format("VK cone promote: origin={}B axis={}B flags={}B baseColor={}B topColor={}B idx={}B",
                                 v4Bytes,
                                 v4Bytes,
                                 fBytes,
                                 v4Bytes,
                                 v4Bytes,
                                 m_indexCount * sizeof(uint32_t));
          entry.vbOrigin = originDst;
          entry.vbAxis = axisDst;
          entry.vbFlags = flagsDst;
          entry.vbBaseColor = baseColorDst;
          entry.vbTopColor = topColorDst;
          entry.ib = ibDst;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_staticCopyPendingKeys.insert(key);
        }
      }
    }
  }
}

} // namespace nim
