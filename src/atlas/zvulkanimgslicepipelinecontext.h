#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;
class ZMesh;
class ZImg;
class Z3DImg;
class ZColorMapParameter;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanImgSlicePipelineContext
{
public:
  explicit ZVulkanImgSlicePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanImgSlicePipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const ImgSlicePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct SliceVertex
  {
    glm::vec3 position{0.0f};
    glm::vec3 texCoord{0.0f};
  };

  struct SlicePipelineKey
  {
    bool validInput = true;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(validInput, colorFormats, depthFormat);
    }

    bool operator<(const SlicePipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct MergePipelineKey
  {
    int numVolumes = 1;
    bool maxProjectionMerge = true;
    bool resultOpaque = false;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(numVolumes, maxProjectionMerge, resultOpaque, colorFormats, depthFormat);
    }

    bool operator<(const MergePipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  struct ChannelResources
  {
    uint64_t volumeGeneration = 0;
    std::unique_ptr<ZVulkanTexture> volumeTexture;
    std::unique_ptr<ZVulkanTexture> colormapTexture;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<SlicePipelineKey, PipelineInstance> m_slicePipelines;
  std::map<MergePipelineKey, PipelineInstance> m_mergePipelines;

  std::optional<vk::raii::DescriptorSetLayout> m_sliceSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_mergeSetLayout;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_sliceDescriptor;
  std::unique_ptr<ZVulkanDescriptorSet> m_mergeDescriptor;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;

  std::unique_ptr<ZVulkanBuffer> m_quadVertexBuffer;
  size_t m_quadVertexCapacity = 0;
  size_t m_quadVertexCount = 0;
  std::vector<ChannelResources> m_channelResources;

  void ensureDescriptorLayouts();
  void ensureDescriptorPool();
  vk::PipelineVertexInputStateCreateInfo makeSliceVertexInputState() const;
  vk::PipelineVertexInputStateCreateInfo makeQuadVertexInputState() const;
  void ensureSliceVertexCapacity(size_t vertexCount);
  void ensureQuadVertexBuffer();
  void uploadSliceGeometry(std::span<const ZMesh> slices);
  ZVulkanTexture& ensureVolumeTexture(size_t channel,
                                      uint64_t generation,
                                      const ZImg& image,
                                      ChannelResources& resources);
  ZVulkanTexture& ensureColormapTexture(size_t channel,
                                        const ZColorMapParameter* parameter,
                                        ChannelResources& resources);
  void transitionToSampled(vk::raii::CommandBuffer& cmd, ZVulkanTexture& texture, vk::ImageLayout desiredLayout);

  PipelineInstance& ensureSlicePipeline(const SlicePipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureMergePipeline(const MergePipelineKey& key, const vulkan::AttachmentFormats& formats);

  void bindSliceDescriptor(ZVulkanTexture& volume, ZVulkanTexture& colormap);
  void bindMergeDescriptor(ZVulkanTexture& colorArray, ZVulkanTexture* depthArray);
};

} // namespace nim
