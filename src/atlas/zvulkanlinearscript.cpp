#include "zvulkanlinearscript.h"
#include "zcommandlineflags.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drenderglobalstate.h"
#include "z3dperfcollector.h"
#include "z3dscratchresourcepool.h"
#include "z3dimg.h"
#include "zcancellation.h"
#include "zlog.h"
#include "zrenderthreadexecutor_tls.h"
#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkanresidencymanager.h"
#include "zvulkantexture.h"

#include <folly/OperationCancelled.h>
#include <folly/coro/Task.h>
#include <folly/ScopeGuard.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>

ABSL_FLAG(bool,
          atlas_vk_linear_script_validate_batch_metadata,
          false,
          "Enable expensive CHECK-based validation of Vulkan batch metadata in ZVulkanLinearScript before executing");

ABSL_FLAG(
  bool,
  atlas_vk_linear_script_merge_rasters,
  true,
  "If true, merge consecutive raster nodes into a single Vulkan submission (reduces overhead). When false, each raster "
  "node is executed as its own submission to aid profiling.");

namespace nim {
namespace {

std::string makeFrameLabel(std::string_view preferred, std::string_view firstPassLabel)
{
  if (!preferred.empty()) {
    return std::string(preferred);
  }
  if (!firstPassLabel.empty()) {
    return std::string(firstPassLabel);
  }
  return std::string("vk_script");
}

void validateVulkanBatchMetadataOrCrash(std::string_view passLabel, const RendererCPUState& state)
{
  // NOTE: ZVulkanLinearScript is Vulkan-only by construction. Validate the
  // captured batch metadata so the backend never has to guess about attachment
  // contracts (finalUse) or cross-pass resource usage (external*Uses).
  for (size_t i = 0; i < state.batches.size(); ++i) {
    const RenderBatch& batch = state.batches[i];

    CHECK(batch.originatingRenderer != nullptr)
      << fmt::format("Vulkan script batch missing originatingRenderer: pass='{}' batchIndex={}", passLabel, i);
    CHECK(batch.clipPlanes.captured)
      << fmt::format("Vulkan script batch missing captured clip planes: pass='{}' batchIndex={}", passLabel, i);
    CHECK(batch.shaderHook.captured)
      << fmt::format("Vulkan script batch missing captured shader hook: pass='{}' batchIndex={}", passLabel, i);

    // Pass attachment contract:
    // - Raster passes must target attachments.
    // - Compute passes must not target attachments (they rely on explicit
    //   externalImageUses/externalBufferUses metadata for synchronization).
    const bool hasColorAttachments = !batch.pass.colorAttachments.empty();
    const bool hasDepthAttachment = batch.pass.depthAttachment.has_value();
    if (batch.pass.kind == BackendPassDesc::Kind::Raster) {
      CHECK(hasColorAttachments || hasDepthAttachment)
        << fmt::format("Vulkan raster batch missing attachments: pass='{}' batchIndex={}", passLabel, i);
    } else {
      CHECK(batch.pass.kind == BackendPassDesc::Kind::Compute)
        << fmt::format("Vulkan script batch has unknown pass kind: pass='{}' batchIndex={}", passLabel, i);
      CHECK(!hasColorAttachments && !hasDepthAttachment)
        << fmt::format("Vulkan compute batch must not specify attachments: pass='{}' batchIndex={}", passLabel, i);
    }

    // Viewport/scissor sanity (these invariants are also enforced at appendBatch
    // time; keep them here to make any failures attributable to the submission
    // label that ultimately executes them).
    if (batch.pass.kind == BackendPassDesc::Kind::Raster) {
      CHECK(batch.pass.viewport.extent.x > 0.0f && batch.pass.viewport.extent.y > 0.0f)
        << fmt::format("Vulkan script batch missing viewport extent: pass='{}' batchIndex={}", passLabel, i);
      if (batch.pass.enableScissor) {
        CHECK(batch.pass.scissorRect.z > 0.0f && batch.pass.scissorRect.w > 0.0f)
          << fmt::format("Vulkan script batch enables scissor but has empty scissorRect: pass='{}' batchIndex={}",
                         passLabel,
                         i);
      }
      CHECK(batch.pass.viewport.maxDepth >= batch.pass.viewport.minDepth)
        << fmt::format("Vulkan script batch invalid depth range: pass='{}' batchIndex={} minDepth={} maxDepth={}",
                       passLabel,
                       i,
                       batch.pass.viewport.minDepth,
                       batch.pass.viewport.maxDepth);
    }

    for (const auto& att : batch.pass.colorAttachments) {
      CHECK(att.handle.valid()) << fmt::format(
        "Vulkan script batch has invalid color attachment handle: pass='{}' batchIndex={}",
        passLabel,
        i);
      CHECK(att.handle.backend == RenderBackend::Vulkan)
        << fmt::format("Vulkan script batch has non-Vulkan color attachment: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       att.handle.id);
      CHECK(att.finalUse != AttachmentFinalUse::Unspecified)
        << fmt::format("Vulkan script batch missing color attachment finalUse: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       att.handle.id);
    }

    if (batch.pass.depthAttachment) {
      const auto& att = *batch.pass.depthAttachment;
      CHECK(att.handle.valid()) << fmt::format(
        "Vulkan script batch has invalid depth attachment handle: pass='{}' batchIndex={}",
        passLabel,
        i);
      CHECK(att.handle.backend == RenderBackend::Vulkan)
        << fmt::format("Vulkan script batch has non-Vulkan depth attachment: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       att.handle.id);
      CHECK(att.finalUse != AttachmentFinalUse::Unspecified)
        << fmt::format("Vulkan script batch missing depth attachment finalUse: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       att.handle.id);
    }

    if (batch.pass.resolveAttachment) {
      const auto& att = *batch.pass.resolveAttachment;
      if (att.handle.valid()) {
        CHECK(att.handle.backend == RenderBackend::Vulkan)
          << fmt::format("Vulkan script batch has non-Vulkan resolve attachment: pass='{}' batchIndex={} handle=0x{:x}",
                         passLabel,
                         i,
                         att.handle.id);
        CHECK(att.finalUse != AttachmentFinalUse::Unspecified) << fmt::format(
          "Vulkan script batch missing resolve attachment finalUse: pass='{}' batchIndex={} handle=0x{:x}",
          passLabel,
          i,
          att.handle.id);
      }
    }

    auto isPassAttachment = [&](const AttachmentHandle& handle) {
      if (!handle.valid()) {
        return false;
      }
      for (const auto& att : batch.pass.colorAttachments) {
        if (att.handle.id == handle.id) {
          return true;
        }
      }
      if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.id == handle.id) {
        return true;
      }
      if (batch.pass.resolveAttachment && batch.pass.resolveAttachment->handle.id == handle.id) {
        return true;
      }
      return false;
    };

    for (const auto& use : batch.pass.externalImageUses) {
      if (!use.handle.valid()) {
        continue;
      }
      CHECK(use.handle.backend == RenderBackend::Vulkan)
        << fmt::format("Vulkan script batch has non-Vulkan external image use: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       use.handle.id);
      CHECK(!isPassAttachment(use.handle))
        << fmt::format("Vulkan script batch external image use references an active attachment (read-while-write): "
                       "pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       use.handle.id);
      if (use.kind == ExternalImageUseKind::SampledRead) {
        CHECK(use.aspectHint != ExternalImageAspectHint::Unspecified) << fmt::format(
          "Vulkan script batch missing aspectHint for sampled external image use: pass='{}' batchIndex={} handle=0x{:x}",
          passLabel,
          i,
          use.handle.id);
      }
    }

    for (const auto& use : batch.pass.externalBufferUses) {
      if (!use.handle.valid()) {
        continue;
      }
      CHECK(use.handle.backend == RenderBackend::Vulkan)
        << fmt::format("Vulkan script batch has non-Vulkan external buffer use: pass='{}' batchIndex={} handle=0x{:x}",
                       passLabel,
                       i,
                       use.handle.id);
      CHECK(use.kind != ExternalBufferUseKind::Unspecified) << fmt::format(
        "Vulkan script batch missing kind for external buffer use: pass='{}' batchIndex={} handle=0x{:x}",
        passLabel,
        i,
        use.handle.id);
    }
  }
}

struct BindlessUseKey
{
  uint64_t id = 0;
  ExternalImageUseKind kind = ExternalImageUseKind::SampledRead;
  ExternalImageAspectHint aspectHint = ExternalImageAspectHint::Unspecified;

  bool operator==(const BindlessUseKey& o) const noexcept
  {
    return id == o.id && kind == o.kind && aspectHint == o.aspectHint;
  }
};

struct BindlessUseKeyHash
{
  size_t operator()(const BindlessUseKey& k) const noexcept
  {
    size_t seed = std::hash<uint64_t>{}(k.id);
    seed ^= (static_cast<size_t>(k.kind) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.aspectHint) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    return seed;
  }
};

struct FontAtlasKey
{
  const uint8_t* pixelsBGRA8 = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const FontAtlasKey& o) const noexcept
  {
    return pixelsBGRA8 == o.pixelsBGRA8 && width == o.width && height == o.height;
  }
};

struct FontAtlasKeyHash
{
  size_t operator()(const FontAtlasKey& k) const noexcept
  {
    size_t seed = std::hash<const void*>{}(static_cast<const void*>(k.pixelsBGRA8));
    seed ^= (static_cast<size_t>(k.width) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.height) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    return seed;
  }
};

struct RaycasterWarmupKey
{
  Z3DImg* image = nullptr;
  const std::vector<Z3DTransferFunction*>* transferFunctions = nullptr;
  bool wants2D = false;
  bool wantsVolume3D = true;
  bool wantsPaging = false;

  bool operator==(const RaycasterWarmupKey& o) const noexcept
  {
    return image == o.image && transferFunctions == o.transferFunctions && wants2D == o.wants2D &&
           wantsVolume3D == o.wantsVolume3D && wantsPaging == o.wantsPaging;
  }
};

struct RaycasterWarmupKeyHash
{
  size_t operator()(const RaycasterWarmupKey& k) const noexcept
  {
    size_t seed = std::hash<const void*>{}(static_cast<const void*>(k.image));
    seed ^= (std::hash<const void*>{}(static_cast<const void*>(k.transferFunctions)) + 0x9e3779b97f4a7c15ull +
             (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.wants2D) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.wantsVolume3D) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.wantsPaging) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    return seed;
  }
};

struct RaycasterWarmupGroup
{
  RaycasterWarmupKey key{};
  std::vector<size_t> channels;
};

struct SliceWarmupKey
{
  Z3DImg* image = nullptr;
  const std::vector<const ZColorMap*>* colormaps = nullptr;
  bool wantsVolume3D = true;
  bool wantsColormap = true;
  bool wantsPaging = false;

  bool operator==(const SliceWarmupKey& o) const noexcept
  {
    return image == o.image && colormaps == o.colormaps && wantsVolume3D == o.wantsVolume3D &&
           wantsColormap == o.wantsColormap && wantsPaging == o.wantsPaging;
  }
};

struct SliceWarmupKeyHash
{
  size_t operator()(const SliceWarmupKey& k) const noexcept
  {
    size_t seed = std::hash<const void*>{}(static_cast<const void*>(k.image));
    seed ^= (std::hash<const void*>{}(static_cast<const void*>(k.colormaps)) + 0x9e3779b97f4a7c15ull + (seed << 6) +
             (seed >> 2));
    seed ^= (static_cast<size_t>(k.wantsVolume3D) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.wantsColormap) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    seed ^= (static_cast<size_t>(k.wantsPaging) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    return seed;
  }
};

struct SliceWarmupGroup
{
  SliceWarmupKey key{};
  std::vector<size_t> channels;
};

void appendBindlessSampledImageUsesFromBatch(
  const RenderBatch& batch,
  std::vector<ExternalImageUseDesc>& out,
  std::vector<Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc>* outFontAtlases)
{
  // Primary source of sampled-image usage metadata: pass.externalImageUses.
  for (const auto& use : batch.pass.externalImageUses) {
    if (!use.handle.valid() || use.handle.backend != RenderBackend::Vulkan) {
      continue;
    }
    if (use.kind != ExternalImageUseKind::SampledRead) {
      continue;
    }
    out.push_back(use);
  }

  // Shader-hook sampled images are not always represented in externalImageUses
  // (they are often already in the correct final layout), but they still need
  // bindless indices.
  if (batch.shaderHook.captured && batch.shaderHook.type == ShaderHookType::DualDepthPeelingPeel) {
    if (batch.shaderHook.para.dualDepthPeelingDepthBlenderHandle.valid()) {
      out.push_back(ExternalImageUseDesc{batch.shaderHook.para.dualDepthPeelingDepthBlenderHandle,
                                         ExternalImageUseKind::SampledRead,
                                         ExternalImageAspectHint::Color});
    }
    if (batch.shaderHook.para.dualDepthPeelingFrontBlenderHandle.valid()) {
      out.push_back(ExternalImageUseDesc{batch.shaderHook.para.dualDepthPeelingFrontBlenderHandle,
                                         ExternalImageUseKind::SampledRead,
                                         ExternalImageAspectHint::Color});
    }
  }

  // Payload-provided sampled images (Mesh/Font).
  std::visit(
    [&](auto&& payload) {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, MeshPayload>) {
        if (payload.textureHandle.valid() && payload.textureHandle.backend == RenderBackend::Vulkan) {
          AttachmentHandle h{};
          h.id = payload.textureHandle.id;
          h.backend = RenderBackend::Vulkan;
          out.push_back(ExternalImageUseDesc{h, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        }
      } else if constexpr (std::is_same_v<T, FontPayload>) {
        if (payload.atlasHandle.valid() && payload.atlasHandle.backend == RenderBackend::Vulkan) {
          AttachmentHandle h{};
          h.id = payload.atlasHandle.id;
          h.backend = RenderBackend::Vulkan;
          out.push_back(ExternalImageUseDesc{h, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        } else if (outFontAtlases != nullptr && payload.atlasPixels != nullptr && payload.atlasWidth > 0u &&
                   payload.atlasHeight > 0u) {
          Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc desc{};
          desc.pixelsBGRA8 = payload.atlasPixels;
          desc.width = payload.atlasWidth;
          desc.height = payload.atlasHeight;
          outFontAtlases->push_back(desc);
        }
      } else {
        // Other payload types expose sampled textures via pass.externalImageUses.
      }
    },
    batch.geometry);
}

bool externalImageUseRequiresContents(ExternalImageUseKind kind)
{
  switch (kind) {
    case ExternalImageUseKind::SampledRead:
    case ExternalImageUseKind::StorageRead:
    case ExternalImageUseKind::StorageReadWrite:
    case ExternalImageUseKind::TransferSrc:
    case ExternalImageUseKind::General:
      return true;
    case ExternalImageUseKind::StorageWrite:
    case ExternalImageUseKind::TransferDst:
      return false;
  }
  return true;
}

void appendTextureUseFromHandle(const AttachmentHandle& handle,
                                bool contentsRequired,
                                std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse>& out)
{
  if (!handle.valid() || handle.backend != RenderBackend::Vulkan) {
    return;
  }
  auto* texture = reinterpret_cast<ZVulkanTexture*>(handle.id);
  if (texture == nullptr) {
    return;
  }
  out.push_back(
    Z3DScratchResourcePool::VulkanScratchTextureUse{.texture = texture, .contentsRequired = contentsRequired});
}

void appendTextureUsesFromBatch(const RenderBatch& batch,
                                std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse>& out)
{
  for (const auto& attachment : batch.pass.colorAttachments) {
    appendTextureUseFromHandle(attachment.handle, attachment.loadOp == LoadOp::Load, out);
  }
  if (batch.pass.depthAttachment) {
    appendTextureUseFromHandle(batch.pass.depthAttachment->handle,
                               batch.pass.depthAttachment->loadOp == LoadOp::Load,
                               out);
  }
  if (batch.pass.resolveAttachment) {
    appendTextureUseFromHandle(batch.pass.resolveAttachment->handle,
                               batch.pass.resolveAttachment->loadOp == LoadOp::Load,
                               out);
  }

  for (const auto& use : batch.pass.externalImageUses) {
    appendTextureUseFromHandle(use.handle, externalImageUseRequiresContents(use.kind), out);
  }

  std::vector<ExternalImageUseDesc> sampledUses;
  appendBindlessSampledImageUsesFromBatch(batch, sampledUses, nullptr);
  for (const auto& use : sampledUses) {
    appendTextureUseFromHandle(use.handle, true, out);
  }

  auto appendLeaseColors = [&](const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& lease,
                               uint32_t requestedAttachments,
                               bool contentsRequired) {
    if (!lease || !lease->hasVulkanImage()) {
      return;
    }
    const uint32_t count =
      requestedAttachments == 0u ? lease->attachments : std::min(requestedAttachments, lease->attachments);
    for (uint32_t i = 0; i < count; ++i) {
      if (auto* texture = lease->colorAttachment(i)) {
        out.push_back(
          Z3DScratchResourcePool::VulkanScratchTextureUse{.texture = texture, .contentsRequired = contentsRequired});
      }
    }
  };

  std::visit(
    [&](auto&& payload) {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, ImgRaycasterPayload>) {
        if (payload.stage == ImgRaycasterPayload::Stage::ProgressiveCompaction) {
          appendLeaseColors(payload.blockIdLease, payload.blockIdEffectiveAttachmentCount, true);
        }
      } else if constexpr (std::is_same_v<T, ImgSlicePayload>) {
        if (payload.stage == ImgSlicePayload::Stage::BlockIdDiscovery && payload.blockIdLease) {
          appendLeaseColors(payload.blockIdLease, 1u, true);
        }
      } else {
      }
    },
    batch.geometry);
}

void appendTextureUsesFromState(const RendererCPUState& state,
                                std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse>& out)
{
  for (const auto& batch : state.batches) {
    appendTextureUsesFromBatch(batch, out);
  }
}

} // namespace

ZVulkanLinearScript::ZVulkanLinearScript(Z3DRendererBase& renderer,
                                         Z3DRendererVulkanBackend& backend,
                                         std::string_view frameLabel)
  : m_renderer(renderer)
  , m_backend(backend)
  , m_frameLabel(std::string(frameLabel))
{
  CHECK(m_renderer.backend() != nullptr) << "ZVulkanLinearScript requires a renderer backend";
  CHECK(m_renderer.activeBackend() == RenderBackend::Vulkan) << "ZVulkanLinearScript requires Vulkan renderer";
  CHECK(!m_renderer.isVulkanFrameActive())
    << "ZVulkanLinearScript requires no active Vulkan frame (script must own submission boundaries)";
  CHECK(currentRenderThreadExecutorOrNull() != nullptr) << "ZVulkanLinearScript must run on the rendering thread";
}

ZVulkanLinearScript::ScopedSubmissionGroup::ScopedSubmissionGroup(ZVulkanLinearScript& script)
  : m_script(&script)
{
  m_script->enterSubmissionGroup();
}

ZVulkanLinearScript::ScopedSubmissionGroup::ScopedSubmissionGroup(ScopedSubmissionGroup&& rhs) noexcept
  : m_script(rhs.m_script)
{
  rhs.m_script = nullptr;
}

ZVulkanLinearScript::ScopedSubmissionGroup::~ScopedSubmissionGroup()
{
  if (m_script != nullptr) {
    m_script->leaveSubmissionGroup();
  }
}

ZVulkanLinearScript::~ZVulkanLinearScript()
{
  if (m_nodes.empty() && m_preRecordNodes.empty() && !m_frameOpen) {
    return;
  }
  if (std::uncaught_exceptions() > 0) {
    LOG(WARNING) << "ZVulkanLinearScript dropping unflushed nodes during exception unwinding";
    return;
  }

  try {
    flushNodes("script_dtor", nullptr);
  }
  catch (const ZCancellationException&) {
    // Cancellation is expected during interactive aborts and shutdown. We
    // must not throw from a destructor; just log and keep going.
    VLOG(1) << "ZVulkanLinearScript destructor flush cancelled (ZCancellationException)";
  }
  catch (const folly::OperationCancelled&) {
    VLOG(1) << "ZVulkanLinearScript destructor flush cancelled (folly::OperationCancelled)";
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "ZVulkanLinearScript destructor flush failed: " << e.what();
    CHECK(false) << "ZVulkanLinearScript destructor flush failed (exception)";
  }
  catch (...) {
    LOG(ERROR) << "ZVulkanLinearScript destructor flush failed (unknown exception)";
    CHECK(false) << "ZVulkanLinearScript destructor flush failed (unknown exception)";
  }
}

void ZVulkanLinearScript::flush(std::string_view reason)
{
  CHECK_EQ(m_submissionGroupDepth, 0u) << "ZVulkanLinearScript::flush called inside a scoped submission group";
  if (m_nodes.empty() && m_preRecordNodes.empty() && !m_frameOpen) {
    return;
  }
  const std::string_view resolved = reason.empty() ? std::string_view("script_flush") : reason;
  flushNodes(resolved, nullptr);
}

void ZVulkanLinearScript::flushAndWaitForCompletion(std::string_view reason)
{
  CHECK_EQ(m_submissionGroupDepth, 0u)
    << "ZVulkanLinearScript::flushAndWaitForCompletion called inside a scoped submission group";
  CHECK(!m_nodes.empty()) << "ZVulkanLinearScript::flushAndWaitForCompletion requires pending GPU work";
  const std::string_view resolved = reason.empty() ? std::string_view("script_flush_wait") : reason;
  flushNodes(resolved, nullptr, true);
}

void ZVulkanLinearScript::validateDeps(std::string_view label, std::span<const SegmentHandle> deps) const
{
  (void)label;
  for (const auto& dep : deps) {
    CHECK(dep.id != 0) << "ZVulkanLinearScript: invalid dependency handle";
    CHECK(dep.id < m_nextSegmentId) << "ZVulkanLinearScript: dependency refers to a future segment (invalid)";
  }
}

ZVulkanLinearScript::SegmentHandle ZVulkanLinearScript::nextHandle()
{
  SegmentHandle h;
  h.id = m_nextSegmentId++;
  return h;
}

ZVulkanLinearScript::SegmentHandle ZVulkanLinearScript::raster(std::string_view label,
                                                               std::span<const SegmentHandle> deps,
                                                               const std::function<void()>& recordBatches)
{
  validateDeps(label, deps);
  const auto handle = nextHandle();
  m_pendingSubmissionHasGpuNodes = true;
  RasterNode node;
  node.label = std::string(label);
  const bool collectPerf = Z3DPerfCollector::enabled();
  const auto captureStart = collectPerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  node.state = m_renderer.captureVulkanBatches(recordBatches, label);
  if (collectPerf) {
    node.captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
  }
  m_nodes.emplace_back(std::move(node));
  if (strictResidencyFlushEachNode()) {
    flushNodes("strict_residency_node", nullptr);
  }
  return handle;
}

ZVulkanLinearScript::SegmentHandle
ZVulkanLinearScript::preRecord(std::string_view label,
                               std::span<const SegmentHandle> deps,
                               const std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)>& fn)
{
  validateDeps(label, deps);
  CHECK(!m_frameOpen) << "ZVulkanLinearScript::preRecord must be enqueued before the script begins recording";
  CHECK(fn) << "ZVulkanLinearScript::preRecord requires a valid function";

  const auto handle = nextHandle();
  PreRecordNode node;
  node.label = std::string(label);
  node.fn = fn;
  m_preRecordNodes.emplace_back(std::move(node));
  return handle;
}

ZVulkanLinearScript::SegmentHandle ZVulkanLinearScript::replay(std::string_view label,
                                                               std::span<const SegmentHandle> deps,
                                                               std::shared_ptr<RendererCPUState> state)
{
  validateDeps(label, deps);
  const auto handle = nextHandle();
  m_pendingSubmissionHasGpuNodes = true;
  ReplayNode node;
  node.label = std::string(label);
  if (state) {
    // Callers may patch captured batches (for example attaching a
    // viewStateOverride for replay-only overlays) before enqueuing replay.
    // Refresh the estimate from the final replay state so uniform arena sizing
    // matches what will actually execute.
    state->uniformBytesEstimate = m_backend.estimateAdditionalUniformBytesForBatches(*state);
  }
  node.state = std::move(state);
  m_nodes.emplace_back(std::move(node));
  if (strictResidencyFlushEachNode()) {
    flushNodes("strict_residency_node", nullptr);
  }
  return handle;
}

ZVulkanLinearScript::SegmentHandle
ZVulkanLinearScript::commands(std::string_view label,
                              std::span<const SegmentHandle> deps,
                              const std::function<void(Z3DRendererVulkanBackend&)>& record)
{
  validateDeps(label, deps);
  CHECK(record) << "ZVulkanLinearScript::commands requires a valid function";
  const auto handle = nextHandle();
  m_pendingSubmissionHasGpuNodes = true;
  CommandsNode node;
  node.label = std::string(label);
  node.record = record;
  m_nodes.emplace_back(std::move(node));
  // Command nodes may use backend helper APIs that prepare, clear, copy, or
  // read textures internally. Those texture effects are intentionally hidden
  // from call sites, so the linear script cannot include them in the later
  // hot-set prepass without executing the command. Treat the command node as
  // an existing safe point: submit everything up to and including it before
  // subsequent raster nodes infer their texture contents requirements.
  flushNodes(strictResidencyFlushEachNode() ? "strict_residency_node" : "command_node", nullptr);
  return handle;
}

ZVulkanLinearScript::SegmentHandle
ZVulkanLinearScript::commandsInSubmission(std::string_view label,
                                          std::span<const SegmentHandle> deps,
                                          const std::function<void(Z3DRendererVulkanBackend&)>& record,
                                          ZVulkanSubmissionRequirements requirements)
{
  return commandsInSubmissionWithScratchUses(label, deps, {}, record, requirements);
}

ZVulkanLinearScript::SegmentHandle ZVulkanLinearScript::commandsInSubmissionWithScratchUses(
  std::string_view label,
  std::span<const SegmentHandle> deps,
  std::span<const Z3DScratchResourcePool::VulkanScratchTextureUse> scratchTextureUses,
  const std::function<void(Z3DRendererVulkanBackend&)>& record,
  ZVulkanSubmissionRequirements requirements)
{
  validateDeps(label, deps);
  CHECK(record) << "ZVulkanLinearScript::commandsInSubmissionWithScratchUses requires a valid function";
  const auto handle = nextHandle();
  m_pendingSubmissionHasGpuNodes = true;
  CommandsNode node;
  node.label = std::string(label);
  node.record = record;
  node.scratchTextureUses.assign(scratchTextureUses.begin(), scratchTextureUses.end());
  node.requirements = requirements;
  m_nodes.emplace_back(std::move(node));
  return handle;
}

ZVulkanLinearScript::ScopedSubmissionGroup ZVulkanLinearScript::scopedSubmissionGroup()
{
  return ScopedSubmissionGroup(*this);
}

void ZVulkanLinearScript::setReadbackSource(ReadbackBufferSpec& spec, ZVulkanBuffer& src)
{
  spec.src = &src;
  spec.srcSlot.reset();
}

void ZVulkanLinearScript::setReadbackSource(ReadbackBufferSpec& spec, const Slot<ZVulkanBuffer*>& srcSlot)
{
  spec.src = nullptr;
  spec.srcSlot = srcSlot;
}

void ZVulkanLinearScript::readbackBufferTo(std::string_view label,
                                           std::span<const SegmentHandle> deps,
                                           ZVulkanBuffer& src,
                                           vk::DeviceSize srcOffset,
                                           void* dst,
                                           size_t bytes)
{
  CHECK_EQ(m_submissionGroupDepth, 0u)
    << "ZVulkanLinearScript::readbackBufferTo called inside a scoped submission group";
  validateDeps(label, deps);
  CHECK(dst != nullptr) << "ZVulkanLinearScript::readbackBufferTo requires dst";
  CHECK_GT(bytes, 0u) << "ZVulkanLinearScript::readbackBufferTo requires bytes > 0";

  ReadbackBufferSpec spec;
  spec.label = std::string(label);
  setReadbackSource(spec, src);
  spec.srcOffset = srcOffset;
  spec.dst = dst;
  spec.bytes = bytes;
  flushNodes("readback", &spec);
}

void ZVulkanLinearScript::readbackBufferTo(std::string_view label,
                                           std::span<const SegmentHandle> deps,
                                           const Slot<ZVulkanBuffer*>& srcSlot,
                                           vk::DeviceSize srcOffset,
                                           void* dst,
                                           size_t bytes)
{
  CHECK_EQ(m_submissionGroupDepth, 0u)
    << "ZVulkanLinearScript::readbackBufferTo called inside a scoped submission group";
  validateDeps(label, deps);
  CHECK(dst != nullptr) << "ZVulkanLinearScript::readbackBufferTo requires dst";
  CHECK_GT(bytes, 0u) << "ZVulkanLinearScript::readbackBufferTo requires bytes > 0";

  ReadbackBufferSpec spec;
  spec.label = std::string(label);
  setReadbackSource(spec, srcSlot);
  spec.srcOffset = srcOffset;
  spec.dst = dst;
  spec.bytes = bytes;
  flushNodes("readback", &spec);
}

uint32_t ZVulkanLinearScript::readbackU32(std::string_view label,
                                          std::span<const SegmentHandle> deps,
                                          ZVulkanBuffer& src,
                                          vk::DeviceSize srcOffset)
{
  uint32_t out = 0u;
  readbackBufferTo(label, deps, src, srcOffset, &out, sizeof(uint32_t));
  return out;
}

uint32_t ZVulkanLinearScript::readbackU32(std::string_view label,
                                          std::span<const SegmentHandle> deps,
                                          const Slot<ZVulkanBuffer*>& srcSlot,
                                          vk::DeviceSize srcOffset)
{
  uint32_t out = 0u;
  readbackBufferTo(label, deps, srcSlot, srcOffset, &out, sizeof(uint32_t));
  return out;
}

void ZVulkanLinearScript::drainNodesIntoExecutionOrder(std::vector<Node>& out)
{
  out.clear();
  out.reserve(m_nodes.size());
  for (auto& node : m_nodes) {
    out.emplace_back(std::move(node));
  }
  m_nodes.clear();
}

bool ZVulkanLinearScript::strictResidencyFlushEachNode() const
{
  return m_submissionGroupDepth == 0u && m_backend.device().residencyManager().strictBudgetActive();
}

bool ZVulkanLinearScript::shouldFlushForResidencyBeforeIndependentWork(std::string_view reason) const
{
  if (m_nodes.empty() && m_preRecordNodes.empty() && !m_frameOpen) {
    return false;
  }

  auto& residency = m_backend.device().residencyManager();
  if (residency.strictBudgetActive()) {
    VLOG(1) << fmt::format("Vulkan script residency split enabled by strict budget: reason='{}'",
                           reason.empty() ? "<unspecified>" : std::string(reason));
    return true;
  }

  const auto pendingNodeSpan = std::span<const Node>(m_nodes.data(), m_nodes.size());
  const auto scratchUses = collectScratchTextureUsesForNodes(pendingNodeSpan);
  const auto scratchEstimate =
    Z3DRenderGlobalState::instance().scratchPool().estimateVulkanScratchTexturesForPass(scratchUses);
  const auto pressure = residency.allocationPressureFor(scratchEstimate.missingBytes);
  const bool shouldFlush = pressure.needsReclaim();
  VLOG(2) << fmt::format(
    "Vulkan script residency split check: reason='{}' flush={} pending_scratch_missing={}B pending_scratch_hot={}B scratch_images={} scratch_textures={} usage={}B budget={}B reclaim={}B",
    reason.empty() ? "<unspecified>" : std::string(reason),
    shouldFlush,
    scratchEstimate.missingBytes,
    scratchEstimate.hotTotalBytes,
    scratchEstimate.hotImageCount,
    scratchEstimate.textureCount,
    pressure.usageBytes,
    pressure.budgetBytes,
    pressure.reclaimBytes);
  return shouldFlush;
}

void ZVulkanLinearScript::enterSubmissionGroup()
{
  CHECK(m_submissionGroupDepth < std::numeric_limits<uint32_t>::max())
    << "ZVulkanLinearScript submission group depth overflow";
  ++m_submissionGroupDepth;
}

void ZVulkanLinearScript::leaveSubmissionGroup()
{
  CHECK_GT(m_submissionGroupDepth, 0u) << "ZVulkanLinearScript submission group underflow";
  --m_submissionGroupDepth;
}

std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse>
ZVulkanLinearScript::collectScratchTextureUsesForNodes(std::span<const Node> nodes) const
{
  std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse> uses;
  std::unordered_map<ZVulkanTexture*, size_t> useIndex;
  std::unordered_set<ZVulkanTexture*> availableWithinFlush;

  auto textureFromHandle = [](const AttachmentHandle& handle) -> ZVulkanTexture* {
    if (!handle.valid() || handle.backend != RenderBackend::Vulkan) {
      return nullptr;
    }
    return reinterpret_cast<ZVulkanTexture*>(handle.id);
  };

  auto ensureUse = [&](ZVulkanTexture* texture) -> size_t {
    CHECK(texture != nullptr);
    auto [it, inserted] = useIndex.emplace(texture, uses.size());
    if (inserted) {
      uses.push_back(Z3DScratchResourcePool::VulkanScratchTextureUse{.texture = texture, .contentsRequired = false});
    }
    return it->second;
  };

  auto requireInitialContents = [&](ZVulkanTexture* texture) {
    if (texture == nullptr) {
      return;
    }
    const size_t index = ensureUse(texture);
    if (availableWithinFlush.find(texture) == availableWithinFlush.end()) {
      uses[index].contentsRequired = true;
    }
  };

  auto markWrite = [&](ZVulkanTexture* texture, bool loadExistingContents) {
    if (texture == nullptr) {
      return;
    }
    if (loadExistingContents) {
      requireInitialContents(texture);
    } else {
      (void)ensureUse(texture);
    }
    availableWithinFlush.insert(texture);
  };

  auto appendOrderedUsesFromBatch = [&](const RenderBatch& batch) {
    for (const auto& attachment : batch.pass.colorAttachments) {
      markWrite(textureFromHandle(attachment.handle), attachment.loadOp == LoadOp::Load);
    }
    if (batch.pass.depthAttachment) {
      markWrite(textureFromHandle(batch.pass.depthAttachment->handle),
                batch.pass.depthAttachment->loadOp == LoadOp::Load);
    }
    if (batch.pass.resolveAttachment) {
      markWrite(textureFromHandle(batch.pass.resolveAttachment->handle),
                batch.pass.resolveAttachment->loadOp == LoadOp::Load);
    }

    for (const auto& use : batch.pass.externalImageUses) {
      auto* texture = textureFromHandle(use.handle);
      if (externalImageUseRequiresContents(use.kind)) {
        requireInitialContents(texture);
      } else if (texture != nullptr) {
        (void)ensureUse(texture);
      }
      if (texture != nullptr &&
          (use.kind == ExternalImageUseKind::StorageWrite || use.kind == ExternalImageUseKind::StorageReadWrite ||
           use.kind == ExternalImageUseKind::TransferDst)) {
        availableWithinFlush.insert(texture);
      }
    }

    std::vector<ExternalImageUseDesc> sampledUses;
    appendBindlessSampledImageUsesFromBatch(batch, sampledUses, nullptr);
    for (const auto& use : sampledUses) {
      requireInitialContents(textureFromHandle(use.handle));
    }

    auto requireLeaseColors = [&](const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& lease,
                                  uint32_t requestedAttachments) {
      if (!lease || !lease->hasVulkanImage()) {
        return;
      }
      const uint32_t count =
        requestedAttachments == 0u ? lease->attachments : std::min(requestedAttachments, lease->attachments);
      for (uint32_t i = 0; i < count; ++i) {
        requireInitialContents(lease->colorAttachment(i));
      }
    };

    std::visit(
      [&](auto&& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, ImgRaycasterPayload>) {
          if (payload.stage == ImgRaycasterPayload::Stage::ProgressiveCompaction) {
            requireLeaseColors(payload.blockIdLease, payload.blockIdEffectiveAttachmentCount);
          }
        } else if constexpr (std::is_same_v<T, ImgSlicePayload>) {
          if (payload.stage == ImgSlicePayload::Stage::BlockIdDiscovery && payload.blockIdLease) {
            requireLeaseColors(payload.blockIdLease, 1u);
          }
        } else {
        }
      },
      batch.geometry);
  };

  uses.reserve(64);
  useIndex.reserve(64);
  for (const auto& node : nodes) {
    if (const auto* rasterNode = std::get_if<RasterNode>(&node)) {
      for (const auto& batch : rasterNode->state.batches) {
        appendOrderedUsesFromBatch(batch);
      }
      continue;
    }
    if (const auto* replayNode = std::get_if<ReplayNode>(&node)) {
      if (replayNode->state) {
        for (const auto& batch : replayNode->state->batches) {
          appendOrderedUsesFromBatch(batch);
        }
      }
      continue;
    }
    if (const auto* commandsNode = std::get_if<CommandsNode>(&node)) {
      for (const auto& use : commandsNode->scratchTextureUses) {
        if (use.texture == nullptr) {
          continue;
        }
        if (use.contentsRequired) {
          requireInitialContents(use.texture);
        } else {
          markWrite(use.texture, /*loadExistingContents=*/false);
        }
      }
      continue;
    }
  }
  return uses;
}

std::vector<ZVulkanTexture*> ZVulkanLinearScript::collectTexturePointersForNodes(std::span<const Node> nodes) const
{
  std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse> uses;
  uses.reserve(64);
  for (const auto& node : nodes) {
    if (const auto* rasterNode = std::get_if<RasterNode>(&node)) {
      appendTextureUsesFromState(rasterNode->state, uses);
      continue;
    }
    if (const auto* replayNode = std::get_if<ReplayNode>(&node)) {
      if (replayNode->state) {
        appendTextureUsesFromState(*replayNode->state, uses);
      }
      continue;
    }
  }

  std::vector<ZVulkanTexture*> textures;
  textures.reserve(uses.size());
  std::unordered_set<ZVulkanTexture*> seen;
  seen.reserve(uses.size());
  for (const auto& use : uses) {
    if (use.texture == nullptr || !seen.insert(use.texture).second) {
      continue;
    }
    textures.push_back(use.texture);
  }
  return textures;
}

void ZVulkanLinearScript::flushNodes(std::string_view reason,
                                     /*nullable*/ const ReadbackBufferSpec* readback,
                                     bool waitForCompletion)
{
  if (m_nodes.empty() && readback == nullptr) {
    CHECK(m_preRecordNodes.empty()) << "ZVulkanLinearScript: preRecord actions enqueued without any GPU work";
    return;
  }

  std::string firstLabelStorage;
  if (!m_nodes.empty()) {
    const auto& first = m_nodes.front();
    if (const auto* rasterNode = std::get_if<RasterNode>(&first)) {
      firstLabelStorage = rasterNode->label;
    } else if (const auto* replayNode = std::get_if<ReplayNode>(&first)) {
      firstLabelStorage = replayNode->label;
    } else if (const auto* cmdNode = std::get_if<CommandsNode>(&first)) {
      firstLabelStorage = cmdNode->label;
    }
  } else if (readback != nullptr) {
    firstLabelStorage = readback->label;
  }
  const std::string_view firstLabel = firstLabelStorage;

  // Uniform arena sizing: beginRender() suballocates per-frame lighting slices,
  // and some pipelines suballocate per-batch UBOs from the same arena. Provide
  // a conservative upper bound *before* opening the Vulkan frame so the backend
  // can allocate a stable arena buffer for the entire submission.
  //
  // This intentionally does not attempt to account for commands()/preRecord()
  // callbacks allocating from the uniform arena; those should remain rare (and
  // ideally avoided) because they are opaque to estimation.
  uint32_t nodeCount = static_cast<uint32_t>(m_nodes.size());
  uint32_t rasterNodeCount = 0;
  uint32_t replayNodeCount = 0;
  uint32_t commandsNodeCount = 0;
  uint32_t batchCount = 0;
  const uint32_t preRecordNodeCount = static_cast<uint32_t>(m_preRecordNodes.size());
  Z3DRendererVulkanBackend::SubmissionRequirements submissionRequirements =
    Z3DRendererVulkanBackend::requirementMask(Z3DRendererVulkanBackend::SubmissionRequirement::None);

  const auto collectSubmissionRequirements = [&](const RendererCPUState& state) {
    for (const auto& batch : state.batches) {
      if (batch.shaderHook.type == ShaderHookType::DualDepthPeelingInit ||
          batch.shaderHook.type == ShaderHookType::DualDepthPeelingPeel) {
        submissionRequirements |=
          Z3DRendererVulkanBackend::requirementMask(Z3DRendererVulkanBackend::SubmissionRequirement::DDPGating);
      }
    }
  };

  const bool collectPerf = Z3DPerfCollector::enabled();
  const auto uniformHintStart =
    collectPerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  size_t uniformBytesHint = m_backend.estimateFrameUniformOverheadBytes();
  for (const auto& node : m_nodes) {
    if (const auto* rasterNode = std::get_if<RasterNode>(&node)) {
      rasterNodeCount++;
      batchCount += static_cast<uint32_t>(rasterNode->state.batches.size());
      uniformBytesHint += rasterNode->state.uniformBytesEstimate;
      collectSubmissionRequirements(rasterNode->state);
    } else if (const auto* replayNode = std::get_if<ReplayNode>(&node)) {
      replayNodeCount++;
      CHECK(replayNode->state) << "ZVulkanLinearScript replay node missing state";
      batchCount += static_cast<uint32_t>(replayNode->state->batches.size());
      uniformBytesHint += replayNode->state->uniformBytesEstimate;
      collectSubmissionRequirements(*replayNode->state);
    } else if (const auto* cmdNode = std::get_if<CommandsNode>(&node)) {
      commandsNodeCount++;
      submissionRequirements |= cmdNode->requirements;
      // commands()/preRecord() are call-site defined and may mutate backend state
      // beyond what we can infer from batches.
    }
  }
  m_backend.hintNextUniformArenaMinCapacity(uniformBytesHint);
  const double uniformHintMs =
    collectPerf ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uniformHintStart).count()
                : 0.0;

  if (!m_frameOpen) {
    m_backend.setPendingBeginRenderRequirements(submissionRequirements);
    if (collectPerf) {
      Z3DRendererVulkanBackend::BeginRenderScriptStats stats{};
      stats.uniformHintMs = uniformHintMs;
      stats.uniformHintBytes = uniformBytesHint;
      stats.nodeCount = nodeCount;
      stats.rasterNodeCount = rasterNodeCount;
      stats.replayNodeCount = replayNodeCount;
      stats.commandsNodeCount = commandsNodeCount;
      stats.preRecordNodeCount = preRecordNodeCount;
      stats.batchCount = batchCount;
      m_backend.setPendingBeginRenderScriptStats(std::move(stats));
    }
  }

  const auto pendingNodeSpan = std::span<const Node>(m_nodes.data(), m_nodes.size());
  const std::vector<ZVulkanTexture*> hotTextures = collectTexturePointersForNodes(pendingNodeSpan);
  const std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse> scratchTextureUses =
    collectScratchTextureUsesForNodes(pendingNodeSpan);
  auto scratchHotProtection = std::make_shared<std::optional<Z3DScratchResourcePool::VulkanScratchProtectionScope>>();

  // Bindless pre-registration:
  // Scan all nodes to discover sampled image usage before opening the Vulkan
  // frame (beginRender) so the backend can update the per-frame-slot bindless
  // descriptor tables before command-buffer recording begins.
  //
  // This avoids per-draw descriptor churn and preserves the invariant that we
  // never update descriptors while a command buffer might read them.
  if (!m_frameOpen) {
    std::vector<ExternalImageUseDesc> sampledUses;
    sampledUses.reserve(64);
    std::vector<Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc> fontAtlases;
    fontAtlases.reserve(8);
    std::unordered_map<RaycasterWarmupKey, std::vector<size_t>, RaycasterWarmupKeyHash> raycasterWarmupChannels;
    raycasterWarmupChannels.reserve(16);
    std::unordered_map<SliceWarmupKey, std::vector<size_t>, SliceWarmupKeyHash> sliceWarmupChannels;
    sliceWarmupChannels.reserve(16);

    struct RaycasterBlockIdCompactionPrime
    {
      std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> lease;
      // 0 means "use lease->attachments" (parity with record() semantics).
      uint32_t effectiveAttachmentCount = 0;
      uint32_t maxBlockId = 0;
    };
    std::unordered_map<const Z3DScratchResourcePool::RenderTargetLease*, RaycasterBlockIdCompactionPrime>
      raycasterBlockIdCompactionPrimes;
    raycasterBlockIdCompactionPrimes.reserve(8);

    struct SliceBlockIdCompactionPrime
    {
      std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> lease;
      uint32_t sliceCount = 0;
      uint32_t maxBlockId = 0;
      bool hasMaxBlockId = false;
      std::unordered_set<uint32_t> sliceIndices;
    };
    std::unordered_map<const Z3DScratchResourcePool::RenderTargetLease*, SliceBlockIdCompactionPrime>
      sliceBlockIdCompactionPrimes;
    sliceBlockIdCompactionPrimes.reserve(8);

    for (const auto& node : m_nodes) {
      if (const auto* rasterNode = std::get_if<RasterNode>(&node)) {
        for (const auto& batch : rasterNode->state.batches) {
          appendBindlessSampledImageUsesFromBatch(batch, sampledUses, &fontAtlases);
          std::visit(
            [&](auto&& payload) {
              using T = std::decay_t<decltype(payload)>;
              if constexpr (std::is_same_v<T, ImgRaycasterPayload>) {
                if (payload.image == nullptr || payload.visibleChannels.empty()) {
                  return;
                }

                if (payload.stage == ImgRaycasterPayload::Stage::ProgressiveCompaction) {
                  if (payload.blockIdLease && payload.blockIdLease->hasVulkanImage()) {
                    auto* raw = payload.blockIdLease.get();
                    auto& entry = raycasterBlockIdCompactionPrimes[raw];
                    if (!entry.lease) {
                      entry.lease = payload.blockIdLease;
                    }
                    const uint32_t request = payload.blockIdEffectiveAttachmentCount;
                    if (entry.effectiveAttachmentCount == 0u || request == 0u) {
                      // 0 means "use all attachments" (parity with record() logic).
                      entry.effectiveAttachmentCount = 0u;
                    } else {
                      entry.effectiveAttachmentCount = std::max(entry.effectiveAttachmentCount, request);
                    }
                    entry.maxBlockId = std::max(entry.maxBlockId, payload.image->maxPagedBlockID());
                  }
                }

                const bool planarGeometry = payload.planarGeometry;

                bool needsWarmup = false;
                bool wants2D = false;
                bool wantsVolume3D = true;
                bool wantsPaging = false;
                std::vector<size_t> channels;

                auto resolveProgressiveChannel = [&]() -> std::optional<size_t> {
                  if (payload.channelIndexRaw < 0) {
                    return std::nullopt;
                  }
                  const size_t raw = static_cast<size_t>(payload.channelIndexRaw);
                  CHECK_LT(raw, payload.visibleChannels.size())
                    << "Vulkan script: raycaster channelIndexRaw out of range for visibleChannels";
                  return payload.visibleChannels[raw];
                };

                switch (payload.stage) {
                  case ImgRaycasterPayload::Stage::FastDirect: {
                    needsWarmup = true;
                    wants2D = planarGeometry && payload.image->is2DData();
                    wantsVolume3D = !wants2D;
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::FastLayers: {
                    needsWarmup = true;
                    wants2D = planarGeometry && payload.image->is2DData();
                    wantsVolume3D = !(planarGeometry && payload.image->is2DData());
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePreviewLayers: {
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = false;
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressiveBlockId:
                  case ImgRaycasterPayload::Stage::ProgressiveRaycast: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePlanarBlockId: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = false;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePlanarDrawLayer: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  default:
                    break;
                }

                if (!needsWarmup || channels.empty()) {
                  return;
                }
                CHECK(payload.transferFunctions != nullptr)
                  << "Vulkan script: raycaster warmup requires transferFunctions vector";

                RaycasterWarmupKey key{};
                key.image = payload.image;
                key.transferFunctions = payload.transferFunctions;
                key.wants2D = wants2D;
                key.wantsVolume3D = wantsVolume3D;
                key.wantsPaging = wantsPaging;

                auto& dest = raycasterWarmupChannels[key];
                dest.insert(dest.end(), channels.begin(), channels.end());
              } else if constexpr (std::is_same_v<T, ImgSlicePayload>) {
                if (payload.image == nullptr || payload.slices.empty()) {
                  return;
                }

                const bool usePaging = (!payload.fastPathOnly && payload.image->isVolumeDownsampled());

                if (batch.pass.kind == BackendPassDesc::Kind::Compute &&
                    payload.stage == ImgSlicePayload::Stage::BlockIdDiscovery) {
                  const bool primeCompaction = usePaging && payload.streamKey != 0u && payload.channelIndexRaw >= 0 &&
                                               payload.roundIndexRaw == 0 && payload.blockIdLease &&
                                               payload.blockIdLease->hasVulkanImage() &&
                                               payload.blockIdSliceIndexRaw >= 0;
                  if (primeCompaction) {
                    const size_t sliceCount = payload.slices.size();
                    CHECK_GT(sliceCount, 0u);
                    const size_t sliceIndex = static_cast<size_t>(payload.blockIdSliceIndexRaw);
                    CHECK_LT(sliceIndex, sliceCount) << "Vulkan script: slice blockId slice index out of range";
                    CHECK(sliceCount <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
                      << "Vulkan script: sliceCount exceeds uint32 range";

                    auto* raw = payload.blockIdLease.get();
                    auto& entry = sliceBlockIdCompactionPrimes[raw];
                    if (!entry.lease) {
                      entry.lease = payload.blockIdLease;
                    }
                    const uint32_t sliceCountU32 = static_cast<uint32_t>(sliceCount);
                    const uint32_t sliceIndexU32 = static_cast<uint32_t>(sliceIndex);
                    if (entry.sliceCount == 0u) {
                      entry.sliceCount = sliceCountU32;
                    } else {
                      CHECK(entry.sliceCount == sliceCountU32)
                        << "Vulkan script: inconsistent sliceCount for blockIdLease";
                    }
                    const uint32_t maxBlockId = payload.image->maxPagedBlockID();
                    if (!entry.hasMaxBlockId) {
                      entry.maxBlockId = maxBlockId;
                      entry.hasMaxBlockId = true;
                    } else {
                      CHECK(entry.maxBlockId == maxBlockId)
                        << "Vulkan script: inconsistent maxBlockId for blockIdLease";
                    }
                    entry.sliceIndices.insert(sliceIndexU32);
                  }
                }

                bool needsWarmup = false;
                bool wantsVolume3D = false;
                bool wantsColormap = false;
                bool wantsPaging = false;
                std::vector<size_t> channels;

                switch (payload.stage) {
                  case ImgSlicePayload::Stage::DrawLayers: {
                    needsWarmup = true;
                    wantsVolume3D = true;
                    wantsColormap = true;
                    wantsPaging =
                      usePaging && payload.streamKey != 0u && payload.channelIndexRaw >= 0 && payload.roundIndexRaw > 0;

                    const bool layered = (payload.layerLease != nullptr);
                    if (layered) {
                      CHECK(!batch.pass.colorAttachments.empty())
                        << "Vulkan script: slice layered draw missing attachments";
                      channels.push_back(static_cast<size_t>(batch.pass.colorAttachments.front().handle.index));
                    } else {
                      channels.push_back(0u);
                    }
                    break;
                  }
                  case ImgSlicePayload::Stage::BlockIdDiscovery: {
                    if (!usePaging || payload.streamKey == 0u) {
                      return;
                    }
                    if (payload.channelIndexRaw < 0) {
                      return;
                    }
                    needsWarmup = true;
                    wantsVolume3D = false;
                    wantsColormap = false;
                    wantsPaging = true;
                    channels.push_back(static_cast<size_t>(payload.channelIndexRaw));
                    break;
                  }
                  default:
                    break;
                }

                if (!needsWarmup || channels.empty()) {
                  return;
                }

                SliceWarmupKey key{};
                key.image = payload.image;
                key.colormaps = payload.colormaps;
                key.wantsVolume3D = wantsVolume3D;
                key.wantsColormap = wantsColormap;
                key.wantsPaging = wantsPaging;

                auto& dest = sliceWarmupChannels[key];
                dest.insert(dest.end(), channels.begin(), channels.end());
              } else {
                // Other payload types expose bindless sampled images via externalImageUses and shaderHook metadata.
              }
            },
            batch.geometry);
        }
        continue;
      }
      if (const auto* replayNode = std::get_if<ReplayNode>(&node)) {
        if (!replayNode->state) {
          continue;
        }
        for (const auto& batch : replayNode->state->batches) {
          appendBindlessSampledImageUsesFromBatch(batch, sampledUses, &fontAtlases);
          std::visit(
            [&](auto&& payload) {
              using T = std::decay_t<decltype(payload)>;
              if constexpr (std::is_same_v<T, ImgRaycasterPayload>) {
                if (payload.image == nullptr || payload.visibleChannels.empty()) {
                  return;
                }

                if (payload.stage == ImgRaycasterPayload::Stage::ProgressiveCompaction) {
                  if (payload.blockIdLease && payload.blockIdLease->hasVulkanImage()) {
                    auto* raw = payload.blockIdLease.get();
                    auto& entry = raycasterBlockIdCompactionPrimes[raw];
                    if (!entry.lease) {
                      entry.lease = payload.blockIdLease;
                    }
                    const uint32_t request = payload.blockIdEffectiveAttachmentCount;
                    if (entry.effectiveAttachmentCount == 0u || request == 0u) {
                      // 0 means "use all attachments" (parity with record() logic).
                      entry.effectiveAttachmentCount = 0u;
                    } else {
                      entry.effectiveAttachmentCount = std::max(entry.effectiveAttachmentCount, request);
                    }
                    entry.maxBlockId = std::max(entry.maxBlockId, payload.image->maxPagedBlockID());
                  }
                }

                const bool planarGeometry = payload.planarGeometry;

                bool needsWarmup = false;
                bool wants2D = false;
                bool wantsVolume3D = true;
                bool wantsPaging = false;
                std::vector<size_t> channels;

                auto resolveProgressiveChannel = [&]() -> std::optional<size_t> {
                  if (payload.channelIndexRaw < 0) {
                    return std::nullopt;
                  }
                  const size_t raw = static_cast<size_t>(payload.channelIndexRaw);
                  CHECK_LT(raw, payload.visibleChannels.size())
                    << "Vulkan script: raycaster channelIndexRaw out of range for visibleChannels";
                  return payload.visibleChannels[raw];
                };

                switch (payload.stage) {
                  case ImgRaycasterPayload::Stage::FastDirect: {
                    needsWarmup = true;
                    wants2D = planarGeometry && payload.image->is2DData();
                    wantsVolume3D = !wants2D;
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::FastLayers: {
                    needsWarmup = true;
                    wants2D = planarGeometry && payload.image->is2DData();
                    wantsVolume3D = !(planarGeometry && payload.image->is2DData());
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePreviewLayers: {
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = false;
                    channels.assign(payload.visibleChannels.begin(), payload.visibleChannels.end());
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressiveBlockId:
                  case ImgRaycasterPayload::Stage::ProgressiveRaycast: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePlanarBlockId: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = false;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  case ImgRaycasterPayload::Stage::ProgressivePlanarDrawLayer: {
                    const auto resolved = resolveProgressiveChannel();
                    if (!resolved.has_value()) {
                      return;
                    }
                    needsWarmup = true;
                    wants2D = false;
                    wantsVolume3D = true;
                    wantsPaging = true;
                    channels.push_back(*resolved);
                    break;
                  }
                  default:
                    break;
                }

                if (!needsWarmup || channels.empty()) {
                  return;
                }
                CHECK(payload.transferFunctions != nullptr)
                  << "Vulkan script: raycaster warmup requires transferFunctions vector";

                RaycasterWarmupKey key{};
                key.image = payload.image;
                key.transferFunctions = payload.transferFunctions;
                key.wants2D = wants2D;
                key.wantsVolume3D = wantsVolume3D;
                key.wantsPaging = wantsPaging;

                auto& dest = raycasterWarmupChannels[key];
                dest.insert(dest.end(), channels.begin(), channels.end());
              } else if constexpr (std::is_same_v<T, ImgSlicePayload>) {
                if (payload.image == nullptr || payload.slices.empty()) {
                  return;
                }

                const bool usePaging = (!payload.fastPathOnly && payload.image->isVolumeDownsampled());

                if (batch.pass.kind == BackendPassDesc::Kind::Compute &&
                    payload.stage == ImgSlicePayload::Stage::BlockIdDiscovery) {
                  const bool primeCompaction = usePaging && payload.streamKey != 0u && payload.channelIndexRaw >= 0 &&
                                               payload.roundIndexRaw == 0 && payload.blockIdLease &&
                                               payload.blockIdLease->hasVulkanImage() &&
                                               payload.blockIdSliceIndexRaw >= 0;
                  if (primeCompaction) {
                    const size_t sliceCount = payload.slices.size();
                    CHECK_GT(sliceCount, 0u);
                    const size_t sliceIndex = static_cast<size_t>(payload.blockIdSliceIndexRaw);
                    CHECK_LT(sliceIndex, sliceCount) << "Vulkan script: slice blockId slice index out of range";
                    CHECK(sliceCount <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
                      << "Vulkan script: sliceCount exceeds uint32 range";

                    auto* raw = payload.blockIdLease.get();
                    auto& entry = sliceBlockIdCompactionPrimes[raw];
                    if (!entry.lease) {
                      entry.lease = payload.blockIdLease;
                    }
                    const uint32_t sliceCountU32 = static_cast<uint32_t>(sliceCount);
                    const uint32_t sliceIndexU32 = static_cast<uint32_t>(sliceIndex);
                    if (entry.sliceCount == 0u) {
                      entry.sliceCount = sliceCountU32;
                    } else {
                      CHECK(entry.sliceCount == sliceCountU32)
                        << "Vulkan script: inconsistent sliceCount for blockIdLease";
                    }
                    const uint32_t maxBlockId = payload.image->maxPagedBlockID();
                    if (!entry.hasMaxBlockId) {
                      entry.maxBlockId = maxBlockId;
                      entry.hasMaxBlockId = true;
                    } else {
                      CHECK(entry.maxBlockId == maxBlockId)
                        << "Vulkan script: inconsistent maxBlockId for blockIdLease";
                    }
                    entry.sliceIndices.insert(sliceIndexU32);
                  }
                }

                bool needsWarmup = false;
                bool wantsVolume3D = false;
                bool wantsColormap = false;
                bool wantsPaging = false;
                std::vector<size_t> channels;

                switch (payload.stage) {
                  case ImgSlicePayload::Stage::DrawLayers: {
                    needsWarmup = true;
                    wantsVolume3D = true;
                    wantsColormap = true;
                    wantsPaging =
                      usePaging && payload.streamKey != 0u && payload.channelIndexRaw >= 0 && payload.roundIndexRaw > 0;

                    const bool layered = (payload.layerLease != nullptr);
                    if (layered) {
                      CHECK(!batch.pass.colorAttachments.empty())
                        << "Vulkan script: slice layered draw missing attachments";
                      channels.push_back(static_cast<size_t>(batch.pass.colorAttachments.front().handle.index));
                    } else {
                      channels.push_back(0u);
                    }
                    break;
                  }
                  case ImgSlicePayload::Stage::BlockIdDiscovery: {
                    if (!usePaging || payload.streamKey == 0u) {
                      return;
                    }
                    if (payload.channelIndexRaw < 0) {
                      return;
                    }
                    needsWarmup = true;
                    wantsVolume3D = false;
                    wantsColormap = false;
                    wantsPaging = true;
                    channels.push_back(static_cast<size_t>(payload.channelIndexRaw));
                    break;
                  }
                  default:
                    break;
                }

                if (!needsWarmup || channels.empty()) {
                  return;
                }

                SliceWarmupKey key{};
                key.image = payload.image;
                key.colormaps = payload.colormaps;
                key.wantsVolume3D = wantsVolume3D;
                key.wantsColormap = wantsColormap;
                key.wantsPaging = wantsPaging;

                auto& dest = sliceWarmupChannels[key];
                dest.insert(dest.end(), channels.begin(), channels.end());
              } else {
                // Other payload types expose bindless sampled images via externalImageUses and shaderHook metadata.
              }
            },
            batch.geometry);
        }
        continue;
      }
      // CommandsNode is opaque; call sites must register their own bindless
      // sampled images via script.preRecord() if they introduce new textures.
    }

    // Font atlases are supplied as CPU pixels (BGRA8) rather than Vulkan handles.
    // They must be uploaded and registered before recording begins.
    if (!fontAtlases.empty()) {
      std::unordered_set<FontAtlasKey, FontAtlasKeyHash> seenAtlases;
      seenAtlases.reserve(fontAtlases.size());
      std::vector<Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc> uniqueAtlases;
      uniqueAtlases.reserve(fontAtlases.size());

      for (const auto& atlas : fontAtlases) {
        if (atlas.pixelsBGRA8 == nullptr || atlas.width == 0u || atlas.height == 0u) {
          continue;
        }
        FontAtlasKey k{};
        k.pixelsBGRA8 = atlas.pixelsBGRA8;
        k.width = atlas.width;
        k.height = atlas.height;
        if (seenAtlases.insert(k).second) {
          uniqueAtlases.push_back(atlas);
        }
      }

      if (!uniqueAtlases.empty()) {
        auto sharedAtlases = std::make_shared<std::vector<Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc>>(
          std::move(uniqueAtlases));
        PreRecordNode fontNode;
        fontNode.label = "bindless_register_font_atlases";
        fontNode.fn = [atlases = std::move(sharedAtlases)](Z3DRendererVulkanBackend& backend,
                                                           Z3DRendererBase& renderer) {
          (void)renderer;
          backend.bindlessPreRegisterFontAtlasPixels(
            std::span<const Z3DRendererVulkanBackend::BindlessFontAtlasPixelsDesc>(atlases->data(), atlases->size()),
            "linear_script");
        };
        m_preRecordNodes.emplace_back(std::move(fontNode));
      }
    }

    if (!raycasterWarmupChannels.empty()) {
      std::vector<RaycasterWarmupGroup> groups;
      groups.reserve(raycasterWarmupChannels.size());
      for (auto& [key, channels] : raycasterWarmupChannels) {
        if (channels.empty()) {
          continue;
        }
        std::sort(channels.begin(), channels.end());
        channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
        RaycasterWarmupGroup g{};
        g.key = key;
        g.channels = std::move(channels);
        groups.emplace_back(std::move(g));
      }

      if (!groups.empty()) {
        auto sharedGroups = std::make_shared<std::vector<RaycasterWarmupGroup>>(std::move(groups));
        PreRecordNode warmupNode;
        warmupNode.label = "bindless_warmup_img_raycaster";
        warmupNode.fn = [groups = std::move(sharedGroups)](Z3DRendererVulkanBackend& backend,
                                                           Z3DRendererBase& renderer) {
          (void)renderer;
          for (const auto& g : *groups) {
            backend.bindlessPreWarmupImgRaycaster(g.key.image,
                                                  g.key.transferFunctions,
                                                  std::span<const size_t>(g.channels.data(), g.channels.size()),
                                                  g.key.wants2D,
                                                  g.key.wantsVolume3D,
                                                  g.key.wantsPaging,
                                                  "linear_script");
          }
        };
        m_preRecordNodes.emplace_back(std::move(warmupNode));
      }
    }

    if (!sliceWarmupChannels.empty()) {
      std::vector<SliceWarmupGroup> groups;
      groups.reserve(sliceWarmupChannels.size());
      for (auto& [key, channels] : sliceWarmupChannels) {
        if (channels.empty()) {
          continue;
        }
        std::sort(channels.begin(), channels.end());
        channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
        SliceWarmupGroup g{};
        g.key = key;
        g.channels = std::move(channels);
        groups.emplace_back(std::move(g));
      }

      if (!groups.empty()) {
        auto sharedGroups = std::make_shared<std::vector<SliceWarmupGroup>>(std::move(groups));
        PreRecordNode warmupNode;
        warmupNode.label = "bindless_warmup_img_slice";
        warmupNode.fn = [groups = std::move(sharedGroups)](Z3DRendererVulkanBackend& backend,
                                                           Z3DRendererBase& renderer) {
          (void)renderer;
          for (const auto& g : *groups) {
            backend.bindlessPreWarmupImgSlice(g.key.image,
                                              g.key.colormaps,
                                              std::span<const size_t>(g.channels.data(), g.channels.size()),
                                              g.key.wantsVolume3D,
                                              g.key.wantsColormap,
                                              g.key.wantsPaging,
                                              "linear_script");
          }
        };
        m_preRecordNodes.emplace_back(std::move(warmupNode));
      }
    }

    if (!hotTextures.empty()) {
      auto sharedTextures = std::make_shared<std::vector<ZVulkanTexture*>>(hotTextures);
      PreRecordNode residencyNode;
      residencyNode.label = "prepare_managed_texture_residency";
      residencyNode.fn = [textures = std::move(sharedTextures)](Z3DRendererVulkanBackend& backend,
                                                                Z3DRendererBase& renderer) {
        (void)renderer;
        auto& residency = backend.device().residencyManager();
        auto protection =
          residency.protectTextures(std::span<ZVulkanTexture* const>(textures->data(), textures->size()));
        (void)protection;
        for (auto* texture : *textures) {
          if (residency.ensureResidentIfManaged(texture, "linear_script")) {
            backend.pinTextureForActiveSubmission(texture);
          }
        }
      };
      m_preRecordNodes.emplace_back(std::move(residencyNode));
    }

    if (!scratchTextureUses.empty()) {
      auto sharedUses =
        std::make_shared<std::vector<Z3DScratchResourcePool::VulkanScratchTextureUse>>(scratchTextureUses);
      PreRecordNode scratchNode;
      scratchNode.label = "prepare_scratch_texture_residency";
      scratchNode.fn = [uses = std::move(sharedUses),
                        protection = scratchHotProtection](Z3DRendererVulkanBackend& backend,
                                                           Z3DRendererBase& renderer) {
        (void)backend;
        (void)renderer;
        auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();
        scratchPool.prepareVulkanScratchTexturesForPass(
          std::span<const Z3DScratchResourcePool::VulkanScratchTextureUse>(uses->data(), uses->size()),
          "linear_script");
        std::vector<ZVulkanTexture*> protectedTextures;
        protectedTextures.reserve(uses->size());
        for (const auto& use : *uses) {
          if (use.texture == nullptr ||
              std::find(protectedTextures.begin(), protectedTextures.end(), use.texture) != protectedTextures.end()) {
            continue;
          }
          protectedTextures.push_back(use.texture);
        }
        protection->emplace(scratchPool.protectVulkanScratchTextures(
          std::span<ZVulkanTexture* const>(protectedTextures.data(), protectedTextures.size())));
      };
      m_preRecordNodes.emplace_back(std::move(scratchNode));
    }

    if (!sampledUses.empty()) {
      std::unordered_set<BindlessUseKey, BindlessUseKeyHash> seen;
      seen.reserve(sampledUses.size());

      std::vector<ExternalImageUseDesc> uniqueUses;
      uniqueUses.reserve(sampledUses.size());
      for (const auto& use : sampledUses) {
        if (!use.handle.valid() || use.handle.backend != RenderBackend::Vulkan) {
          continue;
        }
        BindlessUseKey key{};
        key.id = use.handle.id;
        key.kind = use.kind;
        key.aspectHint = use.aspectHint;
        if (seen.insert(key).second) {
          uniqueUses.push_back(use);
        }
      }

      if (!uniqueUses.empty()) {
        auto sharedUses = std::make_shared<std::vector<ExternalImageUseDesc>>(std::move(uniqueUses));
        PreRecordNode bindlessNode;
        bindlessNode.label = "bindless_register_sampled_images";
        bindlessNode.fn = [uses = std::move(sharedUses)](Z3DRendererVulkanBackend& backend, Z3DRendererBase& renderer) {
          (void)renderer;
          backend.bindlessPreRegisterExternalSampledImageUses(
            std::span<const ExternalImageUseDesc>(uses->data(), uses->size()),
            "linear_script");
        };
        m_preRecordNodes.emplace_back(std::move(bindlessNode));
      }
    }

    if (!raycasterBlockIdCompactionPrimes.empty()) {
      struct PrimeGroup
      {
        const Z3DScratchResourcePool::RenderTargetLease* rawLease = nullptr;
        std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> lease;
        uint32_t effectiveAttachmentCount = 0;
        uint32_t maxBlockId = 0;
      };
      std::vector<PrimeGroup> groups;
      groups.reserve(raycasterBlockIdCompactionPrimes.size());

      for (auto& [raw, entry] : raycasterBlockIdCompactionPrimes) {
        if (!entry.lease) {
          continue;
        }
        PrimeGroup g{};
        g.rawLease = raw;
        g.lease = entry.lease;
        g.effectiveAttachmentCount = entry.effectiveAttachmentCount;
        g.maxBlockId = entry.maxBlockId;
        groups.emplace_back(std::move(g));
      }

      std::sort(groups.begin(), groups.end(), [](const PrimeGroup& a, const PrimeGroup& b) {
        return a.rawLease < b.rawLease;
      });

      if (!groups.empty()) {
        auto sharedGroups = std::make_shared<std::vector<PrimeGroup>>(std::move(groups));
        PreRecordNode primeNode;
        primeNode.label = "bindless_prime_raycaster_blockid_compaction";
        primeNode.fn = [groups = std::move(sharedGroups)](Z3DRendererVulkanBackend& backend,
                                                          Z3DRendererBase& renderer) {
          (void)renderer;
          for (const auto& g : *groups) {
            backend.bindlessPrePrimeImgRaycasterBlockIdCompaction(g.lease,
                                                                  g.effectiveAttachmentCount,
                                                                  g.maxBlockId,
                                                                  "linear_script");
          }
        };
        m_preRecordNodes.emplace_back(std::move(primeNode));
      }
    }

    if (!sliceBlockIdCompactionPrimes.empty()) {
      struct PrimeGroup
      {
        const Z3DScratchResourcePool::RenderTargetLease* rawLease = nullptr;
        std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> lease;
        uint32_t sliceCount = 0;
        uint32_t maxBlockId = 0;
        std::vector<uint32_t> sliceIndices;
      };

      std::vector<PrimeGroup> groups;
      groups.reserve(sliceBlockIdCompactionPrimes.size());

      for (auto& [raw, entry] : sliceBlockIdCompactionPrimes) {
        if (!entry.lease || entry.sliceCount == 0u || !entry.hasMaxBlockId || entry.sliceIndices.empty()) {
          continue;
        }
        PrimeGroup g{};
        g.rawLease = raw;
        g.lease = entry.lease;
        g.sliceCount = entry.sliceCount;
        g.maxBlockId = entry.maxBlockId;
        g.sliceIndices.assign(entry.sliceIndices.begin(), entry.sliceIndices.end());
        std::sort(g.sliceIndices.begin(), g.sliceIndices.end());
        groups.emplace_back(std::move(g));
      }

      std::sort(groups.begin(), groups.end(), [](const PrimeGroup& a, const PrimeGroup& b) {
        return a.rawLease < b.rawLease;
      });

      if (!groups.empty()) {
        auto sharedGroups = std::make_shared<std::vector<PrimeGroup>>(std::move(groups));
        PreRecordNode primeNode;
        primeNode.label = "bindless_prime_slice_blockid_compaction";
        primeNode.fn = [groups = std::move(sharedGroups)](Z3DRendererVulkanBackend& backend,
                                                          Z3DRendererBase& renderer) {
          (void)renderer;
          for (const auto& g : *groups) {
            for (uint32_t sliceIndex : g.sliceIndices) {
              backend.bindlessPrePrimeImgSliceBlockIdCompaction(g.lease,
                                                                g.sliceCount,
                                                                sliceIndex,
                                                                g.maxBlockId,
                                                                "linear_script");
            }
          }
        };
        m_preRecordNodes.emplace_back(std::move(primeNode));
      }
    }
  }

  std::vector<Node> nodes;
  drainNodesIntoExecutionOrder(nodes);

  {
    auto managedHotProtection = m_backend.device().residencyManager().protectTextures(
      std::span<ZVulkanTexture* const>(hotTextures.data(), hotTextures.size()));
    (void)managedHotProtection;
    openFrame(firstLabel);
  }
  auto frameGuard = folly::makeGuard([&]() {
    if (!m_frameOpen) {
      return;
    }
    // Best-effort close on exceptional exits; do not mask the original error.
    scratchHotProtection->reset();
    try {
      closeFrame("flush_abort");
    }
    catch (...) {
    }
  });

  executeNodes(std::span<Node>(nodes.data(), nodes.size()));

  // Recording is finished. Drop lease keep-alives before registering readback
  // completion hooks so scratch release hooks are ordered first at the frame
  // completion safe point.
  m_keepAlives.clear();

  if (readback != nullptr) {
    ZVulkanBuffer* src = readback->src;
    if (readback->srcSlot.has_value()) {
      src = readback->srcSlot->get();
    }
    CHECK(src != nullptr) << "ZVulkanLinearScript readback missing source buffer";
    CHECK(readback->dst != nullptr) << "ZVulkanLinearScript readback missing destination pointer";
    CHECK_GT(readback->bytes, 0u) << "ZVulkanLinearScript readback requires bytes > 0";

    // Readback is a CPU control-flow boundary: force the backend to wait for the
    // active submission fence and run completion safe-point hooks before
    // returning to the caller.
    m_backend.requireCompletionSafePointWaitForActiveSubmission(readback->label);

    auto ticket =
      m_backend.requestEndOfFrameBufferReadbackTicket(*src, readback->srcOffset, readback->bytes, readback->label);

    m_backend.registerAfterCurrentFrameCompletionHook(
      currentRenderThreadExecutorKeepAlive("vulkan_script_readback_consume"),
      [dst = readback->dst, bytes = readback->bytes, ticket = std::move(ticket)](
        Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
        co_await ticket.awaitCopyTo(dst, bytes);
        co_return;
      },
      readback->label);
  }

  // These CPU-side protections only guard pre-record/resource-assembly work.
  // Submitted command buffers are protected by managed texture pins, Vulkan
  // scratch lease lifetimes, and the frame fence. Release before closeFrame()
  // reaches completion hooks so post-fence readback/staging work can reclaim
  // resources from the just-finished submission.
  scratchHotProtection->reset();

  if (readback == nullptr && (waitForCompletion || strictResidencyFlushEachNode())) {
    m_backend.requireCompletionSafePointWaitForActiveSubmission(waitForCompletion ? reason : "strict_residency_node");
  }

  closeFrame(reason);
  frameGuard.dismiss();
  m_pendingSubmissionHasGpuNodes = false;
}

void ZVulkanLinearScript::executeNodes(std::span<Node> nodes)
{
  const bool mergeRasters = absl::GetFlag(FLAGS_atlas_vk_linear_script_merge_rasters);
  const bool validateBatchMetadata = absl::GetFlag(FLAGS_atlas_vk_linear_script_validate_batch_metadata);
  RendererCPUState merged;
  std::vector<std::string_view> mergedLabels;
  size_t mergedNodeCount = 0;
  double mergedCaptureMs = 0.0;

  auto flushMerged = [&]() {
    if (merged.batches.empty()) {
      merged.uniformBytesEstimate = 0;
      mergedNodeCount = 0;
      mergedLabels.clear();
      mergedCaptureMs = 0.0;
      return;
    }

    std::string label = fmt::format("{}", fmt::join(mergedLabels, " + "));
    if (label.empty()) {
      label = fmt::format("raster_coalesced({})", mergedNodeCount);
    }

    if (mergedCaptureMs > 0.0) {
      const std::string captureLabel = fmt::format("capture {}", label);
      m_backend.recordCpuScope(captureLabel, mergedCaptureMs);
      mergedCaptureMs = 0.0;
    }

    m_backend.beginPassScope(label);
    std::optional<size_t> gpuScope;
    if (!label.empty()) {
      gpuScope = m_backend.beginGpuScope(label, /*isPassScope=*/true);
    }
    auto scopeGuard = folly::makeGuard([&]() {
      if (gpuScope.has_value()) {
        m_backend.endGpuScope(*gpuScope);
      }
      m_backend.endPassScope();
    });

    if (validateBatchMetadata) {
      validateVulkanBatchMetadataOrCrash(label, merged);
    }
    m_backend.processBatches(m_renderer, merged);

    merged.batches.clear();
    merged.uniformBytesEstimate = 0;
    mergedNodeCount = 0;
    mergedLabels.clear();
  };

  for (auto& node : nodes) {
    if (auto* rasterNode = std::get_if<RasterNode>(&node)) {
      if (!mergeRasters) {
        flushMerged();

        const std::string& label = rasterNode->label;
        if (rasterNode->captureMs > 0.0) {
          const std::string captureLabel = fmt::format("capture {}", label);
          m_backend.recordCpuScope(captureLabel, rasterNode->captureMs);
        }

        if (rasterNode->state.batches.empty()) {
          continue;
        }

        m_backend.beginPassScope(label);
        std::optional<size_t> gpuScope;
        if (!label.empty()) {
          gpuScope = m_backend.beginGpuScope(label, /*isPassScope=*/true);
        }
        auto scopeGuard = folly::makeGuard([&]() {
          if (gpuScope.has_value()) {
            m_backend.endGpuScope(*gpuScope);
          }
          m_backend.endPassScope();
        });

        if (validateBatchMetadata) {
          validateVulkanBatchMetadataOrCrash(label, rasterNode->state);
        }
        m_backend.processBatches(m_renderer, rasterNode->state);
        continue;
      }

      mergedNodeCount++;
      mergedLabels.push_back(std::string_view(rasterNode->label));
      mergedCaptureMs += rasterNode->captureMs;

      if (!rasterNode->state.batches.empty()) {
        merged.batches.reserve(merged.batches.size() + rasterNode->state.batches.size());
        for (auto& batch : rasterNode->state.batches) {
          merged.batches.push_back(std::move(batch));
        }
        rasterNode->state.batches.clear();
      }
      merged.uniformBytesEstimate += rasterNode->state.uniformBytesEstimate;
      rasterNode->state.uniformBytesEstimate = 0;
      continue;
    }

    if (auto* replayNode = std::get_if<ReplayNode>(&node)) {
      flushMerged();

      CHECK(replayNode->state) << "ZVulkanLinearScript replay node missing state";
      const std::string& label = replayNode->label;
      m_backend.beginPassScope(label);
      std::optional<size_t> gpuScope;
      if (!label.empty()) {
        gpuScope = m_backend.beginGpuScope(label, /*isPassScope=*/true);
      }
      auto scopeGuard = folly::makeGuard([&]() {
        if (gpuScope.has_value()) {
          m_backend.endGpuScope(*gpuScope);
        }
        m_backend.endPassScope();
      });

      if (validateBatchMetadata) {
        validateVulkanBatchMetadataOrCrash(label, *replayNode->state);
      }
      m_backend.processBatches(m_renderer, *replayNode->state);
      continue;
    }

    if (auto* cmdNode = std::get_if<CommandsNode>(&node)) {
      flushMerged();

      const std::string& label = cmdNode->label;
      m_backend.beginPassScope(label);
      std::optional<size_t> gpuScope;
      if (!label.empty()) {
        gpuScope = m_backend.beginGpuScope(label, /*isPassScope=*/true);
      }
      auto scopeGuard = folly::makeGuard([&]() {
        if (gpuScope.has_value()) {
          m_backend.endGpuScope(*gpuScope);
        }
        m_backend.endPassScope();
      });

      if (cmdNode->record) {
        cmdNode->record(m_backend);
      }
      continue;
    }

    CHECK(false) << "ZVulkanLinearScript: unknown node kind";
  }

  flushMerged();
}

void ZVulkanLinearScript::openFrame(std::string_view firstPassLabel)
{
  if (m_frameOpen) {
    CHECK(m_renderer.isVulkanFrameActive()) << "ZVulkanLinearScript: frameOpen without an active Vulkan frame";
    return;
  }
  CHECK(!m_renderer.isVulkanFrameActive())
    << "ZVulkanLinearScript: openFrame called while a Vulkan frame is already active";

  const std::string label = makeFrameLabel(m_frameLabel, firstPassLabel);

  std::vector<Z3DRendererVulkanBackend::BeginRenderPreRecordAction> actions;
  actions.reserve(m_preRecordNodes.size());
  for (auto& node : m_preRecordNodes) {
    Z3DRendererVulkanBackend::BeginRenderPreRecordAction action;
    action.label = std::move(node.label);
    action.fn = std::move(node.fn);
    actions.emplace_back(std::move(action));
  }
  m_preRecordNodes.clear();
  m_backend.setPendingBeginRenderPreRecordActions(std::move(actions), label);

  m_renderer.beginVulkanFrame(label);
  m_frameOpen = true;
}

void ZVulkanLinearScript::closeFrame(std::string_view reason)
{
  if (!m_frameOpen) {
    return;
  }
  CHECK(m_renderer.isVulkanFrameActive()) << "ZVulkanLinearScript closeFrame requires an active Vulkan frame";

  if (VLOG_IS_ON(2) && !reason.empty()) {
    VLOG(2) << "ZVulkanLinearScript submit: reason=" << std::string(reason);
  }
  // Mark the script frame as closed *before* ending the Vulkan frame so that
  // exceptional exits (e.g. cancellation thrown from safe-point hooks) do not
  // trigger a second close attempt from flushNodes()'s frame guard.
  m_frameOpen = false;
  m_renderer.endVulkanFrame();
}

} // namespace nim
