#pragma once

#include "z3drendererbackend.h"
#include "zglmutils.h"
#include "zvulkan.h"

#include <memory>
#include <optional>
#include <vector>

namespace nim {

class ZVulkanDevice;
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

  void preBackendSwitch() override;

  ZVulkanDevice& device();

  const ZVulkanDevice& device() const;

  vk::raii::CommandBuffer& commandBuffer();

  const vk::raii::CommandBuffer& commandBuffer() const;

private:
  void ensureDevice();

  ZVulkanDevice* m_sharedDevice = nullptr; // non-owning; provided by engine/scratch-pool
  std::optional<vk::raii::CommandBuffer> m_activeCommandBuffer;

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

  
};

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend();

} // namespace nim
