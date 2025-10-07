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
#include "zvulkandescriptorpool.h"
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

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

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

vk::PipelineVertexInputStateCreateInfo makeSoAMeshVertexInput(MeshPayload::ColorSource colorSource)
{
  static std::array<vk::VertexInputBindingDescription, 4> bindings{};
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

  static std::array<vk::VertexInputAttributeDescription, 4> attrs{};
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

  uint32_t bindingCount = 3;
  uint32_t attrCount = 3;
  switch (colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      bindings[3] = vk::VertexInputBindingDescription{.binding = 3,
                                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                                      .inputRate = vk::VertexInputRate::eVertex};
      attrs[3] =
        vk::VertexInputAttributeDescription{.location = 3, .binding = 3, .format = vk::Format::eR32Sfloat, .offset = 0};
      bindingCount = 4;
      attrCount = 4;
      break;
    case MeshPayload::ColorSource::Mesh2DTexture:
      bindings[3] = vk::VertexInputBindingDescription{.binding = 3,
                                                      .stride = static_cast<uint32_t>(sizeof(glm::vec2)),
                                                      .inputRate = vk::VertexInputRate::eVertex};
      attrs[3] = vk::VertexInputAttributeDescription{.location = 4,
                                                     .binding = 3,
                                                     .format = vk::Format::eR32G32Sfloat,
                                                     .offset = 0};
      bindingCount = 4;
      attrCount = 4;
      break;
    case MeshPayload::ColorSource::Mesh3DTexture:
      bindings[3] = vk::VertexInputBindingDescription{.binding = 3,
                                                      .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                      .inputRate = vk::VertexInputRate::eVertex};
      attrs[3] = vk::VertexInputAttributeDescription{.location = 5,
                                                     .binding = 3,
                                                     .format = vk::Format::eR32G32B32Sfloat,
                                                     .offset = 0};
      bindingCount = 4;
      attrCount = 4;
      break;
    case MeshPayload::ColorSource::CustomColor:
    case MeshPayload::ColorSource::MeshColor:
    default:
      break;
  }

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
  m_posBuffer = VK_NULL_HANDLE;
  m_normBuffer = VK_NULL_HANDLE;
  m_colorBuffer = VK_NULL_HANDLE;
  m_texBuffer = VK_NULL_HANDLE;
  m_posOffset = m_normOffset = m_colorOffset = m_texOffset = 0;
  m_indexUploadBuffer = VK_NULL_HANDLE;
  m_indexUploadOffset = 0;
  m_texBinding = TexBinding::None;
  resetDescriptors();
  m_transientDescriptorSets.clear();
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
  if (!payload.renderer || payload.meshes.empty()) {
    return;
  }

  // GL parity: for picking pass, require per-mesh picking colors; otherwise skip.
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

  updateLightingUBO(renderer, batch, payload, pickingPass);
  updateTransformUBO(renderer, batch);
  // Ensure OIT params UBO for shaders including include/oit_params.glslinc
  ensureOITResources();
  {
    glm::vec2 extent = batch.pass.viewport.extent;
    if (extent.x <= 0.0f || extent.y <= 0.0f) {
      const auto& viewportState = renderer.frameState().viewport;
      extent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
    }
    glm::vec2 screenRcp =
      (extent.x > 0.0f && extent.y > 0.0f) ? glm::vec2(1.0f / extent.x, 1.0f / extent.y) : glm::vec2(0.0f);
    updateOITParamsUBO(renderer, batch, screenRcp);
  }
  ensureDescriptorSets();
  CHECK(m_dsLighting && m_dsTransforms) << "Mesh pipeline descriptor sets missing (lighting/transforms)";
  // Textures set should be available for normal passes; DDP peel enforces override below.
  if (renderer.shaderHookType() != Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    CHECK(m_dsTextures != nullptr) << "Mesh pipeline textures descriptor set not initialised";
  }
  // Build a per-draw textures descriptor set to avoid any update-after-bind hazards.
  vk::DescriptorSet boundTexturesOverride{};
  if (m_setTextures) {
    if (auto* drawTex = m_backend.allocateOverrideDescriptorSet(**m_setTextures)) {
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
                 payload.textureHandle.backend == AttachmentBackend::Vulkan) {
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
      boundTexturesOverride = drawTex->descriptorSet();
    }
  }
  // If DDP peel is requested but we could not allocate the per-draw override,
  // skip to avoid mutating/binding a shared set.
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    CHECK(boundTexturesOverride) << "Mesh DDP peel: override descriptor allocation failed (fatal)";
  }

  // Descriptor set to bind for texture set (0). For DDP peel we allocate a per-draw override to
  // avoid mutating a set that may have been bound earlier in the same command buffer.
  std::unique_ptr<ZVulkanDescriptorSet> dsTexturesOverride; // legacy path, now unused

  // DDP peel handled by the per-draw override above.

  // Bind SoA streams; order must match makeSoAMeshVertexInput()
  std::vector<vk::Buffer> buffers;
  std::vector<vk::DeviceSize> offsets;
  buffers.push_back(m_posBuffer);
  offsets.push_back(m_posOffset); // binding 0: positions
  buffers.push_back(m_normBuffer);
  offsets.push_back(m_normOffset); // binding 1: normals
  buffers.push_back(m_colorBuffer);
  offsets.push_back(m_colorOffset); // binding 2: colors
  if (m_texBinding != TexBinding::None) {
    buffers.push_back(m_texBuffer);
    offsets.push_back(m_texOffset); // binding 3: tex
  }
  cmd.bindVertexBuffers(0, buffers, offsets);
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    cmd.bindIndexBuffer(m_indexUploadBuffer, m_indexUploadOffset, vk::IndexType::eUint32);
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const bool drawSurface = payload.wireframeMode != MeshPayload::WireframeMode::OnlyWireframe;
  const bool drawWireframe = payload.wireframeMode != MeshPayload::WireframeMode::NoWireframe;

  const FogMode fogMode = renderer.sceneState().fog.mode;

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  if (!m_backend.validateFormatsOrSkip(formats, "mesh")) {
    return;
  }

  PipelineInstance* currentPipeline = nullptr;
  if (drawSurface) {
    for (const auto& draw : m_draws) {
      if (!draw.mesh) {
        continue;
      }

      PipelineKey key;
      key.colorSource = payload.colorSource;
      key.meshType = draw.mesh->type();
      key.wireframe = false;
      key.fogMode = fogMode;
      key.shaderHookType = shaderHook;

      key.colorFormats = formats.colorFormats;
      key.depthFormat = formats.depthFormat;

      PipelineInstance& pipeline = ensurePipeline(key, formats);
      if (&pipeline != currentPipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
        // Dynamic vertex input (VK_EXT_vertex_input_dynamic_state)
        if (m_backend.device().supportsVertexInputDynamicState()) {
          std::vector<vk::VertexInputBindingDescription2EXT> bindings;
          std::vector<vk::VertexInputAttributeDescription2EXT> attrs;
          // binding 0: position
          bindings.emplace_back(
            vk::VertexInputBindingDescription2EXT{.binding = 0,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex,
                                                  .divisor = 1});
          attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 0,
                                                                     .binding = 0,
                                                                     .format = vk::Format::eR32G32B32Sfloat,
                                                                     .offset = 0});
          // binding 1: normal
          bindings.emplace_back(
            vk::VertexInputBindingDescription2EXT{.binding = 1,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                  .inputRate = vk::VertexInputRate::eVertex,
                                                  .divisor = 1});
          attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 1,
                                                                     .binding = 1,
                                                                     .format = vk::Format::eR32G32B32Sfloat,
                                                                     .offset = 0});
          // binding 2: color
          bindings.emplace_back(
            vk::VertexInputBindingDescription2EXT{.binding = 2,
                                                  .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                                  .inputRate = vk::VertexInputRate::eVertex,
                                                  .divisor = 1});
          attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 2,
                                                                     .binding = 2,
                                                                     .format = vk::Format::eR32G32B32A32Sfloat,
                                                                     .offset = 0});
          // binding 3: tex (optional)
          if (m_texBinding == TexBinding::Tex1D) {
            bindings.emplace_back(vk::VertexInputBindingDescription2EXT{.binding = 3,
                                                                        .stride = static_cast<uint32_t>(sizeof(float)),
                                                                        .inputRate = vk::VertexInputRate::eVertex,
                                                                        .divisor = 1});
            attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 3,
                                                                       .binding = 3,
                                                                       .format = vk::Format::eR32Sfloat,
                                                                       .offset = 0});
          } else if (m_texBinding == TexBinding::Tex2D) {
            bindings.emplace_back(
              vk::VertexInputBindingDescription2EXT{.binding = 3,
                                                    .stride = static_cast<uint32_t>(sizeof(glm::vec2)),
                                                    .inputRate = vk::VertexInputRate::eVertex,
                                                    .divisor = 1});
            attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 4,
                                                                       .binding = 3,
                                                                       .format = vk::Format::eR32G32Sfloat,
                                                                       .offset = 0});
          } else if (m_texBinding == TexBinding::Tex3D) {
            bindings.emplace_back(
              vk::VertexInputBindingDescription2EXT{.binding = 3,
                                                    .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                    .inputRate = vk::VertexInputRate::eVertex,
                                                    .divisor = 1});
            attrs.emplace_back(vk::VertexInputAttributeDescription2EXT{.location = 5,
                                                                       .binding = 3,
                                                                       .format = vk::Format::eR32G32B32Sfloat,
                                                                       .offset = 0});
          }
          cmd.setVertexInputEXT(bindings, attrs);
        }
        // Bind descriptor sets with optional textures override for set 0
        if (m_dsLighting && m_dsTransforms) {
          // For DDP peel require override; otherwise fall back to the shared set.
          vk::DescriptorSet texturesSet =
            boundTexturesOverride ? boundTexturesOverride
                                  : (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel
                                       ? vk::DescriptorSet{}
                                       : (m_dsTextures ? m_dsTextures->descriptorSet() : vk::DescriptorSet{}));
          if (texturesSet) {
            std::array<vk::DescriptorSet, 3> sets{texturesSet,
                                                  m_dsLighting->descriptorSet(),
                                                  m_dsTransforms->descriptorSet()};
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
          } else {
            // If we cannot bind textures set (e.g., DDP override unavailable), skip pipeline bind
            // to avoid binding stale/shared sets.
            return;
          }
        } else {
          return;
        }

        if (shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
          const float n = renderer.viewState().nearClip;
          const float f = renderer.viewState().farClip;
          const float a = (f * n) / std::max(f - n, 1e-6f);
          const float b = 0.5f * (f + n) / std::max(f - n, 1e-6f) + 0.5f;
          const float depthScale = renderer.sceneState().weightedBlendedDepthScale;
          glm::vec4 pushConstants(a, b, depthScale, 0.0f);
          cmd.pushConstants<glm::vec4>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       pushConstants);
        }
        currentPipeline = &pipeline;
      }

      updateMaterialUBO(renderer,
                        payload,
                        draw.payloadMeshIndex,
                        draw.useFallbackColor,
                        draw.fallbackColor,
                        pickingPass);

      if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
        cmd.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
      } else {
        cmd.draw(draw.vertexCount, 1, draw.firstVertex, 0);
      }
    }
  }

  if (drawWireframe) {
    currentPipeline = nullptr;
    for (const auto& draw : m_draws) {
      if (!draw.mesh) {
        continue;
      }

      PipelineKey key;
      key.colorSource = payload.colorSource;
      key.meshType = draw.mesh->type();
      key.wireframe = true;
      key.fogMode = fogMode;
      key.shaderHookType = shaderHook;

      key.colorFormats = formats.colorFormats;
      key.depthFormat = formats.depthFormat;

      PipelineInstance& pipeline = ensurePipeline(key, formats);
      if (&pipeline != currentPipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
        bindDescriptorSets(cmd, pipeline);
        currentPipeline = &pipeline;
      }

      const glm::vec4 wireColor = pickingPass ? draw.fallbackColor : payload.wireframeColor;
      updateMaterialUBO(renderer, payload, draw.payloadMeshIndex, true, wireColor, pickingPass);

      if (draw.indexed && draw.indexCount > 0 && m_indexUploadBuffer) {
        cmd.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
      } else {
        cmd.draw(draw.vertexCount, 1, draw.firstVertex, 0);
      }
    }
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setTextures) {
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 4,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setTextures.emplace(vkDevice, createInfo);
  }

  if (!m_setLighting) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setLighting.emplace(vkDevice, createInfo);
  }

  if (!m_setTransforms) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setTransforms.emplace(vkDevice, createInfo);
  }

  if (!m_setOIT) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setOIT.emplace(vkDevice, createInfo);
  }
}

void ZVulkanMeshPipelineContext::ensurePlaceholderTextures()
{
  auto& device = m_backend.device();

  if (!m_placeholder1D) {
    auto info =
      ZVulkanTexture::CreateInfo::make1D(1,
                                         vk::Format::eR8G8B8A8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder1D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder1D->uploadData(&pixel, sizeof(pixel));
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
    m_placeholder2D->uploadData(&pixel, sizeof(pixel));
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
    m_placeholder3D->uploadData(&pixel, sizeof(pixel));
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();
  ensurePlaceholderTextures();

  if (!m_dsTextures) {
    m_dsTextures = m_backend.allocateFrameDescriptorSet(**m_setTextures);
  }
  if (!m_dsLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(**m_setLighting);
  }
  if (!m_dsTransforms) {
    m_dsTransforms = m_backend.allocateFrameDescriptorSet(**m_setTransforms);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }

  // Ensure UBO buffers exist before recording
  auto& device = m_backend.device();
  if (!m_uboLighting) {
    m_uboLighting =
      device.createBuffer(sizeof(LightingUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboTransforms) {
    m_uboTransforms =
      device.createBuffer(sizeof(TransformsUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboMaterial) {
    m_uboMaterial =
      device.createBuffer(sizeof(MaterialUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->writeUniformBufferOnce(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->writeUniformBufferOnce(0, *m_uboTransforms);
    m_dsTransforms->writeUniformBufferOnce(1, *m_uboMaterial);
  }
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
  }
}

void ZVulkanMeshPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
}

void ZVulkanMeshPipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const glm::vec2& fallbackScreenDimRcp)
{
  (void)batch;
  if (!m_uboOIT) {
    return;
  }
  OITParamsUBOStd140 oit{};
  oit.screen_dim_RCP = fallbackScreenDimRcp;
  const float n = renderer.viewState().nearClip;
  const float f = renderer.viewState().farClip;
  const float denom = std::max(f - n, 1e-6f);
  oit.ze_to_zw_a = (f * n) / denom;
  oit.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;
  oit.weighted_blended_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
  m_uboOIT->copyData(&oit, sizeof(oit));
}

void ZVulkanMeshPipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const MeshPayload& payload,
                                                   bool pickingPass)
{
  auto& device = m_backend.device();

  if (!m_uboLighting) {
    m_uboLighting =
      device.createBuffer(sizeof(LightingUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  LightingUBOStd140 lighting{};
  const auto& scene = renderer.sceneState();

  size_t availableLights = scene.lighting.positions.size();
  availableLights = std::min(availableLights, scene.lighting.ambient.size());
  availableLights = std::min(availableLights, scene.lighting.diffuse.size());
  availableLights = std::min(availableLights, scene.lighting.specular.size());
  availableLights = std::min(availableLights, scene.lighting.attenuation.size());
  availableLights = std::min(availableLights, scene.lighting.spotCutoff.size());
  availableLights = std::min(availableLights, scene.lighting.spotExponent.size());
  availableLights = std::min(availableLights, scene.lighting.spotDirection.size());
  availableLights = std::min(availableLights, static_cast<size_t>(scene.lighting.lightCount));

  lighting.numLights = static_cast<int>(std::min(availableLights, lighting.lights.size()));
  const bool enableLighting =
    !pickingPass && lighting.numLights > 0 && payload.renderer && payload.renderer->needLighting();
  lighting.lighting_enabled = enableLighting ? 1 : 0;

  const glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x > 0.0f && extent.y > 0.0f) {
    lighting.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  } else {
    const auto& viewport = renderer.frameState().viewport;
    if (viewport.z > 0 && viewport.w > 0) {
      lighting.screen_dim_RCP = glm::vec2(1.0f / viewport.z, 1.0f / viewport.w);
    }
  }

  lighting.fog_color_top = scene.fog.topColor;
  lighting.fog_color_bottom = scene.fog.bottomColor;
  lighting.fog_end = scene.fog.range.y;
  lighting.fog_scale =
    scene.fog.range.y > scene.fog.range.x ? 1.0f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f) : 0.0f;
  constexpr float kLog2e = 1.44269504088896340735992468100189214f;
  lighting.fog_density_log2e = scene.fog.density * kLog2e;
  lighting.fog_density_density_log2e = scene.fog.density * scene.fog.density * kLog2e;

  for (int i = 0; i < lighting.numLights; ++i) {
    const size_t idx = static_cast<size_t>(i);
    lighting.lights[i].position = scene.lighting.positions[idx];
    lighting.lights[i].ambient = scene.lighting.ambient[idx];
    lighting.lights[i].diffuse = scene.lighting.diffuse[idx];
    lighting.lights[i].specular = scene.lighting.specular[idx];
    lighting.lights[i].attenuation = scene.lighting.attenuation[idx];
    lighting.lights[i].spotCutoff = scene.lighting.spotCutoff[idx];
    lighting.lights[i].spotExponent = scene.lighting.spotExponent[idx];
    lighting.lights[i].spotDirection = scene.lighting.spotDirection[idx];
  }

  m_uboLighting->copyData(&lighting, sizeof(lighting));
}

void ZVulkanMeshPipelineContext::updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch)
{
  auto& device = m_backend.device();

  if (!m_uboTransforms) {
    m_uboTransforms =
      device.createBuffer(sizeof(TransformsUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboMaterial) {
    m_uboMaterial =
      device.createBuffer(sizeof(MaterialUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.pos_transform = renderer.parameterState().coordTransform;

  const glm::mat4 combined = eyeState.viewMatrix * transforms.pos_transform;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const auto& params = renderer.parameterState();
  transforms.parameters = glm::vec4(params.sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);

  m_uboTransforms->copyData(&transforms, sizeof(transforms));

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = params.materialSpecular;
  material.material_shininess = params.materialShininess;
  material.alpha = params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);
  m_uboMaterial->copyData(&material, sizeof(material));
}

void ZVulkanMeshPipelineContext::updateMaterialUBO(Z3DRendererBase& renderer,
                                                   const MeshPayload& payload,
                                                   size_t meshIndex,
                                                   bool useFallbackColor,
                                                   const glm::vec4& fallbackColor,
                                                   bool pickingPass)
{
  if (!m_uboMaterial) {
    return;
  }

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  const auto& params = renderer.parameterState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = params.materialSpecular;
  material.material_shininess = params.materialShininess;
  material.alpha = params.opacity;

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

  m_uboMaterial->copyData(&material, sizeof(material));
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
  std::vector<vk::DescriptorSetLayout> layouts{**m_setTextures, **m_setLighting, **m_setTransforms, **m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);

  auto makeDefaultBlendAttachment = []() {
    vk::PipelineColorBlendAttachmentState state{};
    state.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    state.blendEnable = VK_FALSE;
    return state;
  };

  std::vector<vk::PipelineColorBlendAttachmentState> blendAttachments;
  blendAttachments.reserve(formats.colorFormats.size());

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = makeDefaultBlendAttachment();
        state.blendEnable = VK_TRUE;
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
        state.blendEnable = VK_TRUE;
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
        // Attachments layout (see makeDualDepthPeelDescriptor):
        // 0: RG32F depth blender ping A (MAX), 1: RGBA16 front blender ping A (MAX),
        // 2: RGBA16 back temp ping A (ADD), 3: RG32F depth blender ping B (MAX),
        // 4: RGBA16 front blender ping B (MAX), 5: RGBA16 back temp ping B (ADD),
        // 6: RGBA16 back blend accumulation (unused here), 7: R32F depth texture (unused here)
        if (i == 0 || i == 3) {
          // Depth blender: keep max of (-z, z)
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          // Front blender: monotonically increasing; use MAX for pass-through behavior
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          // Back temp: accumulate within the pass
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          // Unused in these passes
          state.blendEnable = VK_FALSE;
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

  if (payload.colorSource == MeshPayload::ColorSource::CustomColor &&
      payload.meshColors.size() < payload.meshes.size()) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan mesh backend skipping batch: custom color array is incomplete.";
    return;
  }

  size_t totalVertices = 0;
  size_t totalIndices = 0;

  for (size_t i = 0; i < payload.meshes.size(); ++i) {
    ZMesh* mesh = payload.meshes[i];
    if (!mesh) {
      continue;
    }

    if (!payload.pickingPass && !validateTexturePrerequisites(payload, *mesh)) {
      LOG_FIRST_N(WARNING, 5) << "Vulkan mesh backend skipping batch: texture prerequisites not met.";
      m_draws.clear();
      return;
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
  if (payload.renderer) {
    CacheKey key{payload.renderer, payload.colorSource, payload.pickingPass};
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

      // If promoted and sizes match, restage changed streams selectively
      if (entry.promoted && sizeUnchanged) {
        size_t restagedBytes = 0;
        if (!posSame) {
          m_backend.stageCopy(entry.vb, entry.posOffset, posSlice, false);
          entry.posGen = payload.posGen;
          restagedBytes += posBytes;
        }
        if (!normSame) {
          m_backend.stageCopy(entry.vb, entry.normOffset, normSlice, false);
          entry.normGen = payload.normGen;
          restagedBytes += normBytes;
        }
        if (!colorSame) {
          m_backend.stageCopy(entry.vb, entry.colorOffset, colorSlice, false);
          entry.colorGen = payload.colorGen;
          restagedBytes += colorBytes;
        }
        if (texBytes > 0 && !texSame) {
          m_backend.stageCopy(entry.vb, entry.texOffset, texSlice, false);
          entry.texGen = payload.texGen;
          restagedBytes += texBytes;
        }
        if (!idxSame && totalIndices > 0 && m_indexUploadBuffer) {
          Z3DRendererVulkanBackend::UploadSlice idxUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          totalIndices * sizeof(uint32_t)};
          m_backend.stageCopy(entry.ib, entry.indexOffset, idxUpload, true);
          entry.indexGen = payload.indexGen;
          restagedBytes += totalIndices * sizeof(uint32_t);
        }
        if (restagedBytes > 0) {
          m_backend.addMeshBytesStaged(restagedBytes);
        }
        // Bind static slices
        // All attribute streams reside in the same static VB
        m_posBuffer = entry.vb;
        m_normBuffer = entry.vb;
        m_colorBuffer = entry.vb;
        m_texBuffer = entry.vb;
        m_posOffset = entry.posOffset;
        m_normOffset = entry.normOffset;
        m_colorOffset = entry.colorOffset;
        m_texOffset = entry.hasTex ? entry.texOffset : 0;
        if (entry.indexCount > 0 && entry.ib) {
          m_indexUploadBuffer = entry.ib;
          m_indexUploadOffset = entry.indexOffset;
        }
        return; // Done; draws will bind the static buffers
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
          haveTex = texDst.buffer != VK_NULL_HANDLE;
        }
        Z3DRendererVulkanBackend::StaticSlice idxDst{};
        if (totalIndices > 0) {
          idxDst = m_backend.allocateStaticIB(totalIndices * sizeof(uint32_t), alignof(uint32_t));
        }
        if (posDst.buffer && normDst.buffer && colorDst.buffer && (texBytes == 0 || haveTex) &&
            (totalIndices == 0 || idxDst.buffer)) {
          // Record copies and count bytes
          size_t staged = 0;
          m_backend.stageCopy(posDst.buffer, posDst.offset, posSlice, false);
          staged += posBytes;
          m_backend.stageCopy(normDst.buffer, normDst.offset, normSlice, false);
          staged += normBytes;
          m_backend.stageCopy(colorDst.buffer, colorDst.offset, colorSlice, false);
          staged += colorBytes;
          if (texBytes > 0) {
            m_backend.stageCopy(texDst.buffer, texDst.offset, texSlice, false);
            staged += texBytes;
          }
          if (totalIndices > 0) {
            Z3DRendererVulkanBackend::UploadSlice idxUpload{m_indexUploadBuffer,
                                                            m_indexUploadOffset,
                                                            nullptr,
                                                            totalIndices * sizeof(uint32_t)};
            m_backend.stageCopy(idxDst.buffer, idxDst.offset, idxUpload, true);
            staged += totalIndices * sizeof(uint32_t);
          }
          if (staged > 0) {
            m_backend.addMeshBytesStaged(staged);
          }
          // Save cache entry
          entry.vb = posDst.buffer; // same buffer for all VB allocs
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

          // Bind static slices immediately
          m_posBuffer = entry.vb;
          m_normBuffer = entry.vb;
          m_colorBuffer = entry.vb;
          m_texBuffer = entry.vb;
          m_posOffset = entry.posOffset;
          m_normOffset = entry.normOffset;
          m_colorOffset = entry.colorOffset;
          m_texOffset = entry.hasTex ? entry.texOffset : 0;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib;
            m_indexUploadOffset = entry.indexOffset;
          }
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
