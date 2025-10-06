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

vk::PipelineVertexInputStateCreateInfo makeMeshVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(MeshVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 6> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, normal))  },
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, color))   },
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 0,
                                        .format = vk::Format::eR32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex1d))   },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex2d))   },
    vk::VertexInputAttributeDescription{.location = 5,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex3d))   }
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
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

  uploadGeometry(payload);
  if (m_draws.empty() || m_vertexCount == 0) {
    return;
  }

  const bool pickingPass = payload.pickingPass;

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
  if (!pickingPass && m_dsTextures) {
    // Bind Vulkan-native texture if provided; otherwise placeholders remain bound.
    if (payload.textureHandle.valid() && payload.textureHandle.backend == AttachmentBackend::Vulkan) {
      auto& sampledTexture =
        vulkan::textureFromHandle(payload.textureHandle, m_backend.device(), "mesh payload sampled texture");
      switch (payload.colorSource) {
        case MeshPayload::ColorSource::Mesh1DTexture:
          m_dsTextures->updateTexture(0, sampledTexture);
          break;
        case MeshPayload::ColorSource::Mesh2DTexture:
          m_dsTextures->updateTexture(1, sampledTexture);
          break;
        case MeshPayload::ColorSource::Mesh3DTexture:
          m_dsTextures->updateTexture(2, sampledTexture);
          break;
        default:
          break;
      }
    }
  }

  const auto shaderHook = renderer.shaderHookType();
  // Descriptor set to bind for texture set (0). For DDP peel we allocate a per-draw override to
  // avoid mutating a set that may have been bound earlier in the same command buffer.
  std::unique_ptr<ZVulkanDescriptorSet> dsTexturesOverride;

  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const auto& hookPara = renderer.shaderHookPara();
    auto* layouts = m_setTextures ? &*m_setTextures : nullptr;
    if (layouts) {
      dsTexturesOverride = m_backend.allocateFrameDescriptorSet(**m_setTextures);
    }
    auto* dst = dsTexturesOverride ? dsTexturesOverride.get() : m_dsTextures.get();
    if (dst) {
      // For DDP peel, bind depth/front blender textures at bindings 0 and 1 to match shader expectations.
      // Fill unused slots with placeholders as needed.
      if (m_placeholder1D && m_placeholder2D && m_placeholder3D) {
        // Placeholders for non-used bindings to keep layout stable
        dst->updateTexture(2, *m_placeholder3D);
      }
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                       m_backend.device(),
                                                       "mesh dual-depth-peeling depth blender");
        dst->updateTexture(0, depthTexture, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTexture = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                       m_backend.device(),
                                                       "mesh dual-depth-peeling front blender");
        dst->updateTexture(1, frontTexture, m_backend.defaultSampler());
      }
    }
    if (dsTexturesOverride) {
      // Keep alive for the rest of the frame
      m_transientDescriptorSets.push_back(std::move(dsTexturesOverride));
    }
  } else if (m_dsTextures && m_placeholder2D) {
    m_dsTextures->updateTexture(3, *m_placeholder2D);
    m_dsTextures->updateTexture(4, *m_placeholder2D);
  }

  vk::DeviceSize vertexOffset = 0;
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {vertexOffset});
  if (m_indexCount > 0 && m_indexBuffer) {
    cmd.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const bool drawSurface = payload.wireframeMode != MeshPayload::WireframeMode::OnlyWireframe;
  const bool drawWireframe = payload.wireframeMode != MeshPayload::WireframeMode::NoWireframe;

  const FogMode fogMode = renderer.sceneState().fog.mode;

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);

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
        // Initialize default textures set if needed (and not using override)
        if (!dsTexturesOverride && m_dsTextures && !m_texturesSetInitialized &&
            m_placeholder1D && m_placeholder2D && m_placeholder3D) {
          m_dsTextures->updateTexture(0, *m_placeholder1D);
          m_dsTextures->updateTexture(1, *m_placeholder2D);
          m_dsTextures->updateTexture(2, *m_placeholder3D);
          m_dsTextures->updateTexture(3, *m_placeholder2D);
          m_dsTextures->updateTexture(4, *m_placeholder2D);
          m_texturesSetInitialized = true;
        }
        // Bind descriptor sets with optional textures override for set 0
        if (m_dsLighting && m_dsTransforms) {
          vk::DescriptorSet texturesSet = dsTexturesOverride ? dsTexturesOverride->descriptorSet()
                                                            : (m_dsTextures ? m_dsTextures->descriptorSet()
                                                                           : vk::DescriptorSet{});
          if (texturesSet) {
            std::array<vk::DescriptorSet, 3> sets{texturesSet,
                                                  m_dsLighting->descriptorSet(),
                                                  m_dsTransforms->descriptorSet()};
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   pipeline.pipeline->pipelineLayout(),
                                   0,
                                   sets,
                                   {});
            if (m_dsOIT) {
              std::array<vk::DescriptorSet, 1> sets3{m_dsOIT->descriptorSet()};
              cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     pipeline.pipeline->pipelineLayout(),
                                     3,
                                     sets3,
                                     {});
            }
          } else {
            bindDescriptorSets(cmd, pipeline);
          }
        } else {
          bindDescriptorSets(cmd, pipeline);
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

      if (draw.indexed && draw.indexCount > 0 && m_indexBuffer) {
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
        // Initialize default textures set if needed for wireframe path
        if (m_dsTextures && !m_texturesSetInitialized && m_placeholder1D && m_placeholder2D && m_placeholder3D) {
          m_dsTextures->updateTexture(0, *m_placeholder1D);
          m_dsTextures->updateTexture(1, *m_placeholder2D);
          m_dsTextures->updateTexture(2, *m_placeholder3D);
          m_dsTextures->updateTexture(3, *m_placeholder2D);
          m_dsTextures->updateTexture(4, *m_placeholder2D);
          m_texturesSetInitialized = true;
        }
        bindDescriptorSets(cmd, pipeline);
        currentPipeline = &pipeline;
      }

      const glm::vec4 wireColor = pickingPass ? draw.fallbackColor : payload.wireframeColor;
      updateMaterialUBO(renderer, payload, draw.payloadMeshIndex, true, wireColor, pickingPass);

      if (draw.indexed && draw.indexCount > 0 && m_indexBuffer) {
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

  if (!m_dsTextures) m_dsTextures = m_backend.allocateFrameDescriptorSet(**m_setTextures);
  if (!m_dsLighting) m_dsLighting = m_backend.allocateFrameDescriptorSet(**m_setLighting);
  if (!m_dsTransforms) m_dsTransforms = m_backend.allocateFrameDescriptorSet(**m_setTransforms);
  if (!m_dsOIT && m_setOIT) m_dsOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->updateUniformBuffer(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->updateUniformBuffer(0, *m_uboTransforms);
    m_dsTransforms->updateUniformBuffer(1, *m_uboMaterial);
  }
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->updateUniformBuffer(0, *m_uboOIT);
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
                                                    const PipelineInstance& pipeline) const
{
  if (!m_dsTextures || !m_dsLighting || !m_dsTransforms) {
    return;
  }

  std::array<vk::DescriptorSet, 3> sets{m_dsTextures->descriptorSet(),
                                        m_dsLighting->descriptorSet(),
                                        m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  if (m_dsOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_dsOIT->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 3, sets3, {});
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

  auto vertexInput = makeMeshVertexInput();
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
        state.blendEnable = VK_TRUE;
        state.srcColorBlendFactor = vk::BlendFactor::eOne;
        state.dstColorBlendFactor = vk::BlendFactor::eOne;
        state.colorBlendOp = vk::BlendOp::eAdd;
        state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        state.alphaBlendOp = vk::BlendOp::eAdd;
        blendAttachments.push_back(state);
      }
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

void ZVulkanMeshPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(MeshVertex);
  if (requiredBytes <= m_vertexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_vertexCapacity == 0 ? requiredBytes : m_vertexCapacity * 2);
  auto& device = m_backend.device();
  m_vertexBuffer =
    device.createBuffer(newCapacity,
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexCapacity = newCapacity;
}

void ZVulkanMeshPipelineContext::ensureIndexCapacity(size_t indexCount)
{
  const size_t requiredBytes = indexCount * sizeof(uint32_t);
  if (requiredBytes <= m_indexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_indexCapacity == 0 ? requiredBytes : m_indexCapacity * 2);
  auto& device = m_backend.device();
  m_indexBuffer =
    device.createBuffer(newCapacity,
                        vk::BufferUsageFlagBits::eIndexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_indexCapacity = newCapacity;
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

  ensureVertexCapacity(totalVertices);
  if (totalIndices > 0) {
    ensureIndexCapacity(totalIndices);
  }

  auto vertexMapping = m_vertexBuffer->mapRange(0, totalVertices * sizeof(MeshVertex));
  auto* vertexPtr = vertexMapping.as<MeshVertex>();
  if (!vertexPtr) {
    throw ZException("Failed to map mesh vertex buffer");
  }

  ZVulkanBuffer::ScopedMap indexMapping;
  uint32_t* indexPtr = nullptr;
  if (totalIndices > 0 && m_indexBuffer) {
    indexMapping = m_indexBuffer->mapRange(0, totalIndices * sizeof(uint32_t));
    indexPtr = indexMapping.as<uint32_t>();
    if (!indexPtr) {
      throw ZException("Failed to map mesh index buffer");
    }
  }

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

    for (size_t v = 0; v < mesh->numVertices(); ++v) {
      MeshVertex& dst = vertexPtr[vertexCursor + v];
      dst.position = positions[v];
      dst.normal = normals.size() > v ? normals[v] : glm::vec3(0.0f, 0.0f, 1.0f);
      if (hasVertexColors) {
        dst.color = colors[v];
      } else {
        dst.color = glm::vec4(1.0f);
      }

      if (payload.colorSource == MeshPayload::ColorSource::Mesh1DTexture && tex1D.size() > v) {
        dst.tex1d = tex1D[v];
      }
      if (payload.colorSource == MeshPayload::ColorSource::Mesh2DTexture && tex2D.size() > v) {
        dst.tex2d = tex2D[v];
      }
      if (payload.colorSource == MeshPayload::ColorSource::Mesh3DTexture && tex3D.size() > v) {
        dst.tex3d = tex3D[v];
      }
    }

    vertexCursor += mesh->numVertices();

    const auto& indices = mesh->indices();
    if (!indices.empty() && indexPtr) {
      draw.indexed = true;
      draw.firstIndex = static_cast<uint32_t>(indexCursor);
      draw.indexCount = static_cast<uint32_t>(indices.size());
      for (size_t idx = 0; idx < indices.size(); ++idx) {
        indexPtr[indexCursor + idx] = static_cast<uint32_t>(indices[idx]) + draw.firstVertex;
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
}

// No GL texture bridging in Vulkan mesh pipeline. Texture binding, if any,
// should be provided via backend-native resources. Placeholders are already
// bound in ensureDescriptorSets().

} // namespace nim
