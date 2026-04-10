#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"
#include "z3dtexture.h"
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dscratchresourcepool.h"
#include <array>
#include "z3drendercommands.h"
#include "zvulkanlinearscript.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QString;

namespace nim {

class Z3DImg;
class Z3DRendererBase;
class Z3DImgPagingFrameStats;

struct ScreenSpaceSufficiencyAudit
{
  uint64_t contributingSamples = 0;
  uint64_t insufficientSamples = 0;
  uint64_t level0Samples = 0;
  uint64_t level0LimitedSamples = 0;
  uint64_t contributingPixels = 0;
  uint64_t insufficientPixels = 0;
  uint64_t level0Pixels = 0;
  uint64_t level0LimitedPixels = 0;

  [[nodiscard]] uint64_t sufficientSamples() const
  {
    return contributingSamples >= insufficientSamples ? contributingSamples - insufficientSamples : 0;
  }

  [[nodiscard]] uint64_t sufficientPixels() const
  {
    return contributingPixels >= insufficientPixels ? contributingPixels - insufficientPixels : 0;
  }

  [[nodiscard]] double sufficientSampleFraction() const
  {
    return contributingSamples == 0 ? 1.0 : static_cast<double>(sufficientSamples()) / contributingSamples;
  }

  [[nodiscard]] double sufficientPixelFraction() const
  {
    return contributingPixels == 0 ? 1.0 : static_cast<double>(sufficientPixels()) / contributingPixels;
  }

  [[nodiscard]] double level0SampleFraction() const
  {
    return contributingSamples == 0 ? 0.0 : static_cast<double>(level0Samples) / contributingSamples;
  }

  [[nodiscard]] double level0PixelFraction() const
  {
    return contributingPixels == 0 ? 0.0 : static_cast<double>(level0Pixels) / contributingPixels;
  }

  [[nodiscard]] double level0LimitedSampleFraction() const
  {
    return contributingSamples == 0 ? 0.0 : static_cast<double>(level0LimitedSamples) / contributingSamples;
  }

  [[nodiscard]] double level0LimitedPixelFraction() const
  {
    return contributingPixels == 0 ? 0.0 : static_cast<double>(level0LimitedPixels) / contributingPixels;
  }
};

// use raycaster to render volume or 2D Image (stack with depth==1) with color
// transfer functions
class Z3DImgRaycasterRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img);
  // Targets are owned internally; no external override needed

  // Vulkan-only helper: build a sequence of stage-specific payloads that
  // represent the raycaster pipeline as fine-grained passes (entry/exit, layers,
  // merge, progressive stages, ...). This is used by Z3DImgFilter's linear-script
  // orchestration so each script node records one logical pass.
  //
  // Note: This method may update progressive bookkeeping (generation counters,
  // stats sinks) as part of preparing the per-frame payloads.
  [[nodiscard]] std::vector<ImgRaycasterPayload> buildVulkanStagePayloads(Z3DEye eye,
                                                                          bool interactiveProgressivePaging = true);

  // Vulkan-only helper: record the raycaster's stage payloads into a linear
  // script as fine-grained nodes. This centralizes the mapping from stages to
  // render targets/batches (including per-layer expansion) inside the renderer,
  // while the Vulkan pipeline context remains responsible for GPU recording.
  //
  // The filter owns the output lease; this method binds it for output-writing
  // stages (fast direct/merge, progressive merge). Intermediate targets (entry/exit,
  // layer arrays, block-id scratch, accumulators) are owned by the renderer via
  // scratch-pool leases embedded in each stage payload.
  //
  // Returns the last script segment recorded (or deps if no stages were emitted).
  [[nodiscard]] ZVulkanLinearScript::SegmentHandle
  recordVulkanStagesToScript(ZVulkanLinearScript& script,
                             Z3DEye eye,
                             Z3DScratchResourcePool::RenderTargetLease& outputLease,
                             ZVulkanLinearScript::SegmentHandle deps = {},
                             bool interactiveProgressivePaging = true);

  // quad or entry_exit texture should be set before rendering

  // For 2D Image rendering, once set, entry exit textures will be cleared and
  // renderer switch to 2D mode
  // To render a 2D image, quad should contains corner vertex and 2d texture coordinates
  // To render a slice in 3D volume, quad should contains corner vertex and 3d texture coordinates
  // DO NOT call this function for 3d Raycaster
  // clear
  void clearQuads()
  {
    m_quads.clear();
  }

  // add quad
  void addQuad(const ZMesh& quad);

  void setFastRendering(bool v)
  {
    m_fastRendering = v;
  }

  bool isFastRendering() const
  {
    return m_fastRendering;
  }

  //  [[nodiscard]] bool lastRenderingIsFastRendering() const
  //  {
  //    return m_lastRenderingIsFastRendering;
  //  }

  // return true if something is rendered by this renderer
  [[nodiscard]] bool hasVisibleRendering() const;

  void compile() override;

  double renderProgressively(Z3DEye eye);
  // Report progressive progress for Vulkan/GL-agnostic callers.
  // Returns [0.5,1.0] during progressive accumulation, or 1.0 when complete/not started.
  double progressiveProgress(Z3DEye eye) const;

  // When paging frame stats are enabled, log+clear any active stats once this eye is complete.
  // This is primarily used by the Vulkan filter path, where completion can be inferred from
  // progressiveProgress() even if the renderer hasn't observed a "lastRound" sentinel.
  void finalizePagingStatsIfDone(Z3DEye eye);

  // Benchmark-only export: save the scalar MIP field before transfer-function mapping.
  // This is only supported for single-channel MIP-family OpenGL rendering.
  [[nodiscard]] bool saveRawMIPImage(Z3DEye eye, const QString& path, std::string& error);
  // Benchmark-only export: aggregate a screen-space sufficiency audit over contributing samples/pixels.
  [[nodiscard]] bool screenSpaceSufficiencyAudit(Z3DEye eye, ScreenSpaceSufficiencyAudit& audit, std::string& error);
  void setBenchmarkSelectedVoxelWorldSizeHint(float value)
  {
    m_benchmarkSelectedVoxelWorldSizeHint = value;
  }

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;

  bool renderingStarted(Z3DEye eye)
  {
    return m_channelIdx[eye] > -1;
  }

  void setSamplingRate(float value)
  {
    m_samplingRateValue = value;
  }

  void setIsoValue(float value)
  {
    m_isoValue = value;
  }

  void setLocalMIPThreshold(float value)
  {
    m_localMIPThreshold = value;
  }

  void setCompositingMode(ImgCompositingMode mode)
  {
    if (m_compositingModeValue == mode) {
      return;
    }
    m_compositingModeValue = mode;
    compile();
  }

  void setChannelCount(size_t count);

  void setChannelVisibility(size_t index, bool visible);

  void setChannelVisibilities(const std::vector<bool>& visibilities);

  void setTransferFunction(size_t index, Z3DTransferFunction* transferFunction);

  void setTransferFunctions(const std::vector<Z3DTransferFunction*>& transferFunctions);

  [[nodiscard]] const std::vector<bool>& channelVisibilities() const
  {
    return m_channelVisibilities;
  }

  [[nodiscard]] const std::vector<Z3DTransferFunction*>& transferFunctions() const
  {
    return m_transferFunctions;
  }

  [[nodiscard]] const ZMesh& entryExitMesh() const
  {
    return m_entryExitMesh;
  }

  [[nodiscard]] bool entryExitMeshValid() const
  {
    return m_entryExitMeshValid;
  }

  [[nodiscard]] bool entryExitMeshFlipped() const
  {
    return m_entryExitMeshFlipped;
  }

  // Ensure internal targets are sized; size is provided by filter
  void setOutputSize(const glm::uvec2& size)
  {
    if (m_outputSize == size) {
      return;
    }
    m_outputSize = size;
    releaseAllRaycastAccumulators();
  }

  // Compute entry/exit texture for a clipped volume surface, For 3D Raycasting rendering, once called, 2d quads will be
  // cleared and renderer switch to 3D mode
  void prepareEntryExit(const ZMesh& clipped, bool flipped, Z3DEye eye, const glm::uvec2& size);

  // Reset progressive accumulation state for an eye
  void resetProgress(Z3DEye eye);

  // Release entry/exit lease so the scratch pool can reuse it
  void releaseEntryExit()
  {
    if (m_entryExitLease) {
      m_entryExitLease.release();
    }
  }

  // Notify renderer that a progressive round has completed so internal
  // accumulators can be swapped and bookkeeping updated.
  void finalizeProgressiveRound(Z3DEye eye, bool lastRound, size_t channelCount);

  // Release any scratch-pool backed targets retained across frames.
  void releaseScratchResources();

  void releaseBackendResources() override
  {
    // Backend switches invalidate scratch-pool resources (entry/exit, progressive
    // layer targets, accumulators) and also invalidate the progressive bookkeeping
    // that depends on their contents. If we keep m_channelIdx/m_round across the
    // switch, the Vulkan progressive path may skip its round-0 clears and blend
    // against uninitialized accumulators, producing over-saturated output.
    //
    // Resetting here ensures the next frame starts a fresh progressive cycle on
    // the newly-selected backend (matches the "start from Vulkan" behavior).
    releaseScratchResources();

    // Clear GL cache; textures will be deleted with unique_ptr
    m_transferCache.textures.clear();
    m_transferCache.meta.clear();
    Z3DPrimitiveRenderer::releaseBackendResources();
  }

protected:
  void bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const;

  void bindVolumeOnly(Z3DShaderProgram& shader, size_t idx) const;

  void bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

private:
  enum class RaycastExportMode : uint8_t
  {
    Display,
    RawMIP,
    ScreenSpaceAudit,
  };

  void render2DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs);

  double render2DSliceOf3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive = false);

  void render2DSliceOf3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs);

  double render3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive = false);

  // Returns true if this is the last progressive round.
  bool render3DImageForOneRound(Z3DEye eye,
                                size_t c,
                                uint32_t round,
                                bool progressive,
                                float ze_to_zw_a,
                                float ze_to_zw_b,
                                float ze_to_screen_pixel_voxel_size,
                                size_t totalChannels,
                                Z3DShaderProgram& raycasterShader,
                                RaycastExportMode exportMode = RaycastExportMode::Display);

  void render3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs);
  [[nodiscard]] bool
  render3DImageFastRawMIP(Z3DEye eye, const std::vector<size_t>& visibleIdxs, const QString& path, std::string& error);
  [[nodiscard]] bool render3DImageFastScreenSpaceAudit(Z3DEye eye,
                                                       const std::vector<size_t>& visibleIdxs,
                                                       ScreenSpaceSufficiencyAudit& audit,
                                                       std::string& error);
  [[nodiscard]] ScreenSpaceSufficiencyAudit aggregateScreenSpaceAudit(const Z3DTexture& texture) const;
  [[nodiscard]] float benchmarkSelectedVoxelWorldSize() const;

  void ensureRaycastAccumulators(Z3DEye eye);
  void releaseRaycastAccumulators(Z3DEye eye);
  void releaseAllRaycastAccumulators();

protected:
  //  Z3DShaderProgram m_raycasterShader;
  //  Z3DShaderProgram m_2dImageShader;
  //  Z3DShaderProgram m_volumeSliceWithTransferfunShader;

  // single channel version
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_scRaycasterShader;
  std::unique_ptr<Z3DShaderProgram> m_scBenchmarkRawMIPShader;
  std::unique_ptr<Z3DShaderProgram> m_scBenchmarkScreenSpaceAuditShader;
  std::unique_ptr<Z3DShaderProgram> m_sc2dImageShader;
  std::unique_ptr<Z3DShaderProgram> m_scVolumeSliceWithTransferfunShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithTransferfunBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithTransferfunShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DRaycasterBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DRaycasterShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DBenchmarkRawMIPShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DBenchmarkScreenSpaceAuditShader;
  std::unique_ptr<Z3DShaderProgram> m_mergeChannelShader;
  std::unique_ptr<Z3DShaderProgram> m_copyTextureShader;

  // Internal targets
  // Raycast accumulators are acquired from the scratch pool on demand
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_lastRaycastAccum;
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_currentRaycastAccum;
  // Vulkan progressive layer-array targets (one per eye) used to accumulate and
  // merge per-channel results across progressive rounds.
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_vulkanProgressiveLayerLease;

  float m_samplingRateValue = 1.f; // Sampling rate of the raycasting, specified relative to the size of one voxel
  float m_isoValue = 0.5f; // The used isovalue, when isosurface raycasting is enabled
  float m_localMIPThreshold = 0.8f;

  ImgCompositingMode m_compositingModeValue = ImgCompositingMode::DirectVolumeRendering;

  Z3DImg* m_img = nullptr;
  std::vector<std::string> m_volumeUniformNames;
  std::vector<std::string> m_volumeDimensionNames;
  std::vector<std::string> m_transferFuncUniformNames;
  std::vector<bool> m_channelVisibilities;
  std::vector<Z3DTransferFunction*> m_transferFunctions;

private:
  std::vector<ZMesh> m_quads;
  ZMesh m_entryExitMesh;
  bool m_entryExitMeshValid = false;
  bool m_entryExitMeshFlipped = false;

  std::unique_ptr<Z3DVertexArrayObject> m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = true;
  // bool m_lastRenderingIsFastRendering = false;

  int m_channelIdx[3] = {-1, -1, -1};
  int m_round[3] = {0, 0, 0};
  std::array<uint32_t, 3> m_progressiveGeneration{};

  enum class VulkanProgressivePhase : uint8_t
  {
    BlockIdDiscovery,
    Raycast
  };
  std::array<VulkanProgressivePhase, 3> m_vulkanProgressivePhase{VulkanProgressivePhase::BlockIdDiscovery,
                                                                 VulkanProgressivePhase::BlockIdDiscovery,
                                                                 VulkanProgressivePhase::BlockIdDiscovery};

  // Optional per-frame paging read statistics (created only when logging is enabled).
  std::array<std::shared_ptr<Z3DImgPagingFrameStats>, 3> m_pagingFrameStats;
  std::array<uint32_t, 3> m_pagingFrameStatsGeneration{};

  // Output size provided via ensureInternalTargets()
  glm::uvec2 m_outputSize{32, 32};
  std::optional<float> m_benchmarkSelectedVoxelWorldSizeHint;

  // Owned GL resources (moved from filter)
  // GL LUT cache for transfer functions
  struct TransferGLCache
  {
    std::unordered_map<const Z3DTransferFunction*, std::unique_ptr<Z3DTexture>> textures;
    std::unordered_map<const Z3DTransferFunction*, std::pair<uint64_t, uint32_t>> meta; // generation, width
  };
  mutable TransferGLCache m_transferCache;

  Z3DTexture* transferTextureGL(const Z3DTransferFunction& tf) const;
  // Layer textures
  // No per-renderer textures; entry/exit obtained from scratch pool
  Z3DTextureAndEyeCoordinateRenderer m_textureAndEyeCoordinateRenderer;
  Z3DScratchResourcePool::RenderTargetLease
    m_entryExitLease; // holds lifetime of entry/exit render target during a frame
  Z3DScratchResourcePool::RenderTargetLease m_progressiveLayerLease; // persistent across progressive rounds

};

// Helper to finalize progressive rounds for ImgRaycaster by stream identity.
// Used by the Vulkan backend to notify the originating renderer after GPU work completes.
bool finalizeImgRaycasterRoundByKey(Z3DRendererBase& rendererBase,
                                    uint64_t streamKey,
                                    Z3DEye eye,
                                    bool lastRound,
                                    uint32_t channelCount);

} // namespace nim
