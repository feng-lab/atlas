#pragma once

#include "z3drendercommands.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanTexture;
class ZVulkanBuffer;
class Z3DFontRenderer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanFontPipelineContext
{
public:
  explicit ZVulkanFontPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanFontPipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);

  // Pre-record helper for bindless sampled images: ensure the font atlas texture
  // exists (CPU pixels upload path) before command recording begins.
  ZVulkanTexture& ensureAtlasFromCpuPixelsOrCrash(const uint8_t* atlasPixelsBGRA8, uint32_t width, uint32_t height);

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const FontPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct FontVertex
  {
    glm::vec3 position{0.0f};
    glm::vec2 texcoord{0.0f};
    glm::vec4 color{1.0f};
  };

  struct PipelineKey
  {
    bool picking = false;
    bool showOutline = false;
    bool showShadow = false;
    int outlineMode = 0;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(picking, showOutline, showShadow, outlineMode, colorFormats, depthFormat);
    }

    bool operator<(const PipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  // Matches layout(push_constant) in Resources/shader/vulkan/almag.vert/frag
  struct FontPushConstants
  {
    glm::mat4 projectionView{1.0f};
    float alpha = 1.0f;
    float softedgeScale = 80.0f;
    glm::vec2 _pad0{0.0f};
    glm::vec4 outlineColor{1.0f};
    glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 1.0f};
    uint32_t atlasTexture = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;
    uint32_t _pad3 = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  // Upload arena-backed slices
  vk::Buffer m_vertexUploadBuffer{};
  vk::DeviceSize m_vertexUploadOffset{0};
  vk::Buffer m_indexUploadBuffer{};
  vk::DeviceSize m_indexUploadOffset{0};

  // Cache CPU-provided atlases keyed by pixel pointer
  std::unordered_map<const void*, std::unique_ptr<ZVulkanTexture>> m_atlasCache;

  // Static promotion cache for geometry
  struct CacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking);
    }
    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct CacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vb{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t posGen = 0;
    uint32_t texGen = 0;
    uint32_t colorGen = 0; // or picking
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;
  // Guard: if we scheduled upload->static copies for a stream within the
  // current submission, we must not bind the static buffers again until the
  // next submission because copies are flushed after rendering ends.
  std::set<CacheKey> m_staticCopyPendingKeys;

  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const FontPayload& payload);

  ZVulkanTexture* ensureAtlasFromPayload(const FontPayload& payload);

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
