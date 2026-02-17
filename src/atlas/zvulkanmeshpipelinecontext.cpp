#include "zvulkanmeshpipelinecontext.h"

#include "z3drenderervulkanbackend.h"
#include "z3drendererbase.h"
#include "z3drendererstates.h"
#include "z3drendercommands.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zmesh.h"
#include "z3dprimitiverenderer.h"
#include "z3dmeshrenderer.h"
#include "zlog.h"
#include "zexception.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>

#include <folly/coro/Task.h>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

namespace nim {
namespace {

struct MeshVertex
{
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec4 color{1.0f};
  float tex1d = 0.0f;
  glm::vec2 tex2d{0.0f};
  glm::vec3 tex3d{0.0f};
};

vk::PipelineVertexInputStateCreateInfo makeSoAMeshVertexInput(MeshPayload::ColorSource /*colorSource*/)
{
  // Always declare bindings/attributes for all shader-declared locations.
  // mesh.vert (Vulkan) declares locations 0..5 unconditionally; specialization
  // constants gate usage but validation requires attribute descriptions to
  // exist for every declared location when vertex input is not dynamic.
  static std::array<vk::VertexInputBindingDescription, 6> bindings{};
  // binding 0: position (vec3)
  bindings[0] = vk::VertexInputBindingDescription{.binding = 0,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex};
  // binding 1: normal (vec3)
  bindings[1] = vk::VertexInputBindingDescription{.binding = 1,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex};
  // binding 2: color (vec4)
  bindings[2] = vk::VertexInputBindingDescription{.binding = 2,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                                  .inputRate = vk::VertexInputRate::eVertex};
  // binding 3: 1D texcoord (float)
  bindings[3] = vk::VertexInputBindingDescription{.binding = 3,
                                                  .stride = static_cast<uint32_t>(sizeof(float)),
                                                  .inputRate = vk::VertexInputRate::eVertex};
  // binding 4: 2D texcoord (vec2)
  bindings[4] = vk::VertexInputBindingDescription{.binding = 4,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec2)),
                                                  .inputRate = vk::VertexInputRate::eVertex};
  // binding 5: 3D texcoord (vec3)
  bindings[5] = vk::VertexInputBindingDescription{.binding = 5,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex};

  static std::array<vk::VertexInputAttributeDescription, 6> attrs{};
  attrs[0] = vk::VertexInputAttributeDescription{.location = 0,
                                                 .binding = 0,
                                                 .format = vk::Format::eR32G32B32Sfloat,
                                                 .offset = 0};
  attrs[1] = vk::VertexInputAttributeDescription{.location = 1,
                                                 .binding = 1,
                                                 .format = vk::Format::eR32G32B32Sfloat,
                                                 .offset = 0};
  attrs[2] = vk::VertexInputAttributeDescription{.location = 2,
                                                 .binding = 2,
                                                 .format = vk::Format::eR32G32B32A32Sfloat,
                                                 .offset = 0};
  // location 3: 1D texcoord from binding 3
  attrs[3] =
    vk::VertexInputAttributeDescription{.location = 3, .binding = 3, .format = vk::Format::eR32Sfloat, .offset = 0};
  // location 4: 2D texcoord from binding 4
  attrs[4] =
    vk::VertexInputAttributeDescription{.location = 4, .binding = 4, .format = vk::Format::eR32G32Sfloat, .offset = 0};
  // location 5: 3D texcoord from binding 5
  attrs[5] = vk::VertexInputAttributeDescription{.location = 5,
                                                 .binding = 5,
                                                 .format = vk::Format::eR32G32B32Sfloat,
                                                 .offset = 0};

  constexpr uint32_t bindingCount = 6;
  constexpr uint32_t attrCount = 6;

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = bindingCount;
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = attrCount;
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PipelineVertexInputStateCreateInfo makeSoAMeshDepthVertexInput()
{
  // Depth-only mesh pass (DDP init):
  // - mesh_depth.vert only consumes attr_vertex at location 0.
  // - Keep the vertex input state minimal to avoid Vulkan validation warnings
  //   about unused attributes and to reduce vertex fetch work.
  static std::array<vk::VertexInputBindingDescription, 1> bindings{};
  bindings[0] = vk::VertexInputBindingDescription{.binding = 0,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex};

  static std::array<vk::VertexInputAttributeDescription, 1> attrs{};
  attrs[0] = vk::VertexInputAttributeDescription{.location = 0,
                                                 .binding = 0,
                                                 .format = vk::Format::eR32G32B32Sfloat,
                                                 .offset = 0};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PrimitiveTopology toVkTopology(ZMesh::Type type)
{
  switch (type) {
    case ZMesh::Type::TRIANGLES:
      return vk::PrimitiveTopology::eTriangleList;
    case ZMesh::Type::TRIANGLE_STRIP:
      return vk::PrimitiveTopology::eTriangleStrip;
    case ZMesh::Type::TRIANGLE_FAN:
      return vk::PrimitiveTopology::eTriangleFan;
    default:
      return vk::PrimitiveTopology::eTriangleList;
  }
}

bool validateTexturePrerequisites(const MeshPayload& payload, const ZMesh& mesh)
{
  const size_t vertexCount = mesh.numVertices();
  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      return vertexCount > 0 && mesh.num1DTextureCoordinates() >= vertexCount;
    case MeshPayload::ColorSource::Mesh2DTexture:
      return vertexCount > 0 && mesh.num2DTextureCoordinates() >= vertexCount;
    case MeshPayload::ColorSource::Mesh3DTexture:
      return vertexCount > 0 && mesh.num3DTextureCoordinates() >= vertexCount;
    default:
      return true;
  }
}
const glm::vec4 kFallbackMeshColor{0.0f, 0.0f, 0.0f, 1.0f};

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

ZVulkanMeshPipelineContext::ZVulkanMeshPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanMeshPipelineContext::~ZVulkanMeshPipelineContext() = default;

void ZVulkanMeshPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_draws.clear();
  m_posBuffer = nullptr;
  m_normBuffer = nullptr;
  m_colorBuffer = nullptr;
  m_texBuffer = nullptr;
  m_posOffset = m_normOffset = m_colorOffset = m_texOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  m_texBinding = TexBinding::None;
  m_ddpArgsOffsets.clear();
  m_ddpArgsPrepared.clear();
  m_staticCopyPendingKeys.clear();
}

void ZVulkanMeshPipelineContext::evictStream(uint64_t streamKey)
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
    m_backend.releaseStaticSlice(entry.vbPos);
    m_backend.releaseStaticSlice(entry.vbNorm);
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.vbTex);
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

void ZVulkanMeshPipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK mesh retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK mesh retained UBO lifetime");
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(
      keepAlive,
      [fence, keep = sp]() mutable -> folly::coro::Task<void> {
        co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
        co_return;
      }(),
      "VK mesh retained UBO lifetime");
  }
  m_retainedUbos.clear();
}

void ZVulkanMeshPipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const MeshPayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Ensure previously used UBOs survive until this submission completes.
  flushRetainedUbos();
  if (payload.meshes.empty()) {
    return;
  }

  if (payload.pickingPass) {
    if (payload.meshPickingColors.empty() || payload.meshPickingColors.size() != payload.meshes.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_draws.empty() || m_vertexCount == 0) {
    return;
  }

  CHECK(batch.shaderHook.captured) << "Mesh batch missing shader hook snapshot";
  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = batch.shaderHook.type;

  // Match OpenGL: lighting can be disabled per-renderer even when the scene has lights.
  m_dynLightingOffset = (payload.pickingPass || !payload.wantsLighting) ? m_backend.framePickingLightingOffset()
                                                                        : m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload);
  // No OIT UBO; set 3 carries only the DDP flag.
  // Descriptor sets are primed by the backend in beginRender(); avoid record-time rewrites.
  const vk::DescriptorSet dsLighting = m_backend.sharedLightingDescriptorSet();
  const bool usePersistentTransforms = (payload.streamKey != 0);
  const vk::DescriptorSet dsTransforms = usePersistentTransforms ? m_backend.sharedTransformsDescriptorSetPersistent()
                                                                 : m_backend.sharedTransformsDescriptorSetUniform();
  CHECK(dsLighting && dsTransforms) << "Mesh pipeline shared descriptor sets missing (lighting/transforms)";

  // Bindless indices for this batch (constant across all draws in the payload).
  MeshBindlessPushConstants bindlessPC{};
  if (!pickingPass && payload.textureHandle.valid() && payload.textureHandle.backend == RenderBackend::Vulkan) {
    auto& sampledTexture =
      vulkan::textureFromHandle(payload.textureHandle, m_backend.device(), "mesh payload sampled texture");
    const uint32_t texIdx = m_backend.bindlessLookupSampledImageAutoOrCrash(sampledTexture, "mesh payload texture");
    switch (payload.colorSource) {
      case MeshPayload::ColorSource::Mesh1DTexture:
        bindlessPC.tex1D = texIdx;
        break;
      case MeshPayload::ColorSource::Mesh2DTexture:
        bindlessPC.tex2D = texIdx;
        break;
      case MeshPayload::ColorSource::Mesh3DTexture:
        bindlessPC.tex3D = texIdx;
        break;
      default:
        break;
    }
  }

  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const auto& hookPara = batch.shaderHook.para;
    CHECK(hookPara.dualDepthPeelingDepthBlenderHandle.valid()) << "Mesh DDP peel requires a valid depth blender handle";
    CHECK(hookPara.dualDepthPeelingFrontBlenderHandle.valid()) << "Mesh DDP peel requires a valid front blender handle";

    auto& depthTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "mesh dual-depth-peeling depth blender");
    auto& frontTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "mesh dual-depth-peeling front blender");
    bindlessPC.ddpDepthBlender =
      m_backend.bindlessLookupSampledImageAutoOrCrash(depthTexture, "mesh ddp depth blender");
    bindlessPC.ddpFrontBlender =
      m_backend.bindlessLookupSampledImageAutoOrCrash(frontTexture, "mesh ddp front blender");
  }

  // Bind only the bindless sampled-images set up-front. Lighting/transforms use
  // dynamic UBOs and are re-bound per draw with the correct dynamic offsets.
  const std::array<vk::DescriptorSet, 1> baseDescriptorSets{m_backend.bindlessSampledImageDescriptorSet()};
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> baseExtraBinds{};
  // Mesh pipelines always include set 3 (OIT params) in the layout so the same
  // pipeline layout can be used across OIT and non-OIT passes.
  ZVulkanDescriptorBindInfo oitBind{};
  oitBind.firstSet = vkbind::kSetOIT;
  oitDescriptorSets[0] = m_backend.sharedOITDescriptorSet();
  oitBind.sets = oitDescriptorSets;
  baseExtraBinds[0] = oitBind;
  const uint32_t baseExtraBindCount = 1;
  const uint32_t expectedSetCount = std::max<uint32_t>(1u, vkbind::kSetOIT + 1u);

  // Group draws by pipeline instance and prepare a common vertex-binding helper
  const bool drawSurface = payload.wireframeMode != MeshPayload::WireframeMode::OnlyWireframe;
  const bool drawWireframe = payload.wireframeMode != MeshPayload::WireframeMode::NoWireframe;
  const FogMode fogMode = renderer.sceneState().fog.mode;
  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  m_backend.validateFormatsOrCrash(formats, "mesh");

  struct DrawCallInfo
  {
    const MeshDraw* draw;
    bool wireframe;
    glm::vec4 wireColor;
  };

  // Mesh payloads often contain thousands of meshes, but the pipeline key
  // varies only by mesh topology (triangles/strip/fan) and wireframe flag.
  // Avoid per-draw PipelineKey construction (which includes std::vector copies
  // for attachment formats) by grouping into a fixed small set of variants.
  auto meshTypeSlot = [](ZMesh::Type type) -> size_t {
    switch (type) {
      case ZMesh::Type::TRIANGLES:
        return 0u;
      case ZMesh::Type::TRIANGLE_STRIP:
        return 1u;
      case ZMesh::Type::TRIANGLE_FAN:
        return 2u;
      default:
        return 0u;
    }
  };
  constexpr std::array<ZMesh::Type, 3> kMeshTypes{ZMesh::Type::TRIANGLES,
                                                  ZMesh::Type::TRIANGLE_STRIP,
                                                  ZMesh::Type::TRIANGLE_FAN};
  constexpr size_t kMeshTypeSlots = kMeshTypes.size();
  constexpr size_t kWireVariants = 2;
  constexpr size_t kVariantCount = kMeshTypeSlots * kWireVariants;

  auto variantIndex = [&](ZMesh::Type type, bool wireframe) -> size_t {
    const size_t typeIndex = meshTypeSlot(type);
    const size_t wireIndex = wireframe ? 1u : 0u;
    return typeIndex * kWireVariants + wireIndex;
  };

  std::array<std::vector<DrawCallInfo>, kVariantCount> groupedDraws;

  // DDP indirect-count: prepare a stable per-mesh args table during the init pass
  // so peel passes can reference offsets by payload mesh index (order-independent).
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    const size_t meshCount = payload.meshes.size();
    m_ddpArgsOffsets.assign(meshCount, vk::DeviceSize(0));
    m_ddpArgsPrepared.assign(meshCount, 0u);
  }

  for (const auto& draw : m_draws) {
    if (!draw.mesh) {
      continue;
    }
    if (drawSurface) {
      groupedDraws[variantIndex(draw.mesh->type(), false)].push_back(DrawCallInfo{&draw, false, glm::vec4(0.0f)});
    }
    if (drawWireframe) {
      const glm::vec4 wireColor = pickingPass ? draw.fallbackColor : payload.wireframeColor;
      groupedDraws[variantIndex(draw.mesh->type(), true)].push_back(DrawCallInfo{&draw, true, wireColor});
    }
  }

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Mesh record called without an active Vulkan frame-slot key";
  const vk::Buffer dummyVertexBuffer = m_backend.dummyVertexBuffer();
  CHECK(static_cast<VkBuffer>(dummyVertexBuffer) != VK_NULL_HANDLE) << "Mesh pipeline missing dummy vertex buffer";

  auto bindCommonBuffers = [&](vk::raii::CommandBuffer& cb) {
    std::array<vk::Buffer, 3> baseBufs{m_posBuffer, m_normBuffer, m_colorBuffer};
    std::array<vk::DeviceSize, 3> baseOffs{m_posOffset, m_normOffset, m_colorOffset};
    cb.bindVertexBuffers(0, baseBufs, baseOffs);

    std::array<vk::Buffer, 3> texBufs{dummyVertexBuffer, dummyVertexBuffer, dummyVertexBuffer};
    std::array<vk::DeviceSize, 3> texOffs{vk::DeviceSize(0), vk::DeviceSize(0), vk::DeviceSize(0)};
    if (m_texBinding != TexBinding::None && m_texBuffer) {
      uint32_t texBindingIndex = 3;
      if (!m_backend.device().supportsVertexInputDynamicState()) {
        switch (m_texBinding) {
          case TexBinding::Tex1D:
            texBindingIndex = 3;
            break;
          case TexBinding::Tex2D:
            texBindingIndex = 4;
            break;
          case TexBinding::Tex3D:
            texBindingIndex = 5;
            break;
          default:
            break;
        }
      }
      CHECK(texBindingIndex >= 3u && texBindingIndex <= 5u) << "Mesh tex binding index out of range";
      texBufs[texBindingIndex - 3u] = m_texBuffer;
      texOffs[texBindingIndex - 3u] = m_texOffset;
    }
    cb.bindVertexBuffers(3, texBufs, texOffs);

    if (m_indexCount > 0 && m_indexUploadBuffer) {
      cb.bindIndexBuffer(m_indexUploadBuffer, m_indexUploadOffset, vk::IndexType::eUint32);
    }
  };

  // Precompute stable vertex/index binding state for secondary cache signatures.
  std::array<vk::Buffer, 6> signatureVertexBuffers{m_posBuffer,
                                                   m_normBuffer,
                                                   m_colorBuffer,
                                                   dummyVertexBuffer,
                                                   dummyVertexBuffer,
                                                   dummyVertexBuffer};
  std::array<vk::DeviceSize, 6> signatureVertexOffsets{
    m_posOffset,
    m_normOffset,
    m_colorOffset,
    vk::DeviceSize(0),
    vk::DeviceSize(0),
    vk::DeviceSize(0),
  };
  if (m_texBinding != TexBinding::None && m_texBuffer) {
    uint32_t texBindingIndex = 3;
    if (!m_backend.device().supportsVertexInputDynamicState()) {
      switch (m_texBinding) {
        case TexBinding::Tex1D:
          texBindingIndex = 3;
          break;
        case TexBinding::Tex2D:
          texBindingIndex = 4;
          break;
        case TexBinding::Tex3D:
          texBindingIndex = 5;
          break;
        default:
          break;
      }
    }
    CHECK(texBindingIndex >= 3u && texBindingIndex <= 5u) << "Mesh tex binding index out of range";
    signatureVertexBuffers[texBindingIndex] = m_texBuffer;
    signatureVertexOffsets[texBindingIndex] = m_texOffset;
  }

  std::array<uint64_t, 6> signatureVertexSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[3]),
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[4]),
    m_backend.staticArenaSegmentIdForBuffer(signatureVertexBuffers[5]),
  };

  vk::Buffer signatureIndexBuffer{};
  vk::DeviceSize signatureIndexOffset = 0;
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    signatureIndexBuffer = m_indexUploadBuffer;
    signatureIndexOffset = m_indexUploadOffset;
  }
  uint64_t signatureIndexSegmentId = 0;
  if (signatureIndexBuffer) {
    signatureIndexSegmentId = m_backend.staticArenaSegmentIdForBuffer(signatureIndexBuffer);
  }

  uint64_t signatureTexSegmentId = 0;
  if (m_texBinding != TexBinding::None && m_texBuffer) {
    signatureTexSegmentId = m_backend.staticArenaSegmentIdForBuffer(m_texBuffer);
  }

  const bool allStaticSegments = (signatureVertexSegmentIds[0] != 0) && (signatureVertexSegmentIds[1] != 0) &&
                                 (signatureVertexSegmentIds[2] != 0) &&
                                 ((m_texBinding == TexBinding::None) || (signatureTexSegmentId != 0)) &&
                                 ((m_indexCount == 0) || (signatureIndexSegmentId != 0));

  const uint32_t oitRingIndex = m_backend.sharedOITDescriptorSetRingIndex();
  const vk::DescriptorSet dsOit = m_backend.sharedOITDescriptorSet();
  CHECK(dsOit) << "Mesh pipeline shared OIT descriptor set missing";

  for (size_t variant = 0; variant < groupedDraws.size(); ++variant) {
    auto& draws = groupedDraws[variant];
    if (draws.empty()) {
      continue;
    }

    const size_t meshTypeIndex = variant / kWireVariants;
    const bool wireframe = (variant % kWireVariants) == 1u;
    CHECK(meshTypeIndex < kMeshTypes.size()) << "Mesh pipeline: variant index out of range";

    PipelineKey key;
    key.colorSource = payload.colorSource;
    key.meshType = kMeshTypes[meshTypeIndex];
    key.wireframe = wireframe;
    key.fogMode = fogMode;
    key.shaderHookType = shaderHook;
    key.colorFormats = formats.colorFormats;
    key.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensurePipeline(key, formats);

    ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSetFirst = vkbind::kSetBindlessSampledImages;
    drawSpec.descriptorSets = baseDescriptorSets;
    drawSpec.extraDescriptorBinds =
      std::span<const ZVulkanDescriptorBindInfo>(baseExtraBinds.data(), baseExtraBindCount);
    drawSpec.expectedDescriptorSetCount = expectedSetCount;
    drawSpec.instanceCount = 1;

    ZVulkanPipelineCommandRecorder recorder(cmd);

    const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                          shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
    if (ddpHook) {
      // DDP init/peel have additional per-draw side effects (indirect args preparation)
      // and are intentionally not secondary-cached.
      recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& cb) {
        bindCommonBuffers(cb);
        const vk::PipelineLayout layoutHandle = pipeline.pipeline->pipelineLayout();

        // Bindless texture indices for this batch (see MeshBindlessPC in mesh_func.glslinc).
        cb.pushConstants<MeshBindlessPushConstants>(layoutHandle, vk::ShaderStageFlagBits::eFragment, 0, bindlessPC);

        for (const auto& entry : draws) {
          const MeshDraw& draw = *entry.draw;
          if (entry.wireframe) {
            updateMaterialUBO(renderer,
                              payload,
                              draw.payloadMeshIndex,
                              true,
                              entry.wireColor,
                              pickingPass,
                              true,
                              shaderHook);
          } else {
            updateMaterialUBO(renderer,
                              payload,
                              draw.payloadMeshIndex,
                              draw.useFallbackColor,
                              draw.fallbackColor,
                              pickingPass,
                              false,
                              shaderHook);
          }

          // Re-bind dynamic UBO offsets for lighting/frame/object/material before each draw.
          {
            std::array<vk::DescriptorSet, 2> dynSets{dsLighting, dsTransforms};
            std::array<uint32_t, 4> dynOff{static_cast<uint32_t>(m_dynLightingOffset),
                                           static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                           static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                           static_cast<uint32_t>(m_dynMaterialOffset)};
            cb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layoutHandle, 1, dynSets, dynOff);
          }

          if (m_backend.ddpIndirectCountEnabled()) {
            if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
              // Prepare device-local args once per payload mesh index (shared by wireframe/surface draws).
              const size_t meshIndex = draw.payloadMeshIndex;
              CHECK(meshIndex < m_ddpArgsPrepared.size()) << "Mesh DDP init: args table size mismatch";
              if (!m_ddpArgsPrepared[meshIndex]) {
                if (static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE) {
                  if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
                    struct Cmd
                    {
                      uint32_t indexCount, instanceCount, firstIndex;
                      int32_t vertexOffset;
                      uint32_t firstInstance;
                    } cmd{draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0};
                    const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
                    auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
                    if (slice.buffer && slice.mapped) {
                      std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
                    }
                    m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
                    m_ddpArgsOffsets[meshIndex] = off;
                  } else {
                    struct Cmd
                    {
                      uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
                    } cmd{draw.vertexCount, 1, draw.firstVertex, 0};
                    const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
                    auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
                    if (slice.buffer && slice.mapped) {
                      std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
                    }
                    m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
                    m_ddpArgsOffsets[meshIndex] = off;
                  }
                  m_ddpArgsPrepared[meshIndex] = 1u;
                }
              }
              // Emit init draw as usual
              if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
                cb.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
              } else {
                cb.draw(draw.vertexCount, 1, draw.firstVertex, 0);
              }
            } else if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
              const size_t meshIndex = draw.payloadMeshIndex;
              CHECK(meshIndex < m_ddpArgsPrepared.size()) << "Mesh DDP peel: args table size mismatch";
              CHECK(m_ddpArgsPrepared[meshIndex]) << "Mesh DDP peel: args not prepared in init";
              const vk::DeviceSize off = m_ddpArgsOffsets[meshIndex];
              const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
              const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
              if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
                cb.drawIndexedIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
              } else {
                cb.drawIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(uint32_t) * 4);
              }
            } else {
              // Non-DDP
              if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
                cb.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
              } else {
                cb.draw(draw.vertexCount, 1, draw.firstVertex, 0);
              }
            }
          } else {
            if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
              cb.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
            } else {
              cb.draw(draw.vertexCount, 1, draw.firstVertex, 0);
            }
          }
        }
      });
      continue;
    }

    std::vector<SecondarySignature::Draw> drawCalls;
    drawCalls.reserve(draws.size());
    for (const auto& entry : draws) {
      const MeshDraw& draw = *entry.draw;
      if (entry.wireframe) {
        updateMaterialUBO(renderer,
                          payload,
                          draw.payloadMeshIndex,
                          true,
                          entry.wireColor,
                          pickingPass,
                          /*wireframe*/ true,
                          shaderHook);
      } else {
        updateMaterialUBO(renderer,
                          payload,
                          draw.payloadMeshIndex,
                          draw.useFallbackColor,
                          draw.fallbackColor,
                          pickingPass,
                          /*wireframe*/ false,
                          shaderHook);
      }

      SecondarySignature::Draw call{};
      call.materialOffset = static_cast<uint32_t>(m_dynMaterialOffset);
      if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
        call.indexed = true;
        call.indexCount = draw.indexCount;
        call.firstIndex = draw.firstIndex;
        call.vertexOffset = static_cast<int32_t>(draw.firstVertex);
      } else {
        call.indexed = false;
        call.vertexCount = draw.vertexCount;
        call.firstVertex = draw.firstVertex;
      }
      drawCalls.push_back(call);
    }

    const std::vector<SecondarySignature::Draw>* drawCallsPtr = &drawCalls;
    auto drawFn = [&](vk::raii::CommandBuffer& cb) {
      bindCommonBuffers(cb);
      const vk::PipelineLayout layoutHandle = pipeline.pipeline->pipelineLayout();

      // Bindless texture indices for this batch (see MeshBindlessPC in mesh_func.glslinc).
      cb.pushConstants<MeshBindlessPushConstants>(layoutHandle, vk::ShaderStageFlagBits::eFragment, 0, bindlessPC);

      const auto& calls = *drawCallsPtr;
      for (const auto& call : calls) {
        std::array<vk::DescriptorSet, 2> dynSets{dsLighting, dsTransforms};
        std::array<uint32_t, 4> dynOff{static_cast<uint32_t>(m_dynLightingOffset),
                                       static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                       static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                       call.materialOffset};
        cb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layoutHandle, 1, dynSets, dynOff);

        if (call.indexed) {
          cb.drawIndexed(call.indexCount, 1, call.firstIndex, call.vertexOffset, 0);
        } else {
          cb.draw(call.vertexCount, 1, call.firstVertex, 0);
        }
      }
    };

    if (FLAGS_atlas_vk_cache_draw_secondaries &&
        m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
        usePersistentTransforms && m_usedStaticVBThisFrame && allStaticSegments) {
      m_backend.notifyDrawSecondaryCacheAttempt();
      SecondaryCacheKey cacheKey{};
      cacheKey.frameKey = frameKey;
      cacheKey.streamKey = payload.streamKey;
      cacheKey.picking = pickingPass;
      cacheKey.colorSource = payload.colorSource;
      cacheKey.meshType = key.meshType;
      cacheKey.wireframe = wireframe;
      cacheKey.fogMode = fogMode;
      cacheKey.shaderHookType = shaderHook;
      cacheKey.eye = batch.eye;
      cacheKey.oitRingIndex = oitRingIndex;

      SecondarySignature signature{};
      signature.pipeline = drawSpec.pipelineHandle;
      signature.layout = drawSpec.pipelineLayoutHandle;
      signature.descriptorSets = {baseDescriptorSets[0], dsLighting, dsTransforms, dsOit};
      signature.descriptorGenerations = {m_backend.bindlessSampledImageDescriptorSetGeneration(),
                                         m_backend.sharedLightingDescriptorSetGeneration(),
                                         usePersistentTransforms
                                           ? m_backend.sharedTransformsDescriptorSetPersistentGeneration()
                                           : m_backend.sharedTransformsDescriptorSetUniformGeneration(),
                                         m_backend.sharedOITDescriptorSetGeneration()};
      signature.fixedDynamicOffsets = {static_cast<uint32_t>(m_dynLightingOffset),
                                       static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                       static_cast<uint32_t>(m_dynObjectTransformsOffset)};
      signature.vertexBuffers = signatureVertexBuffers;
      signature.vertexOffsets = signatureVertexOffsets;
      signature.vertexBufferSegmentIds = signatureVertexSegmentIds;
      signature.indexBuffer = signatureIndexBuffer;
      signature.indexOffset = signatureIndexOffset;
      signature.indexBufferSegmentId = signatureIndexSegmentId;
      signature.indexType = vk::IndexType::eUint32;
      signature.pushConstants = bindlessPC;
      signature.draws = std::move(drawCalls);
      drawCallsPtr = &signature.draws;
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
            continue;
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
        if (prev.descriptorSets != signature.descriptorSets) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorSets;
        }
        if (prev.descriptorGenerations != signature.descriptorGenerations) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorGenerations;
        }
        if (prev.fixedDynamicOffsets != signature.fixedDynamicOffsets) {
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
        const bool indexStateEq = (prev.indexBuffer == signature.indexBuffer) &&
                                  (prev.indexOffset == signature.indexOffset) &&
                                  (prev.indexType == signature.indexType);
        if (!indexStateEq) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexState;
        }
        if (prev.indexBufferSegmentId != signature.indexBufferSegmentId) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexBufferLifetime;
        }
        if (std::memcmp(&prev.pushConstants, &signature.pushConstants, sizeof(signature.pushConstants)) != 0) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kPushConstants;
        }
        if (prev.draws != signature.draws) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kCounts;
        }
        if (prev.viewport.x != signature.viewport.x || prev.viewport.y != signature.viewport.y ||
            prev.viewport.width != signature.viewport.width || prev.viewport.height != signature.viewport.height ||
            prev.viewport.minDepth != signature.viewport.minDepth ||
            prev.viewport.maxDepth != signature.viewport.maxDepth) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kViewport;
        }
        if (prev.scissor.offset.x != signature.scissor.offset.x ||
            prev.scissor.offset.y != signature.scissor.offset.y ||
            prev.scissor.extent.width != signature.scissor.extent.width ||
            prev.scissor.extent.height != signature.scissor.extent.height) {
          mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kScissor;
        }
        m_backend.notifyDrawSecondaryCacheSignatureMismatchMask(mask);
      }

      SecondaryCacheEntry& entry = m_secondaryCache[cacheKey];
      entry.signature = std::move(signature);
      drawCallsPtr = &entry.signature.draws;
      m_backend.notifyDrawSecondaryCacheBuild();

      // Secondary inheritance: match the active dynamic rendering segment formats.
      vk::SampleCountFlagBits rasterSamples = vk::SampleCountFlagBits::e1;
      bool samplesCaptured = false;
      for (const auto& attachment : batch.pass.colorAttachments) {
        if (!attachment.handle.valid()) {
          continue;
        }
        const auto& tex =
          vulkan::textureFromHandle(attachment.handle, m_backend.device(), "mesh secondary inheritance color");
        if (!samplesCaptured) {
          rasterSamples = tex.info().samples;
          samplesCaptured = true;
        } else {
          CHECK(tex.info().samples == rasterSamples) << "Mesh secondary: mismatched MSAA sample counts in attachments";
        }
      }
      if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
        const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                    m_backend.device(),
                                                    "mesh secondary inheritance depth");
        if (!samplesCaptured) {
          rasterSamples = tex.info().samples;
          samplesCaptured = true;
        } else {
          CHECK(tex.info().samples == rasterSamples) << "Mesh secondary: mismatched MSAA sample counts in attachments";
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
      renderingInheritance.stencilAttachmentFormat = vk::Format::eUndefined;

      vk::CommandBufferInheritanceInfo inheritance{};
      inheritance.pNext = &renderingInheritance;

      ZVulkanSecondaryBuildInfo secondaryInfo{};
      secondaryInfo.device = &m_backend.device().context().device();
      secondaryInfo.commandPool = m_backend.device().context().commandPool();
      secondaryInfo.inheritance = inheritance;

      entry.commandBuffer = buildStaticSecondary(secondaryInfo, [&](vk::raii::CommandBuffer& secondaryCmd) {
        ZVulkanPipelineCommandRecorder secondaryRecorder(secondaryCmd);
        secondaryRecorder.recordGraphicsDraw(drawSpec, drawFn);
      });
      entry.recorded = true;

      m_backend.notifyDrawSecondaryCacheExecute();
      cmd.executeCommands({static_cast<vk::CommandBuffer>(entry.commandBuffer)});
      continue;
    }

    recorder.recordGraphicsDraw(drawSpec, drawFn);
  }
}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanMeshPipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const MeshPayload& payload)
{
  CHECK(batch.shaderHook.captured) << "Mesh batch missing shader hook snapshot";
  (void)renderer;

  CHECK(payload.paramsCaptured) << "Mesh payload missing params";
  // Frame transforms are shared per eye and updated once per submission in beginRender().
  m_dynFrameTransformsOffset = m_backend.frameTransformsOffset(batch.eye);

  ObjectTransformsUBOStd140 transforms{};
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, transforms);

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Mesh updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    StreamUboCache& streamCache = frameCache.byStream[payload.streamKey];

    if (streamCache.objectTransformsValid && streamCache.params == payload.params &&
        streamCache.followCoordTransform == payload.followCoordTransform &&
        streamCache.followSizeScale == payload.followSizeScale &&
        clipPlanesEqual(streamCache.clipPlanes, batch.clipPlanes)) {
      m_dynObjectTransformsOffset = streamCache.objectTransformsOffset;
      return;
    }

    if (!streamCache.objectTransformsValid) {
      auto slice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
      std::memcpy(slice.mapped, &transforms, sizeof(transforms));
      streamCache.objectTransformsOffset = slice.offset;
      streamCache.objectTransformsValid = true;
    } else {
      std::memcpy(
        m_backend.persistentUniformMappedAt(streamCache.objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
        &transforms,
        sizeof(transforms));
    }

    streamCache.params = payload.params;
    streamCache.followCoordTransform = payload.followCoordTransform;
    streamCache.followSizeScale = payload.followSizeScale;
    streamCache.clipPlanes = batch.clipPlanes;
    m_dynObjectTransformsOffset = streamCache.objectTransformsOffset;
    return;
  }

  // Fallback: no stream key, so we cannot assign a stable persistent slice.
  auto slice = m_backend.suballocateUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
  std::memcpy(slice.mapped, &transforms, sizeof(transforms));
  m_dynObjectTransformsOffset = slice.offset;
}

void ZVulkanMeshPipelineContext::updateMaterialUBO(Z3DRendererBase& renderer,
                                                   const MeshPayload& payload,
                                                   size_t meshIndex,
                                                   bool useFallbackColor,
                                                   const glm::vec4& fallbackColor,
                                                   bool pickingPass,
                                                   bool wireframe,
                                                   Z3DRendererBase::ShaderHookType shaderHook)
{
  (void)renderer;
  CHECK(payload.paramsCaptured) << "Mesh payload missing params";
  (void)shaderHook;

  auto vec4Equal = [](const glm::vec4& a, const glm::vec4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
  };

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  material.material_specular = payload.params.materialSpecular;
  material.material_shininess = payload.params.materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;

  bool useCustomColor = false;
  glm::vec4 colorValue = fallbackColor;

  if (pickingPass) {
    useCustomColor = true;
    if (meshIndex < payload.meshPickingColors.size()) {
      colorValue = payload.meshPickingColors[meshIndex];
    } else {
      colorValue = glm::vec4(0.0f);
    }
    material.alpha = 1.0f;
    material.material_specular = glm::vec4(0.0f);
    material.material_shininess = 0.0f;
  } else if (wireframe || useFallbackColor) {
    useCustomColor = true;
  } else if (payload.colorSource == MeshPayload::ColorSource::CustomColor && meshIndex < payload.meshColors.size()) {
    useCustomColor = true;
    colorValue = payload.meshColors[meshIndex];
  }

  material.use_custom_color = useCustomColor ? 1 : 0;
  material.custom_color = colorValue;

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Mesh updateMaterialUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    StreamUboCache& streamCache = frameCache.byStream[payload.streamKey];

    MaterialKey key{};
    key.pickingPass = pickingPass;
    if (pickingPass) {
      key.kind = MaterialKey::Kind::PerMesh;
      key.meshIndex = meshIndex;
    } else if (wireframe) {
      key.kind = MaterialKey::Kind::SharedWireframe;
    } else if (useFallbackColor) {
      key.kind = MaterialKey::Kind::SharedSurfaceFallback;
    } else if (payload.colorSource == MeshPayload::ColorSource::CustomColor) {
      key.kind = MaterialKey::Kind::PerMesh;
      key.meshIndex = meshIndex;
    } else {
      key.kind = MaterialKey::Kind::SharedSurface;
    }

    auto it = streamCache.materials.find(key);
    if (it != streamCache.materials.end()) {
      MaterialCacheEntry& cached = it->second;
      m_dynMaterialOffset = cached.materialOffset;
      if (cached.params == payload.params && cached.followOpacity == payload.followOpacity &&
          cached.pickingPass == pickingPass && cached.useCustomColor == useCustomColor &&
          vec4Equal(cached.customColor, colorValue)) {
        return;
      }

      std::memcpy(m_backend.persistentUniformMappedAt(cached.materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));
      cached.params = payload.params;
      cached.followOpacity = payload.followOpacity;
      cached.pickingPass = pickingPass;
      cached.useCustomColor = useCustomColor;
      cached.customColor = colorValue;
      return;
    }

    auto slice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;

    MaterialCacheEntry entry{};
    entry.params = payload.params;
    entry.followOpacity = payload.followOpacity;
    entry.pickingPass = pickingPass;
    entry.useCustomColor = useCustomColor;
    entry.customColor = colorValue;
    entry.materialOffset = slice.offset;
    streamCache.materials.emplace(key, std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto slice = m_backend.suballocateUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(slice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = slice.offset;

  VLOG(2) << fmt::format("VK mesh material: alpha={:.3f} picking={} useCustomColor={}",
                         material.alpha,
                         pickingPass,
                         material.use_custom_color != 0);
}

ZVulkanMeshPipelineContext::PipelineInstance&
ZVulkanMeshPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
        return "dual_peeling_init_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return "ppll_count_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return "ppll_store_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "mesh.frag.spv";
    }
  };

  const auto fragmentShader = selectFragmentShader(key.shaderHookType);

  const bool depthOnlyPass = (key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                              key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount);

  // Use the depth-only mesh vertex shader for OIT init/count passes. The corresponding
  // fragment shaders rely on gl_FragCoord and compile (-O) to have no stage inputs,
  // so exporting varyings from mesh.vert would trigger Vulkan interface warnings.
  const std::string vertexShader = depthOnlyPass ? "mesh_depth.vert.spv" : "mesh.vert.spv";

  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + vertexShader, shaderBase + fragmentShader, std::nullopt);

  if (!depthOnlyPass) {
    const uint32_t useMeshColor = key.colorSource == MeshPayload::ColorSource::MeshColor ? 1u : 0u;
    const uint32_t use1D = key.colorSource == MeshPayload::ColorSource::Mesh1DTexture ? 1u : 0u;
    const uint32_t use2D = key.colorSource == MeshPayload::ColorSource::Mesh2DTexture ? 1u : 0u;
    const uint32_t use3D = key.colorSource == MeshPayload::ColorSource::Mesh3DTexture ? 1u : 0u;

    std::array<vk::SpecializationMapEntry, 4> vertexEntries{
      vk::SpecializationMapEntry{.constantID = 40, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 41, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 42, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 43, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
    };
    std::array<uint32_t, 4> vertexData{useMeshColor, use1D, use2D, use3D};
    const uint8_t* vertexPtr = reinterpret_cast<const uint8_t*>(vertexData.data());
    std::vector<uint8_t> vertexBytes(vertexPtr, vertexPtr + sizeof(vertexData));
    std::vector<vk::SpecializationMapEntry> vertexSpecs(vertexEntries.begin(), vertexEntries.end());
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, vertexSpecs, vertexBytes);

    const uint32_t useLinearFog = key.fogMode == FogMode::Linear ? 1u : 0u;
    const uint32_t useExpFog = key.fogMode == FogMode::Exponential ? 1u : 0u;
    const uint32_t useExp2Fog = key.fogMode == FogMode::ExponentialSquared ? 1u : 0u;

    std::array<vk::SpecializationMapEntry, 7> fragmentEntries{
      vk::SpecializationMapEntry{.constantID = 40, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 41, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 42, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 43, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 20, .offset = 4 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 21, .offset = 5 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 22, .offset = 6 * sizeof(uint32_t), .size = sizeof(uint32_t)}
    };
    std::array<uint32_t, 7> fragmentData{useMeshColor, use1D, use2D, use3D, useLinearFog, useExpFog, useExp2Fog};
    const uint8_t* fragmentPtr = reinterpret_cast<const uint8_t*>(fragmentData.data());
    std::vector<uint8_t> fragmentBytes(fragmentPtr, fragmentPtr + sizeof(fragmentData));
    std::vector<vk::SpecializationMapEntry> fragmentSpecs(fragmentEntries.begin(), fragmentEntries.end());
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, fragmentSpecs, fragmentBytes);
  }

  auto vertexInput = depthOnlyPass ? makeSoAMeshDepthVertexInput() : makeSoAMeshVertexInput(key.colorSource);
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, toVkTopology(key.meshType));
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Mesh pipeline missing bindless descriptor set layout";
  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Mesh pipeline missing lighting descriptor set layout";
  const vk::DescriptorSetLayout transformsLayout = m_backend.transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Mesh pipeline missing transforms descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "Mesh pipeline missing OIT descriptor set layout";
  std::vector<vk::DescriptorSetLayout> layouts{bindlessLayout, lightingLayout, transformsLayout, oitLayout};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);

  auto makeDefaultBlendAttachment = []() {
    vk::PipelineColorBlendAttachmentState state{};
    state.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    state.blendEnable = false;
    return state;
  };

  std::vector<vk::PipelineColorBlendAttachmentState> blendAttachments;
  blendAttachments.reserve(formats.colorFormats.size());

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = makeDefaultBlendAttachment();
        state.blendEnable = true;
        state.srcColorBlendFactor = vk::BlendFactor::eOne;
        state.dstColorBlendFactor = vk::BlendFactor::eOne;
        state.colorBlendOp = vk::BlendOp::eAdd;
        state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        state.alphaBlendOp = vk::BlendOp::eAdd;
        blendAttachments.push_back(state);
      }
      // Match GL WA init: depth-test against the (loaded) opaque depth buffer
      // when a depth attachment is present, but do not write depth.
      instance.pipeline->setDepthTestEnable(key.depthFormat.has_value());
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = makeDefaultBlendAttachment();
        state.blendEnable = true;
        if (i == 0) {
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          state.srcColorBlendFactor = vk::BlendFactor::eZero;
          state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eZero;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        }
        blendAttachments.push_back(state);
      }
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel: {
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = makeDefaultBlendAttachment();
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
        blendAttachments.push_back(state);
      }
      // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::Normal:
    default:
      break;
  }

  if (!blendAttachments.empty()) {
    instance.pipeline->setColorBlendAttachments(blendAttachments);
  }

  // Mesh shaders consume bindless texture indices via push constants.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(MeshBindlessPushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

  if (key.wireframe) {
    instance.pipeline->setPolygonMode(vk::PolygonMode::eLine);
    instance.pipeline->setDepthBias(true, -1.0f, -1.0f);
    instance.pipeline->setLineWidth(1.0f);
  }

  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

void ZVulkanMeshPipelineContext::uploadGeometry(const MeshPayload& payload)
{
  m_draws.clear();
  m_vertexCount = 0;
  m_indexCount = 0;

  if (payload.meshes.empty()) {
    return;
  }

  CHECK(payload.colorSource != MeshPayload::ColorSource::CustomColor ||
        payload.meshColors.size() >= payload.meshes.size())
    << "Vulkan mesh backend skipping batch: custom color array is incomplete.";

  size_t totalVertices = 0;
  size_t totalIndices = 0;

  for (size_t i = 0; i < payload.meshes.size(); ++i) {
    ZMesh* mesh = payload.meshes[i];
    if (!mesh) {
      continue;
    }

    if (!payload.pickingPass) {
      CHECK(validateTexturePrerequisites(payload, *mesh))
        << "Vulkan mesh backend skipping batch: texture prerequisites not met.";
    }

    if (mesh->numVertices() == 0) {
      continue;
    }

    if (mesh->numNormals() != mesh->numVertices()) {
      mesh->generateNormals();
    }

    totalVertices += mesh->numVertices();
    if (!mesh->indices().empty()) {
      totalIndices += mesh->indices().size();
    }
  }

  if (totalVertices == 0) {
    return;
  }

  // Track totals for draw and binding decisions
  m_vertexCount = totalVertices;
  m_indexCount = totalIndices;

  // Determine active texture coordinate binding (if any). This is used both by
  // the upload path and by the static-buffer fast path.
  TexBinding texBinding = TexBinding::None;
  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      texBinding = TexBinding::Tex1D;
      break;
    case MeshPayload::ColorSource::Mesh2DTexture:
      texBinding = TexBinding::Tex2D;
      break;
    case MeshPayload::ColorSource::Mesh3DTexture:
      texBinding = TexBinding::Tex3D;
      break;
    case MeshPayload::ColorSource::MeshColor:
    case MeshPayload::ColorSource::CustomColor:
    default:
      texBinding = TexBinding::None;
      break;
  }

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    CacheKey key{payload.streamKey, payload.colorSource, payload.pickingPass};
    if (!m_staticCopyPendingKeys.contains(key)) {
      auto it = m_staticCache.find(key);
      if (it != m_staticCache.end()) {
        CacheEntry& entry = it->second;
        const bool sizeUnchanged = entry.vertexCount == m_vertexCount && entry.indexCount == m_indexCount;
        const bool posSame = entry.posGen == payload.posGen;
        const bool normSame = entry.normGen == payload.normGen;
        const bool texSame = entry.texGen == payload.texGen;
        const bool idxSame = entry.indexGen == payload.indexGen;
        const bool colorSame = entry.colorGen == payload.colorGen;
        const bool wantTex = (texBinding != TexBinding::None);
        const bool texLayoutSame = (entry.hasTex == wantTex) && (!wantTex || static_cast<bool>(entry.vbTex));
        const bool buffersOk = static_cast<bool>(entry.vbPos) && static_cast<bool>(entry.vbNorm) &&
                               static_cast<bool>(entry.vbColor) && texLayoutSame &&
                               ((m_indexCount == 0) || static_cast<bool>(entry.ib));
        if (entry.promoted && sizeUnchanged && posSame && normSame && texSame && idxSame && colorSame && buffersOk) {
          // Bind static buffers
          m_posBuffer = entry.vbPos.buffer;
          m_normBuffer = entry.vbNorm.buffer;
          m_colorBuffer = entry.vbColor.buffer;
          m_posOffset = entry.vbPos.offset;
          m_normOffset = entry.vbNorm.offset;
          m_colorOffset = entry.vbColor.offset;
          m_texBinding = texBinding;
          if (wantTex) {
            m_texBuffer = entry.vbTex.buffer;
            m_texOffset = entry.vbTex.offset;
          } else {
            m_texBuffer = vk::Buffer{};
            m_texOffset = 0;
          }
          if (m_indexCount > 0) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          } else {
            m_indexUploadBuffer = vk::Buffer{};
            m_indexUploadOffset = 0;
          }

          vulkan::pinStaticSlicesForActiveSubmission(
            m_backend,
            {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
          m_usedStaticVBThisFrame = true;
          entry.usedStaticOnce = true;

          // Rebuild draw list without touching CPU vertex data.
          size_t vertexCursor = 0;
          size_t indexCursor = 0;
          for (size_t meshIdx = 0; meshIdx < payload.meshes.size(); ++meshIdx) {
            ZMesh* mesh = payload.meshes[meshIdx];
            if (!mesh || mesh->numVertices() == 0) {
              continue;
            }

            MeshDraw draw{};
            draw.mesh = mesh;
            draw.payloadMeshIndex = meshIdx;
            draw.firstVertex = static_cast<uint32_t>(vertexCursor);
            draw.vertexCount = static_cast<uint32_t>(mesh->numVertices());

            const auto& indices = mesh->indices();
            if (!indices.empty() && m_indexCount > 0 && m_indexUploadBuffer) {
              draw.indexed = true;
              draw.firstIndex = static_cast<uint32_t>(indexCursor);
              draw.indexCount = static_cast<uint32_t>(indices.size());
              indexCursor += indices.size();
            }

            const auto& colors = mesh->colors();
            const bool hasVertexColors =
              payload.colorSource == MeshPayload::ColorSource::MeshColor && colors.size() >= mesh->numVertices();
            const bool fallbackColorNeeded = payload.colorSource == MeshPayload::ColorSource::MeshColor && !hasVertexColors;

            if (payload.pickingPass) {
              draw.useFallbackColor = true;
              if (meshIdx < payload.meshPickingColors.size()) {
                draw.fallbackColor = payload.meshPickingColors[meshIdx];
              } else {
                draw.fallbackColor = glm::vec4(0.0f);
              }
            } else {
              draw.useFallbackColor = fallbackColorNeeded;
              draw.fallbackColor = fallbackColorNeeded ? kFallbackMeshColor : glm::vec4(1.0f);
            }

            m_draws.push_back(draw);
            vertexCursor += mesh->numVertices();
          }

          entry.usedStaticOnce = true;
          return;
        }
      }
    }
  }

  // Allocate SoA streams in the per-frame upload arena
  const size_t posBytes = totalVertices * sizeof(glm::vec3);
  const size_t normBytes = totalVertices * sizeof(glm::vec3);
  const size_t colorBytes = totalVertices * sizeof(glm::vec4);
  size_t texBytes = 0;
  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      texBytes = totalVertices * sizeof(float);
      break;
    case MeshPayload::ColorSource::Mesh2DTexture:
      texBytes = totalVertices * sizeof(glm::vec2);
      break;
    case MeshPayload::ColorSource::Mesh3DTexture:
      texBytes = totalVertices * sizeof(glm::vec3);
      break;
    case MeshPayload::ColorSource::MeshColor:
    case MeshPayload::ColorSource::CustomColor:
    default:
      texBytes = 0;
      break;
  }

  m_backend.reserveUploadSlices({
    {posBytes,                        alignof(glm::vec3)},
    {normBytes,                       alignof(glm::vec3)},
    {colorBytes,                      alignof(glm::vec4)},
    {texBytes,                        4                 },
    {totalIndices * sizeof(uint32_t), alignof(uint32_t) }
  });
  auto posSlice = m_backend.suballocateUpload(posBytes, alignof(glm::vec3));
  auto normSlice = m_backend.suballocateUpload(normBytes, alignof(glm::vec3));
  auto colorSlice = m_backend.suballocateUpload(colorBytes, alignof(glm::vec4));
  auto texSlice = (texBytes > 0) ? m_backend.suballocateUpload(texBytes, 4) : Z3DRendererVulkanBackend::UploadSlice{};
  if (!posSlice.buffer || !posSlice.mapped || !normSlice.buffer || !normSlice.mapped || !colorSlice.buffer ||
      !colorSlice.mapped || (texBytes > 0 && (!texSlice.buffer || !texSlice.mapped))) {
    m_draws.clear();
    return;
  }
  // default to upload arena; may be replaced by static cache below
  m_posBuffer = posSlice.buffer;
  m_normBuffer = normSlice.buffer;
  m_colorBuffer = colorSlice.buffer;
  m_texBuffer = texSlice.buffer;
  m_posOffset = posSlice.offset;
  m_normOffset = normSlice.offset;
  m_colorOffset = colorSlice.offset;
  m_texOffset = texSlice.offset;
  m_texBinding = texBinding;

  // Index buffer slice
  uint32_t* indexPtr = nullptr;
  if (totalIndices > 0) {
    auto idxSlice = m_backend.suballocateUpload(totalIndices * sizeof(uint32_t), alignof(uint32_t));
    if (!idxSlice.buffer || !idxSlice.mapped) {
      m_draws.clear();
      return;
    }
    m_indexUploadBuffer = idxSlice.buffer;
    m_indexUploadOffset = idxSlice.offset;
    indexPtr = static_cast<uint32_t*>(idxSlice.mapped);
  }

  auto* posOut = static_cast<glm::vec3*>(posSlice.mapped);
  auto* normOut = static_cast<glm::vec3*>(normSlice.mapped);
  auto* colorOut = static_cast<glm::vec4*>(colorSlice.mapped);

  size_t vertexCursor = 0;
  size_t indexCursor = 0;

  for (size_t meshIdx = 0; meshIdx < payload.meshes.size(); ++meshIdx) {
    ZMesh* mesh = payload.meshes[meshIdx];
    if (!mesh || mesh->numVertices() == 0) {
      continue;
    }

    MeshDraw draw{};
    draw.mesh = mesh;
    draw.payloadMeshIndex = meshIdx;
    draw.firstVertex = static_cast<uint32_t>(vertexCursor);
    draw.vertexCount = static_cast<uint32_t>(mesh->numVertices());

    const auto& positions = mesh->vertices();
    const auto& normals = mesh->normals();
    const auto& colors = mesh->colors();
    const auto& tex1D = mesh->textureCoordinates1D();
    const auto& tex2D = mesh->textureCoordinates2D();
    const auto& tex3D = mesh->textureCoordinates3D();

    const bool hasVertexColors =
      payload.colorSource == MeshPayload::ColorSource::MeshColor && colors.size() >= mesh->numVertices();
    const bool fallbackColorNeeded = payload.colorSource == MeshPayload::ColorSource::MeshColor && !hasVertexColors;

    // Positions
    std::memcpy(posOut + vertexCursor, positions.data(), positions.size() * sizeof(glm::vec3));
    // Normals (ensure size matches)
    if (normals.size() == positions.size()) {
      std::memcpy(normOut + vertexCursor, normals.data(), normals.size() * sizeof(glm::vec3));
    } else {
      for (size_t v = 0; v < positions.size(); ++v) {
        normOut[vertexCursor + v] = glm::vec3(0.0f, 0.0f, 1.0f);
      }
    }
    // Colors
    if (hasVertexColors) {
      std::memcpy(colorOut + vertexCursor, colors.data(), colors.size() * sizeof(glm::vec4));
    } else {
      for (size_t v = 0; v < positions.size(); ++v) {
        colorOut[vertexCursor + v] = glm::vec4(1.0f);
      }
    }
    // Texture coordinates (one active binding only)
    if (m_texBinding == TexBinding::Tex1D) {
      auto* tOut = static_cast<float*>(texSlice.mapped);
      for (size_t v = 0; v < positions.size(); ++v) {
        tOut[vertexCursor + v] = tex1D.size() > v ? tex1D[v] : 0.0f;
      }
    } else if (m_texBinding == TexBinding::Tex2D) {
      auto* tOut = static_cast<glm::vec2*>(texSlice.mapped);
      if (tex2D.size() == positions.size()) {
        std::memcpy(tOut + vertexCursor, tex2D.data(), tex2D.size() * sizeof(glm::vec2));
      } else {
        for (size_t v = 0; v < positions.size(); ++v) {
          tOut[vertexCursor + v] = glm::vec2(0.0f);
        }
      }
    } else if (m_texBinding == TexBinding::Tex3D) {
      auto* tOut = static_cast<glm::vec3*>(texSlice.mapped);
      if (tex3D.size() == positions.size()) {
        std::memcpy(tOut + vertexCursor, tex3D.data(), tex3D.size() * sizeof(glm::vec3));
      } else {
        for (size_t v = 0; v < positions.size(); ++v) {
          tOut[vertexCursor + v] = glm::vec3(0.0f);
        }
      }
    }

    vertexCursor += mesh->numVertices();

    const auto& indices = mesh->indices();
    if (!indices.empty() && indexPtr) {
      draw.indexed = true;
      draw.firstIndex = static_cast<uint32_t>(indexCursor);
      draw.indexCount = static_cast<uint32_t>(indices.size());
      for (size_t idx = 0; idx < indices.size(); ++idx) {
        indexPtr[indexCursor + idx] = static_cast<uint32_t>(indices[idx]);
      }
      indexCursor += indices.size();
    }

    if (payload.pickingPass) {
      draw.useFallbackColor = true;
      if (meshIdx < payload.meshPickingColors.size()) {
        draw.fallbackColor = payload.meshPickingColors[meshIdx];
      } else {
        draw.fallbackColor = glm::vec4(0.0f);
      }
    } else {
      draw.useFallbackColor = fallbackColorNeeded;
      draw.fallbackColor = fallbackColorNeeded ? kFallbackMeshColor : glm::vec4(1.0f);
    }

    m_draws.push_back(draw);
  }

  // ScopedMap unmaps automatically on destruction.

  m_vertexCount = totalVertices;
  m_indexCount = totalIndices;

  // Attempt static SoA promotion based on renderer identity + gen counters
  {
    CHECK(payload.streamKey != 0) << "Mesh payload missing streamKey";
    CacheKey key{payload.streamKey, payload.colorSource, payload.pickingPass};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2; // frames unchanged before promotion
    const auto promoteToStatics = [&](CacheEntry& dstEntry) -> bool {
      const size_t idxBytes = totalIndices * sizeof(uint32_t);
      Z3DRendererVulkanBackend::UploadSlice idxUpload{};
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = nullptr;
      if (idxBytes > 0) {
        if (!m_indexUploadBuffer) {
          return false;
        }
        idxUpload = Z3DRendererVulkanBackend::UploadSlice{m_indexUploadBuffer, m_indexUploadOffset, nullptr, idxBytes};
        idxSrc = &idxUpload;
      }

      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&dstEntry.vbPos,   &posSlice,                          posBytes,   alignof(glm::vec3), false},
              {&dstEntry.vbNorm,  &normSlice,                         normBytes,  alignof(glm::vec3), false},
              {&dstEntry.vbColor, &colorSlice,                        colorBytes, alignof(glm::vec4), false},
              {&dstEntry.vbTex,   texBytes > 0 ? &texSlice : nullptr, texBytes,   4,                  false},
              {&dstEntry.ib,      idxSrc,                             idxBytes,   alignof(uint32_t),  true }
      },
            &stagedBytes)) {
        return false;
      }

      dstEntry.hasTex = texBytes > 0;
      if (stagedBytes > 0) {
        m_backend.addMeshBytesStaged(stagedBytes);
      }
      return true;
    };
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.posGen = payload.posGen;
      entry.normGen = payload.normGen;
      entry.colorGen = payload.colorGen;
      entry.texGen = payload.texGen;
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
      const bool sizeUnchanged = entry.vertexCount == m_vertexCount && entry.indexCount == m_indexCount;
      const bool posSame = entry.posGen == payload.posGen;
      const bool normSame = entry.normGen == payload.normGen;
      const bool texSame = entry.texGen == payload.texGen;
      const bool idxSame = entry.indexGen == payload.indexGen;
      const bool colorSame = entry.colorGen == payload.colorGen;
      if (sizeUnchanged && posSame && normSame && texSame && idxSame) {
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
        entry.posGen = payload.posGen;
        entry.normGen = payload.normGen;
        entry.colorGen = payload.colorGen;
        entry.texGen = payload.texGen;
        entry.indexGen = payload.indexGen;
        return;
      }

      // If promoted and sizes match, restage on the next frame only. For the
      // edit frame (any stream changed), keep using upload slices to avoid
      // hazards; otherwise bind static VBs.
      if (entry.promoted && !sizeUnchanged) {
        vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
        entry.promoted = false;
        entry.usedStaticOnce = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.posGen = payload.posGen;
        entry.normGen = payload.normGen;
        entry.colorGen = payload.colorGen;
        entry.texGen = payload.texGen;
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

      if (entry.promoted && sizeUnchanged) {
        bool anyChanged = (!posSame) || (!normSame) || (!colorSame) || (!texSame) || (!idxSame);

        // If this stream is still changing before we've ever bound the static
        // buffers, drop the statics and fall back to upload-only mode to avoid
        // paying upload->device copies every frame without a benefit.
        if (anyChanged && !entry.usedStaticOnce) {
          vulkan::releaseStaticSlices(m_backend,
                                      {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
          entry.indexCount = static_cast<uint32_t>(m_indexCount);
          entry.posGen = payload.posGen;
          entry.normGen = payload.normGen;
          entry.colorGen = payload.colorGen;
          entry.texGen = payload.texGen;
          entry.indexGen = payload.indexGen;
          return;
        }

        if (!posSame) {
          m_backend.scheduleStaticCopy(entry.vbPos.buffer, entry.vbPos.offset, posSlice, false);
        }
        if (!normSame) {
          m_backend.scheduleStaticCopy(entry.vbNorm.buffer, entry.vbNorm.offset, normSlice, false);
        }
        if (!colorSame) {
          m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colorSlice, false);
        }
        if (texBytes > 0 && !texSame && entry.vbTex) {
          m_backend.scheduleStaticCopy(entry.vbTex.buffer, entry.vbTex.offset, texSlice, false);
        }
        if (!idxSame && totalIndices > 0 && m_indexUploadBuffer) {
          Z3DRendererVulkanBackend::UploadSlice idxUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          totalIndices * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, idxUpload, true);
        }

        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        entry.posGen = payload.posGen;
        entry.normGen = payload.normGen;
        entry.colorGen = payload.colorGen;
        entry.texGen = payload.texGen;
        entry.indexGen = payload.indexGen;

        if (!anyChanged) {
          // Bind static slices (per-attribute VBs)
          m_posBuffer = entry.vbPos.buffer;
          m_normBuffer = entry.vbNorm.buffer;
          m_colorBuffer = entry.vbColor.buffer;
          m_texBuffer = entry.vbTex ? entry.vbTex.buffer : vk::Buffer{};
          m_posOffset = entry.vbPos.offset;
          m_normOffset = entry.vbNorm.offset;
          m_colorOffset = entry.vbColor.offset;
          m_texOffset = entry.hasTex ? entry.vbTex.offset : 0;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib.buffer;
            m_indexUploadOffset = entry.ib.offset;
          }
          vulkan::pinStaticSlicesForActiveSubmission(
            m_backend,
            {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
          entry.usedStaticOnce = true;
          return;
        }
        // Defer restaging to the next frame; keep upload slices bound.
        m_staticCopyPendingKeys.insert(key);
        return;
      }

      // Consider promotion when stable for N frames
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.posGen = payload.posGen;
      entry.normGen = payload.normGen;
      entry.colorGen = payload.colorGen;
      entry.texGen = payload.texGen;
      entry.indexGen = payload.indexGen;

      if (!entry.promoted && sizeUnchanged && entry.unchangedFrames >= kPromotionThreshold) {
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_staticCopyPendingKeys.insert(key);
        } else {
          VLOG(2) << "Mesh static promotion skipped: arena out of space";
        }
      }
    }
  }
}

// No GL texture bridging in Vulkan mesh pipeline. Texture binding, if any,
// is provided via bindless sampled-image tables (set=0). Index 0 in each
// bindless table is reserved for backend-owned placeholder textures so shaders
// can safely treat 0 as "no texture".

} // namespace nim
