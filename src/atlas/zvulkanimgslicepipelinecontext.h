#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;
class ZMesh;
class ZImg;
class Z3DImg;
class ZColorMap;
class ZVulkanImageBlockUploader;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanImgSlicePipelineContext
{
public:
  explicit ZVulkanImgSlicePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanImgSlicePipelineContext();

  void resetFrame();

  struct Finalization
  {
    uint64_t streamKey = 0;
    Z3DEye eye = Z3DEye::MonoEye;
    bool lastRound = false;
    uint32_t channelCount = 0;
  };

  // Called by the Vulkan backend after recording this context. If present,
  // the backend will notify the originating renderer to advance progressive
  // bookkeeping (channel/round progression).
  [[nodiscard]] std::optional<Finalization> takePendingFinalization();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const ImgSlicePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

  // ---------------------------------------------------------------------------
  // Pre-record priming (must run before command-buffer recording begins).
  // These are invoked by ZVulkanLinearScript via backend preRecord actions so
  // bindless descriptor mutations never happen while recording.
  // ---------------------------------------------------------------------------

  struct BindlessWarmupDesc
  {
    Z3DImg* image = nullptr;
    const std::vector<const ZColorMap*>* colormaps = nullptr;
    std::vector<size_t> channels;
    bool wantsVolume3D = true;
    bool wantsColormap = true;
    bool wantsPaging = false;
  };

  void preRecordBindlessWarmup(const BindlessWarmupDesc& desc);

  // Prime descriptor sets and per-slice output buffers for block-ID compaction.
  // This must run before command-buffer recording begins; record() assumes the
  // required per-slice descriptor sets already exist.
  void preRecordPrimeBlockIdCompaction(const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
                                       uint32_t sliceCount,
                                       uint32_t sliceIndex);

private:
  struct SliceVertex
  {
    glm::vec3 position{0.0f};
    glm::vec3 texCoord{0.0f};
  };

  struct SlicePipelineKey
  {
    bool validInput = true;
    uint32_t levelCount = 1;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(validInput, levelCount, colorFormats, depthFormat);
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
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(numVolumes, maxProjectionMerge, colorFormats, depthFormat);
    }

    bool operator<(const MergePipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct BlockIdPipelineKey
  {
    uint32_t levelCount = 1;
    vk::Format colorFormat = vk::Format::eR32G32B32A32Uint;

    auto tie() const
    {
      return std::tuple(levelCount, colorFormat);
    }

    bool operator<(const BlockIdPipelineKey& rhs) const
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
    uint64_t colormapGeneration = 0;
    uint32_t colormapWidth = 0;
    uint32_t levelCount = 0;
  };

  struct SliceDrawRange
  {
    vk::DeviceSize vertexOffsetBytes = 0;
    uint32_t vertexCount = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<SlicePipelineKey, PipelineInstance> m_slicePipelines;
  std::map<MergePipelineKey, PipelineInstance> m_mergePipelines;
  std::map<BlockIdPipelineKey, PipelineInstance> m_blockIdPipelines;

  // Block-ID compaction (compute) to avoid full RGBA32UI readback.
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutSampled;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutSampled;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineSampled;
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutStorage;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutStorage;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineStorage;
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutBuffer;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutBuffer;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineBufferAppend;
  std::unique_ptr<ZVulkanBuffer> m_blockIdPixelBuffer;
  size_t m_blockIdPixelBufferCapacity = 0;
  // Per-frame-slot compaction outputs for slice block-ID discovery. Per-slot
  // storage avoids overwrite-before-readback hazards when multiple Vulkan
  // frames are in flight.
  struct FrameBlockIdOutputs
  {
    size_t bytesPerSlice = 0;
    std::vector<std::unique_ptr<ZVulkanBuffer>> sliceOutputs;
  };
  std::unordered_map<void*, FrameBlockIdOutputs> m_blockIdOutputsByFrame;
  // Pre-record prepared descriptor state for block-ID compaction. Descriptor
  // sets are allocated from the per-frame descriptor arena and must not be
  // mutated while recording.
  std::vector<std::unique_ptr<ZVulkanDescriptorSet>> m_blockIdCompactDescriptors;
  uint32_t m_blockIdCompactDescriptorSliceCount = 0;
  size_t m_blockIdCompactDescriptorBytesPerSlice = 0;

  // Bindless + dynamic-UBO descriptor state.
  // - set 0: backend bindless sampled-image tables (shared, per frame-slot)
  // - set 1: small per-draw UBO for bindless texture indices (fast slice path)
  // - set 2: paging PageData UBO (paged slice + block-ID discovery)

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;
  std::vector<SliceDrawRange> m_sliceDrawRanges;

  std::unique_ptr<ZVulkanBuffer> m_quadVertexBuffer;
  size_t m_quadVertexCapacity = 0;
  size_t m_quadVertexCount = 0;
  std::vector<ChannelResources> m_channelResources;
  std::unique_ptr<ZVulkanImageBlockUploader> m_imageBlockUploader;
  std::optional<Finalization> m_pendingFinalization;

  vk::PipelineVertexInputStateCreateInfo makeSliceVertexInputState() const;
  vk::PipelineVertexInputStateCreateInfo makeQuadVertexInputState() const;
  void ensureSliceVertexCapacity(size_t vertexCount);
  void ensureQuadVertexBuffer();
  void uploadSliceGeometry(std::span<const ZMesh> slices);
  ZVulkanTexture&
  ensureVolumeTexture(size_t channel, uint64_t generation, const ZImg& image, ChannelResources& resources);
  ZVulkanTexture& ensureColormapTexture(size_t channel, const ZColorMap* colorMap, ChannelResources& resources);

  PipelineInstance& ensureSlicePipeline(const SlicePipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureMergePipeline(const MergePipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat);
  void ensureBlockIdCompactionPipeline();

  // Cached slice-geometry identity to avoid re-uploading for each per-layer batch.
  uint64_t m_geometryStreamKey = 0;
  uint64_t m_geometrySignature = 0u;
};

} // namespace nim
