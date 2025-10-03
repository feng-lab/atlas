#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <span>
#include <vector>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanTexture;
class ZVulkanBuffer;
class Z3DTransferFunction;

class ZVulkanImgRaycasterPipelineContext
{
public:
  explicit ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanImgRaycasterPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const ImgRaycasterPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct EntryVertex
  {
    glm::vec3 position{0.f};
    glm::vec3 texCoord{0.f};
  };

  struct ChannelResources
  {
    uint64_t volumeGeneration = 0;
    std::unique_ptr<ZVulkanTexture> volumeTexture;
    std::unique_ptr<ZVulkanTexture> transferTexture;
    std::unique_ptr<ZVulkanDescriptorSet> fastDescriptor;
    std::unique_ptr<ZVulkanDescriptorSet> rayParamDescriptor;
    std::unique_ptr<ZVulkanBuffer> rayParamBuffer;
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  Z3DRendererVulkanBackend& m_backend;

  // Entry/exit state
  std::unique_ptr<ZVulkanBuffer> m_entryVertexBuffer;
  std::unique_ptr<ZVulkanBuffer> m_entryIndexBuffer;
  size_t m_entryVertexCapacity = 0;
  size_t m_entryIndexCapacity = 0;

  // Screen quad
  std::unique_ptr<ZVulkanBuffer> m_quadVertexBuffer;
  size_t m_quadVertexCount = 0;

  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::optional<vk::raii::DescriptorSetLayout> m_entrySetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_fastSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_rayParamSetLayout;

  PipelineInstance m_entryFrontPipeline;
  PipelineInstance m_entryBackPipeline;
  PipelineInstance m_fastPipeline;

  std::vector<ChannelResources> m_channelResources;

  void ensureDescriptorPool();
  void ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount);
  void ensureQuadVertexBuffer();

  void ensureDescriptorLayouts();
  void ensureEntryPipelines();
  void ensureFastPipeline(ImgCompositingMode mode, bool resultOpaque);
  void uploadEntryGeometry(const ImgRaycasterPayload& payload);

  ChannelResources& ensureChannelResources(size_t channelIndex);
  void updateChannelFastDescriptors(ChannelResources& resources,
                                    const ImgRaycasterPayload& payload,
                                    size_t channelIndex,
                                    ZVulkanTexture& entryExitTexture,
                                    ZVulkanTexture& volumeTexture,
                                    ZVulkanTexture& transferTexture,
                                    float zeToZW_a,
                                    float zeToZW_b,
                                    const glm::vec3& volumeDimensions);

  ZVulkanTexture& ensureVolumeTexture(ChannelResources& resources,
                                      const ZImg& image,
                                      size_t channelIndex);
  ZVulkanTexture& ensureTransferTexture(ChannelResources& resources,
                                        const Z3DTransferFunction& transferFunction);

  void renderEntryExit(Z3DRendererBase& renderer,
                       const RenderBatch& batch,
                       const ImgRaycasterPayload& payload,
                       vk::raii::CommandBuffer& cmd);

  void renderFastPath(Z3DRendererBase& renderer,
                      const RenderBatch& batch,
                      const ImgRaycasterPayload& payload,
                      const vk::Viewport& viewport,
                      const vk::Rect2D& scissor,
                      vk::raii::CommandBuffer& cmd);
};

} // namespace nim
