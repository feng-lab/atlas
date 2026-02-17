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

  // ---------------------------------------------------------------------------
  // Pre-record priming (must run before command-buffer recording begins).
  // These are invoked by ZVulkanLinearScript via backend preRecord actions so
  // bindless descriptor mutations never happen while recording.
  // ---------------------------------------------------------------------------

  struct BindlessWarmupDesc
  {
    Z3DImg* image = nullptr;
    const std::vector<Z3DTransferFunction*>* transferFunctions = nullptr;
    std::vector<size_t> channels;
    bool wants2D = false;
    bool wantsVolume3D = true;
    bool wantsPaging = false;
  };

  void preRecordBindlessWarmup(const BindlessWarmupDesc& desc);
  void preRecordPrimeBlockIdCompaction(const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
                                       uint32_t effectiveAttachmentCount);

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
    uint32_t levelCount = 0;
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

  // Bindless + dynamic-UBO descriptor state.
  // - set 0: backend bindless sampled-image tables (shared, per frame-slot)
  // - set 1: small per-draw UBO for planar fast paths (texture indices)
  // - set 2: paging PageData UBO for progressive raycasters

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

  // Block-ID compaction (compute) resources
  // Two read-source variants: 'sampled(bindless)' (utexture2D via bindless set 0) and 'storage' (uimage2D)
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
  // Pre-record prepared descriptor state for compaction.
  // These descriptor sets are allocated from the per-frame descriptor arena and
  // must be written only before command-buffer recording begins.
  std::unique_ptr<ZVulkanDescriptorSet> m_blockIdCompactDescriptorBuffer; // set 1 (buffer variant)
  std::unique_ptr<ZVulkanDescriptorSet> m_blockIdCompactDescriptorSampled; // set 1 (bindless sampled variant)
  struct BlockIdStorageDescriptorPack
  {
    std::vector<std::unique_ptr<ZVulkanDescriptorSet>> perAttachment; // set 1 (storage variant)
  };
  std::unordered_map<const Z3DScratchResourcePool::RenderTargetLease*, BlockIdStorageDescriptorPack>
    m_blockIdCompactDescriptorStorageByLease;

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
    uint32_t levelCount = 1u;
    uint32_t pageDataDynOffset = 0u;
    ZVulkanTexture* entryTexture = nullptr;
    ZVulkanTexture* lastColor = nullptr;
    ZVulkanTexture* lastDepth = nullptr;
    ZVulkanTexture* currentColor = nullptr;
    ZVulkanTexture* currentDepth = nullptr;
    ZVulkanTexture* pageDirectory = nullptr;
    ZVulkanTexture* pageTable = nullptr;
    ZVulkanTexture* imageCache = nullptr;
    ZVulkanTexture* volumeTexture = nullptr;
    ZVulkanTexture* transferTexture = nullptr;
  };

  std::optional<ProgressivePrepState> m_progressivePrep;
  std::optional<ProgressivePrepKey> m_lastDebugDumpBlockIdKey;

  // Track which depth images have been cleared this frame (for first-use clear on merge).
  std::unordered_set<VkImage> m_depthClearedThisFrame;

  // No extra per-stream channel bookkeeping; pending finalization is set after
  // frame completion when compaction finds no missing blocks for the active channel.

  void ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount);
  void ensureQuadVertexBuffer();

  void ensureEntryPipelines(vk::Format colorFormat);
  PipelineInstance& ensureFastPipeline(const FastPipelineKey& key);
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

  void ensureBlockIdCompactionPipeline();
  BlockIdCompactionOutput& ensureBlockIdCompactOutput(size_t bytes);
  void recordBlockIdCompaction(Z3DRendererBase& renderer,
                               const RenderBatch& batch,
                               const ImgRaycasterPayload& payload,
                               vk::raii::CommandBuffer& cmd);

  ChannelResources& ensureChannelResources(size_t channelIndex);

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
