#pragma once

#include "z3drendertarget.h"
#include "zglmutils.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>
#include <boost/container_hash/hash.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace nim {

class ZVulkanTexture; // forward declare

class Z3DPickingManager
{
public:
  struct PickingObject
  {
    size_t objectId = 0u;
    const void* object = nullptr;

    bool operator==(const PickingObject&) const = default;
  };

  struct PickingObjectHash
  {
    [[nodiscard]] size_t operator()(const PickingObject& value) const noexcept
    {
      size_t seed = 0u;
      boost::hash_combine(seed, value.objectId);
      boost::hash_combine(seed, value.object);
      return seed;
    }
  };

  struct VulkanObjectHit
  {
    size_t objectId = 0u;
    const void* object = nullptr;
    uint64_t squaredPhysicalPixelDistance = 0u;
  };

  struct ImageDepthSample
  {
    float depth = 1.f;
    glm::uvec2 topLeftPhysicalPixel{0u};
    glm::uvec2 fullPhysicalExtent{0u};

    // Maps an already-scaled top-left input position to the full-frame physical
    // pixel used by direct image-depth sampling.
    [[nodiscard]] static glm::uvec2 samplePixelForPhysicalInput(glm::ivec2 physicalInputPosition,
                                                                glm::uvec2 fullPhysicalExtent);
    [[nodiscard]] glm::ivec2 bottomLeftUnprojectionPixel() const;
  };

  struct QueryOverride
  {
    std::function<PickingObject(glm::ivec2)> objectAtWidgetPos;
    std::function<GLfloat(glm::ivec2)> depthAtWidgetPos;
    std::function<std::vector<PickingObject>(glm::ivec2, int, bool)> sortObjectsByDistanceToPos;
    std::function<std::optional<ImageDepthSample>(size_t, glm::ivec2)> imageDepthAtPhysicalInput;
  };

  // Borrows a complete query override for one synchronous interaction
  // dispatch. The manager and QueryOverride must outlive this scope.
  class [[nodiscard]] ScopedQueryOverride
  {
  public:
    ScopedQueryOverride(Z3DPickingManager& manager, const QueryOverride& queryOverride);
    ~ScopedQueryOverride();

    ScopedQueryOverride(const ScopedQueryOverride&) = delete;
    ScopedQueryOverride& operator=(const ScopedQueryOverride&) = delete;
    ScopedQueryOverride(ScopedQueryOverride&&) = delete;
    ScopedQueryOverride& operator=(ScopedQueryOverride&&) = delete;

  private:
    Z3DPickingManager* m_manager;
    const QueryOverride* m_queryOverride;
  };

  [[nodiscard]] ScopedQueryOverride scopedQueryOverride(const QueryOverride& queryOverride)
  {
    return ScopedQueryOverride(*this, queryOverride);
  }

  [[nodiscard]] bool hasQueryOverride() const noexcept
  {
    return m_queryOverride != nullptr;
  }

  // Input render target should have color internal format GL_RGBA8.
  void setPickingTarget(Z3DRenderTarget& rt);
  void setPickingTarget(ZVulkanTexture& color, ZVulkanTexture& depth, const glm::uvec2& size);

  // must call
  void setDevicePixelRatio(double dpr)
  {
    m_devicePixelRatio = dpr;
  }

  // Strict attachment-space queries for a regional engine. Coordinates are
  // physical Vulkan attachment pixels with a top-left origin. Unlike the
  // widget-space queries below, these methods require a current Vulkan target
  // and propagate readback failures.
  [[nodiscard]] PickingObject objectAtVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const;
  [[nodiscard]] GLfloat depthAtVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const;
  // The reference may lie outside the attachment so a regional scan can retain
  // distances to the true full-frame query position. The search rectangle must
  // be nonempty and fully inside the attachment.
  [[nodiscard]] std::vector<VulkanObjectHit> objectsNearVulkanAttachmentReference(glm::i64vec2 topLeftReferencePixel,
                                                                                  glm::uvec4 topLeftSearchRect) const;

  void resetRenderTarget();

  // The returned color is the registration token and remains valid until it
  // is passed to deregisterObject().
  [[nodiscard]] glm::col4 registerObject(const void* obj, size_t objectId = 0u);

  void deregisterObject(const glm::col4& col);

  [[nodiscard]] PickingObject pickingObjectOfColor(const glm::col4& col) const;

  [[nodiscard]] PickingObject pickingObjectAtWidgetPos(glm::ivec2 pos);

  const void* objectAtWidgetPos(glm::ivec2 pos);

  // Depth at widget pixel (accounts for devicePixelRatio and y-flip).
  GLfloat depthAtWidgetPos(glm::ivec2 pos);

  // Find all picking identities within a radius of a widget-space position
  // and sort them by pixel distance.
  // - pos: widget coordinates (logical pixels, origin at top-left), consistent with objectAtWidgetPos().
  // - radius: widget pixels. If -1, search the whole image.
  [[nodiscard]] std::vector<PickingObject>
  sortPickingObjectsByDistanceToPos(const glm::ivec2& pos, int radius = -1, bool ascend = true);

  // Return an image filter's depth through the active query override. The
  // top-left input is already scaled by devicePixelRatio using the direct
  // image-depth path's multiply-then-truncate convention. An empty result means
  // either no override is active or the routed image has no depth at this
  // physical input position; use hasQueryOverride() to distinguish them.
  [[nodiscard]] std::optional<ImageDepthSample> imageDepthAtPhysicalInput(size_t imageObjectId,
                                                                          glm::ivec2 physicalInputPosition);

  bool isHit(const glm::ivec2& pos, const void* obj)
  {
    return (objectAtWidgetPos(pos) == obj);
  }

  [[nodiscard]] bool hasGlTarget() const
  {
    return m_renderTarget != nullptr;
  }

  [[nodiscard]] bool hasVulkanTarget() const
  {
    return m_vkColor != nullptr && m_vkDepth != nullptr;
  }

  void bindTarget()
  {
    CHECK(m_renderTarget != nullptr) << "Attempted to bind picking target before it was set";
    m_renderTarget->bind();
  }

  void releaseTarget()
  {
    CHECK(m_renderTarget != nullptr) << "Attempted to release picking target before it was set";
    m_renderTarget->release();
  }

  static void clearTarget();

  [[nodiscard]] Z3DRenderTarget& renderTarget() const
  {
    return *m_renderTarget;
  }

private:
  void beginQueryOverride(const QueryOverride& queryOverride);
  void endQueryOverride(const QueryOverride& queryOverride);
  void checkVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const;
  [[nodiscard]] const uint8_t* currentCachedVulkanColorData() const noexcept;
  [[nodiscard]] std::optional<PickingObject> registeredObjectForColor(const glm::col4& color) const;
  void increaseColor();
  void clearVulkanState();

private:
  boost::unordered_flat_map<glm::col4, PickingObject> m_colorToObject;
  Z3DRenderTarget* m_renderTarget = nullptr;
  glm::col4 m_currentColor{0, 0, 0, 128};
  double m_devicePixelRatio = 1.;
  const QueryOverride* m_queryOverride = nullptr;

  // Vulkan attachments for picking (optional)
  ZVulkanTexture* m_vkColor = nullptr;
  ZVulkanTexture* m_vkDepth = nullptr;
  glm::uvec2 m_vkSize{0u, 0u};

  // Cached CPU copy of Vulkan picking color buffer (latest ready frame)
  std::vector<uint8_t> m_cachedColor;
  glm::uvec2 m_cachedColorSize{0u, 0u};
  bool m_cachedColorValid = false;

public:
  // Update cached picking color buffer (RGBA8) from a CPU pointer.
  // Copies data; safe to call from the rendering thread after fence.
  // data may be null to clear the cache
  void updateCachedVulkanPickingColor(/*nullable*/ const uint8_t* data, size_t bytes, glm::uvec2 size)
  {
    if (!data || size.x == 0u || size.y == 0u || bytes < static_cast<size_t>(size.x) * size.y * 4u) {
      m_cachedColorValid = false;
      m_cachedColor.clear();
      m_cachedColorSize = glm::uvec2(0u);
      return;
    }
    m_cachedColor.resize(static_cast<size_t>(size.x) * size.y * 4u);
    std::memcpy(m_cachedColor.data(), data, m_cachedColor.size());
    m_cachedColorSize = size;
    m_cachedColorValid = true;
  }
};

} // namespace nim
