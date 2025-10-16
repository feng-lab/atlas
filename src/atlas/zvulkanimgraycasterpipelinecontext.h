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

struct CompositingConfig
{
  ImgCompositingMode mode = ImgCompositingMode::DirectVolumeRendering;
  bool resultOpaque = false;
  bool localMip = false;
  bool maxProjectionMerge = false;
};

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanTexture;
class ZVulkanBuffer;
class Z3DTransferFunction;
class ZVulkanImageBlockUploader;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanImgRaycasterPipelineContext
{
public:
  explicit ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanImgRaycasterPipelineContext();

  // Pending-finalization record for progressive rounds
  struct Finalization
  {
    uint64_t streamKey = 0;
    Z3DEye eye = MonoEye;
    bool lastRound = false;
    uint32_t channelCount = 0;
  };

  // Expose pending-finalization pull for the backend driver (no friendship).
  std::optional<Finalization> takePendingFinalization();

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
    uint64_t image2DGeneration = 0;
    std::unique_ptr<ZVulkanTexture> image2DTexture;
    std::unique_ptr<ZVulkanTexture> transferTexture;
    uint64_t transferGeneration = 0;
    uint32_t transferWidth = 0;
    ZVulkanDescriptorSet* fastDescriptor = nullptr; // per-draw override (backend-owned)
    ZVulkanDescriptorSet* image2DDescriptor = nullptr; // per-draw override (backend-owned)
    ZVulkanDescriptorSet* sliceDescriptor = nullptr; // per-draw override (backend-owned)
    // Ray params via push constants; no descriptors or buffers needed
    // Progressive (dynamic) textures (entry/exit, lastDepth, lastColor)
    ZVulkanDescriptorSet* pagedDescriptor = nullptr; // per-draw override (backend-owned)
    // Progressive static textures (page dir, table, cache, volume, transfer)
    ZVulkanDescriptorSet* staticDescriptor = nullptr; // per-draw override (backend-owned)
    std::unique_ptr<ZVulkanDescriptorSet> persistentStaticDescriptor; // cached across frames
    // Page/params UBOs
    ZVulkanDescriptorSet* pageDescriptor = nullptr; // per-draw override (backend-owned)
    std::unique_ptr<ZVulkanDescriptorSet> persistentPageDescriptor; // cached across frames
    std::unique_ptr<ZVulkanBuffer> pageDataBuffer;
    size_t pageDataCapacity = 0;
    ZVulkanBuffer* boundPageDataBuffer = nullptr; // last buffer bound in persistentPageDescriptor
    // Tracking for persistent static set
    class ZVulkanTexture* boundPageDirectoryTex = nullptr;
    class ZVulkanTexture* boundPageTableTex = nullptr;
    class ZVulkanTexture* boundImageCacheTex = nullptr;
    class ZVulkanTexture* boundVolumeTex = nullptr;
    class ZVulkanTexture* boundTransferTex = nullptr;
    uint32_t levelCount = 0;
    std::unique_ptr<ZVulkanDescriptorSet> blockIdDescriptor;
    std::vector<uint32_t> blockIdScratch;
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;
  };

  enum class FastPipelineVariant
  {
    Volume,
    Image2D,
    Slice2D
  };

  struct FastPipelineKey
  {
    FastPipelineVariant variant = FastPipelineVariant::Volume;
    ImgCompositingMode mode = ImgCompositingMode::DirectVolumeRendering;
    bool resultOpaque = false;
    bool depthEnabled = true;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(variant, mode, resultOpaque, depthEnabled, colorFormats, depthFormat);
    }

    bool operator==(const FastPipelineKey& rhs) const
    {
      return tie() == rhs.tie();
    }

    bool operator<(const FastPipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct BlockIdPipelineKey
  {
    uint32_t levelCount = 1u;
    uint32_t attachmentCount = 1u;
    vk::Format colorFormat = vk::Format::eR32G32B32A32Uint;

    auto tie() const
    {
      return std::tuple(levelCount, attachmentCount, colorFormat);
    }

    bool operator<(const BlockIdPipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct ProgressivePipelineKey
  {
    vk::Format colorFormat = vk::Format::eR16G16B16A16Sfloat;
    vk::Format accumulatorFormat = vk::Format::eR32G32Sfloat;
    ImgCompositingMode mode = ImgCompositingMode::DirectVolumeRendering;
    bool localMip = false;
    bool resultOpaque = false;
    uint32_t levelCount = 1u;

    auto tie() const
    {
      return std::tuple(colorFormat, accumulatorFormat, mode, localMip, resultOpaque, levelCount);
    }

    bool operator<(const ProgressivePipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct CopyPipelineKey
  {
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(colorFormats, depthFormat);
    }

    bool operator<(const CopyPipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct MergePipelineKey
  {
    int numVolumes = 1;
    bool maxProjectionMerge = false;
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

  // Debug-only depth ramp pipeline (no color attachments)
  void ensureDepthOnlyRampPipeline(vk::Format depthFormat);

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
  std::optional<vk::raii::DescriptorSetLayout> m_image2DSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_sliceFastSetLayout;
  // Progressive split: static (set=0) and dynamic (set=1)
  std::optional<vk::raii::DescriptorSetLayout> m_progressiveStaticSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_progressiveDynamicSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_pageSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_transformSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_copySetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_mergeSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_emptySetLayout;
  // Ray parameters use push constants; no descriptor set layout needed.

  PipelineInstance m_entryFrontPipeline;
  PipelineInstance m_entryBackPipeline;
  std::map<FastPipelineKey, PipelineInstance> m_fastPipelines;
  std::map<BlockIdPipelineKey, PipelineInstance> m_blockIdPipelines;
  std::map<ProgressivePipelineKey, PipelineInstance> m_progressivePipelines;
  std::map<CopyPipelineKey, PipelineInstance> m_copyPipelines;
  std::map<MergePipelineKey, PipelineInstance> m_mergePipelines;

  // Debug-only depth-ramp pipeline cache
  PipelineInstance m_depthRampPipeline;
  std::optional<vk::Format> m_depthRampFormat;

  std::unique_ptr<ZVulkanDescriptorSet> m_emptyDescriptor; // frame-owned placeholder
  ZVulkanDescriptorSet* m_entryTransformDescriptor = nullptr; // per-draw override (backend-owned)
  ZVulkanDescriptorSet* m_copyDescriptor = nullptr; // per-draw override (backend-owned)
  ZVulkanDescriptorSet* m_mergeDescriptor = nullptr; // per-draw override (backend-owned)
  std::unique_ptr<ZVulkanBuffer> m_entryTransformBuffer;

  std::vector<ChannelResources> m_channelResources;
  std::unique_ptr<ZVulkanImageBlockUploader> m_imageBlockUploader;
  std::unique_ptr<ZVulkanTexture> m_progressiveLayerColor;
  std::unique_ptr<ZVulkanTexture> m_progressiveLayerDepth;
  glm::uvec2 m_progressiveLayerSize{0u, 0u};
  uint32_t m_progressiveLayerCount = 0u;
  uint32_t m_progressiveGeneration = 0u;

  std::optional<Finalization> m_pendingFinalization;

  // Track which depth images have been cleared this frame (for first-use clear on merge).
  std::unordered_set<VkImage> m_depthClearedThisFrame;

  void ensureDescriptorPool();
  void resetDescriptors();
  void ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount);
  void ensureQuadVertexBuffer();

  void ensureDescriptorLayouts();
  void ensureEntryPipelines(vk::Format colorFormat);
  PipelineInstance& ensureFastPipeline(const FastPipelineKey& key);
  void ensureEmptyDescriptor();
  PipelineInstance& ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat);
  PipelineInstance& ensureProgressivePipeline(const ProgressivePipelineKey& key,
                                              const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureCopyPipeline(const CopyPipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureMergePipeline(const MergePipelineKey& key, const vulkan::AttachmentFormats& formats);
  void uploadEntryGeometry(const ImgRaycasterPayload& payload);
  void ensureEntryTransformResources(Z3DRendererBase& renderer,
                                     const RenderBatch& batch,
                                     const ImgRaycasterPayload& payload);

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
  void updateChannelImage2DDescriptors(ChannelResources& resources,
                                       ZVulkanTexture& imageTexture,
                                       ZVulkanTexture& transferTexture);
  void updateChannelSliceDescriptors(ChannelResources& resources,
                                     ZVulkanTexture& volumeTexture,
                                     ZVulkanTexture& transferTexture);

  bool updatePageDescriptors(ChannelResources& resources,
                             const ImgRaycasterPayload& payload,
                             ZVulkanTexture& entryExit,
                             ZVulkanTexture& lastDepth,
                             ZVulkanTexture& lastColor,
                             ZVulkanTexture& volume,
                             ZVulkanTexture& transfer,
                             const Z3DImg& image,
                             size_t channelIndex,
                             float zeToScreenPixelVoxelSize);

  std::vector<vk::DescriptorSet> collectProgressiveDescriptorSets(ChannelResources& resources);
  // depthArray is optional
  void bindMergeDescriptor(ZVulkanTexture& colorArray, /*nullable*/ ZVulkanTexture* depthArray);
  void ensureProgressiveLayerTargets(const glm::uvec2& size,
                                     uint32_t layerCount,
                                     uint32_t generation,
                                     vk::raii::CommandBuffer& cmd);

  ZVulkanTexture&
  ensureVolumeTexture(ChannelResources& resources, const ZImg& image, size_t channelIndex, uint64_t generation);
  ZVulkanTexture& ensureImage2DTexture(ChannelResources& resources, const ZImg& image, uint64_t generation);
  ZVulkanTexture& ensureTransferTexture(ChannelResources& resources, const Z3DTransferFunction& transferFunction);

  void renderEntryExit(Z3DRendererBase& renderer,
                       const RenderBatch& batch,
                       const ImgRaycasterPayload& payload,
                       const vk::Viewport& viewport,
                       const vk::Rect2D& scissor,
                       vk::raii::CommandBuffer& cmd);

  void renderFastPath(Z3DRendererBase& renderer,
                      const RenderBatch& batch,
                      const ImgRaycasterPayload& payload,
                      const vk::Viewport& viewport,
                      const vk::Rect2D& scissor,
                      vk::raii::CommandBuffer& cmd,
                      FastPipelineVariant variant,
                      const CompositingConfig& composite);
  void renderFastVolume(Z3DRendererBase& renderer,
                        const RenderBatch& batch,
                        const ImgRaycasterPayload& payload,
                        const vk::Viewport& viewport,
                        const vk::Rect2D& scissor,
                        vk::raii::CommandBuffer& cmd,
                        const CompositingConfig& composite);
  void renderFastImage2D(Z3DRendererBase& renderer,
                         const RenderBatch& batch,
                         const ImgRaycasterPayload& payload,
                         const vk::Viewport& viewport,
                         const vk::Rect2D& scissor,
                         vk::raii::CommandBuffer& cmd,
                         const CompositingConfig& composite);
  void renderFastSlice2D(Z3DRendererBase& renderer,
                         const RenderBatch& batch,
                         const ImgRaycasterPayload& payload,
                         const vk::Viewport& viewport,
                         const vk::Rect2D& scissor,
                         vk::raii::CommandBuffer& cmd,
                         const CompositingConfig& composite);

  void renderProgressivePath(Z3DRendererBase& renderer,
                             const RenderBatch& batch,
                             const ImgRaycasterPayload& payload,
                             const vk::Viewport& viewport,
                             const vk::Rect2D& scissor,
                             vk::raii::CommandBuffer& cmd,
                             const CompositingConfig& composite);

  // (public) see takePendingFinalization() above
};

} // namespace nim
