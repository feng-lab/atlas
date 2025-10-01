#pragma once

#include "z3drendererbackend.h"
#include "zglmutils.h"
#include "zvulkan.h"

#include <memory>
#include <optional>
#include <vector>

namespace nim {

class ZVulkanContext;
class ZVulkanDevice;
class ZVulkanSwapChain;
class ZVulkanLinePipelineContext;
class ZVulkanMeshPipelineContext;
class ZVulkanEllipsoidPipelineContext;
class Z3DRendererVulkanBackend final : public Z3DRendererBackend
{
public:
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

  ZVulkanDevice& device();

  const ZVulkanDevice& device() const;

  ZVulkanSwapChain& swapChain();

  const ZVulkanSwapChain& swapChain() const;

  vk::raii::CommandBuffer& commandBuffer();

  const vk::raii::CommandBuffer& commandBuffer() const;

  glm::uvec2 surfaceExtent() const
  {
    return m_swapChainExtent;
  }

private:
  void ensureDevice();

  void ensureSwapChain(uint32_t width, uint32_t height);

  RendererFrameState::ActiveSurface describeSurfaceFromSwapChain();

  std::unique_ptr<ZVulkanContext> m_context;
  std::unique_ptr<ZVulkanDevice> m_device;
  std::unique_ptr<ZVulkanSwapChain> m_swapChain;
  std::optional<vk::raii::CommandBuffer> m_activeCommandBuffer;
  glm::uvec2 m_swapChainExtent{0, 0};

  std::unique_ptr<ZVulkanLinePipelineContext> m_lineContext;
  std::unique_ptr<ZVulkanMeshPipelineContext> m_meshContext;
  std::unique_ptr<ZVulkanEllipsoidPipelineContext> m_ellipsoidContext;
};

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
