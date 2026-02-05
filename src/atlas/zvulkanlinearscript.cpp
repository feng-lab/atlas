#include "zvulkanlinearscript.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zcancellation.h"
#include "zlog.h"
#include "zrenderthreadexecutor_tls.h"
#include "zvulkanbuffer.h"

#include <folly/OperationCancelled.h>
#include <folly/coro/Task.h>
#include <folly/ScopeGuard.h>

#include <fmt/format.h>

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

ZVulkanLinearScript::~ZVulkanLinearScript()
{
  if (m_nodes.empty() && m_preRecordNodes.empty() && !m_frameOpen) {
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
  node.state = m_renderer.captureVulkanBatches(recordBatches, label);
  m_nodes.emplace_back(std::move(node));
  return handle;
}

ZVulkanLinearScript::SegmentHandle
ZVulkanLinearScript::preRecord(std::string_view label,
                               std::span<const SegmentHandle> deps,
                               const std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)>& fn)
{
  validateDeps(label, deps);
  CHECK(!m_pendingSubmissionHasGpuNodes)
    << "ZVulkanLinearScript::preRecord must be enqueued before raster/replay/commands nodes for that submission";
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
  node.state = std::move(state);
  m_nodes.emplace_back(std::move(node));
  return handle;
}

ZVulkanLinearScript::SegmentHandle
ZVulkanLinearScript::commands(std::string_view label,
                              std::span<const SegmentHandle> deps,
                              const std::function<void(Z3DRendererVulkanBackend&)>& record)
{
  validateDeps(label, deps);
  const auto handle = nextHandle();
  m_pendingSubmissionHasGpuNodes = true;
  CommandsNode node;
  node.label = std::string(label);
  node.record = record;
  m_nodes.emplace_back(std::move(node));
  return handle;
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

void ZVulkanLinearScript::flushNodes(std::string_view reason, /*nullable*/ const ReadbackBufferSpec* readback)
{
  if (m_nodes.empty() && readback == nullptr) {
    CHECK(m_preRecordNodes.empty()) << "ZVulkanLinearScript: preRecord actions enqueued without any GPU work";
    return;
  }
  CHECK(!m_nodes.empty() || readback == nullptr) << "ZVulkanLinearScript: readback requested without any GPU work";

  std::string_view firstLabel;
  if (!m_nodes.empty()) {
    const auto& first = m_nodes.front();
    if (const auto* rasterNode = std::get_if<RasterNode>(&first)) {
      firstLabel = rasterNode->label;
    } else if (const auto* replayNode = std::get_if<ReplayNode>(&first)) {
      firstLabel = replayNode->label;
    } else if (const auto* cmdNode = std::get_if<CommandsNode>(&first)) {
      firstLabel = cmdNode->label;
    }
  } else if (readback != nullptr) {
    firstLabel = readback->label;
  }

  openFrame(firstLabel);
  auto frameGuard = folly::makeGuard([&]() {
    if (!m_frameOpen) {
      return;
    }
    // Best-effort close on exceptional exits; do not mask the original error.
    try {
      closeFrame("flush_abort");
    }
    catch (...) {
    }
  });

  std::vector<Node> nodes;
  drainNodesIntoExecutionOrder(nodes);
  executeNodes(std::span<Node>(nodes.data(), nodes.size()));

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
    m_backend.requireReadbackWaitForActiveSubmission(readback->label);

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

  closeFrame(reason);
  m_keepAlives.clear();
  frameGuard.dismiss();
  m_pendingSubmissionHasGpuNodes = false;
}

void ZVulkanLinearScript::executeNodes(std::span<Node> nodes)
{
  RendererCPUState merged;
  std::string mergedFirstLabel;
  size_t mergedNodeCount = 0;

  auto flushMerged = [&]() {
    if (merged.batches.empty()) {
      mergedNodeCount = 0;
      mergedFirstLabel.clear();
      return;
    }

    std::string label = mergedFirstLabel;
    if (mergedNodeCount > 1) {
      if (label.empty()) {
        label = fmt::format("raster_coalesced({})", mergedNodeCount);
      } else {
        label = fmt::format("{} (+{})", mergedFirstLabel, mergedNodeCount - 1);
      }
    }

    m_backend.beginPassScope(label);
    std::optional<size_t> gpuScope;
    if (!label.empty()) {
      gpuScope = m_backend.beginGpuScope(label);
    }
    auto scopeGuard = folly::makeGuard([&]() {
      if (gpuScope.has_value()) {
        m_backend.endGpuScope(*gpuScope);
      }
      m_backend.endPassScope();
    });

    m_backend.processBatches(m_renderer, merged);

    merged.batches.clear();
    mergedNodeCount = 0;
    mergedFirstLabel.clear();
  };

  for (auto& node : nodes) {
    if (auto* rasterNode = std::get_if<RasterNode>(&node)) {
      if (mergedNodeCount == 0) {
        mergedFirstLabel = rasterNode->label;
      }
      mergedNodeCount++;

      if (!rasterNode->state.batches.empty()) {
        merged.batches.reserve(merged.batches.size() + rasterNode->state.batches.size());
        for (auto& batch : rasterNode->state.batches) {
          merged.batches.push_back(std::move(batch));
        }
        rasterNode->state.batches.clear();
      }
      continue;
    }

    if (auto* replayNode = std::get_if<ReplayNode>(&node)) {
      flushMerged();

      CHECK(replayNode->state) << "ZVulkanLinearScript replay node missing state";
      const std::string& label = replayNode->label;
      m_backend.beginPassScope(label);
      std::optional<size_t> gpuScope;
      if (!label.empty()) {
        gpuScope = m_backend.beginGpuScope(label);
      }
      auto scopeGuard = folly::makeGuard([&]() {
        if (gpuScope.has_value()) {
          m_backend.endGpuScope(*gpuScope);
        }
        m_backend.endPassScope();
      });

      m_backend.processBatches(m_renderer, *replayNode->state);
      continue;
    }

    if (auto* cmdNode = std::get_if<CommandsNode>(&node)) {
      flushMerged();

      const std::string& label = cmdNode->label;
      m_backend.beginPassScope(label);
      std::optional<size_t> gpuScope;
      if (!label.empty()) {
        gpuScope = m_backend.beginGpuScope(label);
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
  m_renderer.endVulkanFrame();
  m_frameOpen = false;
}

} // namespace nim
