#pragma once

#include "z3dtypes.h"
#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3dscratchresourcepool.h"
#include "z3dcompositorpass.h"

#include <memory>
#include <string>

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

  virtual void processCompositorPass(Z3DRendererBase& /*renderer*/, const Z3DCompositorPass& /*pass*/)
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
};

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend();
std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
