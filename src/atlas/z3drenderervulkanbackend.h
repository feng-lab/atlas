#pragma once

#include "z3drendererbackend.h"
#include "zglmutils.h"
#include "zvulkan.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"
#include "zcoro_spothooks.h"
// Expose a small awaiter type for "after fence" logic.
#include <folly/coro/Baton.h>
// Track detached post-fence tasks for teardown safety.
#include <folly/coro/AsyncScope.h>
// Public hook API uses folly coroutines directly.
#include <folly/coro/Task.h>
// Arena allocations for per-frame descriptor sets
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
// Attachment format helpers
#include "zvulkanrenderconversions.h"
#include "z3dtypes.h"
#include "zvulkanuniforms.h"

#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <folly/Executor.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
class ZVulkanTexturePPLLPipelineContext;
class ZVulkanTextureGlowPipelineContext;
class ZVulkanImgSlicePipelineContext;
class ZVulkanImgRaycasterPipelineContext;
class ZVulkanFontPipelineContext;
class ZVulkanBuffer;
class ZVulkanBindlessDescriptorSet;
class Z3DImg;
class Z3DTransferFunction;
class ZColorMap;

namespace vulkan {

// Compile-time uniform-arena budgeting:
// - Payloads that suballocate from the backend uniform arena must declare a
//   conservative upper bound of per-batch uniform bytes.
// - Call sites allocate via Z3DRendererVulkanBackend::suballocateUniformFor(...)
//   which statically requires a budget trait specialization for the payload type.
//
// This prevents "remember to update the estimator" drift: new uniform-allocating
// payloads fail to compile until they declare an estimation policy.
template<typename PayloadT>
struct UniformArenaBudgetTraits;

inline constexpr size_t alignUp(size_t value, size_t alignment)
{
  const size_t mask = alignment ? (alignment - 1) : 0;
  return alignment ? ((value + mask) & ~mask) : value;
}

template<typename PayloadT>
inline constexpr bool kHasUniformArenaBudgetTraits =
  requires(const PayloadT& payload, size_t uniformAlignment) {
    UniformArenaBudgetTraits<PayloadT>::estimateAdditionalBytes(payload, uniformAlignment);
  };

template<>
struct UniformArenaBudgetTraits<LinePayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const LinePayload&, size_t uniformAlignment)
  {
    static constexpr size_t kObjectBytes = sizeof(ObjectTransformsUBOStd140);
    static constexpr size_t kMaterialBytes = sizeof(MaterialUBOStd140);
    const size_t szObject = alignUp(kObjectBytes, uniformAlignment);
    const size_t szMaterial = alignUp(kMaterialBytes, uniformAlignment);
    return szObject + szMaterial;
  }
};

template<>
struct UniformArenaBudgetTraits<MeshPayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const MeshPayload& payload, size_t uniformAlignment)
  {
    static constexpr size_t kObjectBytes = sizeof(ObjectTransformsUBOStd140);
    static constexpr size_t kMaterialBytes = sizeof(MaterialUBOStd140);
    const size_t szObject = alignUp(kObjectBytes, uniformAlignment);
    const size_t szMaterial = alignUp(kMaterialBytes, uniformAlignment);
    const bool drawSurface = payload.wireframeMode != MeshPayload::WireframeMode::OnlyWireframe;
    const bool drawWireframe = payload.wireframeMode != MeshPayload::WireframeMode::NoWireframe;
    const size_t drawsPerMesh = static_cast<size_t>(drawSurface) + static_cast<size_t>(drawWireframe);
    const size_t meshes = payload.meshes.size();
    return szObject + (meshes * drawsPerMesh * szMaterial);
  }
};

template<>
struct UniformArenaBudgetTraits<SpherePayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const SpherePayload&, size_t uniformAlignment)
  {
    (void)uniformAlignment;
    // Spheres use persistent uniform slices for per-stream object/material UBOs.
    // (Frame UBO is allocated once in beginRender().)
    return 0u;
  }
};

template<>
struct UniformArenaBudgetTraits<EllipsoidPayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const EllipsoidPayload&, size_t uniformAlignment)
  {
    static constexpr size_t kObjectBytes = sizeof(ObjectTransformsUBOStd140);
    static constexpr size_t kMaterialBytes = sizeof(MaterialUBOStd140);
    const size_t szObject = alignUp(kObjectBytes, uniformAlignment);
    const size_t szMaterial = alignUp(kMaterialBytes, uniformAlignment);
    return szObject + szMaterial;
  }
};

template<>
struct UniformArenaBudgetTraits<ConePayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const ConePayload&, size_t uniformAlignment)
  {
    (void)uniformAlignment;
    // Cones use persistent uniform slices for per-stream object/material UBOs.
    // (Frame UBO is allocated once in beginRender().)
    return 0u;
  }
};

template<>
struct UniformArenaBudgetTraits<ImgRaycasterPayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const ImgRaycasterPayload& payload, size_t uniformAlignment);
};

template<>
struct UniformArenaBudgetTraits<ImgSlicePayload>
{
  [[nodiscard]] static size_t estimateAdditionalBytes(const ImgSlicePayload& payload, size_t uniformAlignment);
};

} // namespace vulkan

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

  // ---------------------------------------------------------------------------
  // Linear-script priming (beginRender pre-record actions)
  // ---------------------------------------------------------------------------
  // Higher-level orchestration (e.g. ZVulkanLinearScript) can enqueue opaque
  // actions that must run inside beginRender() before descriptor priming and
  // command buffer recording begins.
  //
  // This keeps call sites explicit ("run these PPLL priming steps before
  // recording") without teaching the orchestration layer about backend-specific
  // resource kinds.
  struct BeginRenderPreRecordAction
  {
    std::string label;
    std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)> fn;
  };

  void setPendingBeginRenderPreRecordActions(std::vector<BeginRenderPreRecordAction> actions,
                                             std::string_view debugLabel = {});

  // ---------------------------------------------------------------------------
  // Linear-script stats (pre_cpu attribution)
  // ---------------------------------------------------------------------------
  // ZVulkanLinearScript performs some CPU work before opening a Vulkan frame
  // (uniform-arena sizing, node bookkeeping, etc). These stats are captured at
  // the call site and forwarded to beginRender() so they can be included in the
  // per-frame perf summary line for easy diffing across runs.
  struct BeginRenderScriptStats
  {
    double uniformHintMs = 0.0;
    size_t uniformHintBytes = 0;
    uint32_t nodeCount = 0;
    uint32_t rasterNodeCount = 0;
    uint32_t replayNodeCount = 0;
    uint32_t commandsNodeCount = 0;
    uint32_t preRecordNodeCount = 0;
    uint32_t batchCount = 0;
  };

  void setPendingBeginRenderScriptStats(BeginRenderScriptStats stats);

  // Estimate uniform-arena bytes that are always consumed at beginRender()
  // regardless of which batches are recorded (e.g. frame-global lighting UBO
  // slices). This is used by higher-level scheduling (linear scripts / segment
  // managers) to pre-size the arena before opening a Vulkan frame.
  [[nodiscard]] size_t estimateFrameUniformOverheadBytes();

  // Conservative upper-bound estimate of additional uniform-arena bytes
  // required to execute the given CPU batches (e.g., dynamic transforms/material
  // UBO slices). This excludes the always-on per-frame overhead returned by
  // estimateFrameUniformOverheadBytes().
  [[nodiscard]] size_t estimateAdditionalUniformBytesForBatches(const RendererCPUState& state);

  // Hint the minimum uniform arena capacity (bytes) for the next begun frame.
  // The backend consumes this hint in ensureUniformArena() at beginRender().
  void hintNextUniformArenaMinCapacity(size_t bytes);

  // Expose the device's dynamic UBO alignment (minUniformBufferOffsetAlignment)
  // for CPU-side budgeting during batch capture/scheduling.
  [[nodiscard]] size_t uniformAlignmentForEstimates() const;

  // Monotonic revision that changes only when the injected Vulkan device
  // pointer changes. Used by higher-level schedulers to invalidate CPU-side
  // cached values derived from VkPhysicalDevice limits.
  [[nodiscard]] uint64_t deviceRevision() const noexcept
  {
    return m_deviceRevision;
  }

  [[nodiscard]] bool supportsCommandLists() const override;

  RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease) override;

  // Shared fallback resources used across pipeline contexts to avoid redundant
  // tiny texture creation.
  ZVulkanTexture& defaultPlaceholderTexture2D();

  // Shared descriptor set layouts reused across pipeline contexts.
  vk::DescriptorSetLayout bindlessSampledImageDescriptorSetLayout();
  vk::DescriptorSetLayout lightingDescriptorSetLayout();
  vk::DescriptorSetLayout transformDescriptorSetLayout();
  vk::DescriptorSetLayout oitDescriptorSetLayout();
  vk::DescriptorSetLayout emptyDescriptorSetLayout();
  // Image renderer helper descriptor set layouts:
  // - set 1 (binding 0): indices UBO (dynamic)
  // - set 2 (binding 2): page-data UBO (dynamic)
  vk::DescriptorSetLayout imgIndicesDescriptorSetLayout();
  vk::DescriptorSetLayout imgPageDataDescriptorSetLayout();

  // Shared per-frame-slot descriptor sets allocated from the persistent
  // descriptor pool. These are updated only at the frame completion safe point
  // (beginRender after slot acquisition) so command buffer recording never
  // performs descriptor writes.
  [[nodiscard]] vk::DescriptorSet sharedEmptyDescriptorSet() const;
  [[nodiscard]] vk::DescriptorSet sharedLightingDescriptorSet() const;
  [[nodiscard]] uint64_t sharedLightingDescriptorSetGeneration() const;
  // Transforms descriptor set variants:
  // - Uniform: all bindings point at the per-frame uniform arena buffer.
  // - Persistent: bindings 1/2 point at the persistent uniform arena buffer.
  [[nodiscard]] vk::DescriptorSet sharedTransformsDescriptorSetUniform() const;
  [[nodiscard]] uint64_t sharedTransformsDescriptorSetUniformGeneration() const;
  [[nodiscard]] vk::DescriptorSet sharedTransformsDescriptorSetPersistent() const;
  [[nodiscard]] uint64_t sharedTransformsDescriptorSetPersistentGeneration() const;
  [[nodiscard]] vk::DescriptorSet sharedOITDescriptorSet() const;
  [[nodiscard]] uint64_t sharedOITDescriptorSetGeneration() const;
  // Ring slot index used to select the shared OIT descriptor set for the
  // current submission (0 when PPLL ring is inactive).
  [[nodiscard]] uint32_t sharedOITDescriptorSetRingIndex() const noexcept;
  [[nodiscard]] vk::DescriptorSet sharedImgIndicesDescriptorSet() const;
  [[nodiscard]] vk::DescriptorSet sharedImgPageDataDescriptorSet() const;

  // Bindless sampled-image table (descriptor indexing). This is a per-frame-slot
  // descriptor set that is bound at set=0 for Atlas graphics pipelines.
  //
  // Mutations (descriptor writes) must happen before command-buffer recording
  // begins for the frame-slot; lookups are allowed during recording.
  [[nodiscard]] vk::DescriptorSet bindlessSampledImageDescriptorSet() const;
  [[nodiscard]] uint64_t bindlessSampledImageDescriptorSetGeneration() const;
  // Convenience wrappers that choose bindless table based on the texture's
  // view type + format (float vs unsigned integer). Sampler state is selected
  // in shaders via bindless.glslinc's immutable sampler bindings.
  //
  // Policy:
  // - Unsigned integer formats use UTexture* tables.
  // - All other sampled images use Texture* tables.
  [[nodiscard]] uint32_t bindlessRegisterSampledImageAuto(class ZVulkanTexture& texture,
                                                          std::string_view debugLabel = {});
  [[nodiscard]] uint32_t bindlessLookupSampledImageAutoOrCrash(class ZVulkanTexture& texture,
                                                               std::string_view debugLabel = {}) const;

  // Debug: dump bindless descriptor entry state for a sampled-image texture.
  // This helps diagnose silent correctness failures where a shader samples a
  // stale or unintended bindless entry while remaining Vulkan-valid.
  void debugDumpBindlessSampledImageEntry(class ZVulkanTexture& texture, std::string_view label = {}) const;

  // Pre-record hook used by ZVulkanLinearScript to build bindless descriptor
  // state once per submission (before command-buffer recording begins).
  void bindlessPreRegisterExternalSampledImageUses(std::span<const ExternalImageUseDesc> uses,
                                                   std::string_view debugLabel = {});

  struct BindlessFontAtlasPixelsDesc
  {
    const uint8_t* pixelsBGRA8 = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  // Pre-record hook used by ZVulkanLinearScript for the Vulkan font renderer.
  // Fonts supply CPU atlas pixels instead of a VkImage handle; the backend must
  // upload those atlases and register them into the bindless tables before
  // command-buffer recording begins.
  void bindlessPreRegisterFontAtlasPixels(std::span<const BindlessFontAtlasPixelsDesc> atlases,
                                          std::string_view debugLabel = {});

  // Pre-record hooks used by ZVulkanLinearScript for image renderers that own
  // internal Vulkan textures (volume uploads, LUTs, paging tables).
  // These must run before command-buffer recording begins so bindless descriptor
  // mutations never happen while recording.
  void bindlessPreWarmupImgRaycaster(Z3DImg* image,
                                     const std::vector<Z3DTransferFunction*>* transferFunctions,
                                     std::span<const size_t> channels,
                                     bool wants2D,
                                     bool wantsVolume3D,
                                     bool wantsPaging,
                                     std::string_view debugLabel = {});

  void bindlessPreWarmupImgSlice(Z3DImg* image,
                                 const std::vector<const ZColorMap*>* colormaps,
                                 std::span<const size_t> channels,
                                 bool wantsVolume3D,
                                 bool wantsColormap,
                                 bool wantsPaging,
                                 std::string_view debugLabel = {});

  // Pre-record hooks for progressive block-ID compaction workflows. These
  // allocate and write per-frame descriptor sets so compute passes never update
  // descriptors while recording.
  void bindlessPrePrimeImgRaycasterBlockIdCompaction(
    const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
    uint32_t effectiveAttachmentCount,
    std::string_view debugLabel = {});

  void bindlessPrePrimeImgSliceBlockIdCompaction(
    const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
    uint32_t sliceCount,
    uint32_t sliceIndex,
    std::string_view debugLabel = {});

  void preBackendSwitch() override;

  // Pass-scope aggregation hooks
  void beginPassScope(std::string_view label) override;
  void endPassScope() override;

  // Per-draw and transition notifications (used by recorders)
  void notifyDrawSubmitted();
  void notifyLayoutTransition(bool wasNoop);
  void notifyBufferBarrier(bool wasNoop);

  ZVulkanDevice& device();

  const ZVulkanDevice& device() const;

  vk::raii::CommandBuffer& commandBuffer();

  const vk::raii::CommandBuffer& commandBuffer() const;

  // Stable key identifying the active frame slot (used by pipeline contexts that
  // need per-frame scratch buffers beyond descriptor arenas).
  // Returns null when no active frame is recording.
  [[nodiscard]] void* activeFrameKey() const;

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
  struct StaticAllocationHandle
  {
    // Opaque pointer to the owning static-arena segment. This is managed by the
    // backend and must not be dereferenced by callers.
    void* segment = nullptr;
    // VMA virtual allocation handle within the segment.
    VmaVirtualAllocation allocation = nullptr;
    // Requested byte size (used for accounting on free).
    size_t size = 0;
    bool isIndexBuffer = false;

    explicit operator bool() const
    {
      return segment != nullptr && allocation != nullptr && size > 0;
    }
  };

  struct StaticSlice
  {
    vk::Buffer buffer{};
    vk::DeviceSize offset = 0;
    size_t size = 0;
    StaticAllocationHandle alloc{};

    explicit operator bool() const
    {
      return static_cast<bool>(buffer) && size > 0 && static_cast<bool>(alloc);
    }
  };

  // Allocate space in the device-local vertex/index arenas. Returns an empty
  // slice on allocation failure.
  StaticSlice allocateStaticVB(size_t bytes, size_t alignment = 16);
  StaticSlice allocateStaticIB(size_t bytes, size_t alignment = 4);

  // Release a previously allocated static slice. This is safe to call even when
  // the slice's backing segment is still pinned by an in-flight submission:
  // frees are deferred until the segment is unpinned to avoid reusing the same
  // memory range before the GPU finishes reading it.
  void releaseStaticSlice(StaticSlice& slice);

  // Pin the slice's backing segment for the current submission so that evictions
  // cannot reuse its memory until the submission fence signals. Must be called
  // for any draw/copy that consumes static slices.
  void pinStaticSliceForActiveSubmission(const StaticSlice& slice);

  // Return a stable identity for static-arena buffers (device-local VB/IB
  // segments). This is used by per-draw secondary command-buffer caches to
  // avoid false cache hits when VkBuffer handles are reused after destruction.
  //
  // Returns 0 when the buffer does not belong to the backend static arenas.
  [[nodiscard]] uint64_t staticArenaSegmentIdForBuffer(vk::Buffer buffer) const;

  // Request eviction of all cached static geometry for a given streamKey.
  // This is processed on the render thread during beginRender().
  void requestEvictStream(uint64_t streamKey);

  // Record a copy from an upload slice into a device-local buffer at dst.
  // Inserts a barrier making the data visible to the appropriate pipeline stage.
  void stageCopy(vk::Buffer dst, vk::DeviceSize dstOffset, const UploadSlice& src, bool isIndexBuffer);

  // Stage 2: Per-frame descriptor arena API
  // Allocate a descriptor set from the current frame's arena. Returns null when
  // no active frame exists (e.g., zero-sized viewport), and logs at VLOG(1).
  std::unique_ptr<ZVulkanDescriptorSet> allocateFrameDescriptorSet(vk::DescriptorSetLayout layout);
  // Allocate a descriptor set that persists for the lifetime of the current
  // frame-slot (ActiveFrame::key()). These sets are allocated from a per-slot
  // descriptor pool that is *not* reset every submission, enabling cached
  // secondary command buffers to bind stable descriptor-set handles across
  // frames.
  std::unique_ptr<ZVulkanDescriptorSet> allocatePersistentDescriptorSet(vk::DescriptorSetLayout layout);

  // Await the active submission fence (if any) as a coroutine.
  //
  // This returns an awaiter object that captures the current active submission
  // (if any) *at call time*. The await itself can happen later (e.g., from a
  // queued coroutine) without relying on mutable backend state.
  //
  // - When there is an active frame submission, the awaiter suspends until its
  //   fence is observed complete by the frame executor.
  // - When no active submission exists, the awaiter conservatively waits for
  //   all in-flight submissions to complete (teardown safety).
  //
  // Must be created on the rendering thread.
  class ActiveSubmissionFenceAwaiter final
  {
  public:
    ActiveSubmissionFenceAwaiter() = default;

    [[nodiscard]] folly::coro::Baton::WaitOperation operator co_await() const noexcept
    {
      // If this triggers, the awaiter was default-constructed or moved-from.
      CHECK(m_state != nullptr) << "ActiveSubmissionFenceAwaiter used without a state";
      return m_state->baton.operator co_await();
    }

  private:
    struct State
    {
      explicit State(bool signalled) noexcept
        : baton(signalled)
      {}
      folly::coro::Baton baton;
    };

    explicit ActiveSubmissionFenceAwaiter(std::shared_ptr<State> state)
      : m_state(std::move(state))
    {}

    std::shared_ptr<State> m_state;
    friend class Z3DRendererVulkanBackend;
  };

  [[nodiscard]] ActiveSubmissionFenceAwaiter awaitActiveSubmissionFence(std::string_view debugLabel = {});
  // Executor-safe wrapper for ActiveSubmissionFenceAwaiter.
  //
  // Important: folly::coro::Baton resumes awaiters inline on the thread that calls
  // baton.post(). Since the frame-executor completion callback may run on the
  // render thread (or another thread), directly `co_await`-ing the low-level
  // awaiter can violate executor affinity.
  //
  // This helper restores executor affinity by re-hopping to the coroutine's
  // current executor after the baton is released.
  [[nodiscard]] static folly::coro::Task<void> waitActiveSubmissionFence(ActiveSubmissionFenceAwaiter fence);

private:
  enum class FrameHookSpot
  {
    AfterFrameCompletionSafePoint
  };

  // Fire-and-forget tasks that must not outlive the backend.
  // This is used for post-fence work that should NOT block the render thread
  // via the frame completion safe-point barrier, but still needs teardown safety
  // (backend switching / renderer destruction).
  folly::coro::AsyncScope m_detachedTaskScope;
  bool m_detachedTaskScopeJoined = false;

public:
  using AfterFrameCompletionHook = folly::Function<folly::coro::Task<void>(Z3DRendererVulkanBackend&)>;

  // Register a coroutine hook to run at the "current frame completion safe point"
  // (applyPendingArenaReset) for the active frame-slot.
  //
  // This is the preferred API for post-fence work that needs correct ordering
  // with respect to the backend's frame completion safe point (after fence-gated
  // completion callbacks, descriptor arena resets, and scratch releases) while
  // allowing the hook body to be written
  // linearly with co_await and to run on the call-site chosen executor.
  //
  // Contract:
  // - Must be called on the rendering thread.
  // - Requires an active frame-slot recording context.
  // - Registration during the completion safe point is forbidden (CHECK) to
  //   avoid re-entrant "safe-point scheduling" that is easy to reason about
  //   incorrectly. If post-frame work is needed, register it during recording.
  // - Does not silently block-wait; teardown code must explicitly call
  //   flushForTeardown()/engine drain helpers.
  void registerAfterCurrentFrameCompletionHook(folly::Executor::KeepAlive<> ex,
                                               AfterFrameCompletionHook hook,
                                               std::string_view debugLabel = {});

  // Start a fire-and-forget coroutine task, tracked so flushForTeardown() waits
  // for completion.
  //
  // Unlike registerAfterCurrentFrameCompletionHook(), this does NOT execute at
  // the frame completion safe point and does NOT block frame-slot reuse.
  //
  // Intended for post-fence work that is safe to run on another executor
  // (debug readback analysis, CPU-side decoding, file I/O, etc.).
  //
  // IMPORTANT (coroutine lambda lifetime):
  // Do not pass tasks created by invoking a capturing coroutine lambda like
  // `[...]() -> Task<void> { ... }()`. The coroutine's implicit `this` may point
  // at the lambda closure object, which would be destroyed at the end of the
  // full-expression, leading to a use-after-free on resume.
  //
  // Use `folly::coro::co_invoke()` instead so the callable outlives the task:
  // `spawnDetachedTask(ex, folly::coro::co_invoke([captures...]() -> Task<void> { ... }), "label")`.
  void
  spawnDetachedTask(folly::Executor::KeepAlive<> ex, folly::coro::Task<void> task, std::string_view debugLabel = {});
  void
  spawnDetachedTask(std::shared_ptr<folly::Executor> ex, folly::coro::Task<void> task, std::string_view debugLabel = {})
  {
    CHECK(ex != nullptr) << "spawnDetachedTask requires a valid executor" << (debugLabel.empty() ? "" : " (")
                         << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
    spawnDetachedTask(folly::getKeepAliveToken(ex.get()), std::move(task), debugLabel);
  }
  void spawnDetachedTask(folly::Executor* ex, folly::coro::Task<void> task, std::string_view debugLabel = {})
  {
    CHECK(ex != nullptr) << "spawnDetachedTask requires a valid executor" << (debugLabel.empty() ? "" : " (")
                         << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
    spawnDetachedTask(folly::getKeepAliveToken(ex), std::move(task), debugLabel);
  }

  // Teardown helper: wait for all in-flight frame submissions and flush any
  // fence-gated callbacks (notably residency unpins).
  // This is intended to run before destroying resources that may still be pinned
  // by an earlier submission.
  void flushForTeardown(std::string_view reason = {}) override;

  // Pin a texture in the device residency manager for the lifetime of the
  // current GPU submission. This prevents eviction of in-flight resources.
  void pinTextureForActiveSubmission(class ZVulkanTexture* texture);
  void notifyPipelineCreated();
  void notifyPipelineBound(vk::Pipeline pipeline);

  struct DrawSecondarySignatureMismatchMask
  {
    static constexpr uint32_t kPipeline = 1u << 0;
    static constexpr uint32_t kLayout = 1u << 1;
    static constexpr uint32_t kBaseDescriptorSets = 1u << 2;
    static constexpr uint32_t kBaseDescriptorGenerations = 1u << 3;
    static constexpr uint32_t kOitDescriptorPresence = 1u << 4;
    static constexpr uint32_t kOitDescriptorSet = 1u << 5;
    static constexpr uint32_t kOitResourcesRevision = 1u << 6;
    static constexpr uint32_t kOitDescriptorGeneration = 1u << 17;
    static constexpr uint32_t kDynamicOffsets = 1u << 7;
    static constexpr uint32_t kVertexBuffers = 1u << 8;
    static constexpr uint32_t kVertexOffsets = 1u << 9;
    static constexpr uint32_t kIndexState = 1u << 10;
    static constexpr uint32_t kCounts = 1u << 11;
    static constexpr uint32_t kPushConstants = 1u << 12;
    static constexpr uint32_t kViewport = 1u << 13;
    static constexpr uint32_t kScissor = 1u << 14;
    static constexpr uint32_t kVertexBufferLifetime = 1u << 15;
    static constexpr uint32_t kIndexBufferLifetime = 1u << 16;
  };

  void notifyDrawSecondaryCacheAttempt();
  void notifyDrawSecondaryCacheKeyFound();
  void notifyDrawSecondaryCacheSignatureMismatch();
  void notifyDrawSecondaryCacheSignatureMismatchMask(uint32_t mask);
  void notifyDrawSecondaryCacheHit();
  void notifyDrawSecondaryCacheBuild();
  void notifyDrawSecondaryCacheExecute();
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

  // Mark the currently recording submission as requiring endRender() to wait
  // for the active submission fence and then enter the backend's "frame
  // completion safe point" (applyPendingArenaReset) before returning.
  //
  // This is the preferred primitive for GPU->CPU control-flow boundaries that
  // gate the next progressive stage (e.g. block-ID compaction uploads) and for
  // script readback boundaries. It is intentionally independent of whether the
  // submission requested any end-of-frame readbacks.
  void requireCompletionSafePointWaitForActiveSubmission(std::string_view debugLabel = {});

  // Poll completed frame fences and enter the backend's frame completion safe
  // point for any finished slots. This is non-blocking and is intended to be
  // called from the rendering thread event loop when the engine is idle so
  // queued readback consumers can run even if no further frames are rendered.
  void pollCompletionsAndPumpSafePoints() override;
  [[nodiscard]] bool hasInFlightFrames() const override;
  [[nodiscard]] uint32_t inFlightCount() const override;
  [[nodiscard]] uint32_t maxFramesInFlight() const override;

  // Stage 4: Async Readback API (offscreen)
  // Enqueue end-of-frame GPU->CPU copies into a host-visible staging ring, then
  // hand out coroutine-friendly tickets for post-fence consumption.

  struct EndOfFrameColorReadbackTicket
  {
    ActiveSubmissionFenceAwaiter fence;
    const void* mapped = nullptr;
    size_t bytes = 0;
    vk::Format format = vk::Format::eUndefined;
    glm::uvec2 size{0u, 0u};
    // Metadata describing the captured view.
    vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
    uint32_t arrayLayer = 0;
    std::function<void()> releaseSlot;

    // Await the submission fence, copy bytes into an owned vector, and release
    // the staging slot back to the backend.
    [[nodiscard]] folly::coro::Task<std::vector<uint8_t>> awaitOwnedBytes();
  };

  // Stage 4b: Safe host-copy readback tickets (debug utilities)
  //
  // Some debug utilities want to run CPU work (decoding, file I/O) on detached
  // background tasks, but must never touch backend-owned Vulkan staging buffers
  // (mapped pointers / slot lifetimes) from those tasks.
  //
  // This ticket type copies the staging buffer into call-site owned host memory
  // after the submission fence signals, releases the staging slot, and only
  // then returns to awaiters.
  class EndOfFrameHostImageReadbackTicket final
  {
  public:
    EndOfFrameHostImageReadbackTicket() = default;
    EndOfFrameHostImageReadbackTicket(const EndOfFrameHostImageReadbackTicket&) = delete;
    EndOfFrameHostImageReadbackTicket& operator=(const EndOfFrameHostImageReadbackTicket&) = delete;
    EndOfFrameHostImageReadbackTicket(EndOfFrameHostImageReadbackTicket&&) noexcept = default;
    EndOfFrameHostImageReadbackTicket& operator=(EndOfFrameHostImageReadbackTicket&&) noexcept = default;
    ~EndOfFrameHostImageReadbackTicket() = default;

    // Await the submission fence, copy into hostBytes, and release the staging
    // slot. After this resumes, data() and dataBytes() are ready to consume
    // from any thread/executor.
    [[nodiscard]] folly::coro::Task<void> awaitReady();

    [[nodiscard]] const uint8_t* data() const
    {
      return m_hostBytes ? m_hostBytes->data() : nullptr;
    }
    [[nodiscard]] size_t dataBytes() const
    {
      return m_hostBytes ? m_hostBytes->size() : 0u;
    }

    vk::Format format = vk::Format::eUndefined;
    glm::uvec2 size{0u, 0u};
    vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
    uint32_t arrayLayer = 0u;

  private:
    EndOfFrameColorReadbackTicket m_stagingTicket;
    std::shared_ptr<std::vector<uint8_t>> m_hostBytes;
    std::shared_ptr<void> m_keepAlive;
    bool m_ready = false;
    friend class Z3DRendererVulkanBackend;
  };

  // Coroutine-friendly readback primitive.
  //
  // Contract:
  // - Must be called on the rendering thread with an active frame.
  // - The returned mapped pointer is valid only after co_await fence.
  // - The returned mapped pointer remains valid until releaseSlot() is invoked.
  // - releaseSlot may be invoked from any thread; it routes the actual slot
  //   release back to the render thread.
  [[nodiscard]] EndOfFrameColorReadbackTicket
  requestEndOfFrameColorReadbackTicket(class ZVulkanTexture& src, Z3DEye eye, std::string_view debugLabel = {});

  // Like requestEndOfFrameColorReadbackTicket(), but allows selecting a specific
  // array layer and aspect (e.g. depth attachment readback).
  //
  // Contract:
  // - Must be called on the rendering thread with an active frame.
  // - aspectMask must be compatible with src.format() (validated by transitionLayout()).
  // - The returned mapped pointer is valid only after co_await fence.
  // - The returned mapped pointer remains valid until releaseSlot() is invoked.
  [[nodiscard]] EndOfFrameColorReadbackTicket requestEndOfFrameImageReadbackTicket(class ZVulkanTexture& src,
                                                                                   Z3DEye eye,
                                                                                   uint32_t arrayLayer,
                                                                                   vk::ImageAspectFlags aspectMask,
                                                                                   std::string_view debugLabel = {});

  // Debug utility readback primitive: enqueue GPU->staging copy for src, then
  // copy into ticket-owned host memory after the submission fence signals
  // (awaitReady()), then release the staging slot.
  //
  // Contract:
  // - Must be called on the rendering thread with an active frame.
  // - keepAlive is an optional shared_ptr to keep arbitrary owner state alive
  //   (e.g. scratch leases) until the fence signals and the host copy is complete.
  [[nodiscard]] EndOfFrameHostImageReadbackTicket
  requestEndOfFrameImageReadbackToHostTicket(class ZVulkanTexture& src,
                                             Z3DEye eye,
                                             uint32_t arrayLayer,
                                             vk::ImageAspectFlags aspectMask,
                                             std::string_view debugLabel = {});

  [[nodiscard]] EndOfFrameHostImageReadbackTicket
  requestEndOfFrameImageReadbackToHostTicket(class ZVulkanTexture& src,
                                             Z3DEye eye,
                                             uint32_t arrayLayer,
                                             vk::ImageAspectFlags aspectMask,
                                             std::shared_ptr<void> keepAlive,
                                             std::string_view debugLabel = {});

  class EndOfFrameBufferReadbackTicket final
  {
  public:
    EndOfFrameBufferReadbackTicket() = default;
    EndOfFrameBufferReadbackTicket(const EndOfFrameBufferReadbackTicket&) = delete;
    EndOfFrameBufferReadbackTicket& operator=(const EndOfFrameBufferReadbackTicket&) = delete;
    EndOfFrameBufferReadbackTicket(EndOfFrameBufferReadbackTicket&&) noexcept = default;
    EndOfFrameBufferReadbackTicket& operator=(EndOfFrameBufferReadbackTicket&&) noexcept = default;
    ~EndOfFrameBufferReadbackTicket() = default;

    // Await the submission fence, copy bytes into an owned vector, and release
    // the staging slot back to the backend.
    [[nodiscard]] folly::coro::Task<std::vector<uint8_t>> awaitOwnedBytes();

    // Await the submission fence, copy bytes into caller-provided storage, and
    // release the staging slot back to the backend.
    [[nodiscard]] folly::coro::Task<void> awaitCopyTo(void* dst, size_t dstBytes);

    // Await the submission fence and release the staging slot back to the
    // backend without copying. Useful for cancellation paths that must drop
    // pending CPU work but still release staging slots.
    [[nodiscard]] folly::coro::Task<void> awaitAndDiscard();

  private:
    ActiveSubmissionFenceAwaiter m_fence;
    const void* m_mapped = nullptr;
    size_t m_bytes = 0;
    std::function<void()> m_releaseSlot;
    friend class Z3DRendererVulkanBackend;
  };

  [[nodiscard]] EndOfFrameBufferReadbackTicket requestEndOfFrameBufferReadbackTicket(class ZVulkanBuffer& src,
                                                                                     vk::DeviceSize srcOffset,
                                                                                     size_t bytes,
                                                                                     std::string_view debugLabel = {});

  // Telemetry hook: record a perf-frame-start → host-ready latency sample for
  // the submission that just reached the backend's frame completion safe point.
  //
  // Contract:
  // - Must be called from an AfterFrameCompletionSafePoint hook (i.e. while the
  //   backend is executing applyPendingArenaReset()).
  // - This hook may execute during teardown from non-render threads (e.g.
  //   flushForTeardown()), so do not require render-thread TLS state here.
  void recordAllMsForCompletionSafePoint(double milliseconds);

  // GPU timestamp scopes (public so pipeline contexts can instrument hot paths)
  std::optional<size_t> beginGpuScope(std::string_view label);
  void endGpuScope(size_t token);

private:
  friend class Z3DRendererBase;
  void ensureDevice();
  void resetFrameResources();
  void ensureDefaultPlaceholders();
  void ensureSharedDescriptorLayouts();
  struct FrameResources;
  FrameResources& ensureFrameResourcesForKey(void* key);
  void ensureSharedDescriptorSetsOnFrame(FrameResources& frame);
  struct GpuScopeRecord
  {
    std::string label;
    uint32_t startQuery = 0;
    uint32_t endQuery = 0;
    bool isPassScope = false; // true for top-level pass scopes (ZVulkanLinearScript / beginPassScope)
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
    // Snapshot of Z3DRendererBase::currentRenderPassIsProgressive() at beginRender().
    // Used to derive the default end-of-frame readback wait policy for this submission.
    bool progressivePassHint = true;
    uint32_t nextQuery = 0;

    std::chrono::steady_clock::time_point cpuStart;
    std::chrono::steady_clock::time_point cpuEnd;
    // Time between perf-frame start (Z3DRenderGlobalState::beginNewPerfFrameToken)
    // and the start of this submission's CPU encode window (cpuStart).
    //
    // This captures engine/filter overhead, Vulkan frame-slot acquisition, safe
    // point pumping, and any other per-frame work that occurs before the backend
    // starts its measured CPU scope for this submission.
    std::optional<double> preCpuStartMs;

    // beginRender() preamble time: duration of Z3DRendererVulkanBackend::beginRender()
    // up to (but not including) cpuStart. This is a subset of pre_cpu.
    double beginRenderPreambleMs = 0.0;

    // Linear-script stats forwarded from ZVulkanLinearScript for this submission.
    double scriptUniformHintMs = 0.0;
    size_t scriptUniformHintBytes = 0;
    uint32_t scriptNodeCount = 0;
    uint32_t scriptRasterNodeCount = 0;
    uint32_t scriptReplayNodeCount = 0;
    uint32_t scriptCommandsNodeCount = 0;
    uint32_t scriptPreRecordNodeCount = 0;
    uint32_t scriptBatchCount = 0;

    // Descriptor arena (per-frame)
    std::unique_ptr<ZVulkanDescriptorPool> descriptorPool; // reset only after fence signal
    // Descriptor arena that persists for the lifetime of this frame-slot. This
    // is intentionally not reset each submission so cached command buffers can
    // keep binding the same descriptor-set handles across frames.
    std::unique_ptr<ZVulkanDescriptorPool> persistentDescriptorPool;
    // Per-frame-slot bindless sampled-image tables (descriptor indexing). These
    // descriptor-set handles remain stable for the lifetime of the frame-slot
    // and are updated only after the slot's previous submission completes.
    std::unique_ptr<ZVulkanBindlessDescriptorSet> bindlessSampledImages;
    // Shared per-frame-slot descriptor sets used across pipeline contexts to
    // avoid per-submission descriptor-set churn. These are allocated from the
    // persistent descriptor pool and updated only at beginRender() (after the
    // frame completion safe point for this slot).
    std::unique_ptr<ZVulkanDescriptorSet> sharedEmpty;
    std::unique_ptr<ZVulkanDescriptorSet> sharedLighting;
    std::unique_ptr<ZVulkanDescriptorSet> sharedTransformsUniform;
    std::unique_ptr<ZVulkanDescriptorSet> sharedTransformsPersistent;
    // Shared OIT descriptor sets are tracked per PPLL ring slot so we can
    // bind a stable VkDescriptorSet handle for cached secondary command buffers
    // without rewriting descriptors every frame-token (which would invalidate
    // previously recorded secondary command buffers and trigger validation
    // errors like VUID-vkCmdExecuteCommands-pCommandBuffers-00089).
    std::vector<std::unique_ptr<ZVulkanDescriptorSet>> sharedOITByRing;
    std::unique_ptr<ZVulkanDescriptorSet> sharedImgIndices;
    std::unique_ptr<ZVulkanDescriptorSet> sharedImgPageData;
    uint32_t descriptorSetsAllocated = 0; // VLOG(1) counter per frame
    bool arenaResetScheduled = false; // scheduled at endRender()
    uint32_t arenaResetsPerformed = 0; // count performed resets (debug)
    // Work to execute at the frame completion safe point. This runs after the
    // GPU finished executing the submission, after fence completion callbacks
    // ran, and after per-frame descriptor pool resets have been applied.
    //
    // Hooks are executed with a barrier at the safe point. Each hook is bound
    // to a call-site chosen executor (render thread, CPU pool, etc.).
    ZCoroSpotHooks<FrameHookSpot, Z3DRendererVulkanBackend> afterFrameCompletionHooks;

    // Stage 3: instrumentation (per-frame)
    uint32_t renderingSegmentsBegan = 0; // number of vkCmdBeginRendering calls
    uint32_t attachmentClears = 0; // number of attachments begun with Clear loadOp
    uint32_t attachmentLoads = 0; // number of attachments begun with Load loadOp
    uint32_t drawsSubmitted = 0; // number of draw calls recorded in this submission

    // Command buffer reuse diagnostics (optional; populated by pipeline contexts)
    uint32_t drawSecondaryCacheAttempts = 0;
    uint32_t drawSecondaryCacheKeyFound = 0;
    uint32_t drawSecondaryCacheSignatureMismatches = 0;
    uint32_t drawSecondaryCacheSignatureMismatchMaskOr = 0;
    uint32_t drawSecondaryCacheHits = 0;
    uint32_t drawSecondaryCacheBuilds = 0;
    uint32_t drawSecondaryCacheExecutes = 0;

    // Pipelines
    uint32_t pipelinesCreated = 0; // graphics pipelines created this frame
    std::unordered_set<uint64_t> pipelinesBound; // unique pipelines bound this frame

    // Residency pins for managed (evictable) textures referenced by this submission.
    // These pins are released via a submission-fence callback after queueSubmit().
    std::unordered_set<class ZVulkanTexture*> residencyPinnedTextures;

    // Pins for static geometry segments referenced by this submission (vertex
    // inputs and/or upload->static transfer destinations). These prevent reuse
    // of suballocated ranges until the submission fence signals.
    std::unordered_set<void*> pinnedStaticSegments;

    // Pre-record primed compute descriptor sets (no descriptor writes during recording).
    // These descriptor-set handles are allocated from the persistent descriptor
    // arena and remain stable for the lifetime of the frame-slot (activeFrameKey()).
    //
    // They are updated only at beginRender()/preRecord safe points (after the
    // slot's previous submission completed).
    std::unique_ptr<ZVulkanDescriptorSet> ddpCountComputeDescriptorSet;
    std::unique_ptr<ZVulkanDescriptorSet> ppllScanLocalComputeDescriptorSet;
    std::unique_ptr<ZVulkanDescriptorSet> ppllScanAddComputeDescriptorSet;

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
      vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
      uint32_t arrayLayer = 0;
      // Assigned staging slot index in m_readbackSlots
      int slotIndex = -1;
      size_t bytes = 0;
    };
    std::vector<PendingReadback> pendingColorReadbacks;

    struct PendingBufferReadback
    {
      class ZVulkanBuffer* src = nullptr;
      vk::DeviceSize srcOffset = 0;
      // Assigned staging slot index in m_readbackSlots
      int slotIndex = -1;
      size_t bytes = 0;
    };
    std::vector<PendingBufferReadback> pendingBufferReadbacks;
    // Force endRender() to synchronously wait for submission completion and
    // enter the frame completion safe point before returning. This is used for
    // control-flow boundaries (paging compaction, script readback, etc.).
    bool forceFenceWaitForCompletionSafePoint = false;
    size_t readbackBytesCopied = 0; // total bytes copied this frame
    uint32_t readbackSlotsInFlight = 0; // slots associated with this frame
    uint32_t allSamples = 0; // count of perf-frame-start → host-ready samples recorded
    std::optional<double> allMaxMs; // max perf-frame-start → host-ready latency within this submission

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
      // Minimum capacity hint supplied for this frame before ensureUniformArena()
      // chose the final capacity. Used for debug validation to detect estimation
      // drift (under-hinting) even when base/carry capacities mask overflow.
      size_t minCapacityHint = 0;
    } uniformArena;

    // Persistent host-visible uniform arena intended for per-stream/per-draw
    // UBO slices whose dynamic offsets must remain stable across frames (e.g.
    // cached command buffers). This arena is NOT reset each frame; it grows as
    // new streams are observed for this frame-slot.
    struct PersistentUniformArena
    {
      std::unique_ptr<class ZVulkanBuffer> buffer; // host-visible, host-coherent
      void* mapped = nullptr; // persistent mapping
      size_t capacity = 0; // bytes
      size_t cursor = 0; // bump pointer (never reset while buffer remains valid)
      size_t highWatermark = 0; // debug
    } persistentUniformArena;
    // Shared per-frame dynamic offset for lighting UBO slice in the uniform arena
    vk::DeviceSize lightingDynOffset = 0;
    // Shared per-frame dynamic offset for a "picking" lighting UBO slice in the
    // uniform arena (lighting disabled). This matches OpenGL picking behavior
    // (no lighting/fog applied to picking colors).
    vk::DeviceSize pickingLightingDynOffset = 0;

    // Shared per-frame dynamic offsets for the split frame-transform UBO slice
    // in the uniform arena. One slice per eye (Left/Mono/Right) so stereo
    // rendering can bind the correct view/projection matrices without per-batch
    // UBO rebuilds.
    std::array<vk::DeviceSize, 3> frameTransformsDynOffsetByEye{0, 0, 0};
    uint32_t frameTransformsDynOffsetValidMask = 0;

    // DDP: per-frame resources for indirect-count early stop
    std::unique_ptr<class ZVulkanBuffer> ddpChangedFlag; // STORAGE | TRANSFER_SRC | TRANSFER_DST
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
  void
  enterCompletionSafePointForKeyIfMatches(void* key, uint64_t expectedRealFrameToken, uint32_t expectedSubmissionId);
  void flushScheduledCopies(vk::raii::CommandBuffer& cmd);
  void ensureStaticArenas();
  void ensureUniformArena(FrameResources& frame);
  void ensurePersistentUniformArena(FrameResources& frame);
  // DDP indirect-count: ensure per-frame buffers exist
  void ensureDDPGatingResources(FrameResources& frame);
  // PPLL (exact OIT): ensure per-frame buffers exist
  void ensurePPLLResources(const glm::uvec4& viewport, uint64_t requestedFragments);
  // DDP indirect-count: ensure shared compute pipeline exists
  void ensureDDPComputePipeline();
  void ensurePPLLComputePipelines();
  size_t uniformAlignment() const;

public:
  // Record an additional CPU scope for perf collection. Intended for
  // external bookkeeping (e.g. batch capture time) during an active frame.
  void recordCpuScope(std::string_view label, double milliseconds);

  // DDP indirect-count gating (device-side early stop for DDP peel)
  bool ddpIndirectCountEnabled() const;
  vk::Buffer ddpChangedFlagBuffer();
  vk::Buffer ddpIndirectCountBuffer();
  class ZVulkanBuffer* ddpChangedFlagBufferObj();
  class ZVulkanBuffer* ddpIndirectCountBufferObj();
  // Device-local indirect args arena helpers
  vk::Buffer ddpDeviceArgsBuffer();
  vk::DeviceSize ddpAllocDeviceArgsSlot(size_t bytes);

  // Exact OIT (per-pixel fragment list / A-buffer) support
  [[nodiscard]] bool supportsFragmentStoresAndAtomics() const
  {
    return m_supportsFragStoresAndAtomics;
  }
  // Explicit PPLL priming for a submission. Intended to be called from a
  // beginRender pre-record action so the required buffers (and ring slot) are
  // established before descriptor sets are primed and before recording begins.
  void primePPLLForCountPass(const glm::uvec4& viewport);
  void primePPLLForStorePass(const glm::uvec4& viewport, uint64_t requestedFragments);
  [[nodiscard]] uint32_t ppllPixelCount() const;
  [[nodiscard]] uint32_t ppllBlockCount() const;
  class ZVulkanBuffer* ppllParamsBufferObj();
  class ZVulkanBuffer* ppllCountsBufferObj();
  class ZVulkanBuffer* ppllOffsetsBufferObj();
  class ZVulkanBuffer* ppllCursorsBufferObj();
  class ZVulkanBuffer* ppllFragmentsBufferObj();
  class ZVulkanBuffer* ppllBlockSumsBufferObj();
  class ZVulkanBuffer* ppllBlockPrefixesBufferObj();
  void ppllWriteBlockPrefixes(const uint32_t* prefixes, size_t count);
  void primeOITDescriptorSet(class ZVulkanDescriptorSet& set);
  // Monotonic revision that changes ONLY when the underlying OIT (PPLL/DDP)
  // resources are destroyed/recreated (i.e. VkBuffer handles change because a
  // buffer was replaced).
  //
  // Cached secondary command buffers use this to decide when they must be
  // rebuilt to avoid executing a secondary recorded against objects that were
  // later destroyed (a common source of validation errors like
  // VUID-vkCmdExecuteCommands-pCommandBuffers-00089).
  [[nodiscard]] uint64_t oitResourcesRevision() const noexcept
  {
    return m_oitResourcesRevision;
  }

  // PPLL command recording helpers (must be called within an active frame)
  void ppllResetCounts(vk::raii::CommandBuffer& cmd);
  void ppllResetCursors(vk::raii::CommandBuffer& cmd);
  void ppllBarrierTransferToFrag(vk::raii::CommandBuffer& cmd);
  void ppllBarrierFragToCompute(vk::raii::CommandBuffer& cmd);
  void ppllBarrierComputeToFrag(vk::raii::CommandBuffer& cmd);
  void ppllBarrierFragToFrag(vk::raii::CommandBuffer& cmd);
  void ppllDispatchScanLocal(vk::raii::CommandBuffer& cmd);
  void ppllDispatchScanAdd(vk::raii::CommandBuffer& cmd);
  void ddpResetForPass(vk::raii::CommandBuffer& cmd, bool firstPass);
  void ddpBarrierTransferToFrag(vk::raii::CommandBuffer& cmd);
  void ddpBarrierFragToCompute(vk::raii::CommandBuffer& cmd);
  void ddpBarrierComputeToIndirect(vk::raii::CommandBuffer& cmd);
  void ddpDispatchCountCompute(vk::raii::CommandBuffer& cmd);
  // Schedule a static copy for indirect args (upload arena -> device-local args buffer)
  void scheduleStaticCopyIndirect(vk::Buffer dst, vk::DeviceSize dstOffset, const UploadSlice& src);
  struct UniformSlice
  {
    vk::Buffer buffer{};
    vk::DeviceSize offset{0};
    void* mapped = nullptr;
    size_t size = 0;
  };

  // Allocate a slice from the per-frame uniform arena for the given payload
  // type. Payloads that allocate from this arena must declare a conservative
  // per-batch upper bound via vulkan::UniformArenaBudgetTraits so higher-level
  // schedulers can pre-size the arena safely.
  template<typename PayloadT>
  UniformSlice suballocateUniformFor(const PayloadT& payload, size_t bytes, size_t alignment = 0)
  {
    (void)payload;
    using CleanPayloadT = std::remove_cvref_t<PayloadT>;
    static_assert(vulkan::kHasUniformArenaBudgetTraits<CleanPayloadT>,
                  "Uniform arena allocation requires vulkan::UniformArenaBudgetTraits specialization for payload");
    return suballocateUniform(bytes, alignment);
  }

  // Allocate a slice from the persistent uniform arena for the given payload
  // type. Intended for per-stream/per-draw UBO data whose dynamic offsets must
  // remain stable across frames for command buffer reuse.
  template<typename PayloadT>
  UniformSlice suballocatePersistentUniformFor(const PayloadT& payload, size_t bytes, size_t alignment = 0)
  {
    (void)payload;
    using CleanPayloadT = std::remove_cvref_t<PayloadT>;
    static_assert(
      vulkan::kHasUniformArenaBudgetTraits<CleanPayloadT>,
      "Persistent uniform arena allocation requires vulkan::UniformArenaBudgetTraits specialization for payload");
    return suballocatePersistentUniform(bytes, alignment);
  }

  class ZVulkanBuffer& uniformArenaBuffer();
  class ZVulkanBuffer& persistentUniformArenaBuffer();
  // Obtain a mapped pointer into the persistent uniform arena for a previously
  // allocated offset range (debug-checked).
  [[nodiscard]] void* persistentUniformMappedAt(vk::DeviceSize offset, size_t bytes);
  // Dynamic offset of the shared per-frame lighting UBO slice
  [[nodiscard]] vk::DeviceSize frameSharedLightingOffset() const
  {
    CHECK(m_activeFrame != nullptr) << "frameSharedLightingOffset called without an active frame";
    return m_activeFrame->lightingDynOffset;
  }
  // Dynamic offset of the shared per-frame picking lighting UBO slice (lighting disabled)
  [[nodiscard]] vk::DeviceSize framePickingLightingOffset() const
  {
    CHECK(m_activeFrame != nullptr) << "framePickingLightingOffset called without an active frame";
    return m_activeFrame->pickingLightingDynOffset;
  }

  // Dynamic offset of the shared per-eye frame transform UBO slice.
  [[nodiscard]] vk::DeviceSize frameTransformsOffset(Z3DEye eye) const
  {
    CHECK(m_activeFrame != nullptr) << "frameTransformsOffset called without an active frame";
    const uint32_t idx = static_cast<uint32_t>(eye);
    CHECK(idx < m_activeFrame->frameTransformsDynOffsetByEye.size()) << "frameTransformsOffset eye index out of bounds";
    CHECK((m_activeFrame->frameTransformsDynOffsetValidMask & (1u << idx)) != 0u)
      << "frameTransformsOffset requested before allocation for eye index " << idx;
    return m_activeFrame->frameTransformsDynOffsetByEye[idx];
  }

private:
  UniformSlice suballocateUniform(size_t bytes, size_t alignment = 0);
  UniformSlice suballocatePersistentUniform(size_t bytes, size_t alignment = 0);

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
  uint64_t m_deviceRevision = 0;
  ZVulkanDevice* m_frameDevice = nullptr; // tracked to rebuild frame resources on device changes
  std::vector<FrameResources> m_frames;
  std::unordered_map<void*, size_t> m_frameResourceMap;
  std::optional<ZVulkanFrameExecutor::ActiveFrame> m_activeFrameHandle;
  FrameResources* m_activeFrame = nullptr;
  // Key of the most recently submitted (or ended) frame-executor slot. Used to
  // defer scratch-pool releases that occur after endRender() has cleared the
  // active recording context, without forcing a device-wide drain.
  void* m_lastSubmittedFrameKey = nullptr;
  Z3DRendererBase* m_activeRenderer = nullptr;
  std::atomic<FrameResources*> m_frameInCompletionSafePoint{nullptr};
  bool m_frameRecording = false;
  // Scratch storage for frame-executor polling: keys of frame slots whose
  // fences completed in the last pollCompletions() call.
  std::vector<void*> m_completedFrameKeysScratch;
  uint32_t m_maxFramesInFlight = 2;
  float m_timestampPeriod = 1.0f;
  mutable size_t m_cachedUniformAlignment = 0;
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
  std::unique_ptr<ZVulkanTexturePPLLPipelineContext> m_texturePPLLContext;
  std::unique_ptr<ZVulkanTextureGlowPipelineContext> m_textureGlowContext;
  std::unique_ptr<ZVulkanImgSlicePipelineContext> m_imgSliceContext;
  std::unique_ptr<ZVulkanImgRaycasterPipelineContext> m_imgRaycasterContext;
  std::unique_ptr<ZVulkanFontPipelineContext> m_fontContext;

  // Shared fallback resources
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholder2D;
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholder2DArray;
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholder3D;
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholderU2D;
  std::unique_ptr<ZVulkanTexture> m_defaultPlaceholderU3D;
  std::unique_ptr<ZVulkanBuffer> m_defaultPlaceholderStorageBuffer;
  std::optional<vk::raii::Sampler> m_defaultSampler;
  std::optional<vk::raii::Sampler> m_nearestClampSampler;

  struct SharedDescriptorLayouts
  {
    std::optional<vk::raii::DescriptorSetLayout> bindlessSampledImages;
    std::optional<vk::raii::DescriptorSetLayout> lighting;
    std::optional<vk::raii::DescriptorSetLayout> transforms;
    std::optional<vk::raii::DescriptorSetLayout> oitParams;
    std::optional<vk::raii::DescriptorSetLayout> empty;
    // Image renderer helper layouts (slice/raycaster).
    std::optional<vk::raii::DescriptorSetLayout> imgIndices; // set 1 (binding 0)
    std::optional<vk::raii::DescriptorSetLayout> imgPageData; // set 2 (binding 2)
  } m_sharedDescriptorLayouts;

  // Shared geometry: fullscreen quad VBO
  std::unique_ptr<ZVulkanBuffer> m_fullscreenQuadVbo;
  std::unique_ptr<ZVulkanBuffer> m_dummyVertexBuffer; // tiny VB for unused bindings

  // Device-local static arenas for geometry
  struct StaticArena
  {
    enum class Kind : uint8_t
    {
      Vertex = 0,
      Index = 1,
    };

    struct Segment
    {
      Kind kind = Kind::Vertex;
      uint64_t id = 0;
      std::unique_ptr<class ZVulkanBuffer> buffer; // device-local
      VmaVirtualBlock block = nullptr; // virtual suballocator over [0, capacity)
      size_t capacity = 0; // bytes
      // Bookkeeping for eviction/trimming:
      // - usedBytes/allocCount track live suballocations (requested sizes)
      // - pinCount tracks in-flight submissions referencing this segment
      size_t usedBytes = 0;
      uint32_t allocCount = 0;
      uint32_t pinCount = 0;
      struct PendingFree
      {
        VmaVirtualAllocation allocation = nullptr;
        size_t size = 0;
      };
      std::vector<PendingFree> pendingFrees;
      ~Segment();
    };

    // Static buffers are never reallocated in place; we grow by appending
    // segments. We use VMA virtual blocks to allow per-stream eviction and
    // trimming without silent caps.
    std::vector<std::unique_ptr<Segment>> vb; // eVertexBuffer|eTransferDst
    std::vector<std::unique_ptr<Segment>> ib; // eIndexBuffer|eTransferDst

    // Map VkBuffer handle to owning segment for pin/unpin and copy safety.
    std::unordered_map<VkBuffer, Segment*> segmentByBuffer;

    // Monotonic segment identity counter. Used to disambiguate VkBuffer handle
    // reuse after segment destruction.
    uint64_t nextSegmentId = 1;

    // Telemetry: peak total allocated bytes across all segments.
    size_t vbHighWatermark = 0;
    size_t ibHighWatermark = 0;
  } m_staticArena;

  // Static arena helpers
  std::unique_ptr<StaticArena::Segment> createStaticArenaSegment(StaticArena::Kind kind, size_t capacityBytes);
  void flushPendingFreesAndMaybeTrimStaticSegment(StaticArena::Segment* segment);

  // Pending per-stream static-geometry evictions requested from outside the
  // render thread (e.g., primitive renderer destruction). Drained in beginRender().
  std::mutex m_pendingEvictionsMutex;
  std::unordered_set<uint64_t> m_pendingEvictStreamKeys;

  // PPLL (exact OIT): per-pixel fragment list resources are maintained in a
  // small ring keyed by the UI/perf frame token (not by Vulkan frame-executor
  // slots). This ensures multiple submissions within the same UI frame (count →
  // scan/readback → store/resolve) all reference the same buffers even when the
  // frame executor advances its command-buffer slot.
  struct PPLLResources
  {
    static constexpr uint32_t kBlockSize = 256u;
    glm::uvec4 viewport{0u}; // x,y,w,h
    uint32_t pixelCount = 0;
    uint32_t blockCount = 0;
    uint64_t requestedFragmentCount = 0;

    // SSBO: viewport + pixelCount + scan metadata (host-visible, host-coherent)
    std::unique_ptr<class ZVulkanBuffer> params;
    void* paramsMapped = nullptr;
    size_t paramsCapacity = 0;

    // SSBO: per-pixel arrays (device-local)
    std::unique_ptr<class ZVulkanBuffer> counts;
    size_t countsCapacityBytes = 0;
    std::unique_ptr<class ZVulkanBuffer> offsets;
    size_t offsetsCapacityBytes = 0;
    std::unique_ptr<class ZVulkanBuffer> cursors;
    size_t cursorsCapacityBytes = 0;

    // SSBO: fragment storage (device-local, grown to total fragments)
    std::unique_ptr<class ZVulkanBuffer> fragments;
    size_t fragmentsCapacityBytes = 0;

    // SSBO: scan intermediates
    std::unique_ptr<class ZVulkanBuffer> blockSums; // device-local, transfer-src for readback
    size_t blockSumsCapacityBytes = 0;
    std::unique_ptr<class ZVulkanBuffer> blockPrefixes; // host-visible, host-coherent (CPU writes)
    void* blockPrefixesMapped = nullptr;
    size_t blockPrefixesCapacityBytes = 0;
  };
  std::vector<PPLLResources> m_ppllFrameRing;
  std::optional<size_t> m_activePPLLIndex;
  // Bumped whenever any PPLL buffer is replaced (destroyed + recreated).
  uint64_t m_oitResourcesRevision = 1;

  // Helpers for descriptor arena lifecycle
  void ensureArenaOnFrame(FrameResources& frame);
  void ensurePersistentArenaOnFrame(FrameResources& frame);
  void ensureBindlessSampledImagesOnFrame(FrameResources& frame);
  void applyPendingArenaReset(FrameResources& frame);
  // Opportunistically enter the "frame completion safe point" for any frame
  // slots whose fences have completed and whose completion callbacks have been
  // executed by the frame executor (pollCompletions / waitForCompletion / etc).
  //
  // Historically, applyPendingArenaReset() (and thus safe-point hooks) would
  // only run when a frame slot was reused (beginFrame) or after an explicit
  // wait (endFrame). When no explicit wait occurs and frames-in-flight > 1,
  // this can delay safe-point work by up to roughly "frames in flight".
  //
  // Pumping safe points after polling completions preserves the safe-point
  // ordering guarantees (after fence-gated completion callbacks like residency
  // unpins) while reducing latency for consumers such as paging compaction
  // readbacks.
  void pumpFrameCompletionSafePoints(const std::vector<void*>& completedFrameKeys);
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

  // PPLL scan compute pipelines (shared across frames)
  std::optional<vk::raii::DescriptorSetLayout> m_ppllScanLocalSetLayout;
  std::optional<vk::raii::PipelineLayout> m_ppllScanLocalPipelineLayout;
  std::optional<vk::raii::Pipeline> m_ppllScanLocalPipeline;

  std::optional<vk::raii::DescriptorSetLayout> m_ppllScanAddSetLayout;
  std::optional<vk::raii::PipelineLayout> m_ppllScanAddPipelineLayout;
  std::optional<vk::raii::Pipeline> m_ppllScanAddPipeline;

  std::vector<BeginRenderPreRecordAction> m_pendingBeginRenderPreRecordActions;
  std::string m_pendingBeginRenderPreRecordLabel;
  std::optional<BeginRenderScriptStats> m_pendingBeginRenderScriptStats;

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
  // Guards detached readback release callbacks that may outlive the backend
  // instance (backend switches destroy and recreate the Vulkan backend).
  std::shared_ptr<bool> m_aliveFlag = std::make_shared<bool>(true);

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
  // Carry-over hint for subsequent frames. This is used to keep the uniform arena
  // sized reasonably even when a particular frame begin does not have an explicit
  // estimate available (e.g., multiple Vulkan frames within one Atlas frame).
  size_t m_uniformMinCapacityCarry = 0;

  struct PassBaseline
  {
    uint32_t descriptorSetsAllocated = 0;
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
    uint64_t bufferBarriers = 0;
    uint64_t bufferBarrierNoops = 0;
  };

  PassScope m_passScope{};

  // Tracks external buffer hazards across all recording sessions within a
  // single Vulkan frame submission. This is cleared at beginRender() (frame
  // start). The key is the VkBuffer handle value (stable for the buffer's
  // lifetime).
  struct ExternalBufferUseState
  {
    vk::PipelineStageFlags2 stage{};
    vk::AccessFlags2 access{};
  };
  std::unordered_map<uint64_t, ExternalBufferUseState> m_externalBufferUseStates;
};

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
