#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include "zcolormap.h"
#include "zmesh.h"
#include "z3dscratchresourcepool.h"
#include "z3drendercommands.h"
#include "zvulkanlinearscript.h"
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nim {

class Z3DImg;

// render 2d slices of volume with colormap
// use colormap of each volume to composite final image
class Z3DImgSliceRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DImgSliceRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img, const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps);

  void setFastRendering(bool v)
  {
    m_fastRendering = v;
  }

  [[nodiscard]] bool isFastRendering() const
  {
    return m_fastRendering;
  }

  // Targets are owned internally; no external override needed

  //  [[nodiscard]] bool lastRenderingIsFastRendering() const
  //  {
  //    return m_lastRenderingIsFastRendering;
  //  }

  // a slice in 3D volume contains plane triangles and 3d texture coordinates
  // clear
  void clearSlices()
  {
    m_slices.clear();
  }

  // add slice
  void addSlice(const ZMesh& slice);

  void compile() override;

  // Ensure internal targets are sized; size is provided by filter
  void setOutputSize(const glm::uvec2& size)
  {
    // Store output size provided by the filter; pooled render targets use this on acquire
    m_outputSize = size;
  }

  // Vulkan-only helper: build a sequence of stage-specific payloads that
  // represent the slice pipeline as fine-grained passes (draw layers, paging
  // cache discovery, merge). This is used by Z3DImgFilter's linear-script
  // orchestration so each script node records one logical pass.
  //
  // Note: This method may update progressive bookkeeping (generation counters)
  // as part of preparing the per-frame payloads.
  [[nodiscard]] std::vector<ImgSlicePayload> buildVulkanStagePayloads(Z3DEye eye);

  // Vulkan-only helper: record the slice pipeline stage payloads into a linear
  // script as fine-grained nodes. This owns the stage→render-target mapping
  // (including per-layer/per-slice expansion) inside the renderer, while the
  // Vulkan pipeline context remains responsible for GPU recording.
  //
  // The filter owns the output lease; this method binds it for output-writing
  // stages (direct draw, merge).
  //
  // Returns the last script segment recorded (or deps if no stages were emitted).
  [[nodiscard]] ZVulkanLinearScript::SegmentHandle
  recordVulkanStagesToScript(ZVulkanLinearScript& script,
                             Z3DEye eye,
                             Z3DScratchResourcePool::RenderTargetLease& outputLease,
                             ZVulkanLinearScript::SegmentHandle deps = {});

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;

  [[nodiscard]] const std::vector<ZMesh>& slices() const
  {
    return m_slices;
  }

  [[nodiscard]] Z3DImg* image() const
  {
    return m_img;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<ZColorMapParameter>>* colormaps() const
  {
    return m_colormaps;
  }

  [[nodiscard]] glm::uvec2 outputSize() const
  {
    return m_outputSize;
  }

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    // GL uses m_progress for its two-step progressive slice (0.5 -> 1.0).
    // Vulkan uses (channelIdx, round) bookkeeping (mirrors raycaster).
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      return m_channelIdx[eye] >= 0;
    }
    return m_progress[eye] > 0.0;
  }

  // Report progressive progress for Vulkan/GL-agnostic callers.
  // Returns [0.5,1.0] during progressive refinement, or 1.0 when complete/not started.
  double progressiveProgress(Z3DEye eye) const;

  // Reset progressive accumulation state for an eye (GL + Vulkan).
  void resetProgress(Z3DEye eye);

  // Notify renderer that a Vulkan progressive round has completed so internal
  // bookkeeping can advance (channel/round progression).
  void finalizeProgressiveRound(Z3DEye eye, bool lastRound, size_t channelCount);

  // Release any scratch-pool backed targets retained across frames.
  void releaseScratchResources();

  void releaseBackendResources() override
  {
    // Backend switches invalidate scratch-pool resources (layer targets) and also
    // invalidate the progressive bookkeeping that depends on their contents.
    releaseScratchResources();
    // Clear GL cache; textures will be deleted with unique_ptr
    m_colormapCache.textures.clear();
    m_colormapCache.meta.clear();
    Z3DPrimitiveRenderer::releaseBackendResources();
  }

protected:
  void bindVolumes(Z3DShaderProgram& shader) const;

  void bindVolume(Z3DShaderProgram& shader, size_t idx) const;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

private:
  double renderSlice(Z3DEye eye, bool progressive = false);

  void renderSliceFast(Z3DEye eye);

protected:
  // Z3DShaderProgram m_volumeSliceShader;
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_scVolumeSliceShader;
  // Internal targets are obtained from the scratch pool
  std::unique_ptr<Z3DShaderProgram> m_mergeChannelShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithColorMapBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithColorMapShader;

  Z3DImg* m_img = nullptr;
  const std::vector<std::unique_ptr<ZColorMapParameter>>* m_colormaps = nullptr;
  // Cached direct ZColorMap pointers for Vulkan payloads
  std::vector<const ZColorMap*> m_colormapsRaw;
  std::vector<std::string> m_volumeUniformNames;
  std::vector<std::string> m_colormapUniformNames;

private:
  std::vector<ZMesh> m_slices;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = true;
  // bool m_lastRenderingIsFastRendering = false;

  double m_progress[3] = {0, 0, 0};

  // Vulkan progressive bookkeeping (mirrors raycaster): per-eye channel and round indices.
  // channelIdx < 0 indicates "not started / done". Vulkan uses channelIndexRaw < 0
  // in the payload for the fast-preview frame and flips channelIdx to 0 via
  // finalizeProgressiveRound().
  int m_channelIdx[3] = {-1, -1, -1};
  int m_round[3] = {0, 0, 0};
  std::array<uint32_t, 3> m_progressiveGeneration{};
  // Persistent Vulkan layer targets across progressive rounds (per eye).
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_progressiveLayerLease;

  // Output size provided via ensureInternalTargets()
  glm::uvec2 m_outputSize{32, 32};

  // GL LUT cache for colormaps when using the OpenGL backend
  struct ColormapGLCache
  {
    std::unordered_map<const ZColorMap*, std::unique_ptr<Z3DTexture>> textures;
    std::unordered_map<const ZColorMap*, std::pair<uint64_t, uint32_t>> meta; // generation, width
  };
  mutable ColormapGLCache m_colormapCache;

  Z3DTexture* colormapTextureGL(const ZColorMap& cm, uint32_t width = 256) const;
};

// Helper to finalize progressive rounds for ImgSlice by stream identity.
// Used by the Vulkan backend to notify the originating renderer after GPU work completes.
bool finalizeImgSliceRoundByKey(Z3DRendererBase& rendererBase,
                                uint64_t streamKey,
                                Z3DEye eye,
                                bool lastRound,
                                uint32_t channelCount);

} // namespace nim
