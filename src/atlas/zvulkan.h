#pragma once

#ifdef __APPLE__

#include <Availability.h>

#define VK_USE_PLATFORM_METAL_EXT 1

#define VK_ENABLE_BETA_EXTENSIONS 1 // VK_KHR_portability_subset

#ifdef __IPHONE_OS_VERSION_MAX_ALLOWED
#define VK_USE_PLATFORM_IOS_MVK 1
#endif

#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#define VK_USE_PLATFORM_MACOS_MVK 1
#endif

#endif

#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>

#include "zglmutils.h"
#include "zlog.h"
#include <memory>
#include <vector>
#include <string>
#include <optional>

namespace nim {

// Forward declarations
class ZVulkanSwapChain;
class ZVulkanDevice;
class ZVulkanPipeline;
class ZVulkanShader;
class ZVulkanTexture;
class ZVulkanBuffer;
class ZVulkanDescriptorSet;
class ZVulkanDescriptorPool;
class ZVulkanRenderPass;

/**
 * @brief Initialize Vulkan instance and debugging features
 */
void initVulkan();

/**
 * @brief Main class for Vulkan device management and resource creation
 * Similar to Z3DContext in the OpenGL implementation
 */
class ZVulkanContext
{
public:
    ZVulkanContext();
    ~ZVulkanContext();

    // Returns the Vulkan RAII instance
    vk::raii::Instance& instance() { return *m_instance; }
    
    // Returns the currently selected physical device
    vk::raii::PhysicalDevice& physicalDevice() { return *m_physicalDevice; }
    
    // Returns the logical device
    vk::raii::Device& device() { return *m_device; }

    // Returns graphics queue
    vk::raii::Queue& graphicsQueue() { return *m_graphicsQueue; }
    
    // Returns presentation queue
    vk::raii::Queue& presentQueue() { return *m_presentQueue; }
    
    // Return command pool
    vk::CommandPool commandPool() const { return *m_commandPool; }
    
    // Queue family indices for the selected physical device
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        
        bool isComplete() const {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    QueueFamilyIndices queueFamilyIndices() const { return m_queueFamilyIndices; }

    // Create a ZVulkanDevice instance from this context
    std::unique_ptr<ZVulkanDevice> createDevice();
    
    // Check if the device supports the required extensions
    bool checkDeviceExtensionSupport(vk::raii::PhysicalDevice& physicalDevice) const;

private:
    // Vulkan initialization steps
    void createInstance();
    void setupDebugMessenger();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createCommandPool();
    
    // Find queue families that support required operations
    QueueFamilyIndices findQueueFamilies(vk::raii::PhysicalDevice& physicalDevice) const;
    
    // Vulkan RAII objects
    std::optional<vk::raii::Context> m_context;
    std::optional<vk::raii::Instance> m_instance;
    std::optional<vk::raii::DebugUtilsMessengerEXT> m_debugMessenger;
    std::optional<vk::raii::PhysicalDevice> m_physicalDevice;
    std::optional<vk::raii::Device> m_device;
    std::optional<vk::raii::Queue> m_graphicsQueue;
    std::optional<vk::raii::Queue> m_presentQueue;
    std::optional<vk::raii::CommandPool> m_commandPool;
    
    QueueFamilyIndices m_queueFamilyIndices;
};

/**
 * @brief Vulkan buffer class for vertex and index data
 */
class ZVulkanBuffer
{
public:
    ZVulkanBuffer(ZVulkanDevice& device, 
                 size_t size,
                 vk::BufferUsageFlags usage,
                 vk::MemoryPropertyFlags properties);
    ~ZVulkanBuffer();
    
    vk::Buffer buffer() const { return *m_buffer; }
    vk::DeviceMemory memory() const { return *m_bufferMemory; }
    size_t size() const { return m_size; }
    
    void copyData(const void* data, size_t size);
    
    // Add memory mapping functions
    void* map(vk::DeviceSize offset, vk::DeviceSize size);
    void unmap();
    
private:
    void createBuffer();
    
    friend class ZVulkanTexture;
    friend class ZVulkanSwapChain;
    
    ZVulkanDevice& m_device;
    std::optional<vk::raii::Buffer> m_buffer;
    std::optional<vk::raii::DeviceMemory> m_bufferMemory;
    size_t m_size;
    vk::BufferUsageFlags m_usage;
    vk::MemoryPropertyFlags m_memoryProperties;
};

/**
 * @brief Represents a logical device and its capabilities
 * Similar to Z3DGlobalParameters in the OpenGL implementation
 */
class ZVulkanDevice
{
public:
    ZVulkanDevice(ZVulkanContext& context);
    ~ZVulkanDevice();
    
    ZVulkanContext& context() { return m_context; }

    // Buffer creation
    std::unique_ptr<ZVulkanBuffer> createBuffer(
        size_t size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties);
        
    // Texture creation functions
    std::unique_ptr<ZVulkanTexture> createTexture(
        uint32_t width, 
        uint32_t height, 
        vk::Format format);
        
    std::unique_ptr<ZVulkanTexture> createTexture(
        uint32_t width, 
        uint32_t height, 
        vk::Format format,
        vk::ImageUsageFlags usage,
        vk::MemoryPropertyFlags memoryProperties);
        
    // Shader creation functions
    std::unique_ptr<ZVulkanShader> createShader(
        const std::string& vertexCode,
        const std::string& fragmentCode);
    
    // Pipeline creation functions
    std::unique_ptr<ZVulkanPipeline> createPipeline(
        ZVulkanShader& shader,
        const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
        const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);
    
    // Descriptor pool and set creation
    std::unique_ptr<ZVulkanDescriptorPool> createDescriptorPool();
    
    std::unique_ptr<ZVulkanDescriptorSet> createDescriptorSet(
        ZVulkanDescriptorPool& pool, 
        vk::DescriptorSetLayout layout);
        
    // SwapChain creation
    std::unique_ptr<ZVulkanSwapChain> createSwapChain(
        uint32_t width, 
        uint32_t height);
    
    // Command buffer management
    vk::raii::CommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(vk::raii::CommandBuffer& cmdBuffer);

private:
    ZVulkanContext& m_context;
};

/**
 * @brief Shader module for Vulkan
 * Similar to Z3DShaderProgram in the OpenGL implementation
 */
class ZVulkanShader
{
public:
    ZVulkanShader(ZVulkanDevice& device, 
                  const std::string& vertexCode, 
                  const std::string& fragmentCode);
    ~ZVulkanShader();
    
    const std::vector<vk::PipelineShaderStageCreateInfo>& shaderStages() const;
    
private:
    void createShaderModule(const std::string& code, vk::ShaderStageFlagBits stage);
    
    ZVulkanDevice& m_device;
    std::string m_vertexCode;
    std::string m_fragmentCode;
    std::vector<vk::PipelineShaderStageCreateInfo> m_shaderStages;
};

/**
 * @brief Graphics pipeline for Vulkan
 * Similar to shader program in the OpenGL implementation
 */
class ZVulkanPipeline
{
public:
    ZVulkanPipeline(ZVulkanDevice& device, 
                    ZVulkanShader& shader, 
                    const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                    const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);
    ~ZVulkanPipeline();
    
    vk::Pipeline pipeline() const { return *m_pipeline; }
    vk::PipelineLayout pipelineLayout() const { return *m_pipelineLayout; }
    
    // Set descriptor set layouts for the pipeline
    void setDescriptorSetLayouts(const std::vector<vk::DescriptorSetLayout>& layouts);
    
    // Set push constant ranges for the pipeline
    void setPushConstantRanges(const std::vector<vk::PushConstantRange>& pushConstantRanges);
    
    // Create the pipeline
    void create();
    
private:
    ZVulkanDevice& m_device;
    ZVulkanShader& m_shader;
    vk::PipelineVertexInputStateCreateInfo m_vertexInputInfo;
    vk::PrimitiveTopology m_topology;
    
    std::vector<vk::DescriptorSetLayout> m_descriptorSetLayouts;
    std::vector<vk::PushConstantRange> m_pushConstantRanges;
    
    std::optional<vk::raii::PipelineLayout> m_pipelineLayout;
    std::optional<vk::raii::Pipeline> m_pipeline;
};

/**
 * @brief Texture for Vulkan
 * Similar to Z3DTexture in the OpenGL implementation
 */
class ZVulkanTexture
{
public:
    ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format);
    
    // Create a texture with specific usage and memory properties
    ZVulkanTexture(ZVulkanDevice& device, 
                  uint32_t width, 
                  uint32_t height, 
                  vk::Format format,
                  vk::ImageUsageFlags usage,
                  vk::MemoryPropertyFlags memoryProperties);
    
    ~ZVulkanTexture();
    
    // Upload data to texture
    void uploadData(const void* data, size_t size);
    
    // Download data from texture to host memory
    void downloadData(void* data, size_t size);
    
    // Transition image layout for various operations
    void transitionLayout(vk::raii::CommandBuffer& cmdBuffer, 
                         vk::ImageLayout oldLayout, 
                         vk::ImageLayout newLayout);
    
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    vk::Format format() const { return m_format; }
    vk::Image image() const { return *m_image; }
    vk::ImageView imageView() const { return *m_imageView; }
    vk::ImageLayout layout() const { return m_currentLayout; }
    
private:
    void createImage();
    void createImageView();
    
    ZVulkanDevice& m_device;
    uint32_t m_width;
    uint32_t m_height;
    vk::Format m_format;
    std::optional<vk::raii::Image> m_image;
    std::optional<vk::raii::DeviceMemory> m_imageMemory;
    std::optional<vk::raii::ImageView> m_imageView;
    
    vk::ImageUsageFlags m_usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags m_memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageLayout m_currentLayout = vk::ImageLayout::eUndefined;
};

/**
 * @brief Manages descriptor sets and layouts for Vulkan
 * Used for binding resources to shaders
 */
class ZVulkanDescriptorPool
{
public:
    ZVulkanDescriptorPool(ZVulkanDevice& device);
    ~ZVulkanDescriptorPool();
    
    vk::DescriptorPool pool() const { return *m_descriptorPool; }
    
    vk::raii::DescriptorSet allocateDescriptorSet(vk::DescriptorSetLayout layout);
    
private:
    ZVulkanDevice& m_device;
    std::optional<vk::raii::DescriptorPool> m_descriptorPool;
};

/**
 * @brief Descriptor set for Vulkan
 * Used for binding uniform buffers and textures to shaders
 */
class ZVulkanDescriptorSet
{
public:
    ZVulkanDescriptorSet(ZVulkanDevice& device, vk::raii::DescriptorSet&& descriptorSet);
    ~ZVulkanDescriptorSet();
    
    // Methods to update descriptor set
    void updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer);
    void updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler);
    
    vk::DescriptorSet descriptorSet() const { return *m_descriptorSet; }
    
private:
    ZVulkanDevice& m_device;
    vk::raii::DescriptorSet m_descriptorSet{nullptr};
};

/**
 * @brief Manages Vulkan framebuffers and render targets
 * Similar to the Z3DRenderTarget in the OpenGL implementation
 */
class ZVulkanSwapChain
{
public:
    ZVulkanSwapChain(ZVulkanDevice& device, uint32_t width, uint32_t height);
    ~ZVulkanSwapChain();
    
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    
    // Resize the swapchain images
    void resize(uint32_t width, uint32_t height);
    
    // Get color attachment for rendering
    ZVulkanTexture& colorAttachment() { return *m_colorAttachment; }
    
    // Get depth attachment for rendering
    ZVulkanTexture& depthAttachment() { return *m_depthAttachment; }
    
    // Begin rendering to the swapchain
    vk::raii::CommandBuffer beginFrame(vk::ClearColorValue clearColor = vk::ClearColorValue(std::array<float, 4>{{0.0f, 0.0f, 0.0f, 1.0f}}), 
                                      vk::ClearDepthStencilValue clearDepthStencil = vk::ClearDepthStencilValue(1.0f, 0));
    
    // End rendering and submit commands
    void endFrame(vk::raii::CommandBuffer& commandBuffer);
    
    // Copy rendered image to provided memory for UI display
    void copyToMemory(void* data, size_t size);
    
private:
    void createAttachments();
    void createSampler();
    void createCommandBuffers();
    
    ZVulkanDevice& m_device;
    uint32_t m_width;
    uint32_t m_height;
    
    std::unique_ptr<ZVulkanTexture> m_colorAttachment;
    std::unique_ptr<ZVulkanTexture> m_depthAttachment;
    std::optional<vk::raii::Sampler> m_sampler;
    std::optional<vk::raii::CommandPool> m_commandPool;
    std::optional<vk::raii::CommandBuffers> m_commandBuffers;
    std::optional<vk::raii::Fence> m_inFlightFence;
    uint32_t m_currentBuffer = 0;
};

// UniformBufferObject structure for matrix transformations
struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

// Shader utilities namespace
namespace ZVulkanShaderUtils
{
  // In a real implementation, this would compile GLSL to SPIR-V
  // For this prototype, we'll just print a warning that this is not implemented
  void compileGLSLToSPIRV(
      const std::string& glslSource,
      vk::ShaderStageFlagBits stage,
      std::vector<uint32_t>& spirvOut);
}

} // namespace nim
