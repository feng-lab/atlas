#include "zvulkanrendererbase.h"

#include "zvulkandevice.h"
#include "zvulkanrenderer.h"
#include "zvulkanswapchain.h"
#include "zlog.h"

namespace nim {

ZVulkanRendererBase::ZVulkanRendererBase(ZVulkanDevice& device, uint32_t width, uint32_t height)
  : m_device(device)
  , m_width(width)
  , m_height(height)
{
  // Create swapchain for rendering
  m_swapChain = device.createSwapChain(width, height);

  // Initialize viewport matrices
  float w = static_cast<float>(width);
  float h = static_cast<float>(height);

  // Create viewport matrix: transforms NDC coordinates to window coordinates
  m_viewportMatrix = glm::mat4(glm::vec4(w / 2.0f, 0.0f, 0.0f, 0.0f),
                               glm::vec4(0.0f, h / 2.0f, 0.0f, 0.0f),
                               glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
                               glm::vec4(w / 2.0f, h / 2.0f, 0.0f, 1.0f));

  // Create inverse viewport matrix
  m_viewportMatrixInverse = glm::inverse(m_viewportMatrix);

  // Initialize cameras with Vulkan coordinate system
  m_globalCamera.setCoordinateSystem(Z3DCoordinateSystem::Vulkan);
  m_camera.setCoordinateSystem(Z3DCoordinateSystem::Vulkan);

  // Set aspect ratio based on window dimensions
  m_globalCamera.setAspectRatio(w / h);
  m_camera.setAspectRatio(w / h);

  // Initialize push constants
  updatePushConstants();

  LOG(INFO) << "ZVulkanRendererBase created: " << width << "x" << height;
}

ZVulkanRendererBase::~ZVulkanRendererBase()
{
  // Make sure all renderer references are cleared
  m_renderers.clear();

  // Release the swapchain explicitly
  m_swapChain.reset();

  LOG(INFO) << "ZVulkanRendererBase destroyed";
}

const vk::PhysicalDeviceFeatures& ZVulkanRendererBase::getPhysicalDeviceFeatures() const
{
  // If there's no direct access, return a static default features object
  static vk::PhysicalDeviceFeatures defaultFeatures;
  return defaultFeatures;
  // Alternatively, implement proper access through the device when available
  // return m_device.getPhysicalDeviceFeatures();
}

void ZVulkanRendererBase::resize(uint32_t width, uint32_t height)
{
  if (width == 0 || height == 0) {
    LOG(WARNING) << "Ignoring resize to zero dimensions";
    return;
  }

  m_width = width;
  m_height = height;
  m_resized = true;

  // Update viewport matrices
  float w = static_cast<float>(width);
  float h = static_cast<float>(height);

  m_viewportMatrix = glm::mat4(glm::vec4(w / 2.0f, 0.0f, 0.0f, 0.0f),
                               glm::vec4(0.0f, h / 2.0f, 0.0f, 0.0f),
                               glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
                               glm::vec4(w / 2.0f, h / 2.0f, 0.0f, 1.0f));

  m_viewportMatrixInverse = glm::inverse(m_viewportMatrix);

  // Update camera aspect ratios
  m_globalCamera.setAspectRatio(w / h);
  m_camera.setAspectRatio(w / h);

  LOG(INFO) << "ZVulkanRendererBase will be resized to " << width << "x" << height;
}

vk::raii::CommandBuffer ZVulkanRendererBase::beginFrame(vk::ClearColorValue clearColor,
                                                        vk::ClearDepthStencilValue clearDepthStencil)
{
  if (m_resized) {
    m_swapChain->resize(m_width, m_height);
    m_resized = false;
  }

  // Update push constants for this frame
  updatePushConstants();

  return m_swapChain->beginFrame(clearColor, clearDepthStencil);
}

void ZVulkanRendererBase::endFrame(vk::raii::CommandBuffer& cmdBuffer)
{
  m_swapChain->endFrame(cmdBuffer);
}

void ZVulkanRendererBase::copyToMemory(void* data, size_t size)
{
  m_swapChain->copyToMemory(data, size);
}

glm::mat4 ZVulkanRendererBase::coordTransform() const
{
  // Return identity matrix for now
  // This could be extended to support more complex transformations
  return glm::mat4(1.0f);
}

void ZVulkanRendererBase::registerRenderer(ZVulkanRenderer* renderer)
{
  if (renderer) {
    m_renderers.insert(renderer);
  }
}

void ZVulkanRendererBase::unregisterRenderer(ZVulkanRenderer* renderer)
{
  if (renderer) {
    m_renderers.erase(renderer);
  }
}

void ZVulkanRendererBase::setClipPlanes(const std::vector<glm::vec4>& clipPlanes)
{
  m_clipPlanes = clipPlanes;
}

void ZVulkanRendererBase::updatePushConstants()
{
  // Update projection-view matrix from camera
  auto& camera = this->camera();

  // Get the matrices for the mono eye (center view)
  glm::mat4 viewMatrix = camera.viewMatrix(MonoEye);
  glm::mat4 projMatrix = camera.projectionMatrix(MonoEye);

  // Combine into a projection-view matrix
  m_pushConstants.projectionViewMatrix = projMatrix * viewMatrix;
  m_pushConstants.modelMatrix = coordTransform();
}

} // namespace nim
