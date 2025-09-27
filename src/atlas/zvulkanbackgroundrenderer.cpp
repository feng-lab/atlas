#include "zvulkanbackgroundrenderer.h"

#include "zsysteminfo.h"
#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"

#include <array>
#include <fmt/format.h>

namespace nim {

namespace {
struct Vertex
{
  float x, y, z;
};

vk::PipelineVertexInputStateCreateInfo fullscreenTriVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                    .stride = sizeof(Vertex),
                                                    .inputRate = vk::VertexInputRate::eVertex};
  static vk::VertexInputAttributeDescription attr{.location = 0,
                                                   .binding = 0,
                                                   .format = vk::Format::eR32G32B32Sfloat,
                                                   .offset = 0};
  static vk::PipelineVertexInputStateCreateInfo vi{};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 1;
  vi.pVertexAttributeDescriptions = &attr;
  return vi;
}

// Background push constant layout in background_func.glslinc
struct BackgroundPC
{
  glm::vec2 screen_dim_RCP;
  glm::vec2 _pad0;
  glm::vec4 color1;
  glm::vec4 color2;
  glm::vec4 region; // {x0, xScale, y0, yScale}
};
} // namespace

ZVulkanBackgroundRenderer::ZVulkanBackgroundRenderer(Z3DRendererBase& rendererBase)
  : ZVulkanRenderer(rendererBase)
{}

ZVulkanBackgroundRenderer::~ZVulkanBackgroundRenderer() = default;

void ZVulkanBackgroundRenderer::ensureVertexBuffer()
{
  if (m_vertexBuffer) {
    return;
  }
  // Fullscreen triangle in NDC
  constexpr std::array<Vertex, 3> tri = {{{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}}};
  auto& dev = device();
  m_vertexBuffer = dev.createBuffer(sizeof(tri),
                                    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexBuffer->copyData(tri.data(), sizeof(tri));
}

void ZVulkanBackgroundRenderer::compile()
{
  auto& dev = device();

  // Load SPIR-V from the deployed resources directory
  // ResourcesDir/shader/vulkan/spv/*
  const QString baseQ = ZSystemInfo::resourcesDirPath() + "/shader/vulkan/spv/";
  const std::string base = baseQ.toStdString();
  m_shader = std::make_unique<ZVulkanShader>(dev,
                                             base + "pass.vert.spv",
                                             base + "background.frag.spv",
                                             std::nullopt);

  // Specialization constants for background modes (parity with Z3DBackgroundRenderer)
  // IDs 30..34: MODE_UNIFORM, MODE_GRADIENT_L2R, MODE_GRADIENT_R2L, MODE_GRADIENT_T2B, MODE_GRADIENT_B2T
  const uint32_t s_uniform = (m_mode == Mode::Uniform) ? 1u : 0u;
  const uint32_t s_l2r =
    (m_mode == Mode::Gradient && m_orientation == GradientOrientation::LeftToRight) ? 1u : 0u;
  const uint32_t s_r2l =
    (m_mode == Mode::Gradient && m_orientation == GradientOrientation::RightToLeft) ? 1u : 0u;
  const uint32_t s_t2b =
    (m_mode == Mode::Gradient && m_orientation == GradientOrientation::TopToBottom) ? 1u : 0u;
  const uint32_t s_b2t =
    (m_mode == Mode::Gradient && m_orientation == GradientOrientation::BottomToTop) ? 1u : 0u;

  std::array<vk::SpecializationMapEntry, 5> entries = {
    vk::SpecializationMapEntry{.constantID = 30, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 31, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 32, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 33, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 34, .offset = 4 * sizeof(uint32_t), .size = sizeof(uint32_t)},
  };
  std::array<uint32_t, 5> data = {s_uniform, s_l2r, s_r2l, s_t2b, s_b2t};
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
  m_shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                       std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
                                       std::vector<uint8_t>(bytes, bytes + sizeof(data)));

  // Vertex input for fullscreen triangle
  auto vi = fullscreenTriVertexInput();
  m_pipeline = device().createPipeline(*m_shader, vi, vk::PrimitiveTopology::eTriangleList);

  // Push constants range for fragment stage
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(BackgroundPC))};
  m_pipeline->setPushConstantRanges({pcRange});
  // No descriptor sets needed
  m_pipeline->setDescriptorSetLayouts({});
  m_pipeline->create();

  ensureVertexBuffer();
}

void ZVulkanBackgroundRenderer::recordRender(Z3DEye eye, vk::raii::CommandBuffer& cmd)
{
  (void)eye;

  if (!m_pipeline) {
    compile();
  }
  ensureVertexBuffer();

  // Bind pipeline
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->pipeline());

  // Set dynamic viewport/scissor
  const auto extent = framebufferExtent();
  vk::Viewport vp{.x = 0.0f,
                  .y = 0.0f,
                  .width = static_cast<float>(extent.x),
                  .height = static_cast<float>(extent.y),
                  .minDepth = 0.0f,
                  .maxDepth = 1.0f};
  vk::Rect2D scissor{{0, 0}, {extent.x, extent.y}};
  cmd.setViewport(0, vp);
  cmd.setScissor(0, scissor);

  // Push constants
  BackgroundPC pc{};
  pc.screen_dim_RCP = m_screenDimRCP;
  pc.color1 = m_color1;
  pc.color2 = m_color2;
  pc.region = m_region;
  cmd.pushConstants<BackgroundPC>(m_pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pc);

  // Bind vertex buffer and draw
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
  cmd.draw(3, 1, 0, 0);
}

} // namespace nim

