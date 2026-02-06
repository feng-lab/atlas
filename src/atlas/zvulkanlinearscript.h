#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanBuffer;

// Callsite-facing helper for expressing Vulkan compositor logic in a linear,
// GL-like style ("segments + explicit CPU readbacks + branches"), while the
// backend owns Vulkan frame/submission boundaries and safe-point handling.
//
// Design goals:
// - Call sites never call beginVulkanFrame/endVulkanFrame directly.
// - CPU readbacks are explicit boundaries (no global wait toggles).
// - Dependencies are declared for clarity and future optimization, but the
//   initial implementation executes in call order for correctness.
//
// Terminology / conceptual guideline (not required for correctness):
// - Atlas Vulkan uses dynamic rendering (`vkCmdBeginRendering`), so the classic
//   VkRenderPass "subpass" terminology does not really apply here.
// - A "script node" is one call to raster()/replay()/commands(). Nodes form a
//   small IR that the script/backend may coalesce for submission efficiency.
// - Call sites should keep nodes fine-grained and single-purpose: a raster()
//   node should generally target one output surface / attachment set (one
//   logical pass) and should not switch render targets mid-node. Split work into
//   multiple nodes when changing surfaces so labels, dependencies, and
//   load/store/final-use semantics stay easy to reason about.
class ZVulkanLinearScript final
{
public:
  template<typename T>
  class Slot
  {
  public:
    Slot()
      : m_state(std::make_shared<State>())
    {}

    void set(T value) const
    {
      CHECK(m_state != nullptr);
      CHECK(!m_state->value.has_value()) << "ZVulkanLinearScript::Slot set twice";
      m_state->value = std::move(value);
    }

    [[nodiscard]] bool hasValue() const
    {
      return m_state != nullptr && m_state->value.has_value();
    }

    [[nodiscard]] const T& get() const
    {
      CHECK(hasValue()) << "ZVulkanLinearScript::Slot read before set";
      return *m_state->value;
    }

  private:
    struct State
    {
      std::optional<T> value;
    };
    std::shared_ptr<State> m_state;
  };

  struct SegmentHandle
  {
    uint32_t id = 0;
    [[nodiscard]] explicit operator bool() const
    {
      return id != 0;
    }
  };

  ZVulkanLinearScript(Z3DRendererBase& renderer, Z3DRendererVulkanBackend& backend, std::string_view frameLabel = {});
  ZVulkanLinearScript(const ZVulkanLinearScript&) = delete;
  ZVulkanLinearScript& operator=(const ZVulkanLinearScript&) = delete;
  ZVulkanLinearScript(ZVulkanLinearScript&&) = delete;
  ZVulkanLinearScript& operator=(ZVulkanLinearScript&&) = delete;
  ~ZVulkanLinearScript();

  template<typename T>
  [[nodiscard]] Slot<T> makeSlot() const
  {
    return Slot<T>{};
  }

  // Keep an arbitrary object alive until the next submission flush boundary.
  // This is primarily used for Vulkan scratch RenderTargetLease objects whose
  // release must not occur before the script has opened a Vulkan frame and
  // recorded/submitted the work that references them.
  template<typename T>
  void keepAlive(std::shared_ptr<T> value)
  {
    if (!value) {
      return;
    }
    m_keepAlives.push_back(std::move(value));
  }

  // Enqueue a pre-record action for the next Vulkan submission. The provided
  // function runs inside the backend's beginRender() *before* descriptor priming
  // and before command buffer recording begins.
  //
  // Contract:
  // - Must not record GPU commands (command buffer is not recording yet).
  // - Runs once at the start of the next submission flush boundary.
  SegmentHandle preRecord(std::string_view label,
                          std::span<const SegmentHandle> deps,
                          const std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)>& fn);
  SegmentHandle preRecord(std::string_view label,
                          std::initializer_list<SegmentHandle> deps,
                          const std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)>& fn)
  {
    return preRecord(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), fn);
  }

  // Record a raster segment by collecting CPU batches via recordBatches.
  // This does not immediately submit; execution is deferred until:
  // - a CPU readback boundary is requested, or
  // - the script is destroyed.
  //
  // Guideline: keep the callback focused on one logical pass. Prefer recording
  // batches that target a single output surface / attachment set; split into
  // multiple raster() nodes when switching render targets.
  SegmentHandle
  raster(std::string_view label, std::span<const SegmentHandle> deps, const std::function<void()>& recordBatches);
  SegmentHandle
  raster(std::string_view label, std::initializer_list<SegmentHandle> deps, const std::function<void()>& recordBatches)
  {
    return raster(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), recordBatches);
  }

  // Replay an already-captured batch list. Like raster(), this records into the
  // script IR and defers execution until a flush boundary.
  SegmentHandle
  replay(std::string_view label, std::span<const SegmentHandle> deps, std::shared_ptr<RendererCPUState> state);
  SegmentHandle
  replay(std::string_view label, std::initializer_list<SegmentHandle> deps, std::shared_ptr<RendererCPUState> state)
  {
    return replay(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), std::move(state));
  }

  // Record backend-specific command buffer work (barriers, compute dispatch,
  // clears, etc.) into the script. Execution is deferred until a flush boundary
  // so the backend/script can optimize submission boundaries.
  SegmentHandle commands(std::string_view label,
                         std::span<const SegmentHandle> deps,
                         const std::function<void(Z3DRendererVulkanBackend&)>& record);
  SegmentHandle commands(std::string_view label,
                         std::initializer_list<SegmentHandle> deps,
                         const std::function<void(Z3DRendererVulkanBackend&)>& record)
  {
    return commands(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), record);
  }

  // Request an end-of-submission buffer readback and block until the value is
  // available on CPU. This creates a submission boundary: pending GPU work is
  // submitted, the fence is waited, and frame-completion safe-point hooks run.
  //
  // Note: readback methods return only after the fence wait + safe-point hook
  // execution, so any subsequent segments record into a new submission.
  void readbackBufferTo(std::string_view label,
                        std::span<const SegmentHandle> deps,
                        ZVulkanBuffer& src,
                        vk::DeviceSize srcOffset,
                        void* dst,
                        size_t bytes);
  void readbackBufferTo(std::string_view label,
                        std::span<const SegmentHandle> deps,
                        const Slot<ZVulkanBuffer*>& srcSlot,
                        vk::DeviceSize srcOffset,
                        void* dst,
                        size_t bytes);
  void readbackBufferTo(std::string_view label,
                        std::initializer_list<SegmentHandle> deps,
                        ZVulkanBuffer& src,
                        vk::DeviceSize srcOffset,
                        void* dst,
                        size_t bytes)
  {
    readbackBufferTo(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), src, srcOffset, dst, bytes);
  }
  void readbackBufferTo(std::string_view label,
                        std::initializer_list<SegmentHandle> deps,
                        const Slot<ZVulkanBuffer*>& srcSlot,
                        vk::DeviceSize srcOffset,
                        void* dst,
                        size_t bytes)
  {
    readbackBufferTo(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), srcSlot, srcOffset, dst, bytes);
  }

  [[nodiscard]] uint32_t readbackU32(std::string_view label,
                                     std::span<const SegmentHandle> deps,
                                     ZVulkanBuffer& src,
                                     vk::DeviceSize srcOffset = 0);
  [[nodiscard]] uint32_t readbackU32(std::string_view label,
                                     std::span<const SegmentHandle> deps,
                                     const Slot<ZVulkanBuffer*>& srcSlot,
                                     vk::DeviceSize srcOffset = 0);
  [[nodiscard]] uint32_t readbackU32(std::string_view label,
                                     std::initializer_list<SegmentHandle> deps,
                                     ZVulkanBuffer& src,
                                     vk::DeviceSize srcOffset = 0)
  {
    return readbackU32(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), src, srcOffset);
  }
  [[nodiscard]] uint32_t readbackU32(std::string_view label,
                                     std::initializer_list<SegmentHandle> deps,
                                     const Slot<ZVulkanBuffer*>& srcSlot,
                                     vk::DeviceSize srcOffset = 0)
  {
    return readbackU32(label, std::span<const SegmentHandle>(deps.begin(), deps.size()), srcSlot, srcOffset);
  }

private:
  struct PreRecordNode
  {
    std::string label;
    std::function<void(Z3DRendererVulkanBackend&, Z3DRendererBase&)> fn;
  };

  struct RasterNode
  {
    std::string label;
    RendererCPUState state;
  };

  struct ReplayNode
  {
    std::string label;
    std::shared_ptr<RendererCPUState> state;
  };

  struct CommandsNode
  {
    std::string label;
    std::function<void(Z3DRendererVulkanBackend&)> record;
  };

  using Node = std::variant<RasterNode, ReplayNode, CommandsNode>;

  struct ReadbackBufferSpec
  {
    std::string label;
    ZVulkanBuffer* src = nullptr;
    std::optional<Slot<ZVulkanBuffer*>> srcSlot;
    vk::DeviceSize srcOffset = 0;
    void* dst = nullptr;
    size_t bytes = 0;
  };

  void setReadbackSource(ReadbackBufferSpec& spec, ZVulkanBuffer& src);
  void setReadbackSource(ReadbackBufferSpec& spec, const Slot<ZVulkanBuffer*>& srcSlot);

  void flushNodes(std::string_view reason, /*nullable*/ const ReadbackBufferSpec* readback);
  void drainNodesIntoExecutionOrder(std::vector<Node>& out);
  void executeNodes(std::span<Node> nodes);
  void openFrame(std::string_view firstPassLabel);
  void closeFrame(std::string_view reason);
  void validateDeps(std::string_view label, std::span<const SegmentHandle> deps) const;
  SegmentHandle nextHandle();

  Z3DRendererBase& m_renderer;
  Z3DRendererVulkanBackend& m_backend;
  std::string m_frameLabel;
  uint32_t m_nextSegmentId = 1;

  std::vector<std::shared_ptr<void>> m_keepAlives;

  std::vector<PreRecordNode> m_preRecordNodes;
  bool m_pendingSubmissionHasGpuNodes = false;

  std::vector<Node> m_nodes;
  bool m_frameOpen = false;
};

} // namespace nim
