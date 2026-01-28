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
#include "zvulkandescriptorset.h"
#include "zvulkantexture.h"
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

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>

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

} // namespace

ZVulkanMeshPipelineContext::ZVulkanMeshPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanMeshPipelineContext::~ZVulkanMeshPipelineContext() = default;

void ZVulkanMeshPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_draws.clear();
  m_posBuffer = nullptr;
  m_normBuffer = nullptr;
  m_colorBuffer = nullptr;
  m_texBuffer = nullptr;
  m_posOffset = m_normOffset = m_colorOffset = m_texOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  m_texBinding = TexBinding::None;
  // Retire per-frame UBOs so they are not overwritten while still in use by
  // earlier in-flight frames. We defer destruction until the current active
  // submission finishes.
  
  resetDescriptors();
  m_transientDescriptorSets.clear();
  m_ddpTransformsFrozen = false;
  m_ddpMaterialFrozen = false;
  m_ddpArgsOffsets.clear();
}

void ZVulkanMeshPipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  for (auto& sp : m_retainedUbos) {
    auto keep = sp;
    m_backend.scheduleAfterActiveSubmissionFence([keep]() {});
  }
  m_retainedUbos.clear();
}

void ZVulkanMeshPipelineContext::resetDescriptors()
{
  m_dsTextures.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  m_dsOIT.reset();
  m_texturesSetInitialized = false;
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

  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = renderer.shaderHookType();

  m_dynLightingOffset = m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload);
  ensureOITResources();
  {
    glm::vec2 extent = batch.pass.viewport.extent;
    if (extent.x <= 0.0f || extent.y <= 0.0f) {
      const auto& viewportState = renderer.frameState().viewport;
      extent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
    }
    // No OIT UBO; set 3 carries only the DDP flag
    
  }
  // Descriptor sets are primed in beginRender(); avoid record-time rewrites.
  CHECK(m_dsLighting && m_dsTransforms) << "Mesh pipeline descriptor sets missing (lighting/transforms)";
  if (shaderHook != Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    CHECK(m_dsTextures != nullptr) << "Mesh pipeline textures descriptor set not initialised";
  }

  ZVulkanDescriptorSet* texturesOverride = nullptr;
  if (m_setTextures) {
    if (auto* drawTex = m_backend.allocateOverrideDescriptorSet(m_setTextures)) {
      ensurePlaceholderTextures();
      auto sampler = m_backend.defaultSampler();
      if (m_placeholder1D && m_placeholder2D && m_placeholder3D) {
        drawTex->updateTexture(0, *m_placeholder1D, sampler);
        drawTex->updateTexture(1, *m_placeholder2D, sampler);
        drawTex->updateTexture(2, *m_placeholder3D, sampler);
        drawTex->updateTexture(3, *m_placeholder2D, sampler);
        drawTex->updateTexture(4, *m_placeholder2D, sampler);
      }
      if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
        const auto& hookPara = renderer.shaderHookPara();
        if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
          auto& depthTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                         m_backend.device(),
                                                         "mesh dual-depth-peeling depth blender");
          drawTex->updateTexture(vkbind::kBindingDDPMeshDepthBlender, depthTexture, sampler);
        }
        if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
          auto& frontTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                         m_backend.device(),
                                                         "mesh dual-depth-peeling front blender");
          drawTex->updateTexture(vkbind::kBindingDDPMeshFrontBlender, frontTexture, sampler);
        }
      } else if (!pickingPass && payload.textureHandle.valid() &&
                 payload.textureHandle.backend == RenderBackend::Vulkan) {
        auto& sampledTexture =
          vulkan::textureFromHandle(payload.textureHandle, m_backend.device(), "mesh payload sampled texture");
        switch (payload.colorSource) {
          case MeshPayload::ColorSource::Mesh1DTexture:
            drawTex->updateTexture(0, sampledTexture, sampler);
            break;
          case MeshPayload::ColorSource::Mesh2DTexture:
            drawTex->updateTexture(1, sampledTexture, sampler);
            break;
          case MeshPayload::ColorSource::Mesh3DTexture:
            drawTex->updateTexture(2, sampledTexture, sampler);
            break;
          default:
            break;
        }
      }
      m_transientDescriptorSets.push_back(drawTex);
      texturesOverride = m_transientDescriptorSets.back();
    }
  }
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    CHECK(texturesOverride) << "Mesh DDP peel: override descriptor allocation failed (fatal)";
  }

  const vk::DescriptorSet texturesSet = texturesOverride
                                          ? texturesOverride->descriptorSet()
                                          : (m_dsTextures ? m_dsTextures->descriptorSet() : vk::DescriptorSet{});
  if (!texturesSet) {
    return;
  }
  // Bind only the textures set up-front. Lighting/transforms use dynamic UBOs and are
  // re-bound per draw with the correct dynamic offsets below to satisfy validation.
  std::vector<vk::DescriptorSet> baseDescriptorSets{texturesSet};
  std::vector<ZVulkanDescriptorBindInfo> baseExtraBinds;
  uint32_t expectedSetCount = 1;
  if (m_dsOIT) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitBind.sets = {m_dsOIT->descriptorSet()};
    baseExtraBinds.push_back(oitBind);
    expectedSetCount = std::max(expectedSetCount, vkbind::kSetOITParams + 1);
  }

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

  std::unordered_map<const PipelineInstance*, std::vector<DrawCallInfo>> groupedDraws;

  auto recordDrawForKey = [&](const MeshDraw& d, bool wire, glm::vec4 wireColor) {
    PipelineKey key;
    key.colorSource = payload.colorSource;
    key.meshType = d.mesh->type();
    key.wireframe = wire;
    key.fogMode = fogMode;
    key.shaderHookType = shaderHook;
    key.colorFormats = formats.colorFormats;
    key.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensurePipeline(key, formats);
    groupedDraws[&pipeline].push_back(DrawCallInfo{&d, wire, wireColor});
  };

  for (const auto& draw : m_draws) {
    if (!draw.mesh) {
      continue;
    }
    if (drawSurface) {
      recordDrawForKey(draw, false, glm::vec4(0.0f));
    }
    if (drawWireframe) {
      const glm::vec4 wireColor = pickingPass ? draw.fallbackColor : payload.wireframeColor;
      recordDrawForKey(draw, true, wireColor);
    }
  }

  auto bindCommonBuffers = [&](vk::raii::CommandBuffer& cb) {
    std::array<vk::Buffer, 3> baseBufs{m_posBuffer, m_normBuffer, m_colorBuffer};
    std::array<vk::DeviceSize, 3> baseOffs{m_posOffset, m_normOffset, m_colorOffset};
    cb.bindVertexBuffers(0, baseBufs, baseOffs);

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
      std::array<vk::Buffer, 1> texBuf{m_texBuffer};
      std::array<vk::DeviceSize, 1> texOff{m_texOffset};
      cb.bindVertexBuffers(texBindingIndex, texBuf, texOff);
    }

    std::array<vk::Buffer, 1> dummyBuf{m_backend.dummyVertexBuffer()};
    std::array<vk::DeviceSize, 1> dummyOff{0};
    if (m_texBinding != TexBinding::Tex1D) {
      cb.bindVertexBuffers(3, dummyBuf, dummyOff);
    }
    if (m_texBinding != TexBinding::Tex2D) {
      cb.bindVertexBuffers(4, dummyBuf, dummyOff);
    }
    if (m_texBinding != TexBinding::Tex3D) {
      cb.bindVertexBuffers(5, dummyBuf, dummyOff);
    }
    if (m_indexCount > 0 && m_indexUploadBuffer) {
      cb.bindIndexBuffer(m_indexUploadBuffer, m_indexUploadOffset, vk::IndexType::eUint32);
    }
  };

  for (const auto& groupedEntry : groupedDraws) {
    const PipelineInstance* pipelinePtr = groupedEntry.first;
    const auto& draws = groupedEntry.second;
    if (draws.empty()) {
      continue;
    }

    ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
    drawSpec.viewports = {viewport};
    drawSpec.scissors = {scissor};
    drawSpec.pipelineHandle = pipelinePtr->pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipelinePtr->pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSetFirst = vkbind::kSetInputs;
    drawSpec.descriptorSets = baseDescriptorSets;
    drawSpec.extraDescriptorBinds = baseExtraBinds;
    drawSpec.expectedDescriptorSetCount = expectedSetCount;
    drawSpec.instanceCount = 1;

    ZVulkanPipelineCommandRecorder recorder(cmd);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& cb) {
      bindCommonBuffers(cb);
      const vk::PipelineLayout layoutHandle = pipelinePtr->pipeline->pipelineLayout();

      for (const auto& entry : draws) {
        const MeshDraw& draw = *entry.draw;
        if (entry.wireframe) {
          updateMaterialUBO(payload, draw.payloadMeshIndex, true, entry.wireColor, pickingPass);
        } else {
          updateMaterialUBO(payload, draw.payloadMeshIndex, draw.useFallbackColor, draw.fallbackColor, pickingPass);
        }

        if (!entry.wireframe && shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
          const float n = renderer.viewState().nearClip;
          const float f = renderer.viewState().farClip;
          const float denom = std::max(f - n, 1e-6f);
          const float a = (f * n) / denom;
          const float b = 0.5f * (f + n) / denom + 0.5f;
          const float depthScale = renderer.sceneState().weightedBlendedDepthScale;
          glm::vec4 pushConstants(a, b, depthScale, 0.0f);
          cb.pushConstants<glm::vec4>(layoutHandle, vk::ShaderStageFlagBits::eFragment, 0, pushConstants);
        }

        // Re-bind dynamic UBO offsets for lighting/transforms/material before each draw.
        {
          std::array<vk::DescriptorSet, 2> dynSets{m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
          std::array<uint32_t, 3> dynOff{static_cast<uint32_t>(m_dynLightingOffset),
                                         static_cast<uint32_t>(m_dynTransformsOffset),
                                         static_cast<uint32_t>(m_dynMaterialOffset)};
          cb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layoutHandle, 1, dynSets, dynOff);
        }

        if (m_backend.ddpIndirectCountEnabled()) {
          if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
            // Prepare device-local args and store offsets
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
                m_ddpArgsOffsets.push_back(off);
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
                m_ddpArgsOffsets.push_back(off);
              }
            }
            // Emit init draw as usual
            if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
              cb.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
            } else {
              cb.draw(draw.vertexCount, 1, draw.firstVertex, 0);
            }
          } else if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
            // Use prepared args in order
            static size_t ddpCursor = 0;
            CHECK(ddpCursor < m_ddpArgsOffsets.size()) << "Mesh DDP peel: args not prepared in init";
            const vk::DeviceSize off = m_ddpArgsOffsets[ddpCursor++];
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
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorLayouts()
{
  if (!m_setTextures) {
    m_setTextures = m_backend.meshTextureDescriptorSetLayout();
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

void ZVulkanMeshPipelineContext::ensurePlaceholderTextures()
{
  auto& device = m_backend.device();

  if (!m_placeholder1D) {
    // Vulkan portability: use 2D Nx1 for the 1D mesh texture path.
    auto info =
      ZVulkanTexture::CreateInfo::make2D(1,
                                         1,
                                         vk::Format::eR8G8B8A8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder1D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder1D->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  if (!m_placeholder2D) {
    auto info =
      ZVulkanTexture::CreateInfo::make2D(1,
                                         1,
                                         vk::Format::eR8G8B8A8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder2D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder2D->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  if (!m_placeholder3D) {
    auto info =
      ZVulkanTexture::CreateInfo::make3D(1,
                                         1,
                                         1,
                                         vk::Format::eR8G8B8A8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder3D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder3D->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();
  ensurePlaceholderTextures();

  if (!m_dsTextures) {
    m_dsTextures = m_backend.allocateFrameDescriptorSet(m_setTextures);
  }
  if (!m_dsLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(m_setLighting);
  }
  if (!m_dsTransforms) {
    m_dsTransforms = m_backend.allocateFrameDescriptorSet(m_setTransforms);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }

  // Prime dynamic UBO bindings to the per-frame uniform arena once
  if (m_dsLighting) {
    m_dsLighting->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (m_dsTransforms) {
    m_dsTransforms->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(TransformsUBOStd140));
    m_dsTransforms->writeUniformBufferDynamicOnce(1, m_backend.uniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }

  // Set 3 now only carries the DDP flag SSBO
  if (m_dsOIT && !m_backend.isRecording()) {
    if (auto* buf = m_backend.ddpChangedFlagBufferObj()) {
      m_dsOIT->writeStorageBufferOnce(vkbind::kBindingOITDDPFlag, *buf);
    }
  }
}

void ZVulkanMeshPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
}

 

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanMeshPipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const MeshPayload& payload)
{
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);

  CHECK(payload.params != nullptr) << "Mesh payload missing params";
  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  const glm::mat4 model =
    (payload.followCoordTransform && payload.params) ? payload.params->coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  const glm::mat4 combined = eyeState.viewMatrix * transforms.pos_transform;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const float sizeScale = (payload.followSizeScale && payload.params) ? payload.params->sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);

  if (!(ddp && m_ddpTransformsFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(TransformsUBOStd140));
    std::memcpy(slice.mapped, &transforms, sizeof(transforms));
    m_dynTransformsOffset = slice.offset;
    if (ddp) {
      m_ddpTransformsFrozen = true;
    }
  }

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params->materialAmbient;
  material.material_specular = payload.params->materialSpecular;
  material.material_shininess = payload.params->materialShininess;
  material.alpha = (!payload.followOpacity || !payload.params) ? 1.0f : payload.params->opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);
  if (!(ddp && m_ddpMaterialFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;
    if (ddp) {
      m_ddpMaterialFrozen = true;
    }
  }

  VLOG(2) << fmt::format("VK mesh xf params: sizeScale={:.3f} ortho={}",
                         payload.params->sizeScale,
                         (eyeState.isPerspective ? 0 : 1));
}

void ZVulkanMeshPipelineContext::updateMaterialUBO(const MeshPayload& payload,
                                                   size_t meshIndex,
                                                   bool useFallbackColor,
                                                   const glm::vec4& fallbackColor,
                                                   bool pickingPass)
{
  MaterialUBOStd140 material{};
  material.material_ambient = payload.params->materialAmbient;
  material.material_specular = payload.params->materialSpecular;
  material.material_shininess = payload.params->materialShininess;
  material.alpha = payload.params->opacity;

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
  } else if (useFallbackColor) {
    useCustomColor = true;
  } else if (payload.colorSource == MeshPayload::ColorSource::CustomColor && meshIndex < payload.meshColors.size()) {
    useCustomColor = true;
    colorValue = payload.meshColors[meshIndex];
  }

  material.use_custom_color = useCustomColor ? 1 : 0;
  material.custom_color = colorValue;

  {
    auto slice = m_backend.suballocateUniform(sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;
  }

  VLOG(2) << fmt::format("VK mesh material: alpha={:.3f} picking={} useCustomColor={}",
                         material.alpha,
                         pickingPass,
                         material.use_custom_color != 0);
}

void ZVulkanMeshPipelineContext::bindDescriptorSets(vk::raii::CommandBuffer& cmd,
                                                    const PipelineInstance& pipeline,
                                                    ZVulkanDescriptorSet* texturesOverride) const
{
  if ((!m_dsTextures && !texturesOverride) || !m_dsLighting || !m_dsTransforms) {
    return;
  }

  const vk::DescriptorSet texSet = texturesOverride ? texturesOverride->descriptorSet() : m_dsTextures->descriptorSet();
  std::array<vk::DescriptorSet, 3> sets{texSet, m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                         pipeline.pipeline->pipelineLayout(),
                         vkbind::kSetInputs,
                         sets,
                         {});
  if (m_dsOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_dsOIT->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           pipeline.pipeline->pipelineLayout(),
                           vkbind::kSetOITParams,
                           sets3,
                           {});
  }
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

  ensureDescriptorLayouts();

  PipelineInstance instance;

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> std::string {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return "dual_peeling_init_mesh.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_mesh.frag.spv";
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

  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "mesh.vert.spv", shaderBase + fragmentShader, std::nullopt);

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

  auto vertexInput = makeSoAMeshVertexInput(key.colorSource);
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, toVkTopology(key.meshType));
  std::vector<vk::DescriptorSetLayout> layouts{m_setTextures, m_setLighting, m_setTransforms, m_setOIT};
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
      instance.pipeline->setDepthTestEnable(false);
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

      vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                  .offset = 0,
                                  .size = sizeof(glm::vec4)};
      instance.pipeline->setPushConstantRanges({range});
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
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          state.blendEnable = false;
        }
        blendAttachments.push_back(state);
      }
      // DDP passes do their own depth arbitration; disable depth testing/writes.
      instance.pipeline->setDepthTestEnable(false);
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

  // Allocate SoA streams in the per-frame upload arena
  const size_t posBytes = totalVertices * sizeof(glm::vec3);
  const size_t normBytes = totalVertices * sizeof(glm::vec3);
  const size_t colorBytes = totalVertices * sizeof(glm::vec4);
  size_t texBytes = 0;
  TexBinding texBinding = TexBinding::None;
  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      texBinding = TexBinding::Tex1D;
      texBytes = totalVertices * sizeof(float);
      break;
    case MeshPayload::ColorSource::Mesh2DTexture:
      texBinding = TexBinding::Tex2D;
      texBytes = totalVertices * sizeof(glm::vec2);
      break;
    case MeshPayload::ColorSource::Mesh3DTexture:
      texBinding = TexBinding::Tex3D;
      texBytes = totalVertices * sizeof(glm::vec3);
      break;
    case MeshPayload::ColorSource::MeshColor:
    case MeshPayload::ColorSource::CustomColor:
    default:
      texBinding = TexBinding::None;
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
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.posGen = payload.posGen;
      entry.normGen = payload.normGen;
      entry.colorGen = payload.colorGen;
      entry.texGen = payload.texGen;
      entry.indexGen = payload.indexGen;
      m_staticCache.emplace(key, entry);
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

      // If promoted and sizes match, restage on the next frame only. For the
      // edit frame (any stream changed), keep using upload slices to avoid
      // hazards; otherwise bind static VBs.
      if (entry.promoted && sizeUnchanged) {
        bool anyChanged = (!posSame) || (!normSame) || (!colorSame) || (!texSame) || (!idxSame);
        if (!posSame) {
          m_backend.scheduleStaticCopy(entry.vbPos, entry.posOffset, posSlice, false);
        }
        if (!normSame) {
          m_backend.scheduleStaticCopy(entry.vbNorm, entry.normOffset, normSlice, false);
        }
        if (!colorSame) {
          m_backend.scheduleStaticCopy(entry.vbColor, entry.colorOffset, colorSlice, false);
        }
        if (texBytes > 0 && !texSame && entry.vbTex) {
          m_backend.scheduleStaticCopy(entry.vbTex, entry.texOffset, texSlice, false);
        }
        if (!idxSame && totalIndices > 0 && m_indexUploadBuffer) {
          Z3DRendererVulkanBackend::UploadSlice idxUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          totalIndices * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib, entry.indexOffset, idxUpload, true);
        }
        if (!anyChanged) {
          // Bind static slices (per-attribute VBs)
          m_posBuffer = entry.vbPos;
          m_normBuffer = entry.vbNorm;
          m_colorBuffer = entry.vbColor;
          m_texBuffer = entry.vbTex ? entry.vbTex : vk::Buffer{};
          m_posOffset = entry.posOffset;
          m_normOffset = entry.normOffset;
          m_colorOffset = entry.colorOffset;
          m_texOffset = entry.hasTex ? entry.texOffset : 0;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib;
            m_indexUploadOffset = entry.indexOffset;
          }
          return;
        }
        // Defer restaging to the next frame; keep upload slices bound.
        return;
      }

      // Consider promotion when stable for N frames
      if (!entry.promoted && sizeUnchanged && entry.unchangedFrames >= kPromotionThreshold) {
        auto posDst = m_backend.allocateStaticVB(posBytes, alignof(glm::vec3));
        auto normDst = m_backend.allocateStaticVB(normBytes, alignof(glm::vec3));
        auto colorDst = m_backend.allocateStaticVB(colorBytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice texDst{};
        bool haveTex = false;
        if (texBytes > 0) {
          texDst = m_backend.allocateStaticVB(texBytes, 4);
          haveTex = static_cast<bool>(texDst.buffer);
        }
        Z3DRendererVulkanBackend::StaticSlice idxDst{};
        if (totalIndices > 0) {
          idxDst = m_backend.allocateStaticIB(totalIndices * sizeof(uint32_t), alignof(uint32_t));
        }
        if (posDst.buffer && normDst.buffer && colorDst.buffer && (texBytes == 0 || haveTex) &&
            (totalIndices == 0 || idxDst.buffer)) {
          // Record copies and count bytes
          size_t staged = 0;
          m_backend.scheduleStaticCopy(posDst.buffer, posDst.offset, posSlice, false);
          staged += posBytes;
          m_backend.scheduleStaticCopy(normDst.buffer, normDst.offset, normSlice, false);
          staged += normBytes;
          m_backend.scheduleStaticCopy(colorDst.buffer, colorDst.offset, colorSlice, false);
          staged += colorBytes;
          if (texBytes > 0) {
            m_backend.scheduleStaticCopy(texDst.buffer, texDst.offset, texSlice, false);
            staged += texBytes;
          }
          if (totalIndices > 0) {
            Z3DRendererVulkanBackend::UploadSlice idxUpload{m_indexUploadBuffer,
                                                            m_indexUploadOffset,
                                                            nullptr,
                                                            totalIndices * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(idxDst.buffer, idxDst.offset, idxUpload, true);
            staged += totalIndices * sizeof(uint32_t);
          }
          if (staged > 0) {
            m_backend.addMeshBytesStaged(staged);
          }
          // Save cache entry (per-attribute buffers)
          entry.vbPos = posDst.buffer;
          entry.vbNorm = normDst.buffer;
          entry.vbColor = colorDst.buffer;
          entry.vbTex = texBytes > 0 ? texDst.buffer : vk::Buffer{};
          entry.posOffset = posDst.offset;
          entry.normOffset = normDst.offset;
          entry.colorOffset = colorDst.offset;
          entry.texOffset = texDst.offset;
          entry.hasTex = texBytes > 0 && haveTex;
          entry.ib = idxDst.buffer;
          entry.indexOffset = idxDst.offset;
          entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
          entry.indexCount = static_cast<uint32_t>(m_indexCount);
          entry.posGen = payload.posGen;
          entry.normGen = payload.normGen;
          entry.colorGen = payload.colorGen;
          entry.texGen = payload.texGen;
          entry.indexGen = payload.indexGen;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        } else {
          VLOG(2) << "Mesh static promotion skipped: arena out of space";
        }
      }
    }
  }
}

// No GL texture bridging in Vulkan mesh pipeline. Texture binding, if any,
// should be provided via backend-native resources. Placeholders are already
// bound in ensureDescriptorSets().

} // namespace nim
