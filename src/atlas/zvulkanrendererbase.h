#pragma once

#include "zvulkan.h"
#include "z3dcamera.h"
#include "zglmutils.h"
#include <memory>
#include <set>
#include <vector>

namespace nim {

class ZVulkanRenderer;
class ZVulkanDevice;
class ZVulkanSwapChain;

/**
 * @brief Base class for Vulkan renderers
 * Equivalent to Z3DRendererBase in the OpenGL implementation
 * Provides common parameters and context for rendering
 */
class ZVulkanRendererBase
{
public:
  ZVulkanRendererBase(ZVulkanDevice& device, uint32_t width, uint32_t height);
  virtual ~ZVulkanRendererBase();

  // Device access
  ZVulkanDevice& device()
  {
    return m_device;
  }
  const ZVulkanDevice& device() const
  {
    return m_device;
  }

  // Get physical device features
  const vk::PhysicalDeviceFeatures& getPhysicalDeviceFeatures() const;

  // Dimension access
  uint32_t width() const
  {
    return m_width;
  }
  uint32_t height() const
  {
    return m_height;
  }

  // Resize the renderer
  void resize(uint32_t width, uint32_t height);

  // Swapchain for rendering
  ZVulkanSwapChain& swapChain()
  {
    return *m_swapChain;
  }

  // Frame management
  vk::raii::CommandBuffer beginFrame(vk::ClearColorValue clearColor = vk::ClearColorValue(std::array<float, 4>{
                                       {0.0f, 0.0f, 0.0f, 1.0f}
  }),
                                     vk::ClearDepthStencilValue clearDepthStencil = vk::ClearDepthStencilValue(1.0f,
                                                                                                               0));

  // End frame and present
  void endFrame(vk::raii::CommandBuffer& cmdBuffer);

  // Copy rendered image to memory
  void copyToMemory(void* data, size_t size);

  // Camera management
  void setCamera(const Z3DCamera& camera)
  {
    m_camera = camera;
    m_hasCustomCamera = true;
  }

  void unsetCamera()
  {
    m_hasCustomCamera = false;
  }

  Z3DCamera& camera()
  {
    return m_hasCustomCamera ? m_camera : m_globalCamera;
  }

  const Z3DCamera& camera() const
  {
    return m_hasCustomCamera ? m_camera : m_globalCamera;
  }

  Z3DCamera& globalCamera()
  {
    return m_globalCamera;
  }

  // Coordinate transform matrix
  glm::mat4 coordTransform() const;

  // Viewport matrices (NDC <-> window) used by some shaders (e.g., wide lines)
  const glm::mat4& viewportMatrix() const { return m_viewportMatrix; }
  const glm::mat4& viewportMatrixInverse() const { return m_viewportMatrixInverse; }

  // Global rendering parameters
  float opacity() const
  {
    return m_globalOpacity;
  }
  void setOpacity(float opacity)
  {
    m_globalOpacity = opacity;
  }

  float sizeScale() const
  {
    return m_globalSizeScale;
  }
  void setSizeScale(float scale)
  {
    m_globalSizeScale = scale;
  }

  // Push constants for shader program
  struct PushConstants
  {
    glm::mat4 projectionViewMatrix;
    glm::mat4 modelMatrix;
  };

  // Push constants for the current frame
  const PushConstants& pushConstants() const
  {
    return m_pushConstants;
  }

  // Register/unregister renderers
  void registerRenderer(ZVulkanRenderer* renderer);
  void unregisterRenderer(ZVulkanRenderer* renderer);

  // Clip plane management
  void setClipPlanes(const std::vector<glm::vec4>& clipPlanes);
  const std::vector<glm::vec4>& clipPlanes() const
  {
    return m_clipPlanes;
  }
  void enableClipping(bool enable)
  {
    m_clipEnabled = enable;
  }
  bool clippingEnabled() const
  {
    return m_clipEnabled;
  }

protected:
  // Update push constants based on current state
  void updatePushConstants();

private:
  // Device and swapchain
  ZVulkanDevice& m_device;
  uint32_t m_width;
  uint32_t m_height;
  std::unique_ptr<ZVulkanSwapChain> m_swapChain;

  // Rendering state
  bool m_resized = false;
  PushConstants m_pushConstants;

  // Camera
  bool m_hasCustomCamera = false;
  Z3DCamera m_camera;
  Z3DCamera m_globalCamera;

  // Global parameters
  float m_globalOpacity = 1.0f;
  float m_globalSizeScale = 1.0f;

  // Viewport matrix for transformations
  glm::mat4 m_viewportMatrix;
  glm::mat4 m_viewportMatrixInverse;

  // Connected renderers
  std::set<ZVulkanRenderer*> m_renderers;

  // Clipping planes
  std::vector<glm::vec4> m_clipPlanes;
  bool m_clipEnabled = false;
};

} // namespace nim
