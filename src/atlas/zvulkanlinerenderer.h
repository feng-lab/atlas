#pragma once

#include "zvulkanrenderer.h"
#include <glm/glm.hpp>
#include <vector>

namespace nim {

/**
 * @brief Enhanced line renderer for Vulkan
 * Provides smooth line rendering with accurate widths and visual options
 * matching the functionality of Z3DLineRenderer
 */
class ZVulkanLineRenderer : public ZVulkanRenderer
{
public:
    ZVulkanLineRenderer(ZVulkanRendererBase& rendererBase);
    ~ZVulkanLineRenderer() override;
    
    // Set line data in two different ways
    // 1. As an array of endpoints (similar to Z3DLineRenderer::setData)
    void setData(const std::vector<glm::vec3>& linesInput);
    
    // 2. As separate arrays of start and end points (used in the current implementation)
    void setLines(const std::vector<glm::vec3>& startPoints, const std::vector<glm::vec3>& endPoints);
    
    // Set vertex colors (similar to Z3DLineRenderer::setDataColors)
    void setDataColors(const std::vector<glm::vec4>& colors);
    
    // Use 1D texture for coloring (similar to Z3DLineRenderer::setTexture)
    void setTexture(ZVulkanTexture* texture);
    
    // Set line width (similar to Z3DLineRenderer::setLineWidth)
    void setLineWidth(float width);
    
    // Enable/disable line strip mode (default: false, like Z3DLineRenderer::setLineStrip)
    void setLineStrip(bool enable);
    
    // Enable/disable smooth lines (default: true, like Z3DLineRenderer::setUseSmoothLine)
    void setUseSmoothLine(bool enable);
    
    // Enable/disable round caps (default: true, like Z3DLineRenderer::setRoundCap)
    void setRoundCap(bool enable);
    
    // Enable/disable screen alignment (default: false, like Z3DLineRenderer::setScreenAlign)
    void setScreenAlign(bool enable);
    
    // Compile the line data (similar to Z3DLineRenderer::compile)
    void compile() override;
    
    // Render lines using the given command buffer
    void render(vk::raii::CommandBuffer& cmdBuffer) override;
    
private:
    // Create shader modules for different rendering modes
    void createShaders();
    
    // Create graphics pipeline
    void createPipeline();
    
    // Helper method to create buffers and upload data
    void createBuffers();
    
    // Similar to Z3DLineRenderer::renderSmooth
    void renderSmooth(vk::raii::CommandBuffer& cmdBuffer);
    
    // Helper method for thick line rendering
    void processLinesForSmooth();
    
    // Line geometry data
    std::vector<glm::vec3> m_vertices;      // Regular vertices for basic lines
    std::vector<glm::vec4> m_colors;        // Colors for each vertex
    std::vector<uint32_t> m_indices;        // Indices for connecting vertices
    
    // Smooth line data
    std::vector<glm::vec3> m_smoothLineP0s;  // Start points
    std::vector<glm::vec3> m_smoothLineP1s;  // End points
    std::vector<glm::vec4> m_smoothLineP0Colors; // Colors for start points
    std::vector<glm::vec4> m_smoothLineP1Colors; // Colors for end points
    std::vector<float> m_smoothLineFlags;  // Flags for shader behavior
    
    // Rendering configuration
    float m_lineWidth = 1.0f;
    bool m_isLineStrip = false;
    bool m_useSmoothLine = true;
    bool m_roundCap = true;
    bool m_screenAligned = false;
    bool m_useTextureColor = false;
    
    // Buffers
    std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
    std::unique_ptr<ZVulkanBuffer> m_colorBuffer;
    std::unique_ptr<ZVulkanBuffer> m_indexBuffer;
    
    // Smooth line buffers
    std::unique_ptr<ZVulkanBuffer> m_p0Buffer;
    std::unique_ptr<ZVulkanBuffer> m_p1Buffer;
    std::unique_ptr<ZVulkanBuffer> m_p0ColorBuffer; 
    std::unique_ptr<ZVulkanBuffer> m_p1ColorBuffer;
    std::unique_ptr<ZVulkanBuffer> m_flagsBuffer;
    
    // Shaders for different rendering modes
    std::unique_ptr<ZVulkanShader> m_standardLineShader;  // Basic lines
    std::unique_ptr<ZVulkanShader> m_smoothLineShader;    // Smooth lines with geometry shader
    std::unique_ptr<ZVulkanShader> m_smoothLineAlternativeShader; // Smooth lines without geometry shader
    
    // Pipelines for different rendering modes
    std::unique_ptr<ZVulkanPipeline> m_standardLinePipeline;
    std::unique_ptr<ZVulkanPipeline> m_smoothLinePipeline;
    std::unique_ptr<ZVulkanPipeline> m_smoothLineAlternativePipeline;
    
    // Texture (optional)
    ZVulkanTexture* m_texture = nullptr;
    
    // Descriptor set for textures
    std::unique_ptr<ZVulkanDescriptorSet> m_descriptorSet;
    
    // State tracking
    bool m_dataChanged = false;
    bool m_useGeometryShader = true;  // Will be determined based on device capabilities
};

} // namespace nim 