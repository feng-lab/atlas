#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
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

  struct DeferredProgressive
  {
    uint64_t streamKey = 0;
    Z3DEye eye = MonoEye;
    uint32_t channelCount = 0;
    uint32_t progressiveGeneration = 0u;
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
    std::shared_ptr<ZVulkanBuffer> pageDataBuffer;
    size_t pageDataCapacity = 0;
    std::shared_ptr<ZVulkanBuffer> boundPageDataBuffer; // last buffer bound in persistentPageDescriptor
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
  struct EntryGeometryBuffers
  {
    std::unique_ptr<ZVulkanBuffer> vertexBuffer;
    std::unique_ptr<ZVulkanBuffer> indexBuffer;
    size_t vertexCapacity = 0;
    size_t indexCapacity = 0;
  };
  // Per-frame-slot entry geometry buffers (host-visible). Keyed by backend activeFrameKey()
  // so resizing/replacement is safe even with multiple frames in flight.
  std::unordered_map<void*, EntryGeometryBuffers> m_entryGeometryBuffers;
  // Convenience pointers to the current active frame-slot buffers (set by ensureEntryVertexCapacity()).
  ZVulkanBuffer* m_entryVertexBuffer = nullptr;
  ZVulkanBuffer* m_entryIndexBuffer = nullptr;
  // Entry geometry is shared by per-layer batches (EntryExit + planar fast paths).
  // Upload at most once per frame and verify it does not change mid-frame.
  bool m_entryGeometryUploadedThisFrame = false;
  size_t m_entryGeometryVertexCountThisFrame = 0;
  size_t m_entryGeometryIndexCountThisFrame = 0;

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

  // (probes removed)

  // Debug-only depth-ramp pipeline cache
  PipelineInstance m_depthRampPipeline;
  std::optional<vk::Format> m_depthRampFormat;

  std::unique_ptr<ZVulkanDescriptorSet> m_emptyDescriptor; // frame-owned placeholder
  ZVulkanDescriptorSet* m_entryTransformDescriptor = nullptr; // per-draw override (backend-owned)
  ZVulkanDescriptorSet* m_copyDescriptor = nullptr; // per-draw override (backend-owned)
  ZVulkanDescriptorSet* m_mergeDescriptor = nullptr; // per-draw override (backend-owned)
  std::unique_ptr<ZVulkanBuffer> m_entryTransformBuffer;

  // Block-ID compaction (compute) resources
  // Two read-source variants: 'sampled' (combined image sampler) and 'storage' (uimage2D)
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutSampled;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutSampled;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineSampled;
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutStorage;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutStorage;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineStorage;
  // Buffer-based compaction (image copied to SSBO, compute scans SSBO)
  std::optional<vk::raii::DescriptorSetLayout> m_blockIdCompactSetLayoutBuffer;
  std::optional<vk::raii::PipelineLayout> m_blockIdCompactPipelineLayoutBuffer;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineBuffer;
  std::optional<vk::raii::Pipeline> m_blockIdCompactPipelineBufferAppend;
  std::unique_ptr<ZVulkanBuffer> m_blockIdPixelBuffer; // device-local, TRANSFER_DST | STORAGE_BUFFER
  size_t m_blockIdPixelBufferCapacity = 0;
  // Block-ID compaction output is read back on CPU after the submission fence signals.
  // With >1 frames in flight, this must be per frame-slot to avoid CPU/GPU hazards
  // (CPU memset while GPU writes; overwrite-before-parse).
  struct BlockIdCompactionOutput
  {
    std::shared_ptr<ZVulkanBuffer> buffer;
    size_t capacity = 0; // bytes
  };
  std::unordered_map<void*, BlockIdCompactionOutput> m_blockIdCompactOutputs;
  ZVulkanDevice* m_blockIdCompactDevice = nullptr; // non-owning; used to detect executor rebuilds
  uint32_t m_blockIdCompactMaxFramesInFlight = 0;
  // Per-attachment snapshot of append counts (host-visible), used to detect
  // whether an attachment contributed any IDs (delta == 0 => attachment all zeros).

  std::vector<ChannelResources> m_channelResources;
  std::unique_ptr<ZVulkanImageBlockUploader> m_imageBlockUploader;

  std::optional<Finalization> m_pendingFinalization;
  std::optional<DeferredProgressive> m_deferredProgressive;

  struct StreamEyeKey
  {
    uint64_t streamKey = 0;
    Z3DEye eye = MonoEye;
    bool operator==(const StreamEyeKey& o) const
    {
      return streamKey == o.streamKey && eye == o.eye;
    }
  };

  struct StreamEyeKeyHash
  {
    size_t operator()(const StreamEyeKey& key) const
    {
      const size_t h0 = std::hash<uint64_t>{}(key.streamKey);
      const size_t h1 = std::hash<int>{}(static_cast<int>(key.eye));
      return h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6) + (h0 >> 2));
    }
  };

  // When a progressive finalization is pending for a stream/eye, the first
  // stage batch will cause the backend to consume the finalization and update
  // the originating renderer. Subsequent stage batches in the same frame must
  // no-op (otherwise we'd accidentally record work after the channel advances).
  std::unordered_set<StreamEyeKey, StreamEyeKeyHash> m_skipStagesThisFrame;

  struct ProgressivePrepKey
  {
    uint64_t streamKey = 0;
    Z3DEye eye = MonoEye;
    uint32_t progressiveGeneration = 0u;
    int32_t channelIndexRaw = -1;
    int32_t roundIndexRaw = 0;

    bool operator==(const ProgressivePrepKey& o) const
    {
      return streamKey == o.streamKey && eye == o.eye && progressiveGeneration == o.progressiveGeneration &&
             channelIndexRaw == o.channelIndexRaw && roundIndexRaw == o.roundIndexRaw;
    }
  };

  struct ProgressivePrepState
  {
    ProgressivePrepKey key{};
    bool ready = false;
    uint32_t channelCount = 0u;
    uint32_t activeChannelIndex = 0u; // index into visibleChannels
    size_t channelIndex = 0; // actual image channel index
    bool skipBlockIdPass = false;
    std::optional<Finalization> finalizeAfterProgressive;
    float zeToZW_a = 0.0f;
    float zeToZW_b = 0.0f;
    float zeToScreenPixelVoxelSize = 0.0f;
    ZVulkanTexture* entryTexture = nullptr;
    ZVulkanTexture* lastColor = nullptr;
    ZVulkanTexture* lastDepth = nullptr;
    ZVulkanTexture* currentColor = nullptr;
    ZVulkanTexture* currentDepth = nullptr;
  };

  std::optional<ProgressivePrepState> m_progressivePrep;

  // Track which depth images have been cleared this frame (for first-use clear on merge).
  std::unordered_set<VkImage> m_depthClearedThisFrame;

  // No extra per-stream channel bookkeeping; pending finalization is set after
  // frame completion when compaction finds no missing blocks for the active channel.

  void ensureDescriptorPool();
  void resetDescriptors();
  void ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount);
  void ensureQuadVertexBuffer();

  void ensureDescriptorLayouts();
  void ensureEntryPipelines(vk::Format colorFormat);
  PipelineInstance& ensureFastPipeline(const FastPipelineKey& key);
  void ensureEmptyDescriptor();
  PipelineInstance& ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat);
  // Lazily create the image block uploader when a Vulkan device is guaranteed
  // to be available (e.g., during recording after beginRender()).
  void ensureUploader();
  PipelineInstance& ensureProgressivePipeline(const ProgressivePipelineKey& key,
                                              const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureCopyPipeline(const CopyPipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureMergePipeline(const MergePipelineKey& key, const vulkan::AttachmentFormats& formats);
  void uploadEntryGeometry(const ImgRaycasterPayload& payload);
  void ensureEntryGeometryUploadedThisFrame(const ImgRaycasterPayload& payload);
  void ensureEntryTransformResources(Z3DRendererBase& renderer,
                                     const RenderBatch& batch,
                                     const ImgRaycasterPayload& payload);

  void ensureBlockIdCompactionPipeline(uint32_t attachmentCount, int mode);
  BlockIdCompactionOutput& ensureBlockIdCompactOutput(size_t bytes);
  void recordBlockIdCompaction(Z3DRendererBase& renderer,
                               const RenderBatch& batch,
                               const ImgRaycasterPayload& payload,
                               vk::raii::CommandBuffer& cmd);

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

  // If freshOverrideDescriptors is true, force allocation of new per-draw
  // override descriptor sets to avoid updating sets already bound earlier in
  // the same command buffer.
  bool updatePageDescriptors(ChannelResources& resources,
                             const ImgRaycasterPayload& payload,
                             ZVulkanTexture& entryExit,
                             ZVulkanTexture& lastDepth,
                             ZVulkanTexture& lastColor,
                             ZVulkanTexture& volume,
                             ZVulkanTexture& transfer,
                             const Z3DImg& image,
                             size_t channelIndex,
                             float zeToScreenPixelVoxelSize,
                             bool freshOverrideDescriptors = false);

  std::vector<vk::DescriptorSet> collectProgressiveDescriptorSets(ChannelResources& resources,
                                                                  bool preferOverrideStatic = false);
  // depthArray is optional
  void bindMergeDescriptor(ZVulkanTexture& colorArray, /*nullable*/ ZVulkanTexture* depthArray);

  ZVulkanTexture&
  ensureVolumeTexture(ChannelResources& resources, const ZImg& image, size_t channelIndex, uint64_t generation);
  ZVulkanTexture& ensureImage2DTexture(ChannelResources& resources, const ZImg& image, uint64_t generation);
  ZVulkanTexture& ensureTransferTexture(ChannelResources& resources, const Z3DTransferFunction& transferFunction);

  // ---------------------------------------------------------------------------
  // Stage-based recording helpers (linear-script friendly).
  // Each stage records work for one logical target/pass; see ImgRaycasterPayload::Stage.
  // ---------------------------------------------------------------------------
  void recordStageEntryExit(Z3DRendererBase& renderer,
                            const RenderBatch& batch,
                            const ImgRaycasterPayload& payload,
                            const vk::Viewport& viewport,
                            const vk::Rect2D& scissor,
                            vk::raii::CommandBuffer& cmd);
  void recordStageFastDirect(Z3DRendererBase& renderer,
                             const RenderBatch& batch,
                             const ImgRaycasterPayload& payload,
                             const vk::Viewport& viewport,
                             const vk::Rect2D& scissor,
                             vk::raii::CommandBuffer& cmd);
  void recordStageFastLayers(Z3DRendererBase& renderer,
                             const RenderBatch& batch,
                             const ImgRaycasterPayload& payload,
                             const vk::Viewport& viewport,
                             const vk::Rect2D& scissor,
                             vk::raii::CommandBuffer& cmd);
  void recordStageFastMerge(Z3DRendererBase& renderer,
                            const RenderBatch& batch,
                            const ImgRaycasterPayload& payload,
                            const vk::Viewport& viewport,
                            const vk::Rect2D& scissor,
                            vk::raii::CommandBuffer& cmd);
  void recordStageProgressivePreviewLayers(Z3DRendererBase& renderer,
                                           const RenderBatch& batch,
                                           const ImgRaycasterPayload& payload,
                                           const vk::Viewport& viewport,
                                           const vk::Rect2D& scissor,
                                           vk::raii::CommandBuffer& cmd);
  void recordStageProgressiveBlockId(Z3DRendererBase& renderer,
                                     const RenderBatch& batch,
                                     const ImgRaycasterPayload& payload,
                                     const vk::Viewport& viewport,
                                     const vk::Rect2D& scissor,
                                     vk::raii::CommandBuffer& cmd);
  void recordStageProgressiveCompaction(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const ImgRaycasterPayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd);
  void recordStageProgressiveRaycast(Z3DRendererBase& renderer,
                                     const RenderBatch& batch,
                                     const ImgRaycasterPayload& payload,
                                     const vk::Viewport& viewport,
                                     const vk::Rect2D& scissor,
                                     vk::raii::CommandBuffer& cmd);
  void recordStageProgressiveCopyToLayers(Z3DRendererBase& renderer,
                                          const RenderBatch& batch,
                                          const ImgRaycasterPayload& payload,
                                          const vk::Viewport& viewport,
                                          const vk::Rect2D& scissor,
                                          vk::raii::CommandBuffer& cmd);
  void recordStageProgressiveMerge(Z3DRendererBase& renderer,
                                   const RenderBatch& batch,
                                   const ImgRaycasterPayload& payload,
                                   const vk::Viewport& viewport,
                                   const vk::Rect2D& scissor,
                                   vk::raii::CommandBuffer& cmd);

  [[nodiscard]] bool stageSkippedThisFrame(uint64_t streamKey, Z3DEye eye) const;
  void markStageSkippedThisFrame(uint64_t streamKey, Z3DEye eye);

  [[nodiscard]] std::optional<ProgressivePrepState> ensurePreparedProgressiveRound(Z3DRendererBase& renderer,
                                                                                   const RenderBatch& batch,
                                                                                   const ImgRaycasterPayload& payload,
                                                                                   const vk::Viewport& viewport,
                                                                                   const vk::Rect2D& scissor,
                                                                                   vk::raii::CommandBuffer& cmd,
                                                                                   const CompositingConfig& composite);

  void recordFastPlanarLayersOnly(Z3DRendererBase& renderer,
                                  const RenderBatch& batch,
                                  const ImgRaycasterPayload& payload,
                                  const vk::Viewport& viewport,
                                  const vk::Rect2D& scissor,
                                  vk::raii::CommandBuffer& cmd,
                                  const CompositingConfig& composite);

  void recordFastVolumeLayersOnly(Z3DRendererBase& renderer,
                                  const RenderBatch& batch,
                                  const ImgRaycasterPayload& payload,
                                  const vk::Viewport& viewport,
                                  const vk::Rect2D& scissor,
                                  vk::raii::CommandBuffer& cmd,
                                  const CompositingConfig& composite);
  void recordMergeFromLayers(const RenderBatch& batch,
                             const vk::Viewport& viewport,
                             const vk::Rect2D& scissor,
                             vk::raii::CommandBuffer& cmd,
                             const CompositingConfig& composite,
                             ZVulkanTexture& layerColor,
                             /*nullable*/ ZVulkanTexture* layerDepth,
                             uint32_t channelCount);

  // (public) see takePendingFinalization() above
};

} // namespace nim
