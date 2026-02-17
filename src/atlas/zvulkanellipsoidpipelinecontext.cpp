#include "zvulkanellipsoidpipelinecontext.h"

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
#include "zsysteminfo.h"
#include "zlog.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

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

} // namespace

ZVulkanEllipsoidPipelineContext::ZVulkanEllipsoidPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
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
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
  m_staticCopyPendingKeys.clear();
}

void ZVulkanEllipsoidPipelineContext::evictStream(uint64_t streamKey)
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
    m_backend.releaseStaticSlice(entry.vbAxis1);
    m_backend.releaseStaticSlice(entry.vbAxis2);
    m_backend.releaseStaticSlice(entry.vbAxis3);
    m_backend.releaseStaticSlice(entry.vbCenter);
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.vbSpecular);
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
  if (FLAGS_atlas_vk_cache_draw_secondaries &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();
    SecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
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
      CHECK(m_ddpArgsPrepared) << "Ellipsoid DDP peel: args not prepared in init";
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
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];

    for (auto& cached : entries) {
      if (cached.pickingPass != pickingPass) {
        continue;
      }

      m_dynObjectTransformsOffset = cached.objectTransformsOffset;
      m_dynMaterialOffset = cached.materialOffset;

      if (cached.params == payload.params && clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        return;
      }

      std::memcpy(m_backend.persistentUniformMappedAt(cached.objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
                  &transforms,
                  sizeof(transforms));
      std::memcpy(m_backend.persistentUniformMappedAt(cached.materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      cached.params = payload.params;
      cached.clipPlanes = batch.clipPlanes;
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
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> std::string {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return "dual_peeling_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return "ppll_count_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return "ppll_store_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "ellipsoid.frag.spv";
    }
  };

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "ellipsoid.vert.spv",
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
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
  m_vertexCount = payload.centers.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axis1.size() == m_vertexCount && payload.axis2.size() == m_vertexCount &&
        payload.axis3.size() == m_vertexCount && payload.centers.size() == m_vertexCount)
    << "Vulkan ellipsoid backend skipping batch: axis buffers are incomplete.";

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    CacheKey key{payload.streamKey, payload.pickingPass, payload.useDynamicMaterial};
    if (!m_staticCopyPendingKeys.contains(key)) {
      auto it = m_staticCache.find(key);
      if (it != m_staticCache.end()) {
        CacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
        const bool shapeSame = (entry.centersGen == payload.centersGen) && (entry.axesGen == payload.axesGen) &&
                               (entry.flagsGen == payload.flagsGen) && (entry.indexGen == payload.indexGen);
        const bool colorsSame = payload.pickingPass ? (entry.pickingColorsGen == payload.pickingColorsGen)
                                                    : (entry.colorsGen == payload.colorsGen);
        const bool specularSame =
          (payload.pickingPass || !payload.useDynamicMaterial) ? true : (entry.specularGen == payload.specularGen);
        if (entry.promoted && sizeSame && shapeSame && colorsSame && specularSame && entry.vbAxis1 && entry.vbAxis2 &&
            entry.vbAxis3 && entry.vbCenter && entry.vbColor && entry.vbFlags && entry.vbSpecular &&
            (m_indexCount == 0 || entry.ib)) {
          m_axis1Buffer = entry.vbAxis1.buffer;
          m_axis2Buffer = entry.vbAxis2.buffer;
          m_axis3Buffer = entry.vbAxis3.buffer;
          m_centerBuffer = entry.vbCenter.buffer;
          m_colorBuffer = entry.vbColor.buffer;
          m_flagsBuffer = entry.vbFlags.buffer;
          m_specularBuffer = entry.vbSpecular.buffer;
          m_axis1Offset = entry.vbAxis1.offset;
          m_axis2Offset = entry.vbAxis2.offset;
          m_axis3Offset = entry.vbAxis3.offset;
          m_centerOffset = entry.vbCenter.offset;
          m_colorOffset = entry.vbColor.offset;
          m_flagsOffset = entry.vbFlags.offset;
          m_specularOffset = entry.vbSpecular.offset;
          if (m_indexCount > 0) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                     {&entry.vbAxis1,
                                                      &entry.vbAxis2,
                                                      &entry.vbAxis3,
                                                      &entry.vbCenter,
                                                      &entry.vbColor,
                                                      &entry.vbFlags,
                                                      &entry.vbSpecular,
                                                      &entry.ib});
          m_usedStaticVBThisFrame = true;
          entry.usedStaticOnce = true;
          return;
        }
      }
    }
  }

  // Allocate SoA slices
  const size_t axisBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t centerBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t colorBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t flagsBytes = m_vertexCount * sizeof(float);
  const size_t specBytes = m_vertexCount * sizeof(glm::vec4);
  m_backend.reserveUploadSlices({
    {axisBytes,                       alignof(glm::vec4)},
    {axisBytes,                       alignof(glm::vec4)},
    {axisBytes,                       alignof(glm::vec4)},
    {centerBytes,                     alignof(glm::vec4)},
    {colorBytes,                      alignof(glm::vec4)},
    {flagsBytes,                      alignof(float)    },
    {specBytes,                       alignof(glm::vec4)},
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });

  auto axis1Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto axis2Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto axis3Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto centerSlice = m_backend.suballocateUpload(centerBytes, alignof(glm::vec4));
  auto colorSlice = m_backend.suballocateUpload(colorBytes, alignof(glm::vec4));
  auto flagsSlice = m_backend.suballocateUpload(flagsBytes, alignof(float));
  auto specSlice = m_backend.suballocateUpload(specBytes, alignof(glm::vec4));

  if (!axis1Slice.buffer || !axis1Slice.mapped || !axis2Slice.buffer || !axis2Slice.mapped || !axis3Slice.buffer ||
      !axis3Slice.mapped || !centerSlice.buffer || !centerSlice.mapped || !colorSlice.buffer || !colorSlice.mapped ||
      !flagsSlice.buffer || !flagsSlice.mapped || !specSlice.buffer || !specSlice.mapped) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  if (payload.useDynamicMaterial) {
    CHECK(payload.specularAndShininess.size() >= m_vertexCount)
      << "Vulkan ellipsoid backend: dynamic material buffer is incomplete; missing values default to zero.";
  }
  auto* axis1Out = static_cast<glm::vec4*>(axis1Slice.mapped);
  auto* axis2Out = static_cast<glm::vec4*>(axis2Slice.mapped);
  auto* axis3Out = static_cast<glm::vec4*>(axis3Slice.mapped);
  auto* centerOut = static_cast<glm::vec4*>(centerSlice.mapped);
  auto* colorOut = static_cast<glm::vec4*>(colorSlice.mapped);
  auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
  auto* specOut = static_cast<glm::vec4*>(specSlice.mapped);

  const bool pickingPass = payload.pickingPass;

  std::memcpy(axis1Out, payload.axis1.data(), axisBytes);
  std::memcpy(axis2Out, payload.axis2.data(), axisBytes);
  std::memcpy(axis3Out, payload.axis3.data(), axisBytes);
  std::memcpy(centerOut, payload.centers.data(), centerBytes);
  if (pickingPass) {
    // Use picking colors when available; else default to zero
    for (size_t i = 0; i < m_vertexCount; ++i) {
      colorOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
    }
  } else {
    // Default to black if missing
    for (size_t i = 0; i < m_vertexCount; ++i) {
      colorOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
  }
  for (size_t i = 0; i < m_vertexCount; ++i) {
    flagsOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
  }
  if (!pickingPass && payload.useDynamicMaterial) {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      specOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
    }
  } else {
    std::memset(specOut, 0, specBytes);
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
    m_indexUploadBuffer = VK_NULL_HANDLE;
    m_indexUploadOffset = 0;
  }
  // Save SoA buffer + offsets
  m_axis1Buffer = axis1Slice.buffer;
  m_axis2Buffer = axis2Slice.buffer;
  m_axis3Buffer = axis3Slice.buffer;
  m_centerBuffer = centerSlice.buffer;
  m_colorBuffer = colorSlice.buffer;
  m_flagsBuffer = flagsSlice.buffer;
  m_specularBuffer = specSlice.buffer;
  m_axis1Offset = axis1Slice.offset;
  m_axis2Offset = axis2Slice.offset;
  m_axis3Offset = axis3Slice.offset;
  m_centerOffset = centerSlice.offset;
  m_colorOffset = colorSlice.offset;
  m_flagsOffset = flagsSlice.offset;
  m_specularOffset = specSlice.offset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Ellipsoid payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass, payload.useDynamicMaterial};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    const auto promoteToStatics = [&](CacheEntry& dstEntry) -> bool {
      const size_t idxBytes = m_indexCount > 0 ? m_indexCount * sizeof(uint32_t) : 0u;
      Z3DRendererVulkanBackend::UploadSlice idxUpload{};
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = nullptr;
      if (idxBytes > 0) {
        if (!m_indexUploadBuffer) {
          return false;
        }
        idxUpload = Z3DRendererVulkanBackend::UploadSlice{m_indexUploadBuffer, m_indexUploadOffset, nullptr, idxBytes};
        idxSrc = &idxUpload;
      }

      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&dstEntry.vbAxis1,    &axis1Slice,  axisBytes,   alignof(glm::vec4), false},
              {&dstEntry.vbAxis2,    &axis2Slice,  axisBytes,   alignof(glm::vec4), false},
              {&dstEntry.vbAxis3,    &axis3Slice,  axisBytes,   alignof(glm::vec4), false},
              {&dstEntry.vbCenter,   &centerSlice, centerBytes, alignof(glm::vec4), false},
              {&dstEntry.vbColor,    &colorSlice,  colorBytes,  alignof(glm::vec4), false},
              {&dstEntry.vbFlags,    &flagsSlice,  flagsBytes,  alignof(float),     false},
              {&dstEntry.vbSpecular, &specSlice,   specBytes,   alignof(glm::vec4), false},
              {&dstEntry.ib,         idxSrc,       idxBytes,    alignof(uint32_t),  true }
      })) {
        return false;
      }

      VLOG(1) << fmt::format(
        "VK ellipsoid promote: axis1={}B axis2={}B axis3={}B center={}B color={}B flags={}B spec={}B idx={}B",
        axisBytes,
        axisBytes,
        axisBytes,
        centerBytes,
        colorBytes,
        flagsBytes,
        specBytes,
        idxBytes);
      return true;
    };
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.axesGen = payload.axesGen;
      entry.colorsGen = payload.colorsGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.specularGen = payload.specularGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      auto [inserted, _] = m_staticCache.emplace(key, entry);
      CacheEntry& insertedEntry = inserted->second;

      // UX: stage into device-local statics on first sight so steady-state
      // frames can bind fast device-local buffers starting on the next frame.
      if (promoteToStatics(insertedEntry)) {
        insertedEntry.promoted = true;
        insertedEntry.usedStaticOnce = false;
        // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        m_staticCopyPendingKeys.insert(key);
        return;
      }
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool dataSame = entry.centersGen == payload.centersGen && entry.axesGen == payload.axesGen &&
                            entry.flagsGen == payload.flagsGen && entry.indexGen == payload.indexGen &&
                            entry.specularGen == payload.specularGen;
      if (sizeSame && dataSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_staticCopyPendingKeys.contains(key)) {
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.axesGen = payload.axesGen;
        entry.colorsGen = payload.colorsGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        entry.specularGen = payload.specularGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;
        return;
      }

      if (entry.promoted && !sizeSame) {
        vulkan::releaseStaticSlices(m_backend,
                                    {&entry.vbAxis1,
                                     &entry.vbAxis2,
                                     &entry.vbAxis3,
                                     &entry.vbCenter,
                                     &entry.vbColor,
                                     &entry.vbFlags,
                                     &entry.vbSpecular,
                                     &entry.ib});
        entry.promoted = false;
        entry.usedStaticOnce = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.axesGen = payload.axesGen;
        entry.colorsGen = payload.colorsGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        entry.specularGen = payload.specularGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;

        // Recreate static slices immediately so the next steady frame can bind
        // device-local buffers without a multi-frame warmup.
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_staticCopyPendingKeys.insert(key);
          return;
        }
        return;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = (entry.axesGen != payload.axesGen) || (entry.centersGen != payload.centersGen) ||
                          (entry.flagsGen != payload.flagsGen) || (entry.indexGen != payload.indexGen);
        if (payload.pickingPass) {
          anyChanged = anyChanged || (entry.pickingColorsGen != payload.pickingColorsGen);
        } else {
          anyChanged =
            anyChanged || (entry.colorsGen != payload.colorsGen) || (entry.specularGen != payload.specularGen);
        }

        // If this stream is still changing before we've ever bound the static
        // buffers, drop the statics and fall back to upload-only mode to avoid
        // paying upload->device copies every frame without a benefit.
        if (anyChanged && !entry.usedStaticOnce) {
          vulkan::releaseStaticSlices(m_backend,
                                      {&entry.vbAxis1,
                                       &entry.vbAxis2,
                                       &entry.vbAxis3,
                                       &entry.vbCenter,
                                       &entry.vbColor,
                                       &entry.vbFlags,
                                       &entry.vbSpecular,
                                       &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
          entry.indexCount = static_cast<uint32_t>(m_indexCount);
          entry.centersGen = payload.centersGen;
          entry.axesGen = payload.axesGen;
          entry.colorsGen = payload.colorsGen;
          entry.pickingColorsGen = payload.pickingColorsGen;
          entry.specularGen = payload.specularGen;
          entry.flagsGen = payload.flagsGen;
          entry.indexGen = payload.indexGen;
          return;
        }

        if (entry.axesGen != payload.axesGen) {
          m_backend.scheduleStaticCopy(entry.vbAxis1.buffer, entry.vbAxis1.offset, axis1Slice, false);
          m_backend.scheduleStaticCopy(entry.vbAxis2.buffer, entry.vbAxis2.offset, axis2Slice, false);
          m_backend.scheduleStaticCopy(entry.vbAxis3.buffer, entry.vbAxis3.offset, axis3Slice, false);
        }
        if (entry.centersGen != payload.centersGen) {
          m_backend.scheduleStaticCopy(entry.vbCenter.buffer, entry.vbCenter.offset, centerSlice, false);
        }
        if (payload.pickingPass) {
          if (entry.pickingColorsGen != payload.pickingColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colorSlice, false);
          }
        } else {
          if (entry.colorsGen != payload.colorsGen) {
            m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colorSlice, false);
          }
          if (entry.specularGen != payload.specularGen) {
            m_backend.scheduleStaticCopy(entry.vbSpecular.buffer, entry.vbSpecular.offset, specSlice, false);
          }
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
        }
        if (entry.indexGen != payload.indexGen && m_indexUploadBuffer && m_indexCount > 0) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                        m_indexUploadOffset,
                                                        nullptr,
                                                        m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, iUpload, true);
        }

        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.centersGen = payload.centersGen;
        entry.axesGen = payload.axesGen;
        entry.colorsGen = payload.colorsGen;
        entry.pickingColorsGen = payload.pickingColorsGen;
        entry.specularGen = payload.specularGen;
        entry.flagsGen = payload.flagsGen;
        entry.indexGen = payload.indexGen;

        if (anyChanged) {
          m_staticCopyPendingKeys.insert(key);
          return; // use upload slices for this frame
        }

        // Bind static
        m_axis1Buffer = entry.vbAxis1.buffer;
        m_axis2Buffer = entry.vbAxis2.buffer;
        m_axis3Buffer = entry.vbAxis3.buffer;
        m_centerBuffer = entry.vbCenter.buffer;
        m_colorBuffer = entry.vbColor.buffer;
        m_flagsBuffer = entry.vbFlags.buffer;
        m_specularBuffer = entry.vbSpecular.buffer;
        m_axis1Offset = entry.vbAxis1.offset;
        m_axis2Offset = entry.vbAxis2.offset;
        m_axis3Offset = entry.vbAxis3.offset;
        m_centerOffset = entry.vbCenter.offset;
        m_colorOffset = entry.vbColor.offset;
        m_flagsOffset = entry.vbFlags.offset;
        m_specularOffset = entry.vbSpecular.offset;
        if (entry.indexCount > 0 && entry.ib) {
          m_indexUploadBuffer = entry.ib.buffer;
          m_indexUploadOffset = entry.ib.offset;
        }
        vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                   {&entry.vbAxis1,
                                                    &entry.vbAxis2,
                                                    &entry.vbAxis3,
                                                    &entry.vbCenter,
                                                    &entry.vbColor,
                                                    &entry.vbFlags,
                                                    &entry.vbSpecular,
                                                    &entry.ib});
        entry.usedStaticOnce = true;
        return;
      }

      // Not promoted: keep observed state up-to-date so unchangedFrames can
      // reach the promotion threshold after a data change.
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.axesGen = payload.axesGen;
      entry.colorsGen = payload.colorsGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.specularGen = payload.specularGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_staticCopyPendingKeys.insert(key);
        }
      }
    }
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
