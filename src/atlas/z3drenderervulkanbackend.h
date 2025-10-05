#pragma once

#include "z3drendererbackend.h"
#include "zglmutils.h"
#include "zvulkan.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
  friend class Z3DRendererBase;
  void ensureDevice();
  void resetFrameResources();
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
    vk::raii::QueryPool queryPool{nullptr};
    std::vector<GpuScopeRecord> gpuScopes;
    std::vector<CpuScopeRecord> cpuScopes;
    uint32_t nextQuery = 0;
    std::chrono::steady_clock::time_point cpuStart;
    std::chrono::steady_clock::time_point cpuEnd;
  };

  void collectFrameTimings(FrameResources& frame);
  std::optional<size_t> beginGpuScope(std::string_view label);
  void endGpuScope(size_t token);
  void recordCpuScope(std::string_view label, double milliseconds);

  ZVulkanDevice* m_sharedDevice = nullptr; // non-owning; provided by engine/scratch-pool
  ZVulkanDevice* m_frameDevice = nullptr;    // tracked to rebuild frame resources on device changes
  std::vector<FrameResources> m_frames;
  std::unordered_map<void*, size_t> m_frameResourceMap;
  std::optional<ZVulkanFrameExecutor::ActiveFrame> m_activeFrameHandle;
  FrameResources* m_activeFrame = nullptr;
  bool m_frameRecording = false;
  uint32_t m_maxFramesInFlight = 2;
  float m_timestampPeriod = 1.0f;

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
