#pragma once

#include "zvulkanrendererbase.h"
#include "zglmutils.h"

namespace nim {

/**
 * @brief Base class for Vulkan primitive renderers
 * Equivalent to Z3DPrimitiveRenderer in the OpenGL implementation
 * Provides common functionality for Vulkan rendering objects
 */
class ZVulkanRenderer
{
public:
    ZVulkanRenderer(ZVulkanRendererBase& rendererBase);
    virtual ~ZVulkanRenderer();
    
    // Get the renderer base
    ZVulkanRendererBase& rendererBase() { return m_rendererBase; }
    const ZVulkanRendererBase& rendererBase() const { return m_rendererBase; }
    
    // Enable/disable lighting for this renderer
    void setNeedLighting(bool enable) { m_needLighting = enable; }
    bool needLighting() const { return m_needLighting; }
    
    // Control whether renderer follows coordinate transformation
    void setFollowCoordTransform(bool enable) { m_followCoordTransform = enable; }
    bool followCoordTransform() const { return m_followCoordTransform; }
    
    // Control whether renderer follows opacity
    void setFollowOpacity(bool enable) { m_followOpacity = enable; }
    bool followOpacity() const { return m_followOpacity; }
    
    // Control whether renderer follows size scaling
    void setFollowSizeScale(bool enable) { m_followSizeScale = enable; }
    bool followSizeScale() const { return m_followSizeScale; }
    
    // Coordinate transformation matrix
    glm::mat4 coordTransform() const;
    
    // Opacity value
    float opacity() const;
    
    // Size scale value
    float sizeScale() const;
    
    // Compile the renderer (to be implemented by derived classes)
    virtual void compile() = 0;
    
    // Render using the provided command buffer
    virtual void render(vk::raii::CommandBuffer& cmdBuffer) = 0;
    
    // Render for picking (optional, can be implemented by derived classes)
    virtual void renderPicking(vk::raii::CommandBuffer&) {}
    
protected:
    // Reference to the renderer base (provides global parameters and context)
    ZVulkanRendererBase& m_rendererBase;
    
    // Rendering flags
    bool m_needLighting = true;
    bool m_followCoordTransform = true;
    bool m_followOpacity = true;
    bool m_followSizeScale = true;
    
    // Helper method to get push constants for shaders
    struct PushConstants {
        glm::mat4 projectionViewMatrix;
        glm::mat4 modelMatrix;
        float opacity;
        float sizeScale;
        int enableLighting;
        int padding;  // for alignment
    };
    
    // Prepare push constants with current state
    PushConstants preparePushConstants() const;
};

} // namespace nim 
