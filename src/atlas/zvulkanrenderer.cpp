#include "zvulkanrenderer.h"

namespace nim {

ZVulkanRenderer::ZVulkanRenderer(ZVulkanRendererBase& rendererBase)
    : m_rendererBase(rendererBase)
{
    // Register this renderer with the renderer base
    m_rendererBase.registerRenderer(this);
}

ZVulkanRenderer::~ZVulkanRenderer()
{
    // Unregister this renderer from the renderer base
    m_rendererBase.unregisterRenderer(this);
}

glm::mat4 ZVulkanRenderer::coordTransform() const
{
    if (m_followCoordTransform) {
        // Return the coordinate transform from the renderer base
        return m_rendererBase.coordTransform();
    } else {
        // Return identity matrix if not following coordinate transform
        return glm::mat4(1.0f);
    }
}

float ZVulkanRenderer::opacity() const
{
    if (m_followOpacity) {
        // Return the opacity from the renderer base
        return m_rendererBase.opacity();
    } else {
        // Return full opacity if not following global opacity
        return 1.0f;
    }
}

float ZVulkanRenderer::sizeScale() const
{
    if (m_followSizeScale) {
        // Return the size scale from the renderer base
        return m_rendererBase.sizeScale();
    } else {
        // Return unit scale if not following global size scale
        return 1.0f;
    }
}

ZVulkanRenderer::PushConstants ZVulkanRenderer::preparePushConstants() const
{
    PushConstants constants;
    
    // Get the camera from the renderer base
    auto& camera = m_rendererBase.camera();
    
    // Get the matrices for the mono eye (center view)
    glm::mat4 viewMatrix = camera.viewMatrix(MonoEye);
    glm::mat4 projMatrix = camera.projectionMatrix(MonoEye);
    
    // Combine into a projection-view matrix
    constants.projectionViewMatrix = projMatrix * viewMatrix;
    
    // Set the model matrix with coordinate transform
    constants.modelMatrix = coordTransform();
    
    // Set other parameters
    constants.opacity = opacity();
    constants.sizeScale = sizeScale();
    constants.enableLighting = m_needLighting ? 1 : 0;
    constants.padding = 0;  // Padding for alignment
    
    return constants;
}

} // namespace nim 
