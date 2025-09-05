#include "zvulkanlinerenderer.h"
#include <algorithm>

namespace nim {

ZVulkanLineRenderer::ZVulkanLineRenderer(ZVulkanRendererBase& rendererBase)
    : ZVulkanRenderer(rendererBase)
    , m_lineWidth(1.0f)
    , m_isLineStrip(false)
    , m_useSmoothLine(true)
    , m_roundCap(true)
    , m_screenAligned(false)
    , m_useTextureColor(false)
    , m_dataChanged(false)
    , m_useGeometryShader(true)
{
    // Determine if geometry shaders are supported
    auto& features = rendererBase.getPhysicalDeviceFeatures();
    m_useGeometryShader = features.geometryShader;
    
    // Create shaders for different rendering modes
    createShaders();
    
    // Create initial pipeline with default settings
    createPipeline();
}

ZVulkanLineRenderer::~ZVulkanLineRenderer()
{
    // Clean up resources
    m_vertexBuffer.reset();
    m_colorBuffer.reset();
    m_indexBuffer.reset();
    m_p0Buffer.reset();
    m_p1Buffer.reset();
    m_p0ColorBuffer.reset();
    m_p1ColorBuffer.reset();
    m_flagsBuffer.reset();
    
    m_standardLineShader.reset();
    m_smoothLineShader.reset();
    m_smoothLineAlternativeShader.reset();
    
    m_standardLinePipeline.reset();
    m_smoothLinePipeline.reset();
    m_smoothLineAlternativePipeline.reset();
    
    m_descriptorSet.reset();
}

void ZVulkanLineRenderer::setData(const std::vector<glm::vec3>& linesInput)
{
    m_vertices = linesInput;
    
    // If not using geometry shader, prepare line data in a different format
    if (!m_useGeometryShader) {
        processLinesForSmooth();
    }
    
    m_dataChanged = true;
}

void ZVulkanLineRenderer::setLines(const std::vector<glm::vec3>& startPoints, const std::vector<glm::vec3>& endPoints)
{
    if (startPoints.size() != endPoints.size()) {
        // Error: Start and end points must match in size
        return;
    }
    
    m_smoothLineP0s = startPoints;
    m_smoothLineP1s = endPoints;
    m_dataChanged = true;
}

void ZVulkanLineRenderer::setDataColors(const std::vector<glm::vec4>& colors)
{
    m_colors = colors;
    
    // If colors provided for each vertex
    if (!m_useGeometryShader && !m_colors.empty()) {
        // Prepare color data for the alternative smooth line rendering
        m_smoothLineP0Colors.clear();
        m_smoothLineP1Colors.clear();
        
        if (m_isLineStrip) {
            for (size_t i = 1; i < m_colors.size(); ++i) {
                for (int j = 0; j < 4; ++j) {
                    m_smoothLineP0Colors.push_back(m_colors[i - 1]);
                    m_smoothLineP1Colors.push_back(m_colors[i]);
                }
            }
        } else {
            for (size_t i = 0; i < m_colors.size(); i += 2) {
                if (i + 1 < m_colors.size()) {
                    for (int j = 0; j < 4; ++j) {
                        m_smoothLineP0Colors.push_back(m_colors[i]);
                        m_smoothLineP1Colors.push_back(m_colors[i + 1]);
                    }
                }
            }
        }
    }
    
    m_dataChanged = true;
}

void ZVulkanLineRenderer::setTexture(ZVulkanTexture* texture)
{
    m_texture = texture;
    m_useTextureColor = (texture != nullptr);
    
    // If texture changed, need to recreate descriptors
    if (m_useTextureColor) {
        // Create or update descriptor set for the texture
        // This will be implemented in the next part
    }
    
    // Update pipeline for texture usage
    createPipeline();
}

void ZVulkanLineRenderer::setLineWidth(float width)
{
    m_lineWidth = std::max(1.0f, width);
}

void ZVulkanLineRenderer::setLineStrip(bool enable)
{
    if (m_isLineStrip != enable) {
        m_isLineStrip = enable;
        
        // If data is already set, reprocess it for the new mode
        if (!m_vertices.empty() || (!m_smoothLineP0s.empty() && !m_smoothLineP1s.empty())) {
            if (!m_useGeometryShader) {
                processLinesForSmooth();
            }
            m_dataChanged = true;
        }
    }
}

void ZVulkanLineRenderer::setUseSmoothLine(bool enable)
{
    if (m_useSmoothLine != enable) {
        m_useSmoothLine = enable;
        createPipeline();
    }
}

void ZVulkanLineRenderer::setRoundCap(bool enable)
{
    if (m_roundCap != enable) {
        m_roundCap = enable;
        
        // Round cap and screen alignment are mutually exclusive
        if (enable && m_screenAligned) {
            m_screenAligned = false;
        }
        
        createPipeline();
    }
}

void ZVulkanLineRenderer::setScreenAlign(bool enable)
{
    if (m_screenAligned != enable) {
        m_screenAligned = enable;
        
        // Screen alignment and round cap are mutually exclusive
        if (enable && m_roundCap) {
            m_roundCap = false;
        }
        
        createPipeline();
    }
}

void ZVulkanLineRenderer::compile()
{
    if (m_dataChanged) {
        // Create/update GPU buffers with current data
        createBuffers();
        m_dataChanged = false;
    }
}

void ZVulkanLineRenderer::render(vk::raii::CommandBuffer& cmdBuffer)
{
    if ((m_vertices.empty() && m_smoothLineP0s.empty()) || 
        (!m_useSmoothLine && m_vertices.empty()) ||
        (m_useSmoothLine && !m_useGeometryShader && m_smoothLineP0s.empty())) {
        // Nothing to render
        return;
    }
    
    if (m_useSmoothLine) {
        renderSmooth(cmdBuffer);
    } else {
        // Standard line rendering
        if (m_standardLinePipeline && m_vertexBuffer) {
            // Bind the standard line pipeline
            cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_standardLinePipeline->pipeline());
            
            // Set dynamic states if needed (like line width)
            cmdBuffer.setLineWidth(m_lineWidth);
            
            // Configure descriptor sets if using textures
            if (m_useTextureColor && m_descriptorSet) {
                cmdBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    m_standardLinePipeline->pipelineLayout(),
                    0, 
                    *m_descriptorSet, 
                    nullptr);
            }
            
            // Push constants for transformation matrices and parameters
            auto pushConstants = preparePushConstants();
            cmdBuffer.pushConstants(m_standardLinePipeline->pipelineLayout(), 
                                  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 
                                  0,
                                  sizeof(pushConstants),
                                  &pushConstants);
            
            // Bind vertex and index buffers
            std::array<vk::Buffer, 1> vertexBuffers = { m_vertexBuffer->buffer() };
            std::array<vk::DeviceSize, 1> offsets = { 0 };
            cmdBuffer.bindVertexBuffers(0, vertexBuffers, offsets);
            
            if (!m_colors.empty() && m_colorBuffer) {
                std::array<vk::Buffer, 1> colorBuffers = { m_colorBuffer->buffer() };
                cmdBuffer.bindVertexBuffers(1, colorBuffers, offsets);
            }
            
            if (!m_indices.empty() && m_indexBuffer) {
                cmdBuffer.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);
                cmdBuffer.drawIndexed(static_cast<uint32_t>(m_indices.size()), 1, 0, 0, 0);
            } else {
                // Draw without indices
                cmdBuffer.draw(static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
            }
        }
    }
}

void ZVulkanLineRenderer::renderSmooth(vk::raii::CommandBuffer& cmdBuffer)
{
    if (m_useGeometryShader) {
        // Smooth line rendering with geometry shader
        if (m_smoothLinePipeline && (m_vertexBuffer || (m_p0Buffer && m_p1Buffer))) {
            // Bind the smooth line pipeline
            cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_smoothLinePipeline->pipeline());
            
            // Prepare push constants with standard renderer values
            auto basePushConstants = preparePushConstants();
            
            // Add line width and other parameters to push constants
            struct LineParams {
                float lineWidth;
                float sizeScale;
                int roundCap;
                int screenAlign;
            } lineParams;
            
            lineParams.lineWidth = m_lineWidth;
            lineParams.sizeScale = getSizeScale();  // Use size scale from the renderer
            lineParams.roundCap = m_roundCap ? 1 : 0;
            lineParams.screenAlign = m_screenAligned ? 1 : 0;
            
            // Push base constants (matrices, etc.)
            cmdBuffer.pushConstants(m_smoothLinePipeline->pipelineLayout(), 
                                  vk::ShaderStageFlagBits::eVertex | 
                                  vk::ShaderStageFlagBits::eGeometry | 
                                  vk::ShaderStageFlagBits::eFragment, 
                                  0,
                                  sizeof(basePushConstants),
                                  &basePushConstants);
            
            // Push line-specific parameters
            cmdBuffer.pushConstants(m_smoothLinePipeline->pipelineLayout(),
                                  vk::ShaderStageFlagBits::eGeometry | 
                                  vk::ShaderStageFlagBits::eFragment,
                                  sizeof(basePushConstants),
                                  sizeof(lineParams),
                                  &lineParams);
            
            // Configure descriptor sets if using textures
            if (m_useTextureColor && m_descriptorSet) {
                cmdBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    m_smoothLinePipeline->pipelineLayout(),
                    0, 
                    *m_descriptorSet, 
                    nullptr);
            }
            
            // Determine what to draw based on available data
            if (m_p0Buffer && m_p1Buffer) {
                // Draw using separate start/end point buffers (more efficient for geometry shader)
                std::array<vk::Buffer, 2> vertBuffers = { m_p0Buffer->buffer(), m_p1Buffer->buffer() };
                std::array<vk::DeviceSize, 2> offsets = { 0, 0 };
                cmdBuffer.bindVertexBuffers(0, vertBuffers, offsets);
                
                if (m_p0ColorBuffer && m_p1ColorBuffer) {
                    std::array<vk::Buffer, 2> colorBuffers = { m_p0ColorBuffer->buffer(), m_p1ColorBuffer->buffer() };
                    cmdBuffer.bindVertexBuffers(2, colorBuffers, offsets);
                }
                
                cmdBuffer.draw(static_cast<uint32_t>(m_smoothLineP0s.size()), 1, 0, 0);
            } else if (m_vertexBuffer) {
                // Draw using standard vertex buffer (for line strips or regular lines)
                std::array<vk::Buffer, 1> vertexBuffers = { m_vertexBuffer->buffer() };
                std::array<vk::DeviceSize, 1> offsets = { 0 };
                cmdBuffer.bindVertexBuffers(0, vertexBuffers, offsets);
                
                if (m_colorBuffer) {
                    std::array<vk::Buffer, 1> colorBuffers = { m_colorBuffer->buffer() };
                    cmdBuffer.bindVertexBuffers(1, colorBuffers, offsets);
                }
                
                if (m_isLineStrip) {
                    // Draw as line strip
                    cmdBuffer.draw(static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
                } else {
                    // Draw as individual lines
                    cmdBuffer.draw(static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
                }
            }
        }
    } else {
        // Smooth line rendering without geometry shader (using pre-transformed quads)
        if (m_smoothLineAlternativePipeline && m_p0Buffer && m_p1Buffer && m_flagsBuffer) {
            // Bind the alternative smooth line pipeline
            cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_smoothLineAlternativePipeline->pipeline());
            
            // Push constants for transformation and line parameters
            auto basePushConstants = preparePushConstants();
            struct LineParams {
                float lineWidth;
                int roundCap;
                int screenAlign;
                float padding;
            } lineParams;
            
            lineParams.lineWidth = m_lineWidth;
            lineParams.roundCap = m_roundCap ? 1 : 0;
            lineParams.screenAlign = m_screenAligned ? 1 : 0;
            lineParams.padding = 0.0f;
            
            // Push standard constants
            cmdBuffer.pushConstants(m_smoothLineAlternativePipeline->pipelineLayout(), 
                                  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 
                                  0,
                                  sizeof(basePushConstants),
                                  &basePushConstants);
            
            // Push line parameters
            cmdBuffer.pushConstants(m_smoothLineAlternativePipeline->pipelineLayout(),
                                  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                  sizeof(basePushConstants),
                                  sizeof(lineParams),
                                  &lineParams);
            
            // Configure descriptor sets for textures
            if (m_useTextureColor && m_descriptorSet) {
                cmdBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    m_smoothLineAlternativePipeline->pipelineLayout(),
                    0, 
                    *m_descriptorSet, 
                    nullptr);
            }
            
            // Bind vertex attributes
            std::array<vk::Buffer, 3> buffers = { 
                m_p0Buffer->buffer(),
                m_p1Buffer->buffer(),
                m_flagsBuffer->buffer()
            };
            std::array<vk::DeviceSize, 3> offsets = { 0, 0, 0 };
            cmdBuffer.bindVertexBuffers(0, buffers, offsets);
            
            // Bind color attributes if available
            if (m_p0ColorBuffer && m_p1ColorBuffer) {
                std::array<vk::Buffer, 2> colorBuffers = {
                    m_p0ColorBuffer->buffer(),
                    m_p1ColorBuffer->buffer()
                };
                std::array<vk::DeviceSize, 2> colorOffsets = { 0, 0 };
                cmdBuffer.bindVertexBuffers(3, colorBuffers, colorOffsets);
            }
            
            // Bind index buffer and draw
            if (m_indexBuffer) {
                cmdBuffer.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);
                cmdBuffer.drawIndexed(static_cast<uint32_t>(m_indices.size()), 1, 0, 0, 0);
            } else {
                // Draw without indices - requires correct vertex arrangements
                cmdBuffer.draw(static_cast<uint32_t>(m_smoothLineP0s.size() * 4), 1, 0, 0);
            }
        }
    }
}

void ZVulkanLineRenderer::createShaders()
{
    // Create standard line shader (for basic line rendering)
    m_standardLineShader = std::make_unique<ZVulkanShader>(m_rendererBase);
    m_standardLineShader->addVertexShaderFromFile("line.vert.spv");
    m_standardLineShader->addFragmentShaderFromFile("line_func.frag.spv");
    m_standardLineShader->setVertexInputBinding(0, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // Position
    m_standardLineShader->setVertexInputAttribute(0, 0, vk::Format::eR32G32B32Sfloat, 0);             // Position offset
    
    if (!m_useTextureColor) {
        // Add color attribute if not using texture
        m_standardLineShader->setVertexInputBinding(1, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // Color
        m_standardLineShader->setVertexInputAttribute(1, 1, vk::Format::eR32G32B32A32Sfloat, 0);         // Color offset
    }
    
    // Create shader for smooth lines with geometry shader
    if (m_useGeometryShader) {
        m_smoothLineShader = std::make_unique<ZVulkanShader>(m_rendererBase);
        m_smoothLineShader->addVertexShaderFromFile("wideline.vert.spv");
        m_smoothLineShader->addGeometryShaderFromFile("wideline.geom.spv");
        m_smoothLineShader->addFragmentShaderFromFile("wideline_func.frag.spv");
        
        // Configure vertex inputs for geometry shader approach
        if (m_p0Buffer && m_p1Buffer) {
            // Using separate start/end points (more efficient for geometry shader)
            m_smoothLineShader->setVertexInputBinding(0, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // Start point
            m_smoothLineShader->setVertexInputBinding(1, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // End point
            m_smoothLineShader->setVertexInputAttribute(0, 0, vk::Format::eR32G32B32Sfloat, 0);            // Start offset
            m_smoothLineShader->setVertexInputAttribute(1, 1, vk::Format::eR32G32B32Sfloat, 0);            // End offset
            
            if (!m_useTextureColor) {
                // Add color attributes for both points
                m_smoothLineShader->setVertexInputBinding(2, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // Start color
                m_smoothLineShader->setVertexInputBinding(3, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // End color
                m_smoothLineShader->setVertexInputAttribute(2, 2, vk::Format::eR32G32B32A32Sfloat, 0);         // Start color offset
                m_smoothLineShader->setVertexInputAttribute(3, 3, vk::Format::eR32G32B32A32Sfloat, 0);         // End color offset
            }
        } else {
            // Using standard vertex buffer (for line strips or regular lines)
            m_smoothLineShader->setVertexInputBinding(0, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // Position
            m_smoothLineShader->setVertexInputAttribute(0, 0, vk::Format::eR32G32B32Sfloat, 0);             // Position offset
            
            if (!m_useTextureColor) {
                // Add color attribute
                m_smoothLineShader->setVertexInputBinding(1, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // Color
                m_smoothLineShader->setVertexInputAttribute(1, 1, vk::Format::eR32G32B32A32Sfloat, 0);         // Color offset
            }
        }
    }
    
    // Create alternative shader for smooth lines without geometry shader
    m_smoothLineAlternativeShader = std::make_unique<ZVulkanShader>(m_rendererBase);
    m_smoothLineAlternativeShader->addVertexShaderFromFile("wideline1.vert.spv");
    m_smoothLineAlternativeShader->addFragmentShaderFromFile("wideline_func1.frag.spv");
    
    // Configure vertex inputs for the alternative approach
    m_smoothLineAlternativeShader->setVertexInputBinding(0, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // P0
    m_smoothLineAlternativeShader->setVertexInputBinding(1, sizeof(glm::vec3), vk::VertexInputRate::eVertex);  // P1
    m_smoothLineAlternativeShader->setVertexInputBinding(2, sizeof(float), vk::VertexInputRate::eVertex);      // Flags
    m_smoothLineAlternativeShader->setVertexInputAttribute(0, 0, vk::Format::eR32G32B32Sfloat, 0);             // P0 offset
    m_smoothLineAlternativeShader->setVertexInputAttribute(1, 1, vk::Format::eR32G32B32Sfloat, 0);             // P1 offset
    m_smoothLineAlternativeShader->setVertexInputAttribute(2, 2, vk::Format::eR32Sfloat, 0);                  // Flags offset
    
    if (!m_useTextureColor) {
        // Add color attributes
        m_smoothLineAlternativeShader->setVertexInputBinding(3, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // P0 color
        m_smoothLineAlternativeShader->setVertexInputBinding(4, sizeof(glm::vec4), vk::VertexInputRate::eVertex);  // P1 color
        m_smoothLineAlternativeShader->setVertexInputAttribute(3, 3, vk::Format::eR32G32B32A32Sfloat, 0);         // P0 color offset
        m_smoothLineAlternativeShader->setVertexInputAttribute(4, 4, vk::Format::eR32G32B32A32Sfloat, 0);         // P1 color offset
    }
}

void ZVulkanLineRenderer::createPipeline()
{
    // Create pipeline states with common settings
    ZVulkanPipelineState pipelineState;
    pipelineState.cullMode = vk::CullModeFlagBits::eNone;
    pipelineState.depthTestEnable = true;
    pipelineState.depthWriteEnable = true;
    pipelineState.depthCompareOp = vk::CompareOp::eLess;
    
    // Standard line pipeline with basic settings
    if (m_standardLineShader) {
        pipelineState.topology = m_isLineStrip ? vk::PrimitiveTopology::eLineStrip : vk::PrimitiveTopology::eLineList;
        pipelineState.dynamicStates.push_back(vk::DynamicState::eLineWidth);
        
        // Create standard line pipeline
        m_standardLinePipeline = std::make_unique<ZVulkanPipeline>(
            m_rendererBase, 
            *m_standardLineShader, 
            pipelineState,
            m_useTextureColor ? m_texture->getDescriptorSetLayout() : vk::raii::DescriptorSetLayout(nullptr)
        );
    }
    
    // Smooth line pipeline with geometry shader
    if (m_useGeometryShader && m_smoothLineShader) {
        // For smooth lines we use points as input primitives
        pipelineState.topology = vk::PrimitiveTopology::ePointList;
        if (m_isLineStrip) {
            pipelineState.topology = vk::PrimitiveTopology::eLineStrip;
        }
        
        // Reset dynamic states
        pipelineState.dynamicStates.clear();
        
        // Create smooth line pipeline
        m_smoothLinePipeline = std::make_unique<ZVulkanPipeline>(
            m_rendererBase, 
            *m_smoothLineShader, 
            pipelineState,
            m_useTextureColor ? m_texture->getDescriptorSetLayout() : vk::raii::DescriptorSetLayout(nullptr)
        );
    }
    
    // Alternative smooth line pipeline without geometry shader
    if (m_smoothLineAlternativeShader) {
        // For alternative smooth lines we use triangles
        pipelineState.topology = vk::PrimitiveTopology::eTriangleList;
        
        // Reset dynamic states
        pipelineState.dynamicStates.clear();
        
        // Create alternative smooth line pipeline
        m_smoothLineAlternativePipeline = std::make_unique<ZVulkanPipeline>(
            m_rendererBase, 
            *m_smoothLineAlternativeShader, 
            pipelineState,
            m_useTextureColor ? m_texture->getDescriptorSetLayout() : vk::raii::DescriptorSetLayout(nullptr)
        );
    }
    
    // If using texture, create/update descriptor set
    if (m_useTextureColor && m_texture) {
        // Create or update descriptor for the texture
        // Will be implemented in the next part
    }
}

void ZVulkanLineRenderer::createBuffers()
{
    // Create or update buffers based on current data
    auto& device = m_rendererBase.getDevice();
    
    // Standard vertex buffer for basic line rendering
    if (!m_vertices.empty()) {
        vk::DeviceSize bufferSize = m_vertices.size() * sizeof(glm::vec3);
        
        // Create vertex buffer if it doesn't exist or needs to be resized
        if (!m_vertexBuffer || m_vertexBuffer->getSize() < bufferSize) {
            m_vertexBuffer = std::make_unique<ZVulkanBuffer>(
                m_rendererBase,
                bufferSize,
                vk::BufferUsageFlagBits::eVertexBuffer,
                VMA_MEMORY_USAGE_CPU_TO_GPU
            );
        }
        
        // Upload vertex data
        void* mappedData = m_vertexBuffer->map();
        std::memcpy(mappedData, m_vertices.data(), bufferSize);
        m_vertexBuffer->unmap();
    }
    
    // Color buffer for vertex colors
    if (!m_colors.empty()) {
        vk::DeviceSize bufferSize = m_colors.size() * sizeof(glm::vec4);
        
        // Create color buffer if it doesn't exist or needs to be resized
        if (!m_colorBuffer || m_colorBuffer->getSize() < bufferSize) {
            m_colorBuffer = std::make_unique<ZVulkanBuffer>(
                m_rendererBase,
                bufferSize,
                vk::BufferUsageFlagBits::eVertexBuffer,
                VMA_MEMORY_USAGE_CPU_TO_GPU
            );
        }
        
        // Upload color data
        void* mappedData = m_colorBuffer->map();
        std::memcpy(mappedData, m_colors.data(), bufferSize);
        m_colorBuffer->unmap();
    }
    
    // Index buffer for indexed rendering
    if (!m_indices.empty()) {
        vk::DeviceSize bufferSize = m_indices.size() * sizeof(uint32_t);
        
        // Create index buffer if it doesn't exist or needs to be resized
        if (!m_indexBuffer || m_indexBuffer->getSize() < bufferSize) {
            m_indexBuffer = std::make_unique<ZVulkanBuffer>(
                m_rendererBase,
                bufferSize,
                vk::BufferUsageFlagBits::eIndexBuffer,
                VMA_MEMORY_USAGE_CPU_TO_GPU
            );
        }
        
        // Upload index data
        void* mappedData = m_indexBuffer->map();
        std::memcpy(mappedData, m_indices.data(), bufferSize);
        m_indexBuffer->unmap();
    }
    
    // For smooth lines without geometry shader, we need additional buffers
    if (!m_useGeometryShader) {
        // P0 buffer (start points)
        if (!m_smoothLineP0s.empty()) {
            vk::DeviceSize bufferSize = m_smoothLineP0s.size() * sizeof(glm::vec3);
            
            if (!m_p0Buffer || m_p0Buffer->getSize() < bufferSize) {
                m_p0Buffer = std::make_unique<ZVulkanBuffer>(
                    m_rendererBase,
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    VMA_MEMORY_USAGE_CPU_TO_GPU
                );
            }
            
            void* mappedData = m_p0Buffer->map();
            std::memcpy(mappedData, m_smoothLineP0s.data(), bufferSize);
            m_p0Buffer->unmap();
        }
        
        // P1 buffer (end points)
        if (!m_smoothLineP1s.empty()) {
            vk::DeviceSize bufferSize = m_smoothLineP1s.size() * sizeof(glm::vec3);
            
            if (!m_p1Buffer || m_p1Buffer->getSize() < bufferSize) {
                m_p1Buffer = std::make_unique<ZVulkanBuffer>(
                    m_rendererBase,
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    VMA_MEMORY_USAGE_CPU_TO_GPU
                );
            }
            
            void* mappedData = m_p1Buffer->map();
            std::memcpy(mappedData, m_smoothLineP1s.data(), bufferSize);
            m_p1Buffer->unmap();
        }
        
        // Flags buffer for smooth lines
        if (!m_smoothLineFlags.empty()) {
            vk::DeviceSize bufferSize = m_smoothLineFlags.size() * sizeof(float);
            
            if (!m_flagsBuffer || m_flagsBuffer->getSize() < bufferSize) {
                m_flagsBuffer = std::make_unique<ZVulkanBuffer>(
                    m_rendererBase,
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    VMA_MEMORY_USAGE_CPU_TO_GPU
                );
            }
            
            void* mappedData = m_flagsBuffer->map();
            std::memcpy(mappedData, m_smoothLineFlags.data(), bufferSize);
            m_flagsBuffer->unmap();
        }
        
        // Color buffers for smooth lines
        if (!m_smoothLineP0Colors.empty()) {
            vk::DeviceSize bufferSize = m_smoothLineP0Colors.size() * sizeof(glm::vec4);
            
            if (!m_p0ColorBuffer || m_p0ColorBuffer->getSize() < bufferSize) {
                m_p0ColorBuffer = std::make_unique<ZVulkanBuffer>(
                    m_rendererBase,
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    VMA_MEMORY_USAGE_CPU_TO_GPU
                );
            }
            
            void* mappedData = m_p0ColorBuffer->map();
            std::memcpy(mappedData, m_smoothLineP0Colors.data(), bufferSize);
            m_p0ColorBuffer->unmap();
        }
        
        if (!m_smoothLineP1Colors.empty()) {
            vk::DeviceSize bufferSize = m_smoothLineP1Colors.size() * sizeof(glm::vec4);
            
            if (!m_p1ColorBuffer || m_p1ColorBuffer->getSize() < bufferSize) {
                m_p1ColorBuffer = std::make_unique<ZVulkanBuffer>(
                    m_rendererBase,
                    bufferSize,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    VMA_MEMORY_USAGE_CPU_TO_GPU
                );
            }
            
            void* mappedData = m_p1ColorBuffer->map();
            std::memcpy(mappedData, m_smoothLineP1Colors.data(), bufferSize);
            m_p1ColorBuffer->unmap();
        }
    }
    
    // Setup texture descriptor set if needed
    if (m_useTextureColor && m_texture) {
        // Create descriptor set layout and descriptor set for texture
        auto descriptorType = vk::DescriptorType::eCombinedImageSampler;
        auto stageFlags = vk::ShaderStageFlagBits::eFragment;
        
        m_descriptorSet = std::make_unique<ZVulkanDescriptorSet>(
            m_rendererBase,
            m_texture->getDescriptorSetLayout()
        );
        
        // Update descriptor set with texture information
        vk::DescriptorImageInfo imageInfo(
            *m_texture->getSampler(),
            *m_texture->getImageView(),
            vk::ImageLayout::eShaderReadOnlyOptimal
        );
        
        vk::WriteDescriptorSet descriptorWrite(
            **m_descriptorSet,
            0,
            0,
            1,
            descriptorType,
            &imageInfo,
            nullptr,
            nullptr
        );
        
        (*device).updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
    }
}

void ZVulkanLineRenderer::processLinesForSmooth()
{
    // Convert standard vertex array to start/end point format for smooth line rendering
    m_smoothLineP0s.clear();
    m_smoothLineP1s.clear();
    m_smoothLineFlags.clear();
    m_indices.clear();
    
    if (m_vertices.empty()) {
        return;
    }
    
    // Common flags for different vertex positions in a quad
    static const float flags[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
    
    uint32_t quadIndex = 0;
    
    if (m_isLineStrip) {
        // Process as line strip (connecting sequential points)
        for (size_t i = 1; i < m_vertices.size(); ++i) {
            // For each line segment, create 4 vertices (a quad)
            for (int j = 0; j < 4; ++j) {
                m_smoothLineP0s.push_back(m_vertices[i-1]);
                m_smoothLineP1s.push_back(m_vertices[i]);
                m_smoothLineFlags.push_back(flags[j]);
            }
            
            // Add indices for two triangles per quad (6 indices per quad)
            uint32_t baseIndex = quadIndex * 4;
            m_indices.push_back(baseIndex);
            m_indices.push_back(baseIndex + 1);
            m_indices.push_back(baseIndex + 2);
            m_indices.push_back(baseIndex + 1);
            m_indices.push_back(baseIndex + 3);
            m_indices.push_back(baseIndex + 2);
            
            quadIndex++;
        }
    } else {
        // Process as line list (pairs of points define lines)
        for (size_t i = 0; i < m_vertices.size(); i += 2) {
            if (i + 1 >= m_vertices.size()) {
                break;
            }
            
            // For each line segment, create 4 vertices (a quad)
            for (int j = 0; j < 4; ++j) {
                m_smoothLineP0s.push_back(m_vertices[i]);
                m_smoothLineP1s.push_back(m_vertices[i+1]);
                m_smoothLineFlags.push_back(flags[j]);
            }
            
            // Add indices for two triangles per quad (6 indices per quad)
            uint32_t baseIndex = quadIndex * 4;
            m_indices.push_back(baseIndex);
            m_indices.push_back(baseIndex + 1);
            m_indices.push_back(baseIndex + 2);
            m_indices.push_back(baseIndex + 1);
            m_indices.push_back(baseIndex + 3);
            m_indices.push_back(baseIndex + 2);
            
            quadIndex++;
        }
    }
    
    // Process colors if available
    if (!m_colors.empty()) {
        m_smoothLineP0Colors.clear();
        m_smoothLineP1Colors.clear();
        
        if (m_isLineStrip) {
            for (size_t i = 1; i < m_colors.size(); ++i) {
                for (int j = 0; j < 4; ++j) {
                    m_smoothLineP0Colors.push_back(m_colors[i-1]);
                    m_smoothLineP1Colors.push_back(m_colors[i]);
                }
            }
        } else {
            for (size_t i = 0; i < m_colors.size(); i += 2) {
                if (i + 1 >= m_colors.size()) {
                    break;
                }
                
                for (int j = 0; j < 4; ++j) {
                    m_smoothLineP0Colors.push_back(m_colors[i]);
                    m_smoothLineP1Colors.push_back(m_colors[i+1]);
                }
            }
        }
    } else if (!m_vertices.empty()) {
        // Use default white color if no colors provided
        glm::vec4 defaultColor(1.0f, 1.0f, 1.0f, 1.0f);
        
        m_smoothLineP0Colors.clear();
        m_smoothLineP1Colors.clear();
        
        for (size_t i = 0; i < m_smoothLineP0s.size(); ++i) {
            m_smoothLineP0Colors.push_back(defaultColor);
            m_smoothLineP1Colors.push_back(defaultColor);
        }
    }
}

} // namespace nim 