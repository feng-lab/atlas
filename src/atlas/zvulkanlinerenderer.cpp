#include "zvulkanlinerenderer.h"

#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkanrendererbase.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zsysteminfo.h"
#include "zlog.h"

#include <cstddef>
#include <cmath>

namespace nim {

namespace {
struct LightingUBOStd140 {
  alignas(4) int lighting_enabled; // bool as int
  alignas(4) int numLights;
  alignas(8) glm::vec2 _pad0;
  alignas(16) glm::vec3 fog_color_top; float fog_end;
  alignas(16) glm::vec3 fog_color_bottom; float fog_scale;
  alignas(8) float fog_density_log2e; float fog_density_density_log2e;
  alignas(8) glm::vec2 screen_dim_RCP; glm::vec2 _pad1;
  struct LightSource {
    glm::vec4 position;
    glm::vec4 ambient;
    glm::vec4 diffuse;
    glm::vec4 specular;
    glm::vec4 attenuation_spare; // attenuation.xyz + pad
    glm::vec4 spotCutoffExponent_pad; // cutoff, exponent, pad
    glm::vec4 spotDirection_pad; // direction.xyz + pad
  } lights[16];
};

struct TransformsUBOStd140 {
  glm::mat4 projection_view_matrix;
  glm::mat4 view_matrix;
  glm::mat4 pos_transform;
  glm::mat4 pos_transform_normal_matrix; // over-provision for std140 mat3
};

struct MaterialUBOStd140 {
  glm::vec4 scene_ambient;
  glm::vec4 material_ambient;
  glm::vec4 material_specular;
  float material_shininess;
  float alpha;
  int use_custom_color;
  float _pad0;
  glm::vec4 custom_color;
};
} // namespace

ZVulkanLineRenderer::ZVulkanLineRenderer(ZVulkanRendererBase& rendererBase)
  : ZVulkanRenderer(rendererBase)
{
  // default state mirrors GL renderer
  m_useSmoothLine = true;
  m_roundCap = true;
  m_screenAligned = false;
  m_srcLineWidth = 1.0f;
}

ZVulkanLineRenderer::~ZVulkanLineRenderer() = default;

void ZVulkanLineRenderer::setData(std::vector<glm::vec3>* linesInput)
{
  m_linesPt = linesInput;
  m_dirtyCPU = true; m_dirtyGPU = true;
}

void ZVulkanLineRenderer::setDataColors(std::vector<glm::vec4>* colorsInput)
{
  m_lineColorsPt = colorsInput;
  m_dirtyCPU = true; m_dirtyGPU = true;
}

void ZVulkanLineRenderer::setDataPickingColors(std::vector<glm::vec4>* pickColorsInput)
{
  m_linePickingColorsPt = pickColorsInput;
  // no immediate rebuild unless picking pass; same layout as color
}

void ZVulkanLineRenderer::ensureThinVertexBuffer()
{
  if (m_vertexBufferThin) return;
  // thin path is legacy fallback; not emphasized for parity
  struct ThinVertex { glm::vec3 pos; glm::vec4 color; };
  std::vector<ThinVertex> thin;
  if (m_linesPt) {
    thin.reserve(m_linesPt->size());
    for (size_t i = 0; i < m_linesPt->size(); ++i) {
      glm::vec4 c(0,0,0,1);
      if (m_lineColorsPt && i < m_lineColorsPt->size()) c = (*m_lineColorsPt)[i];
      thin.push_back(ThinVertex{(*m_linesPt)[i], c});
    }
  }
  auto& device = m_rendererBase.device();
  m_vertexBufferThin = device.createBuffer(thin.size() * sizeof(ThinVertex),
                                           vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                           vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  if (!thin.empty()) m_vertexBufferThin->copyData(thin.data(), thin.size() * sizeof(ThinVertex));
}

void ZVulkanLineRenderer::rebuildWideCPU()
{
  m_wideVerticesCPU.clear();
  m_indicesCPU.clear();
  if (!m_linesPt || m_linesPt->empty()) return;

  const auto& L = *m_linesPt;
  const std::vector<glm::vec4>* C = m_lineColorsPt;

  auto emitSegment = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& c0, const glm::vec4& c1) {
    // Corner flags consistent with GL path: (0,0),(2,0),(0,2),(2,2) encoded as v = up*16 + right
    float flags[4] = {0.f, 2.f, 32.f, 34.f};
    uint32_t base = static_cast<uint32_t>(m_wideVerticesCPU.size());
    m_wideVerticesCPU.push_back(WideVertex{p0, p1, c0, c1, flags[0], 0});
    m_wideVerticesCPU.push_back(WideVertex{p0, p1, c0, c1, flags[1], 0});
    m_wideVerticesCPU.push_back(WideVertex{p0, p1, c0, c1, flags[2], 0});
    m_wideVerticesCPU.push_back(WideVertex{p0, p1, c0, c1, flags[3], 0});
    // indices 0,1,2, 2,1,3
    static const uint32_t idx[6] = {0,1,2, 2,1,3};
    for (int k = 0; k < 6; ++k) m_indicesCPU.push_back(base + idx[k]);
  };

  auto colorAt = [&](size_t i){ return (C && i < C->size()) ? (*C)[i] : glm::vec4(0,0,0,1); };

  if (m_isLineStrip) {
    for (size_t i = 1; i < L.size(); ++i) {
      emitSegment(L[i-1], L[i], colorAt(i-1), colorAt(i));
    }
  } else {
    for (size_t i = 0; i + 1 < L.size(); i += 2) {
      emitSegment(L[i], L[i+1], colorAt(i), colorAt(i+1));
    }
  }
}

void ZVulkanLineRenderer::ensureWideBuffers()
{
  if (m_dirtyCPU) {
    rebuildWideCPU();
    m_dirtyCPU = false;
  }
  if (!m_vertexBufferWide || !m_indexBufferWide || m_dirtyGPU) {
    auto& device = m_rendererBase.device();
    size_t vsize = m_wideVerticesCPU.size() * sizeof(WideVertex);
    size_t isize = m_indicesCPU.size() * sizeof(uint32_t);
    m_vertexBufferWide = device.createBuffer(std::max<size_t>(vsize, 1),
                                            vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_indexBufferWide = device.createBuffer(std::max<size_t>(isize, 1),
                                           vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                           vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (vsize) m_vertexBufferWide->copyData(m_wideVerticesCPU.data(), vsize);
    if (isize) m_indexBufferWide->copyData(m_indicesCPU.data(), isize);
    m_dirtyGPU = false;
  }
}

void ZVulkanLineRenderer::createDescriptorLayouts()
{
  auto& dev = m_rendererBase.device().context().device();
  // Dummy set 0
  vk::DescriptorSetLayoutCreateInfo d0{};
  m_set0Dummy.emplace(dev, d0);
  // Optional set 0: combined image sampler for 1D texture color
  if (m_useTextureColor) {
    vk::DescriptorSetLayoutBinding t0{.binding = 0,
                                      .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                      .descriptorCount = 1,
                                      .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo set0{.bindingCount = 1, .pBindings = &t0};
    m_set0Texture.emplace(dev, set0);
  } else {
    m_set0Texture.reset();
  }

  // set=1, binding 0: Lighting UBO
  vk::DescriptorSetLayoutBinding l0{.binding = 0,
                                    .descriptorType = vk::DescriptorType::eUniformBuffer,
                                    .descriptorCount = 1,
                                    .stageFlags = vk::ShaderStageFlagBits::eFragment};
  vk::DescriptorSetLayoutCreateInfo set1{.bindingCount = 1, .pBindings = &l0};
  m_set1Lighting.emplace(dev, set1);

  // set=2, binding 0: Transforms; binding 1: Material (VS+FS)
  std::array<vk::DescriptorSetLayoutBinding, 2> b{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eUniformBuffer,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
    vk::DescriptorSetLayoutBinding{.binding = 1,
                                   .descriptorType = vk::DescriptorType::eUniformBuffer,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment}};
  vk::DescriptorSetLayoutCreateInfo set2{.bindingCount = static_cast<uint32_t>(b.size()), .pBindings = b.data()};
  m_set2Transforms.emplace(dev, set2);
}

vk::PipelineVertexInputStateCreateInfo ZVulkanLineRenderer::viThin()
{
  // Thin vertex: {vec3 pos, vec4 color} @ locations 0,1
  static vk::VertexInputBindingDescription binding{.binding = 0, .stride = sizeof(glm::vec3) + sizeof(glm::vec4), .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = static_cast<uint32_t>(sizeof(glm::vec3))}};
  static vk::PipelineVertexInputStateCreateInfo vi{};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  vi.pVertexAttributeDescriptions = attrs.data();
  return vi;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanLineRenderer::viWide()
{
  static vk::VertexInputBindingDescription binding{.binding = 0, .stride = sizeof(WideVertex), .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = static_cast<uint32_t>(offsetof(WideVertex, p1))},
    vk::VertexInputAttributeDescription{.location = 2, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = static_cast<uint32_t>(offsetof(WideVertex, c0))},
    vk::VertexInputAttributeDescription{.location = 3, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = static_cast<uint32_t>(offsetof(WideVertex, c1))},
    vk::VertexInputAttributeDescription{.location = 4, .binding = 0, .format = vk::Format::eR32Sfloat, .offset = static_cast<uint32_t>(offsetof(WideVertex, flags))}};
  static vk::PipelineVertexInputStateCreateInfo vi{};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  vi.pVertexAttributeDescriptions = attrs.data();
  return vi;
}

void ZVulkanLineRenderer::createPipelines()
{
  if (m_pipelineWide && m_pipelineThin && !m_dirtyPipeline) return;

  createDescriptorLayouts();

  auto& device = m_rendererBase.device();
  const QString baseQ = ZSystemInfo::resourcesDirPath() + "/shader/vulkan/spv/";
  const std::string base = baseQ.toStdString();

  // Wide pipeline (triangle list)
  m_shaderWide = std::make_unique<ZVulkanShader>(device, base + "wideline1.vert.spv", base + "wideline.frag.spv", std::nullopt);
  // Specialization for round cap and lighting
  uint32_t useTex = m_useTextureColor ? 1u : 0u;
  uint32_t roundCap = m_roundCap ? 1u : 0u;
  uint32_t lighting = needLighting() ? 1u : 0u;
  std::array<vk::SpecializationMapEntry, 3> specEntries{
    vk::SpecializationMapEntry{.constantID = 98, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 99, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 100, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
  std::array<uint32_t, 3> specData{useTex, roundCap, lighting};
  m_shaderWide->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                           std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
                                           std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                                                reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));
  // Vertex stage: screen-aligned specialization
  vk::SpecializationMapEntry vEntry{.constantID = 101, .offset = 0, .size = sizeof(uint32_t)};
  uint32_t vSA = m_screenAligned ? 1u : 0u;
  m_shaderWide->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex,
                                           {vEntry},
                                           std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&vSA),
                                                                reinterpret_cast<const uint8_t*>(&vSA) + sizeof(uint32_t)));

  auto viW = viWide();
  m_pipelineWide = device.createPipeline(*m_shaderWide, viW, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> setLayoutsW = {m_set0Texture ? **m_set0Texture : **m_set0Dummy,
                                                      **m_set1Lighting,
                                                      **m_set2Transforms};
  // Push constants for wideline
  vk::PushConstantRange pcW{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                            .offset = 0,
                            .size = static_cast<uint32_t>(sizeof(glm::mat4) * 2 + sizeof(float) * 2)};
  m_pipelineWide->setDescriptorSetLayouts(setLayoutsW);
  m_pipelineWide->setPushConstantRanges({pcW});
  m_pipelineWide->create();

  // Wide picking pipeline (lighting disabled)
  m_shaderWidePick = std::make_unique<ZVulkanShader>(device, base + "wideline1.vert.spv", base + "wideline.frag.spv", std::nullopt);
  uint32_t lightingOff = 0u;
  std::array<uint32_t, 3> specDataPick{useTex, roundCap, lightingOff};
  m_shaderWidePick->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                               std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
                                               std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specDataPick.data()),
                                                                    reinterpret_cast<const uint8_t*>(specDataPick.data()) + sizeof(specDataPick)));
  m_shaderWidePick->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex,
                                               {vEntry},
                                               std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&vSA),
                                                                    reinterpret_cast<const uint8_t*>(&vSA) + sizeof(uint32_t)));
  auto viWP = viWide();
  m_pipelineWidePick = device.createPipeline(*m_shaderWidePick, viWP, vk::PrimitiveTopology::eTriangleList);
  m_pipelineWidePick->setDescriptorSetLayouts(setLayoutsW);
  m_pipelineWidePick->setPushConstantRanges({pcW});
  m_pipelineWidePick->create();

  // Thin line pipeline (optional fallback)
  m_shaderThin = std::make_unique<ZVulkanShader>(device, base + "line.vert.spv", base + "line.frag.spv", std::nullopt);
  auto viT = viThin();
  m_pipelineThin = device.createPipeline(*m_shaderThin, viT, vk::PrimitiveTopology::eLineList);
  std::vector<vk::DescriptorSetLayout> setLayoutsT = {**m_set0Dummy, **m_set1Lighting, **m_set2Transforms};
  m_pipelineThin->setDescriptorSetLayouts(setLayoutsT);
  m_pipelineThin->create();

  m_dirtyPipeline = false;
}
void ZVulkanLineRenderer::uploadUBOs()
{
  auto& device = m_rendererBase.device();
  // Lighting
  if (!m_uboLighting) {
    m_uboLighting = device.createBuffer(sizeof(LightingUBOStd140),
                                        vk::BufferUsageFlagBits::eUniformBuffer,
                                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  LightingUBOStd140 lighting{};
  lighting.lighting_enabled = 0; // disable
  lighting.numLights = 0;
  m_uboLighting->copyData(&lighting, sizeof(lighting));

  // Transforms
  if (!m_uboTransforms) {
    m_uboTransforms = device.createBuffer(sizeof(TransformsUBOStd140),
                                          vk::BufferUsageFlagBits::eUniformBuffer,
                                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  TransformsUBOStd140 xf{};
  auto& cam = m_rendererBase.camera();
  xf.view_matrix = cam.viewMatrix(MonoEye);
  xf.projection_view_matrix = cam.projectionMatrix(MonoEye) * xf.view_matrix;
  xf.pos_transform = m_rendererBase.coordTransform();
  xf.pos_transform_normal_matrix = glm::mat4(1.0f);
  m_uboTransforms->copyData(&xf, sizeof(xf));

  // Material
  if (!m_uboMaterial) {
    m_uboMaterial = device.createBuffer(sizeof(MaterialUBOStd140),
                                        vk::BufferUsageFlagBits::eUniformBuffer,
                                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  MaterialUBOStd140 mat{};
  mat.scene_ambient = glm::vec4(1.0f);
  mat.material_ambient = glm::vec4(1.0f);
  mat.material_specular = glm::vec4(0.0f);
  mat.material_shininess = 32.0f;
  mat.alpha = 1.0f;
  mat.use_custom_color = 0;
  mat.custom_color = glm::vec4(1.0f);
  m_uboMaterial->copyData(&mat, sizeof(mat));
}

void ZVulkanLineRenderer::createDescriptorSets()
{
  if (!m_descPool) m_descPool = m_rendererBase.device().createDescriptorPool();
  auto& dev = m_rendererBase.device();

  // Allocate sets
  auto ds1 = m_descPool->allocateDescriptorSet(**m_set1Lighting);
  auto ds2 = m_descPool->allocateDescriptorSet(**m_set2Transforms);
  m_dsLighting = std::make_unique<ZVulkanDescriptorSet>(dev, std::move(ds1));
  m_dsTransforms = std::make_unique<ZVulkanDescriptorSet>(dev, std::move(ds2));
  if (m_set0Texture) {
    auto ds0 = m_descPool->allocateDescriptorSet(**m_set0Texture);
    m_dsTexture = std::make_unique<ZVulkanDescriptorSet>(dev, std::move(ds0));
  } else {
    m_dsTexture.reset();
  }

  // Update
  m_dsLighting->updateUniformBuffer(0, *m_uboLighting);
  m_dsTransforms->updateUniformBuffer(0, *m_uboTransforms);
  m_dsTransforms->updateUniformBuffer(1, *m_uboMaterial);
  if (m_dsTexture && m_tex1D && m_sampler) {
    m_dsTexture->updateTexture(0, *m_tex1D, **m_sampler);
  }
}

void ZVulkanLineRenderer::compile()
{
  createPipelines();
  uploadUBOs();
  createDescriptorSets();
  if (m_useSmoothLine) ensureWideBuffers(); else ensureThinVertexBuffer();
}

void ZVulkanLineRenderer::render(vk::raii::CommandBuffer& cmd)
{
  createPipelines();
  uploadUBOs();
  createDescriptorSets();

  if (m_useSmoothLine) ensureWideBuffers(); else ensureThinVertexBuffer();

  auto& pipe = m_useSmoothLine ? *m_pipelineWide : *m_pipelineThin;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe.pipeline());

  // Dynamic viewport/scissor
  vk::Viewport vp{.x = 0.0f,
                  .y = 0.0f,
                  .width = static_cast<float>(m_rendererBase.width()),
                  .height = static_cast<float>(m_rendererBase.height()),
                  .minDepth = 0.0f,
                  .maxDepth = 1.0f};
  vk::Rect2D scissor{{0, 0}, {m_rendererBase.width(), m_rendererBase.height()}};
  cmd.setViewport(0, vp);
  cmd.setScissor(0, scissor);

  // Bind descriptor sets at set=1,2
  // Bind descriptor sets at set=0,1,2 (0 optional)
  if (m_dsTexture) {
    std::array<vk::DescriptorSet, 3> sets{m_dsTexture->descriptorSet(), m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipe.pipelineLayout(), 0, sets, {});
  } else {
    std::array<vk::DescriptorSet, 2> sets{m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipe.pipelineLayout(), 1, sets, {});
  }

  if (m_useSmoothLine) {
    if (m_wideVerticesCPU.empty()) return;
    // Push constants
    auto computeWidth = [&](float base){
      // emulate GL's (src-0.9) * sizeScale; MSAA/devicePixelRatio elided for now
      return std::max(1.f, base) - 0.9f;
    };

    vk::DeviceSize offset = 0;
    cmd.bindVertexBuffers(0, {m_vertexBufferWide->buffer()}, {offset});
    cmd.bindIndexBuffer(m_indexBufferWide->buffer(), 0, vk::IndexType::eUint32);

    if (!m_lineWidthArray.empty()) {
      // one draw per segment
      const uint32_t segCount = static_cast<uint32_t>(m_indicesCPU.size() / 6);
      for (uint32_t i = 0; i < segCount && i < m_lineWidthArray.size(); ++i) {
        pushWidePC(cmd, computeWidth(m_lineWidthArray[i]));
        cmd.drawIndexed(6, 1, i * 6, i * 4, 0);
      }
    } else {
      pushWidePC(cmd, computeWidth(m_srcLineWidth));
      cmd.drawIndexed(static_cast<uint32_t>(m_indicesCPU.size()), 1, 0, 0, 0);
    }
  } else {
    if (!m_linesPt || m_linesPt->empty()) return;
    vk::DeviceSize offset = 0;
    cmd.bindVertexBuffers(0, {m_vertexBufferThin->buffer()}, {offset});
    cmd.draw(static_cast<uint32_t>(m_linesPt->size()), 1, 0, 0);
  }
}

void ZVulkanLineRenderer::pushWidePC(vk::raii::CommandBuffer& cmd, float lineWidth)
{
  struct WideLinePC { glm::mat4 viewport_matrix; glm::mat4 viewport_matrix_inverse; float line_width; float size_scale; };
  WideLinePC pc{};
  pc.viewport_matrix = m_rendererBase.viewportMatrix();
  pc.viewport_matrix_inverse = m_rendererBase.viewportMatrixInverse();
  pc.line_width = lineWidth;
  pc.size_scale = m_rendererBase.sizeScale();
  cmd.pushConstants<WideLinePC>(m_pipelineWide->pipelineLayout(),
                                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                0,
                                pc);
}

void ZVulkanLineRenderer::renderPicking(vk::raii::CommandBuffer& cmd)
{
  // Same pipeline; just use picking colors if provided
  if (!m_linesPt || m_linesPt->empty()) return;
  createPipelines();
  uploadUBOs();
  createDescriptorSets();
  ensureWideBuffers();

  auto& pipe = *m_pipelineWidePick;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe.pipeline());

  vk::Viewport vp{.x = 0.0f,
                  .y = 0.0f,
                  .width = static_cast<float>(m_rendererBase.width()),
                  .height = static_cast<float>(m_rendererBase.height()),
                  .minDepth = 0.0f,
                  .maxDepth = 1.0f};
  vk::Rect2D scissor{{0, 0}, {m_rendererBase.width(), m_rendererBase.height()}};
  cmd.setViewport(0, vp);
  cmd.setScissor(0, scissor);

  if (m_dsTexture) {
    std::array<vk::DescriptorSet, 3> sets{m_dsTexture->descriptorSet(), m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipe.pipelineLayout(), 0, sets, {});
  } else {
    std::array<vk::DescriptorSet, 2> sets{m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipe.pipelineLayout(), 1, sets, {});
  }

  // Temporarily upload picking colors if available
  std::vector<WideVertex> backup = m_wideVerticesCPU; // keep original
  if (m_linePickingColorsPt && !backup.empty()) {
    // Build a temp CPU buffer with picking colors on top of the same geometry
    std::vector<WideVertex> temp = backup;
    size_t segCount = temp.size() / 4;
    auto pickColorAt = [&](size_t i){ return (*m_linePickingColorsPt)[i]; };
    auto colorAt = [&](size_t i){ return (m_lineColorsPt && i < m_lineColorsPt->size()) ? (*m_lineColorsPt)[i] : glm::vec4(0,0,0,1); };
    // We must map per-segment endpoint colors; reconstruct from input lines
    if (m_linesPt && m_linePickingColorsPt && (!m_isLineStrip ? (m_linePickingColorsPt->size() >= m_linesPt->size()) : (m_linePickingColorsPt->size() >= m_linesPt->size()))) {
      size_t segIdx = 0;
      if (m_isLineStrip) {
        for (size_t i = 1; i < m_linesPt->size() && segIdx < segCount; ++i, ++segIdx) {
          glm::vec4 c0 = pickColorAt(i-1);
          glm::vec4 c1 = pickColorAt(i);
          for (int v = 0; v < 4; ++v) { temp[segIdx*4 + v].c0 = c0; temp[segIdx*4 + v].c1 = c1; }
        }
      } else {
        for (size_t i = 0; i + 1 < m_linesPt->size() && segIdx < segCount; i += 2, ++segIdx) {
          glm::vec4 c0 = pickColorAt(i);
          glm::vec4 c1 = pickColorAt(i+1);
          for (int v = 0; v < 4; ++v) { temp[segIdx*4 + v].c0 = c0; temp[segIdx*4 + v].c1 = c1; }
        }
      }
    } else {
      // fallback: use existing colors
      (void)colorAt; // suppress unused
    }
    size_t vsize = temp.size() * sizeof(WideVertex);
    if (vsize) m_vertexBufferWide->copyData(temp.data(), vsize);
  }

  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, {m_vertexBufferWide->buffer()}, {offset});
  cmd.bindIndexBuffer(m_indexBufferWide->buffer(), 0, vk::IndexType::eUint32);
  pushWidePC(cmd, std::max(1.f, m_srcLineWidth) - 0.9f);
  cmd.drawIndexed(static_cast<uint32_t>(m_indicesCPU.size()), 1, 0, 0, 0);

  // restore original vertex buffer data if we altered it
  if (!backup.empty() && m_linePickingColorsPt) {
    size_t vsize = backup.size() * sizeof(WideVertex);
    if (vsize) m_vertexBufferWide->copyData(backup.data(), vsize);
  }
}

} // namespace nim
