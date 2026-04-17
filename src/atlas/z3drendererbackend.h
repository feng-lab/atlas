#pragma once

#include "z3dtypes.h"
#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3dscratchresourcepool.h"

#include <memory>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace nim {

class Z3DRendererBase;
class Z3DPrimitiveRenderer;
class Z3DShaderProgram;
class Z3DScratchResourcePool;

class Z3DRendererBackend
{
public:
  virtual ~Z3DRendererBackend() = default;

  virtual void setGlobalShaderParameters(Z3DRendererBase& renderer, Z3DShaderProgram& shader, Z3DEye eye) = 0;

  [[nodiscard]] virtual std::string generateHeader(const Z3DRendererBase& renderer) const = 0;

  [[nodiscard]] virtual std::string generateGeomHeader(const Z3DRendererBase& renderer) const = 0;

  virtual void beginRender(Z3DRendererBase& renderer) = 0;

  virtual void endRender(Z3DRendererBase& renderer) = 0;

  // Transitional hook: renderers can describe batches via RendererCPUState;
  // backends that support the command list façade can override this to
  // translate and execute the batches. Legacy backends can ignore it.
  virtual void processBatches(Z3DRendererBase& /*renderer*/, const RendererCPUState& /*state*/)
  {}

  [[nodiscard]] virtual bool supportsCommandLists() const
  {
    return false;
  }

  virtual RendererFrameState::ActiveSurface
  describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease) = 0;

  // Called before switching away from the current backend. Allows implementations
  // (e.g., Vulkan) to idle the device and drop swapchains to ensure safe teardown
  // of resources referenced by in-flight work.
  virtual void preBackendSwitch() {}

  // Teardown helper: backends that schedule fence-gated work (e.g. Vulkan safe-point
  // hooks, async readbacks) can override this to synchronously drain in-flight work
  // and execute any completion callbacks before dependent resources are destroyed.
  // Default no-op for immediate backends (e.g. OpenGL).
  virtual void flushForTeardown(std::string_view /*reason*/ = {}) {}

  // Optional pass-scope hooks (no-op by default). Vulkan backend uses these
  // to aggregate per-pass counters and emit a concise end-of-pass summary.
  virtual void beginPassScope(std::string_view /*label*/) {}
  virtual void endPassScope() {}

  // ---------------------------------------------------------------------------
  // Optional async-completion hooks (no-op by default)
  // ---------------------------------------------------------------------------
  // Backends that queue GPU work (e.g. Vulkan) can override these to allow the
  // engine to pump completion callbacks and to apply backpressure when frame
  // slots are exhausted. Immediate backends (e.g. OpenGL) keep defaults.
  virtual void pollCompletionsAndPumpSafePoints() {}
  virtual void reclaimTransientResourcesForMemoryPressure(Z3DScratchResourcePool::VulkanScratchReclaimMode /*mode*/,
                                                          std::string_view /*reason*/ = {})
  {}
  [[nodiscard]] virtual bool hasInFlightFrames() const
  {
    return false;
  }
  [[nodiscard]] virtual uint32_t inFlightCount() const
  {
    return 0;
  }
  [[nodiscard]] virtual uint32_t maxFramesInFlight() const
  {
    return 0;
  }

  // Hard byte limit for one monolithic geometry attribute/index stream emitted
  // by renderers before a backend would need a second level of chunking.
  // Backends that can accept arbitrarily large monolithic streams can keep the
  // default.
  [[nodiscard]] virtual size_t maxMonolithicGeometryStreamBytes() const
  {
    return std::numeric_limits<size_t>::max();
  }
};

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend();
std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
