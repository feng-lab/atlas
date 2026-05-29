#include "zvulkanellipsoidpipelinecontext.h"
#include "zcommandlineflags.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "z3dellipsoidrenderer.h"
#include "zvulkanuniforms.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zlog.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zexception.h"

#include <QString>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cstring>
#include <cstdint>

ABSL_DECLARE_FLAG(bool, atlas_vk_cache_draw_secondaries);

namespace nim {
namespace {

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

bool clipPlanesEqual(const ClipPlanesState& a, const ClipPlanesState& b)
{
  if (a.captured != b.captured) {
    return false;
  }
  if (a.enabled != b.enabled) {
    return false;
  }
  if (a.planeCount != b.planeCount) {
    return false;
  }

  const uint32_t count = std::min<uint32_t>(a.planeCount, static_cast<uint32_t>(a.planes.size()));
  for (uint32_t i = 0; i < count; ++i) {
    const glm::vec4 pa = a.planes[i];
    const glm::vec4 pb = b.planes[i];
    if (pa.x != pb.x || pa.y != pb.y || pa.z != pb.z || pa.w != pb.w) {
      return false;
    }
  }
  return true;
}

size_t staticSliceBytes(const Z3DRendererVulkanBackend::StaticSlice& slice)
{
  return slice ? slice.size : 0u;
}

} // namespace

ZVulkanEllipsoidPipelineContext::ZVulkanEllipsoidPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_streamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner::Ellipsoid)
{}

ZVulkanEllipsoidPipelineContext::~ZVulkanEllipsoidPipelineContext() = default;

void ZVulkanEllipsoidPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_axis1Buffer = VK_NULL_HANDLE;
  m_axis2Buffer = VK_NULL_HANDLE;
  m_axis3Buffer = VK_NULL_HANDLE;
  m_centerBuffer = VK_NULL_HANDLE;
  m_colorBuffer = VK_NULL_HANDLE;
  m_flagsBuffer = VK_NULL_HANDLE;
  m_specularBuffer = VK_NULL_HANDLE;
  m_axis1Offset = 0;
  m_axis2Offset = 0;
  m_axis3Offset = 0;
  m_centerOffset = 0;
  m_colorOffset = 0;
  m_flagsOffset = 0;
  m_specularOffset = 0;
  m_indexUploadBuffer = VK_NULL_HANDLE;
  m_indexUploadOffset = 0;
  m_ddpArgsByStream.clear();
  m_geometryStreamCache.resetFrame();
  m_appearanceStreamCache.resetFrame();
}

void ZVulkanEllipsoidPipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  m_streamUsageTracker.eraseStream(streamKey);

  m_geometryStreamCache.evictStream(streamKey, [this](GeometryCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbAxis1);
    m_backend.releaseStaticSlice(entry.vbAxis2);
    m_backend.releaseStaticSlice(entry.vbAxis3);
    m_backend.releaseStaticSlice(entry.vbCenter);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
  });
  m_appearanceStreamCache.evictStream(streamKey, [this](AppearanceCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.vbSpecular);
  });

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

  for (auto it = m_ddpArgsByStream.begin(); it != m_ddpArgsByStream.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_ddpArgsByStream.erase(it);
    } else {
      ++it;
    }
  }
}

void ZVulkanEllipsoidPipelineContext::touchStaticStream(uint64_t streamKey)
{
  m_streamUsageTracker.touch(streamKey, m_backend.currentStaticCacheEpoch());
}

size_t
ZVulkanEllipsoidPipelineContext::staticBytesForStream(uint64_t streamKey,
                                                      Z3DRendererVulkanBackend::StaticPressureDomain domain) const
{
  return m_geometryStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const GeometryCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbAxis1) + staticSliceBytes(entry.vbAxis2) +
                      staticSliceBytes(entry.vbAxis3) + staticSliceBytes(entry.vbCenter) +
                      staticSliceBytes(entry.vbFlags);
             }
             return staticSliceBytes(entry.ib);
           }) +
         m_appearanceStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const AppearanceCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbColor) + staticSliceBytes(entry.vbSpecular);
             }
             return size_t{0};
           });
}

std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
ZVulkanEllipsoidPipelineContext::oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                                             uint64_t protectedEpoch) const
{
  return m_streamUsageTracker.oldestEvictableStaticStream(
    domain,
    protectedEpoch,
    [this](uint64_t streamKey, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      return staticBytesForStream(streamKey, cacheDomain);
    });
}

size_t ZVulkanEllipsoidPipelineContext::evictStaticStreamForPressure(uint64_t streamKey)
{
  const size_t vbBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Vertex);
  const size_t ibBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Index);
  evictStream(streamKey);
  return vbBytes + ibBytes;
}

void ZVulkanEllipsoidPipelineContext::record(Z3DRendererBase& renderer,
                                             const RenderBatch& batch,
                                             const EllipsoidPayload& payload,
                                             const vk::Viewport& viewport,
                                             const vk::Rect2D& scissor,
                                             vk::raii::CommandBuffer& cmd)
{
  if (payload.centers.empty()) {
    return;
  }

  // GL parity: in picking pass, skip if per-ellipsoid picking colors are missing/mismatched.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.centers.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

  CHECK(batch.shaderHook.captured) << "Ellipsoid batch missing shader hook snapshot";
  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = batch.shaderHook.type;

  // Match OpenGL: lighting is controlled per-renderer (Z3DPrimitiveRenderer::m_needLighting),
  // not purely by whether the scene has lights.
  m_dynLightingOffset = (payload.pickingPass || !payload.wantsLighting) ? m_backend.framePickingLightingOffset()
                                                                        : m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // Descriptor sets are primed by the backend in beginRender(); avoid record-time rewrites.
  const vk::DescriptorSet dsLighting = m_backend.sharedLightingDescriptorSet();
  const vk::DescriptorSet dsTransforms = m_backend.sharedTransformsDescriptorSetPersistent();
  CHECK(dsLighting && dsTransforms) << "Ellipsoid pipeline shared descriptor sets missing (lighting/transforms)";

  const auto& hookPara = batch.shaderHook.para;
  DDPPeelPushConstants ddpPc{};
  const bool usesDdpPeelPc = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (usesDdpPeelPc) {
    CHECK(hookPara.dualDepthPeelingDepthBlenderHandle.valid())
      << "Ellipsoid DDP peel requires a valid depth blender handle";
    CHECK(hookPara.dualDepthPeelingFrontBlenderHandle.valid())
      << "Ellipsoid DDP peel requires a valid front blender handle";
    auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                               m_backend.device(),
                                               "ellipsoid dual-depth-peeling depth blender");
    auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                               m_backend.device(),
                                               "ellipsoid dual-depth-peeling front blender");
    ddpPc.ddpDepthBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(depthTex, "ellipsoid ddp depth blender");
    ddpPc.ddpFrontBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(frontTex, "ellipsoid ddp front blender");
  }

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);

  m_backend.validateFormatsOrCrash(formats, "ellipsoid");

  PipelineKey key;
  key.dynamicMaterial = payload.useDynamicMaterial && !pickingPass;
  key.fogMode = renderer.sceneState().fog.mode;
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Draw-only path: backend manages render area/attachments

  // Build draw-only spec
  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetBindlessSampledImages;

  const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                        dsLighting,
                                                        dsTransforms};
  drawSpec.descriptorSets = descriptorSets;
  const std::array<uint32_t, 4> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset),
                                               static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                               static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                               static_cast<uint32_t>(m_dynMaterialOffset)};
  drawSpec.dynamicOffsets = dynamicOffsets;

  if (usesDdpPeelPc) {
    drawSpec.pushConstantsData = &ddpPc;
    drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(ddpPc));
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
  }

  uint32_t expectedSets = static_cast<uint32_t>(descriptorSets.size());
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> extraBinds{};
  // Ellipsoid pipelines always include set 3 (OIT params) in the layout.
  ZVulkanDescriptorBindInfo oitBind{};
  oitBind.firstSet = vkbind::kSetOIT;
  oitDescriptorSets[0] = m_backend.sharedOITDescriptorSet();
  oitBind.sets = oitDescriptorSets;
  extraBinds[0] = oitBind;
  const uint32_t extraBindCount = 1;
  expectedSets = std::max(expectedSets, vkbind::kSetOIT + 1u);
  drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);
  drawSpec.expectedDescriptorSetCount = expectedSets;

  const std::array<vk::Buffer, 7> vertexBuffers{m_axis1Buffer,
                                                m_axis2Buffer,
                                                m_axis3Buffer,
                                                m_centerBuffer,
                                                m_colorBuffer,
                                                m_flagsBuffer,
                                                m_specularBuffer};
  const std::array<vk::DeviceSize, 7> vertexOffsets{m_axis1Offset,
                                                    m_axis2Offset,
                                                    m_axis3Offset,
                                                    m_centerOffset,
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

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Ellipsoid record called without an active Vulkan frame-slot key";

  const std::array<uint64_t, 7> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[3]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[4]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[5]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[6]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments =
    (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) && (vertexBufferSegmentIds[2] != 0) &&
    (vertexBufferSegmentIds[3] != 0) && (vertexBufferSegmentIds[4] != 0) && (vertexBufferSegmentIds[5] != 0) &&
    (vertexBufferSegmentIds[6] != 0) && ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));

  const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                        shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (absl::GetFlag(FLAGS_atlas_vk_cache_draw_secondaries) &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();
    SecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.streamSegmentOrdinal = payload.streamSegmentOrdinal;
    cacheKey.picking = pickingPass;
    cacheKey.dynamicMaterial = key.dynamicMaterial;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;
    cacheKey.oitRingIndex = m_backend.sharedOITDescriptorSetRingIndex();

    SecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = descriptorSets;
    signature.baseDescriptorGenerations = {m_backend.bindlessSampledImageDescriptorSetGeneration(),
                                           m_backend.sharedLightingDescriptorSetGeneration(),
                                           m_backend.sharedTransformsDescriptorSetPersistentGeneration()};
    signature.hasOit = true;
    signature.oitDescriptorSet = m_backend.sharedOITDescriptorSet();
    signature.oitDescriptorGeneration = m_backend.sharedOITDescriptorSetGeneration();
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
      if (prev.oitDescriptorGeneration != signature.oitDescriptorGeneration) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorGeneration;
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
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "ellipsoid secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples)
          << "Ellipsoid secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "ellipsoid secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples)
          << "Ellipsoid secondary: mismatched MSAA sample counts in attachments";
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
  auto prepareDdpArgs = [&](DDPArgs& ddp, bool indexed, const char* context) {
    if (ddp.prepared) {
      CHECK(ddp.indexed == indexed) << context << ": indexed mode mismatch for streamKey";
      CHECK(ddp.indexCount == drawSpec.indexCount) << context << ": index count mismatch for streamKey";
      CHECK(ddp.vertexCount == drawSpec.vertexCount) << context << ": vertex count mismatch for streamKey";
      return;
    }

    const vk::Buffer argsBuffer = m_backend.ddpDeviceArgsBuffer();
    CHECK(argsBuffer != vk::Buffer{}) << context << ": device args buffer missing";
    ddp.offset =
      m_backend.ddpAllocDeviceArgsSlot(indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand));
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
      CHECK(slice.buffer != vk::Buffer{} && slice.mapped != nullptr)
        << context << ": failed to allocate indexed args upload slice";
      std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
      m_backend.scheduleStaticCopyIndirect(argsBuffer, ddp.offset, slice);
    } else {
      struct Cmd
      {
        uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
      } cmdPayload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
      auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
      CHECK(slice.buffer != vk::Buffer{} && slice.mapped != nullptr)
        << context << ": failed to allocate args upload slice";
      std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
      m_backend.scheduleStaticCopyIndirect(argsBuffer, ddp.offset, slice);
    }
    ddp.prepared = true;
    ddp.indexed = indexed;
    ddp.vertexCount = drawSpec.vertexCount;
    ddp.indexCount = drawSpec.indexCount;
  };

  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    const bool indexed = (drawSpec.indexCount > 0);
    CHECK(payload.streamKey != 0) << "Ellipsoid DDP init: missing streamKey";
    DDPArgs& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    prepareDdpArgs(ddp, indexed, "Ellipsoid DDP init");
    recorder.recordGraphicsDraw(drawSpec);
  } else if (m_backend.ddpIndirectCountEnabled() &&
             shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const bool indexed = (drawSpec.indexCount > 0);
    CHECK(payload.streamKey != 0) << "Ellipsoid DDP peel: missing streamKey";
    DDPArgs& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    if (!ddp.prepared) {
      prepareDdpArgs(ddp, indexed, "Ellipsoid DDP peel");
      recorder.recordGraphicsDraw(drawSpec);
      return;
    }
    CHECK(ddp.indexed == indexed) << "Ellipsoid DDP peel: indexed mode mismatch for streamKey";
    CHECK(ddp.indexCount == drawSpec.indexCount) << "Ellipsoid DDP peel: index count mismatch";
    CHECK(ddp.vertexCount == drawSpec.vertexCount) << "Ellipsoid DDP peel: vertex count mismatch";
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(argsBuf != vk::Buffer{}) << "Ellipsoid DDP peel: device args buffer missing";
      CHECK(cntBuf != vk::Buffer{}) << "Ellipsoid DDP peel: indirect count buffer missing";
      if (indexed) {
        c.drawIndexedIndirectCount(argsBuf, ddp.offset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        c.drawIndirectCount(argsBuf, ddp.offset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanEllipsoidPipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                         const RenderBatch& batch,
                                                         const EllipsoidPayload& payload,
                                                         bool pickingPass)
{
  CHECK(batch.shaderHook.captured) << "Ellipsoid batch missing shader hook snapshot";
  (void)renderer;
  CHECK(payload.paramsCaptured) << "Ellipsoid payload missing params";

  // Frame transforms are shared per eye and updated once per submission in beginRender().
  m_dynFrameTransformsOffset = m_backend.frameTransformsOffset(batch.eye);

  ObjectTransformsUBOStd140 transforms{};
  transforms.pos_transform = payload.params.coordTransform;
  // Ellipsoid shaders do not consume the normal matrix; keep identity.
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  transforms.parameters = glm::vec4(payload.params.sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, transforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  material.material_specular = payload.params.materialSpecular;
  material.material_shininess = payload.params.materialShininess;
  material.alpha = pickingPass ? 1.0f : payload.params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  if (pickingPass) {
    material.material_specular = glm::vec4(0.0f);
    material.material_shininess = 0.0f;
  }

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Ellipsoid updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    const uint32_t submissionId = m_backend.activeSubmissionId();
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];
    UboCacheEntry* reusableEntry = nullptr;

    for (auto& cached : entries) {
      if (cached.pickingPass != pickingPass) {
        continue;
      }

      if (cached.params == payload.params && clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        cached.lastSubmissionId = submissionId;
        m_dynObjectTransformsOffset = cached.objectTransformsOffset;
        m_dynMaterialOffset = cached.materialOffset;
        return;
      }

      if (cached.lastSubmissionId != submissionId && reusableEntry == nullptr) {
        reusableEntry = &cached;
      }
    }

    if (reusableEntry != nullptr) {
      std::memcpy(
        m_backend.persistentUniformMappedAt(reusableEntry->objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
        &transforms,
        sizeof(transforms));
      std::memcpy(m_backend.persistentUniformMappedAt(reusableEntry->materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      reusableEntry->params = payload.params;
      reusableEntry->clipPlanes = batch.clipPlanes;
      reusableEntry->lastSubmissionId = submissionId;
      m_dynObjectTransformsOffset = reusableEntry->objectTransformsOffset;
      m_dynMaterialOffset = reusableEntry->materialOffset;
      return;
    }

    auto transformsSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
    std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
    m_dynObjectTransformsOffset = transformsSlice.offset;

    auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(materialSlice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = materialSlice.offset;

    UboCacheEntry entry{};
    entry.pickingPass = pickingPass;
    entry.params = payload.params;
    entry.clipPlanes = batch.clipPlanes;
    entry.objectTransformsOffset = transformsSlice.offset;
    entry.materialOffset = materialSlice.offset;
    entry.lastSubmissionId = submissionId;
    entries.push_back(std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto transformsSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
  std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
  m_dynObjectTransformsOffset = transformsSlice.offset;

  auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(materialSlice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = materialSlice.offset;
}

ZVulkanEllipsoidPipelineContext::PipelineInstance&
ZVulkanEllipsoidPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  PipelineInstance instance;

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> QString {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return QStringLiteral("dual_peeling_init_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return QStringLiteral("dual_peeling_peel_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return QStringLiteral("ppll_count_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return QStringLiteral("ppll_store_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return QStringLiteral("wavg_init_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return QStringLiteral("wblended_init_ellipsoid.frag.spv");
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return QStringLiteral("ellipsoid.frag.spv");
    }
  };

  instance.shader =
    std::make_unique<ZVulkanShader>(device,
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("ellipsoid.vert.spv")),
                                    ZVulkanShader::spirvResourcePath(selectFragmentShader(key.shaderHookType)),
                                    std::nullopt);

  const uint32_t useDynamic = key.dynamicMaterial ? 1u : 0u;
  std::array<vk::SpecializationMapEntry, 1> vertEntries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0, .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 1> vertData{useDynamic};
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eVertex,
    std::vector<vk::SpecializationMapEntry>(vertEntries.begin(), vertEntries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(vertData.data()),
                         reinterpret_cast<const uint8_t*>(vertData.data()) + sizeof(vertData)));

  const uint32_t useLinearFog = key.fogMode == FogMode::Linear ? 1u : 0u;
  const uint32_t useExpFog = key.fogMode == FogMode::Exponential ? 1u : 0u;
  const uint32_t useExp2Fog = key.fogMode == FogMode::ExponentialSquared ? 1u : 0u;

  std::array<vk::SpecializationMapEntry, 4> fragEntries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 20, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 21, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 22, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 4> fragData{useDynamic, useLinearFog, useExpFog, useExp2Fog};
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(fragEntries.begin(), fragEntries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(fragData.data()),
                         reinterpret_cast<const uint8_t*>(fragData.data()) + sizeof(fragData)));

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Ellipsoid pipeline missing bindless descriptor set layout";
  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Ellipsoid pipeline missing lighting descriptor set layout";
  const vk::DescriptorSetLayout transformsLayout = m_backend.transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Ellipsoid pipeline missing transforms descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "Ellipsoid pipeline missing OIT descriptor set layout";
  std::vector<vk::DescriptorSetLayout> layouts{bindlessLayout, lightingLayout, transformsLayout, oitLayout};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  // Dual-depth-peeling peel shader consumes bindless blender indices via push constants.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(DDPPeelPushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = VK_FALSE;

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
        attachment.blendEnable = VK_TRUE;
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
      instance.pipeline->setDepthTestEnable(key.depthFormat.has_value());
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      if (!formats.colorFormats.empty()) {
        std::vector<vk::PipelineColorBlendAttachmentState> attachments;
        attachments.reserve(formats.colorFormats.size());
        for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
          auto state = baseBlend;
          state.blendEnable = VK_TRUE;
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
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          // Match GL DDP peel: back-temp uses MAX blending (one fragment per pixel).
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else {
          state.blendEnable = VK_FALSE;
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

void ZVulkanEllipsoidPipelineContext::uploadGeometry(const EllipsoidPayload& payload)
{
  m_usedStaticVBThisFrame = false;
  m_vertexCount = payload.centers.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axis1.size() == m_vertexCount && payload.axis2.size() == m_vertexCount &&
        payload.axis3.size() == m_vertexCount && payload.centers.size() == m_vertexCount)
    << "Vulkan ellipsoid backend skipping batch: axis buffers are incomplete.";
  const size_t axisBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t centerBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t colorBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t flagsBytes = m_vertexCount * sizeof(float);
  const size_t specBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t idxBytes = m_indexCount * sizeof(uint32_t);
  const bool hasStreamKey = payload.streamKey != 0;
  const GeometryCacheKey geometryKey{payload.streamKey, payload.streamSegmentOrdinal};
  const AppearanceCacheKey appearanceKey{payload.streamKey,
                                         payload.streamSegmentOrdinal,
                                         payload.pickingPass,
                                         payload.useDynamicMaterial};
  const bool dynamicSpecular = !payload.pickingPass && payload.useDynamicMaterial;

  GeometryCacheEntry* boundGeometryStaticEntry = nullptr;
  AppearanceCacheEntry* boundAppearanceStaticEntry = nullptr;
  bool geometryBound = false;
  bool appearanceBound = false;

  auto bindGeometryStatic = [&](GeometryCacheEntry& entry) {
    m_axis1Buffer = entry.vbAxis1.buffer;
    m_axis2Buffer = entry.vbAxis2.buffer;
    m_axis3Buffer = entry.vbAxis3.buffer;
    m_centerBuffer = entry.vbCenter.buffer;
    m_flagsBuffer = entry.vbFlags.buffer;
    m_axis1Offset = entry.vbAxis1.offset;
    m_axis2Offset = entry.vbAxis2.offset;
    m_axis3Offset = entry.vbAxis3.offset;
    m_centerOffset = entry.vbCenter.offset;
    m_flagsOffset = entry.vbFlags.offset;
    if (m_indexCount > 0) {
      m_indexUploadBuffer = entry.ib.buffer;
      m_indexUploadOffset = entry.ib.offset;
    } else {
      m_indexUploadBuffer = vk::Buffer{};
      m_indexUploadOffset = 0;
    }
    boundGeometryStaticEntry = &entry;
    geometryBound = true;
  };
  auto bindAppearanceStatic = [&](AppearanceCacheEntry& entry) {
    m_colorBuffer = entry.vbColor.buffer;
    m_specularBuffer = entry.vbSpecular.buffer;
    m_colorOffset = entry.vbColor.offset;
    m_specularOffset = entry.vbSpecular.offset;
    boundAppearanceStaticEntry = &entry;
    appearanceBound = true;
  };

  if (hasStreamKey) {
    if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
      if (const PendingGeometryUploadBinding* pending = m_geometryStreamCache.findPendingUpload(geometryKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount && pending->indexCount == m_indexCount;
        const bool gensSame = pending->centersGen == payload.centersGen && pending->axesGen == payload.axesGen &&
                              pending->flagsGen == payload.flagsGen && pending->indexGen == payload.indexGen;
        const bool buffersOk = pending->axis1.buffer && pending->axis2.buffer && pending->axis3.buffer &&
                               pending->center.buffer && pending->flags.buffer &&
                               ((m_indexCount == 0) || pending->index.buffer);
        if (sizeSame && gensSame && buffersOk) {
          m_axis1Buffer = pending->axis1.buffer;
          m_axis2Buffer = pending->axis2.buffer;
          m_axis3Buffer = pending->axis3.buffer;
          m_centerBuffer = pending->center.buffer;
          m_flagsBuffer = pending->flags.buffer;
          m_axis1Offset = pending->axis1.offset;
          m_axis2Offset = pending->axis2.offset;
          m_axis3Offset = pending->axis3.offset;
          m_centerOffset = pending->center.offset;
          m_flagsOffset = pending->flags.offset;
          if (m_indexCount > 0) {
            m_indexUploadBuffer = pending->index.buffer;
            m_indexUploadOffset = pending->index.offset;
          } else {
            m_indexUploadBuffer = vk::Buffer{};
            m_indexUploadOffset = 0;
          }
          geometryBound = true;
        }
      }
    }
    if (!geometryBound && !m_geometryStreamCache.hasPendingCopy(geometryKey)) {
      if (GeometryCacheEntry* entry = m_geometryStreamCache.findEntry(geometryKey); entry != nullptr) {
        const bool sizeSame = (entry->vertexCount == m_vertexCount) && (entry->indexCount == m_indexCount);
        const bool gensSame = (entry->centersGen == payload.centersGen) && (entry->axesGen == payload.axesGen) &&
                              (entry->flagsGen == payload.flagsGen) && (entry->indexGen == payload.indexGen);
        if (entry->promoted && sizeSame && gensSame && entry->vbAxis1 && entry->vbAxis2 && entry->vbAxis3 &&
            entry->vbCenter && entry->vbFlags && (m_indexCount == 0 || entry->ib)) {
          bindGeometryStatic(*entry);
        }
      }
    }

    if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (const PendingAppearanceUploadBinding* pending = m_appearanceStreamCache.findPendingUpload(appearanceKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount;
        const bool colorSame = payload.pickingPass ? (pending->pickingColorsGen == payload.pickingColorsGen)
                                                   : (pending->colorsGen == payload.colorsGen);
        const bool specularSame = !dynamicSpecular || (pending->specularGen == payload.specularGen);
        const bool buffersOk = pending->color.buffer && pending->specular.buffer;
        if (sizeSame && colorSame && specularSame && buffersOk) {
          m_colorBuffer = pending->color.buffer;
          m_specularBuffer = pending->specular.buffer;
          m_colorOffset = pending->color.offset;
          m_specularOffset = pending->specular.offset;
          appearanceBound = true;
        }
      }
    }
    if (!appearanceBound && !m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (AppearanceCacheEntry* entry = m_appearanceStreamCache.findEntry(appearanceKey); entry != nullptr) {
        const bool sizeSame = entry->vertexCount == m_vertexCount;
        const bool colorSame = payload.pickingPass ? (entry->pickingColorsGen == payload.pickingColorsGen)
                                                   : (entry->colorsGen == payload.colorsGen);
        const bool specularSame = !dynamicSpecular || (entry->specularGen == payload.specularGen);
        if (entry->promoted && sizeSame && colorSame && specularSame && entry->vbColor && entry->vbSpecular) {
          bindAppearanceStatic(*entry);
        }
      }
    }
  }

  if (geometryBound && appearanceBound) {
    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbAxis1,
                                                  &boundGeometryStaticEntry->vbAxis2,
                                                  &boundGeometryStaticEntry->vbAxis3,
                                                  &boundGeometryStaticEntry->vbCenter,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
    return;
  }

  const bool needGeometryUpload = !geometryBound;
  const bool needAppearanceUpload = !appearanceBound;
  // A pass may reuse only geometry or only appearance statics while uploading the
  // other half. Protect the selected static slices before upload reservation or
  // promotion can invoke the global residency broker.
  if (boundGeometryStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                               {&boundGeometryStaticEntry->vbAxis1,
                                                &boundGeometryStaticEntry->vbAxis2,
                                                &boundGeometryStaticEntry->vbAxis3,
                                                &boundGeometryStaticEntry->vbCenter,
                                                &boundGeometryStaticEntry->vbFlags,
                                                &boundGeometryStaticEntry->ib});
    boundGeometryStaticEntry->usedStaticOnce = true;
    touchStaticStream(geometryKey.streamKey);
  }
  if (boundAppearanceStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(
      m_backend,
      {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
    boundAppearanceStaticEntry->usedStaticOnce = true;
    touchStaticStream(appearanceKey.streamKey);
  }
  m_backend.reserveUploadSlices({
    {needGeometryUpload ? axisBytes : 0u,    alignof(glm::vec4)},
    {needGeometryUpload ? axisBytes : 0u,    alignof(glm::vec4)},
    {needGeometryUpload ? axisBytes : 0u,    alignof(glm::vec4)},
    {needGeometryUpload ? centerBytes : 0u,  alignof(glm::vec4)},
    {needAppearanceUpload ? colorBytes : 0u, alignof(glm::vec4)},
    {needGeometryUpload ? flagsBytes : 0u,   alignof(float)    },
    {needAppearanceUpload ? specBytes : 0u,  alignof(glm::vec4)},
    {needGeometryUpload ? idxBytes : 0u,     alignof(uint32_t) }
  });

  Z3DRendererVulkanBackend::UploadSlice axis1Slice{};
  Z3DRendererVulkanBackend::UploadSlice axis2Slice{};
  Z3DRendererVulkanBackend::UploadSlice axis3Slice{};
  Z3DRendererVulkanBackend::UploadSlice centerSlice{};
  Z3DRendererVulkanBackend::UploadSlice colorSlice{};
  Z3DRendererVulkanBackend::UploadSlice flagsSlice{};
  Z3DRendererVulkanBackend::UploadSlice specSlice{};
  Z3DRendererVulkanBackend::UploadSlice indexSlice{};
  if (needGeometryUpload) {
    axis1Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
    axis2Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
    axis3Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
    centerSlice = m_backend.suballocateUpload(centerBytes, alignof(glm::vec4));
    flagsSlice = m_backend.suballocateUpload(flagsBytes, alignof(float));
  }
  if (needAppearanceUpload) {
    colorSlice = m_backend.suballocateUpload(colorBytes, alignof(glm::vec4));
    specSlice = m_backend.suballocateUpload(specBytes, alignof(glm::vec4));
  }
  if (needGeometryUpload && idxBytes > 0) {
    indexSlice = m_backend.suballocateUpload(idxBytes, alignof(uint32_t));
  }

  if ((needGeometryUpload &&
       (!axis1Slice.buffer || !axis1Slice.mapped || !axis2Slice.buffer || !axis2Slice.mapped || !axis3Slice.buffer ||
        !axis3Slice.mapped || !centerSlice.buffer || !centerSlice.mapped || !flagsSlice.buffer || !flagsSlice.mapped ||
        (idxBytes > 0 && (!indexSlice.buffer || !indexSlice.mapped)))) ||
      (needAppearanceUpload && (!colorSlice.buffer || !colorSlice.mapped || !specSlice.buffer || !specSlice.mapped))) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  if (payload.useDynamicMaterial) {
    CHECK(payload.specularAndShininess.size() >= m_vertexCount)
      << "Vulkan ellipsoid backend: dynamic material buffer is incomplete; missing values default to zero.";
  }

  if (needGeometryUpload) {
    std::memcpy(axis1Slice.mapped, payload.axis1.data(), axisBytes);
    std::memcpy(axis2Slice.mapped, payload.axis2.data(), axisBytes);
    std::memcpy(axis3Slice.mapped, payload.axis3.data(), axisBytes);
    std::memcpy(centerSlice.mapped, payload.centers.data(), centerBytes);
    auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      flagsOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    }
    if (idxBytes > 0) {
      auto* dst = static_cast<uint32_t*>(indexSlice.mapped);
      for (size_t i = 0; i < m_indexCount; ++i) {
        CHECK(payload.indices[i] >= payload.indexValueBias) << "Ellipsoid segment index rebasing underflow";
        dst[i] = payload.indices[i] - payload.indexValueBias;
      }
    }
    m_axis1Buffer = axis1Slice.buffer;
    m_axis2Buffer = axis2Slice.buffer;
    m_axis3Buffer = axis3Slice.buffer;
    m_centerBuffer = centerSlice.buffer;
    m_flagsBuffer = flagsSlice.buffer;
    m_axis1Offset = axis1Slice.offset;
    m_axis2Offset = axis2Slice.offset;
    m_axis3Offset = axis3Slice.offset;
    m_centerOffset = centerSlice.offset;
    m_flagsOffset = flagsSlice.offset;
    if (idxBytes > 0) {
      m_indexUploadBuffer = indexSlice.buffer;
      m_indexUploadOffset = indexSlice.offset;
    } else {
      m_indexUploadBuffer = vk::Buffer{};
      m_indexUploadOffset = 0;
    }
  }

  if (needAppearanceUpload) {
    auto* colorOut = static_cast<glm::vec4*>(colorSlice.mapped);
    if (payload.pickingPass) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        colorOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
      }
    } else {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        colorOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
      }
    }
    auto* specOut = static_cast<glm::vec4*>(specSlice.mapped);
    if (dynamicSpecular) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        specOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
      }
    } else {
      std::memset(specOut, 0, specBytes);
    }
    m_colorBuffer = colorSlice.buffer;
    m_specularBuffer = specSlice.buffer;
    m_colorOffset = colorSlice.offset;
    m_specularOffset = specSlice.offset;
  }

  if (hasStreamKey) {
    auto rememberPendingGeometryUpload = [&]() {
      if (!needGeometryUpload) {
        return;
      }
      PendingGeometryUploadBinding binding{};
      binding.axis1 = axis1Slice;
      binding.axis2 = axis2Slice;
      binding.axis3 = axis3Slice;
      binding.center = centerSlice;
      binding.flags = flagsSlice;
      binding.index = indexSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.indexCount = static_cast<uint32_t>(m_indexCount);
      binding.centersGen = payload.centersGen;
      binding.axesGen = payload.axesGen;
      binding.flagsGen = payload.flagsGen;
      binding.indexGen = payload.indexGen;
      m_geometryStreamCache.rememberPendingUploadBinding(geometryKey, binding);
    };
    auto rememberPendingAppearanceUpload = [&]() {
      if (!needAppearanceUpload) {
        return;
      }
      PendingAppearanceUploadBinding binding{};
      binding.color = colorSlice;
      binding.specular = specSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.colorsGen = payload.colorsGen;
      binding.pickingColorsGen = payload.pickingColorsGen;
      binding.specularGen = payload.specularGen;
      m_appearanceStreamCache.rememberPendingUploadBinding(appearanceKey, binding);
    };

    const int kPromotionThreshold = 2;
    auto promoteGeometryToStatics = [&](GeometryCacheEntry& entry) -> bool {
      if (!needGeometryUpload) {
        return false;
      }
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = idxBytes > 0 ? &indexSlice : nullptr;
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&entry.vbAxis1,  &axis1Slice,  axisBytes,   alignof(glm::vec4), false},
              {&entry.vbAxis2,  &axis2Slice,  axisBytes,   alignof(glm::vec4), false},
              {&entry.vbAxis3,  &axis3Slice,  axisBytes,   alignof(glm::vec4), false},
              {&entry.vbCenter, &centerSlice, centerBytes, alignof(glm::vec4), false},
              {&entry.vbFlags,  &flagsSlice,  flagsBytes,  alignof(float),     false},
              {&entry.ib,       idxSrc,       idxBytes,    alignof(uint32_t),  true }
      },
            &stagedBytes)) {
        return false;
      }
      m_backend.addEllipsoidBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK ellipsoid geometry promote: axis={}B center={}B flags={}B idx={}B",
                             axisBytes * 3,
                             centerBytes,
                             flagsBytes,
                             idxBytes);
      return true;
    };
    auto promoteAppearanceToStatics = [&](AppearanceCacheEntry& entry) -> bool {
      if (!needAppearanceUpload) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&entry.vbColor,    &colorSlice, colorBytes, alignof(glm::vec4), false},
              {&entry.vbSpecular, &specSlice,  specBytes,  alignof(glm::vec4), false},
      },
            &stagedBytes)) {
        return false;
      }
      m_backend.addEllipsoidBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK ellipsoid appearance promote: color={}B specular={}B", colorBytes, specBytes);
      return true;
    };

    {
      auto [entry, inserted] = m_geometryStreamCache.tryEmplaceEntry(geometryKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevIndexCount = entry.indexCount;
      const uint32_t prevCentersGen = entry.centersGen;
      const uint32_t prevAxesGen = entry.axesGen;
      const uint32_t prevFlagsGen = entry.flagsGen;
      const uint32_t prevIndexGen = entry.indexGen;
      const bool sizeSame = (prevVertexCount == m_vertexCount) && (prevIndexCount == m_indexCount);
      const bool gensSame = (prevCentersGen == payload.centersGen) && (prevAxesGen == payload.axesGen) &&
                            (prevFlagsGen == payload.flagsGen) && (prevIndexGen == payload.indexGen);
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (needGeometryUpload) {
        if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        } else if (inserted) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(
            m_backend,
            {&entry.vbAxis1, &entry.vbAxis2, &entry.vbAxis3, &entry.vbCenter, &entry.vbFlags, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool axesChanged = prevAxesGen != payload.axesGen;
          const bool centersChanged = prevCentersGen != payload.centersGen;
          const bool flagsChanged = prevFlagsGen != payload.flagsGen;
          const bool indexChanged = (prevIndexGen != payload.indexGen) && (idxBytes > 0);
          const bool anyChanged = axesChanged || centersChanged || flagsChanged || indexChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(
              m_backend,
              {&entry.vbAxis1, &entry.vbAxis2, &entry.vbAxis3, &entry.vbCenter, &entry.vbFlags, &entry.ib});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (axesChanged) {
              m_backend.scheduleStaticCopy(entry.vbAxis1.buffer, entry.vbAxis1.offset, axis1Slice, false);
              m_backend.scheduleStaticCopy(entry.vbAxis2.buffer, entry.vbAxis2.offset, axis2Slice, false);
              m_backend.scheduleStaticCopy(entry.vbAxis3.buffer, entry.vbAxis3.offset, axis3Slice, false);
              restagedBytes += axisBytes * 3;
            }
            if (centersChanged) {
              m_backend.scheduleStaticCopy(entry.vbCenter.buffer, entry.vbCenter.offset, centerSlice, false);
              restagedBytes += centerBytes;
            }
            if (flagsChanged) {
              m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
              restagedBytes += flagsBytes;
            }
            if (indexChanged) {
              m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, indexSlice, true);
              restagedBytes += idxBytes;
            }
            if (restagedBytes > 0) {
              m_backend.addEllipsoidBytesStaged(restagedBytes);
            }
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.axesGen = payload.axesGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
    }

    {
      auto [entry, inserted] = m_appearanceStreamCache.tryEmplaceEntry(appearanceKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevColorsGen = entry.colorsGen;
      const uint32_t prevPickingColorsGen = entry.pickingColorsGen;
      const uint32_t prevSpecularGen = entry.specularGen;
      const bool sizeSame = prevVertexCount == m_vertexCount;
      const bool colorSame =
        payload.pickingPass ? (prevPickingColorsGen == payload.pickingColorsGen) : (prevColorsGen == payload.colorsGen);
      const bool specularSame = !dynamicSpecular || (prevSpecularGen == payload.specularGen);
      entry.unchangedFrames = (!inserted && sizeSame && colorSame && specularSame) ? (entry.unchangedFrames + 1) : 0;

      if (needAppearanceUpload) {
        if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        } else if (inserted) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbColor, &entry.vbSpecular});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool colorChanged = payload.pickingPass ? (prevPickingColorsGen != payload.pickingColorsGen)
                                                        : (prevColorsGen != payload.colorsGen);
          const bool specularChanged = dynamicSpecular && (prevSpecularGen != payload.specularGen);
          const bool anyChanged = colorChanged || specularChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbColor, &entry.vbSpecular});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (colorChanged) {
              m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colorSlice, false);
              restagedBytes += colorBytes;
            }
            if (specularChanged) {
              m_backend.scheduleStaticCopy(entry.vbSpecular.buffer, entry.vbSpecular.offset, specSlice, false);
              restagedBytes += specBytes;
            }
            if (restagedBytes > 0) {
              m_backend.addEllipsoidBytesStaged(restagedBytes);
            }
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.colorsGen = payload.colorsGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.specularGen = payload.specularGen;
    }

    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbAxis1,
                                                  &boundGeometryStaticEntry->vbAxis2,
                                                  &boundGeometryStaticEntry->vbAxis3,
                                                  &boundGeometryStaticEntry->vbCenter,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanEllipsoidPipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 7> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis1
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis2
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis3
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // center
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // color
    vk::VertexInputBindingDescription{.binding = 5,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 6,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // specular
  };
  static std::array<vk::VertexInputAttributeDescription, 7> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 2,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 3,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 4,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 5, .binding = 5, .format = vk::Format::eR32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 6,
                                        .binding = 6,
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

} // namespace nim
