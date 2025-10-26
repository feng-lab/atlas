#pragma once

#include "z3drendererbackend.h"
#include "zglmutils.h"
#include "zvulkan.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"
// Arena allocations for per-frame descriptor sets
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
// Attachment format helpers
#include "zvulkanrenderconversions.h"
#include "z3dtypes.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nim {

class ZVulkanLinePipelineContext;
class ZVulkanMeshPipelineContext;
class ZVulkanEllipsoidPipelineContext;
class ZVulkanConePipelineContext;
class ZVulkanSpherePipelineContext;
class ZVulkanBackgroundPipelineContext;
class ZVulkanTextureCopyPipelineContext;
class ZVulkanTextureBlendPipelineContext;
class ZVulkanTextureDualPeelPipelineContext;
class ZVulkanTextureWeightedAveragePipelineContext;
class ZVulkanTextureWeightedBlendedPipelineContext;
class ZVulkanTextureGlowPipelineContext;
class ZVulkanImgSlicePipelineContext;
class ZVulkanImgRaycasterPipelineContext;
class ZVulkanFontPipelineContext;
class ZVulkanBuffer;
// Vulkan renderer backend borrows the shared ZVulkanDevice injected through the scratch pool.
// Lifetime notes:
//  * Z3DRenderingEngine owns the Vulkan context/device and calls setVulkanDevice() on the pool
//    prior to rendering. The backend simply caches the latest pointer and never destroys it.
//  * Scratch pool leases own all VkImage-backed render targets; pipeline contexts must treat
//    ZVulkanTexture* obtained through AttachmentHandle as transient and valid only for the lease.
//  * Frames rotate through a small pool of command buffers guarded by fences. Each frame is
//    submitted once; backend waits on the frame fence before reusing resources.
class Z3DRendererVulkanBackend final : public Z3DRendererBackend
{
public:
  // Current backend bound to the rendering thread (TLS)
  static Z3DRendererVulkanBackend* current();

  Z3DRendererVulkanBackend();
  ~Z3DRendererVulkanBackend() override;

  void setGlobalShaderParameters(Z3DRendererBase& renderer, Z3DShaderProgram& shader, Z3DEye eye) override;

  [[nodiscard]] std::string generateHeader(const Z3DRendererBase& renderer) const override;

  [[nodiscard]] std::string generateGeomHeader(const Z3DRendererBase& renderer) const override;

  void beginRender(Z3DRendererBase& renderer) override;

  void endRender(Z3DRendererBase& renderer) override;

  void processBatches(Z3DRendererBase& renderer, const RendererCPUState& state) override;

  void processCompositorPass(Z3DRendererBase& renderer, const Z3DCompositorPass& pass) override;

  [[nodiscard]] bool supportsCommandLists() const override;

  RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease) override;

  // Shared fallback resources used across pipeline contexts to avoid redundant
  // tiny texture/sampler creation.
  ZVulkanTexture& defaultPlaceholderTexture2D();
  vk::Sampler defaultSampler();
  vk::Sampler nearestClampSampler();

  // Shared descriptor set layouts reused across pipeline contexts.
  vk::DescriptorSetLayout meshTextureDescriptorSetLayout();
  vk::DescriptorSetLayout lightingDescriptorSetLayout();
  vk::DescriptorSetLayout transformDescriptorSetLayout();
  vk::DescriptorSetLayout oitDescriptorSetLayout();
  vk::DescriptorSetLayout dualTexturePlaceholderDescriptorSetLayout();
  vk::DescriptorSetLayout emptyDescriptorSetLayout();  

  void preBackendSwitch() override;

  // Pass-scope aggregation hooks
  void beginPassScope(std::string_view label) override;
  void endPassScope() override;

  // Per-draw and transition notifications (used by recorders)
  void notifyDrawSubmitted();
  void notifyLayoutTransition(bool wasNoop);

  ZVulkanDevice& device();

  const ZVulkanDevice& device() const;

  vk::raii::CommandBuffer& commandBuffer();

  const vk::raii::CommandBuffer& commandBuffer() const;

  // Shared geometry
  ZVulkanBuffer& fullscreenQuadVertexBuffer();

  // Upload arena helpers (per-frame transient vertex/index data)
  struct UploadSlice
  {
    vk::Buffer buffer{};
    vk::DeviceSize offset = 0;
    void* mapped = nullptr; // points into the arena at [offset, offset+size)
    size_t size = 0; // requested bytes
  };

  // Obtain a suballocation in the current frame's upload arena. Grows the
  // underlying buffer if necessary. Returns an empty slice when no active
  // frame is recording (e.g., zero-sized viewport).
  UploadSlice suballocateUpload(size_t bytes, size_t alignment = 16);

  // Precisely reserve space in the per-frame upload arena for a sequence of
  // slices, each given as {bytes, alignment}. When enabled via flag, this
  // performs at most one growth before any suballocations to keep all slices
  // within the same underlying buffer. No-op if not recording or disabled.
  void reserveUploadSlices(std::initializer_list<std::pair<size_t, size_t>> slices);

  // Device-local static buffer arena (lifetime: backend)
  struct StaticSlice
  {
    vk::Buffer buffer{};
    vk::DeviceSize offset = 0;
    size_t size = 0;
  };

  // Allocate space in the device-local vertex/index arenas. Returns an empty
  // slice on allocation failure.
  StaticSlice allocateStaticVB(size_t bytes, size_t alignment = 16);
  StaticSlice allocateStaticIB(size_t bytes, size_t alignment = 4);

  // Record a copy from an upload slice into a device-local buffer at dst.
  // Inserts a barrier making the data visible to the appropriate pipeline stage.
  void stageCopy(vk::Buffer dst, vk::DeviceSize dstOffset, const UploadSlice& src, bool isIndexBuffer);

  // Stage 2: Per-frame descriptor arena API
  // Allocate a descriptor set from the current frame's arena. Returns null when
  // no active frame exists (e.g., zero-sized viewport), and logs at VLOG(1).
  std::unique_ptr<ZVulkanDescriptorSet> allocateFrameDescriptorSet(vk::DescriptorSetLayout layout);

  // Allocate a per-draw override descriptor set and keep it alive until the
  // current frame fence signals. Returns a raw pointer owned by the backend.
  // Use this when updating descriptors per draw to avoid update-after-bind.
  ZVulkanDescriptorSet* allocateOverrideDescriptorSet(vk::DescriptorSetLayout layout);

  // Stage 2: Schedule a callback to run once the current frame's fence signals
  void scheduleAfterCurrentFrameCompletion(std::function<void()> fn);
  // Schedule a callback gated by the current submission fence. Runs as soon as
  // the submission finishes (fence signals), without waiting for frame-slot reuse.
  void scheduleAfterActiveSubmissionFence(std::function<void()> fn);
  void notifyPipelineCreated();
  void notifyPipelineBound(vk::Pipeline pipeline);
  // Queue a static copy (upload slice -> device-local VB/IB) to be executed
  // outside dynamic rendering before command buffer end in this frame.
  void scheduleStaticCopy(vk::Buffer dst, vk::DeviceSize dstOffset, const UploadSlice& src, bool isIndexBuffer);

  // Current segment attachment formats (if a dynamic rendering segment is open)
  [[nodiscard]] const std::optional<vulkan::AttachmentFormats>& currentSegmentFormats() const;
  // Crash when pipelineFormats does match the current segment
  void validateFormatsOrCrash(const vulkan::AttachmentFormats& pipelineFormats,
                              /*nullable*/ const char* contextTag = nullptr);

  // Recording state helpers and telemetry hooks
  [[nodiscard]] bool isRecording() const
  {
    return m_frameRecording;
  }
  void notifyDescriptorWriteWhileRecording(bool rewriteAttempt)
  {
    if (m_activeFrame) {
      m_activeFrame->descriptorWritesWhileRecording++;
      if (rewriteAttempt) {
        m_activeFrame->boundSetRewriteAttempts++;
      }
    }
  }

  // Stage 4: Async Readback API (offscreen)
  // Request an end-of-frame copy of a color attachment into a host-visible staging buffer.
  // The provided onReady callback executes after the frame fence signals (on the
  // rendering thread) with a pointer to the mapped staging memory and metadata.
  // The callback must not retain the pointer beyond the callback's lifetime.
  void requestEndOfFrameColorReadback(
    class ZVulkanTexture& src,
    Z3DEye eye,
    std::function<
      void(const void* mapped, size_t bytes, vk::Format format, glm::uvec2 size, std::function<void()> releaseSlot)>
      onReady);

  // GPU timestamp scopes (public so pipeline contexts can instrument hot paths)
  std::optional<size_t> beginGpuScope(std::string_view label);
  void endGpuScope(size_t token);

  // For self-managed recorder passes: decide whether to Clear or Load attachments.
  // Returns true exactly once per unique attachment set (color IDs + depth ID)
  // within the current processBatches() invocation.

private:
  friend class Z3DRendererBase;
  void ensureDevice();
  void resetFrameResources();
  void ensureDefaultPlaceholders();
  void ensureSharedDescriptorLayouts();
  struct FrameResources;
  FrameResources& ensureFrameResourcesForKey(void* key);
  struct GpuScopeRecord
  {
    std::string label;
    uint32_t startQuery = 0;
    uint32_t endQuery = 0;
  };
  struct CpuScopeRecord
  {
    std::string label;
    double milliseconds = 0.0;
  };
  struct FrameResources
  {
    // Aggregation keys for per-submission ingestion
    uint64_t realFrameToken = 0;
    uint32_t submissionId = 0;
    vk::raii::QueryPool queryPool{nullptr};
    std::vector<GpuScopeRecord> gpuScopes;
    std::vector<CpuScopeRecord> cpuScopes;
    std::string frameName; // optional: name of this frame for logs
    uint32_t nextQuery = 0;

    std::chrono::steady_clock::time_point cpuStart;
    std::chrono::steady_clock::time_point cpuEnd;

    // Descriptor arena (per-frame)
    std::unique_ptr<ZVulkanDescriptorPool> descriptorPool; // reset only after fence signal
    uint32_t descriptorSetsAllocated = 0; // VLOG(1) counter per frame
    bool arenaResetScheduled = false; // scheduled at endRender()
    uint32_t arenaResetsPerformed = 0; // count performed resets (debug)
    // Fence-gated deferred actions (e.g., scratch slot releases)
    std::vector<std::function<void()>> deferredReleases;
    uint32_t leaseRecycleQueued = 0;
    uint32_t leaseRecycleExecuted = 0;

    // Stage 3: instrumentation (per-frame)
    uint32_t renderingSegmentsBegan = 0; // number of vkCmdBeginRendering calls
    uint32_t attachmentClears = 0; // number of attachments begun with Clear loadOp
    uint32_t attachmentLoads = 0; // number of attachments begun with Load loadOp

    // Pipelines
    uint32_t pipelinesCreated = 0; // graphics pipelines created this frame
    std::unordered_set<uint64_t> pipelinesBound; // unique pipelines bound this frame

    // Per-draw override descriptor sets kept alive until fence
    std::vector<std::unique_ptr<ZVulkanDescriptorSet>> transientOverrideSets;
    uint32_t overrideSetsAllocated = 0; // count of per-draw override sets

    // Formats of currently open dynamic rendering segment
    std::optional<vulkan::AttachmentFormats> activeSegmentFormats;
    // Skipped draws due to pipeline/segment format mismatch
    uint32_t skippedBatchesFormatMismatch = 0;

    // Descriptor guardrails counters
    uint32_t descriptorWritesWhileRecording = 0; // attempted writes during recording
    uint32_t boundSetRewriteAttempts = 0; // attempted rewrites of persistent sets

    // Stage 4: end-of-frame readback bookkeeping
    struct PendingReadback
    {
      class ZVulkanTexture* src = nullptr;
      Z3DEye eye = MonoEye;
      glm::uvec2 size{0u, 0u};
      vk::Format format = vk::Format::eUndefined;
      // Assigned staging slot index in m_readbackSlots
      int slotIndex = -1;
      size_t bytes = 0;
      // Consumer callback (runs after fence signal)
      std::function<
        void(const void* mapped, size_t bytes, vk::Format fmt, glm::uvec2 size, std::function<void()> releaseSlot)>
        onReady;
    };
    std::vector<PendingReadback> pendingColorReadbacks;
    size_t readbackBytesCopied = 0; // total bytes copied this frame
    uint32_t readbackSlotsInFlight = 0; // slots associated with this frame

    // Per-frame CPU→GPU upload arena for transient vertex/index data
    struct UploadArena
    {
      std::unique_ptr<class ZVulkanBuffer> buffer; // host-visible, host-coherent
      VmaVirtualBlock block = nullptr; // VMA virtual allocator over buffer
      void* mapped = nullptr; // persistent mapping
      size_t capacity = 0; // bytes
      size_t highWatermark = 0; // max used this frame (debug)
      // Keep previous buffers + virtual blocks alive if we grow during the frame so earlier
      // returned mapped pointers remain valid until the frame completes.
      struct Retired
      {
        std::unique_ptr<class ZVulkanBuffer> buffer;
        VmaVirtualBlock block = nullptr;
      };
      std::vector<Retired> retiredBuffers;
    } uploadArena;

    // Per-frame CPU->GPU uniform arena for dynamic UBO slices (host-visible,
    // persistently mapped); offsets are aligned to
    // minUniformBufferOffsetAlignment.
    struct UniformArena
    {
      std::unique_ptr<class ZVulkanBuffer> buffer; // host-visible, host-coherent
      void* mapped = nullptr; // persistent mapping
      size_t capacity = 0; // bytes
      size_t cursor = 0; // bump pointer
      size_t highWatermark = 0; // debug
    } uniformArena;
    // Shared per-frame dynamic offset for lighting UBO slice in the uniform arena
    vk::DeviceSize lightingDynOffset = 0;

    // DDP: per-frame resources for indirect-count early stop
    std::unique_ptr<class ZVulkanBuffer> ddpChangedFlag; // STORAGE | TRANSFER_DST
    std::unique_ptr<class ZVulkanBuffer> ddpIndirectCount; // STORAGE | INDIRECT_BUFFER | TRANSFER_DST
    // Removed host-visible args arena; device-local args only
    // Device-local arena for DDP indirect draw command payloads (args). Contents
    // are populated via upload arena + scheduleStaticCopy outside dynamic rendering.
    struct IndirectDeviceArena
    {
      std::unique_ptr<class ZVulkanBuffer> buffer; // INDIRECT_BUFFER | TRANSFER_DST (device-local)
      size_t capacity = 0;
      size_t cursor = 0;
      std::vector<std::unique_ptr<class ZVulkanBuffer>> retiredBuffers; // keep old buffers alive for frame
    } ddpArgsDevice;

    // Static device-local staging stats
    size_t staticBytesStaged = 0; // bytes staged to device-local this frame
    uint32_t staticStreamRestaged = 0; // number of restaged streams
    // Per-stream-type bytes staged (debug)
    size_t linesBytesStaged = 0;
    size_t fontsBytesStaged = 0;
    size_t meshesBytesStaged = 0;
    size_t spheresBytesStaged = 0;

    struct ScheduledCopy
    {
      enum class Usage
      {
        Vertex,
        Index,
        Indirect
      };
      vk::Buffer dst{};
      vk::DeviceSize dstOffset{0};
      UploadSlice src{};
      Usage usage = Usage::Vertex;
    };
    std::vector<ScheduledCopy> scheduledCopies;
  };

  void collectFrameTimings(FrameResources& frame);
  void flushScheduledCopies(vk::raii::CommandBuffer& cmd);
  void recordCpuScope(std::string_view label, double milliseconds);
  void ensureStaticArenas();
  void ensureUniformArena(FrameResources& frame);
  // DDP indirect-count: ensure per-frame buffers exist
  void ensureDDPGatingResources(FrameResources& frame);
  // DDP indirect-count: ensure shared compute pipeline exists
  void ensureDDPComputePipeline();
  size_t uniformAlignment() const;

public:
  // DDP indirect-count gating (device-side early stop for DDP peel)
  bool ddpIndirectCountEnabled() const;
  vk::Buffer ddpChangedFlagBuffer();
  vk::Buffer ddpIndirectCountBuffer();
  class ZVulkanBuffer* ddpChangedFlagBufferObj();
  class ZVulkanBuffer* ddpIndirectCountBufferObj();
  // Device-local indirect args arena helpers
  vk::Buffer ddpDeviceArgsBuffer();
  vk::DeviceSize ddpAllocDeviceArgsSlot(size_t bytes);
  void ddpResetForPass(vk::raii::CommandBuffer& cmd, bool firstPass);
  void ddpBarrierTransferToFrag(vk::raii::CommandBuffer& cmd);
  void ddpBarrierFragToCompute(vk::raii::CommandBuffer& cmd);
  void ddpBarrierComputeToIndirect(vk::raii::CommandBuffer& cmd);
  void ddpDispatchCountCompute(vk::raii::CommandBuffer& cmd);
  // Schedule a static copy for indirect args (upload arena -> device-local args buffer)
  void scheduleStaticCopyIndirect(vk::Buffer dst, vk::DeviceSize dstOffset, const UploadSlice& src);

  // Orchestrate DDP passes while the draw callback records the peel draw per pass.
  // The callback must record all necessary batches for the given pass into 'cmd'.
  void ddpOrchestrate(uint32_t maxPasses,
                      bool useIndirectCount,
                      const std::function<void(uint32_t, vk::raii::CommandBuffer&)>& drawPass);
  struct UniformSlice
  {
    vk::Buffer buffer{};
    vk::DeviceSize offset{0};
    void* mapped = nullptr;
    size_t size = 0;
  };
  UniformSlice suballocateUniform(size_t bytes, size_t alignment = 0);
  class ZVulkanBuffer& uniformArenaBuffer();
  // Dynamic offset of the shared per-frame lighting UBO slice
  [[nodiscard]] vk::DeviceSize frameSharedLightingOffset() const
  {
    CHECK(m_activeFrame != nullptr) << "frameSharedLightingOffset called without an active frame";
    return m_activeFrame->lightingDynOffset;
  }
  static size_t alignUp(size_t value, size_t alignment)
  {
    const size_t mask = alignment ? (alignment - 1) : 0;
    return alignment ? ((value + mask) & ~mask) : value;
  }

public:
  void addLineBytesStaged(size_t bytes)
  {
    if (m_activeFrame) {
      m_activeFrame->linesBytesStaged += bytes;
    }
  }
  void addFontBytesStaged(size_t bytes)
  {
    if (m_activeFrame) {
      m_activeFrame->fontsBytesStaged += bytes;
    }
  }
  void addMeshBytesStaged(size_t bytes)
  {
    if (m_activeFrame) {
      m_activeFrame->meshesBytesStaged += bytes;
    }
  }
  void addSphereBytesStaged(size_t bytes)
  {
    if (m_activeFrame) {
      m_activeFrame->spheresBytesStaged += bytes;
    }
  }

  ZVulkanDevice* m_sharedDevice = nullptr; // non-owning; provided by engine/scratch-pool
  ZVulkanDevice* m_frameDevice = nullptr; // tracked to rebuild frame resources on device changes
  std::vector<FrameResources> m_frames;
  std::unordered_map<void*, size_t> m_frameResourceMap;
  std::optional<ZVulkanFrameExecutor::ActiveFrame> m_activeFrameHandle;
  FrameResources* m_activeFrame = nullptr;
  bool m_frameRecording = false;
  uint32_t m_maxFramesInFlight = 2;
  float m_timestampPeriod = 1.0f;
  // Deliver first Vulkan frame to UI immediately after backend switch by
  // pumping the fence and executing deferred readback consumers once.
  bool m_pumpFenceAfterFirstSubmit = true;

  std::unique_ptr<ZVulkanLinePipelineContext> m_lineContext;
  std::unique_ptr<ZVulkanMeshPipelineContext> m_meshContext;
  std::unique_ptr<ZVulkanEllipsoidPipelineContext> m_ellipsoidContext;
  std::unique_ptr<ZVulkanSpherePipelineContext> m_sphereContext;
  std::unique_ptr<ZVulkanConePipelineContext> m_coneContext;
  std::unique_ptr<ZVulkanBackgroundPipelineContext> m_backgroundContext;
  std::unique_ptr<ZVulkanTextureCopyPipelineContext> m_textureCopyContext;
  std::unique_ptr<ZVulkanTextureBlendPipelineContext> m_textureBlendContext;
  std::unique_ptr<ZVulkanTextureDualPeelPipelineContext> m_textureDualPeelContext;
  std::unique_ptr<ZVulkanTextureWeightedAveragePipelineContext> m_textureWeightedAverageContext;
  std::unique_ptr<ZVulkanTextureWeightedBlendedPipelineContext> m_textureWeightedBlendedContext;
  std::unique_ptr<ZVulkanTextureGlowPipelineContext> m_textureGlowContext;
  std::unique_ptr<ZVulkanImgSlicePipelineContext> m_imgSliceContext;
  std::unique_ptr<ZVulkanImgRaycasterPipelineContext> m_imgRaycasterContext;
  std::unique_ptr<ZVulkanFontPipelineContext> m_fontContext;

  // Tracking for self-managed clear policy within a single processBatches call
  std::unordered_set<uint64_t> m_selfManagedClearKeys;

  // Shared fallback resources
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholder2D;
  std::optional<vk::raii::Sampler> m_defaultSampler;
  std::optional<vk::raii::Sampler> m_nearestClampSampler;

  struct SharedDescriptorLayouts
  {
    std::optional<vk::raii::DescriptorSetLayout> meshTextures;
    std::optional<vk::raii::DescriptorSetLayout> lighting;
    std::optional<vk::raii::DescriptorSetLayout> transforms;
    std::optional<vk::raii::DescriptorSetLayout> oitParams;
    std::optional<vk::raii::DescriptorSetLayout> dualTexturePlaceholder;
    std::optional<vk::raii::DescriptorSetLayout> empty;
  } m_sharedDescriptorLayouts;

  // Shared geometry: fullscreen quad VBO
  std::unique_ptr<ZVulkanBuffer> m_fullscreenQuadVbo;
  std::unique_ptr<ZVulkanBuffer> m_dummyVertexBuffer; // tiny VB for unused bindings

  // Device-local static arenas for geometry
  struct StaticArena
  {
    std::unique_ptr<class ZVulkanBuffer> vb; // device-local, eVertexBuffer|eTransferDst
    std::unique_ptr<class ZVulkanBuffer> ib; // device-local, eIndexBuffer|eTransferDst
    size_t vbCapacity = 0; // bytes
    size_t ibCapacity = 0; // bytes
    size_t vbOffset = 0; // bump cursor
    size_t ibOffset = 0; // bump cursor
    size_t vbHighWatermark = 0; // peak used for telemetry
    size_t ibHighWatermark = 0;
  } m_staticArena;

  // Helpers for descriptor arena lifecycle
  void ensureArenaOnFrame(FrameResources& frame);
  void applyPendingArenaReset(FrameResources& frame);
  // Drain post-fence callbacks whose fences have signaled.
  void drainPostFenceCallbacks();
  void scheduleArenaReset(FrameResources& frame);
  void vlogFrameRecyclingStats(const FrameResources& frame) const;

  void ensureSharedSamplers();
  void ensureFullscreenQuad();
  void ensureDummyVertexBuffer();
  vk::Buffer dummyVertexBuffer();

  // DDP compute pipeline (shared across frames)
  std::optional<vk::raii::DescriptorSetLayout> m_ddpCountSetLayout;
  std::optional<vk::raii::PipelineLayout> m_ddpCountPipelineLayout;
  std::optional<vk::raii::Pipeline> m_ddpCountPipeline;

  // Device feature gates (cached on device change)
  bool m_supportsDrawIndirectCount = false;
  bool m_supportsFragStoresAndAtomics = false;

  // Upload arena helpers moved to public API above

  // Stage 4: Readback ring buffers
  struct ReadbackSlot
  {
    std::unique_ptr<class ZVulkanBuffer> buffer;
    void* mapped = nullptr; // persistent mapping
    size_t capacity = 0; // bytes
    bool inUse = false; // associated with an in-flight frame
    // Optional tag for debugging
    const char* tag = "color";
  };
  std::vector<ReadbackSlot> m_readbackSlots; // shared across frames
  uint32_t m_readbackCursor = 0;

  // Post-fence callbacks keyed by the submission's raw fence handle.
  // We poll fence status and run ready callbacks early to reduce per-frame latency.
  std::vector<std::pair<VkFence, std::function<void()>>> m_postFenceCallbacks;

  // Ensure at least N slots exist and each has capacity >= minBytes
  void ensureReadbackSlots(size_t minBytes, uint32_t minSlots);
  // Acquire a free slot; returns index or -1 if none available
  int acquireReadbackSlot(size_t requiredBytes);
  // Mark slot free (after fence and consumer copy)
  void releaseReadbackSlot(int index);

  // TLS current backend pointer
  static thread_local Z3DRendererVulkanBackend* s_currentBackend;

  // If non-zero, beginRender will ensure the uniform arena capacity is at least this many bytes
  // before priming persistent descriptor sets. Reset to zero after use.
  size_t m_nextUniformMinCapacity = 0;

  struct PassBaseline
  {
    uint32_t descriptorSetsAllocated = 0;
    uint32_t overrideSetsAllocated = 0;
    size_t pipelinesBoundUnique = 0;
    uint32_t renderingSegmentsBegan = 0;
    uint32_t attachmentClears = 0;
    uint32_t attachmentLoads = 0;
    uint32_t descriptorWritesWhileRecording = 0;
    uint32_t boundSetRewriteAttempts = 0;
    vk::DeviceSize uploadHighWatermark = 0;
    uint64_t staticBytesStaged = 0;
    uint64_t readbackBytesCopied = 0;
    uint32_t readbackSlotsInFlight = 0;
  };

  struct PassScope
  {
    bool active = false;
    std::string label;
    std::chrono::steady_clock::time_point start;
    PassBaseline baseline{};
    uint64_t draws = 0;
    uint64_t layoutTransitions = 0;
    uint64_t layoutNoops = 0;
  };

  PassScope m_passScope{};

  // Submission index within a real-frame token
  std::unordered_map<uint64_t, uint32_t> m_submissionCursor;
};

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
