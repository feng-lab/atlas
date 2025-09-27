#pragma once

#include "zvulkanrenderer.h"
#include "zvulkantexture.h"
#include <memory>

namespace nim {

class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanBuffer;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;

class ZVulkanLineRenderer : public ZVulkanRenderer
{
public:
  explicit ZVulkanLineRenderer(Z3DRendererBase& rendererBase);
  ~ZVulkanLineRenderer() override;

  void compile() override;

protected:
  void recordRender(Z3DEye eye, vk::raii::CommandBuffer& cmdBuffer) override;
  void recordPicking(Z3DEye eye, vk::raii::CommandBuffer& cmdBuffer) override;

public:
  // Parity APIs w.r.t. Z3DLineRenderer
  void setLineStrip(bool v) { m_isLineStrip = v; m_dirtyCPU = true; }
  void setUseSmoothLine(bool v) { m_useSmoothLine = v; }
  void setRoundCap(bool v) { m_roundCap = v; m_screenAligned = m_screenAligned && !v; m_dirtyPipeline = true; }
  void setScreenAlign(bool v) { m_screenAligned = v; m_roundCap = m_roundCap && !v; m_dirtyPipeline = true; }
  void setLineWidth(float v) { m_srcLineWidth = std::max(1.f, v); m_dirtyPC = true; }
  void setLineWidth(const std::vector<float>& widths) { m_lineWidthArray = widths; }
  void setData(std::vector<glm::vec3>* linesInput);
  void setDataColors(std::vector<glm::vec4>* colorsInput);
  void setDataPickingColors(std::vector<glm::vec4>* pickColorsInput);
  // Optional 1D texture color (emulated as Wx1 2D texture)
  void setTexture1DColors(const std::vector<glm::vec4>& table);

private:
  void createPipelines();
  void createDescriptorLayouts();
  void createDescriptorSets();
  void uploadUBOs();
  void ensureThinVertexBuffer();
  void ensureWideBuffers();
  void rebuildWideCPU();
  void pushWidePC(vk::raii::CommandBuffer& cmd, float lineWidth);
  vk::PipelineVertexInputStateCreateInfo viThin();
  vk::PipelineVertexInputStateCreateInfo viWide();

  // GPU resources
  std::unique_ptr<ZVulkanShader> m_shaderThin;
  std::unique_ptr<ZVulkanPipeline> m_pipelineThin;   // line list
  std::unique_ptr<ZVulkanPipeline> m_pipelinePoints; // point list (optional)

  std::unique_ptr<ZVulkanShader> m_shaderWide;
  std::unique_ptr<ZVulkanPipeline> m_pipelineWide;   // triangle list (quad expansion)
  std::unique_ptr<ZVulkanShader> m_shaderWidePick;
  std::unique_ptr<ZVulkanPipeline> m_pipelineWidePick; // picking (no lighting)

  std::unique_ptr<ZVulkanBuffer> m_vertexBufferThin;
  std::unique_ptr<ZVulkanBuffer> m_vertexBufferWide;
  std::unique_ptr<ZVulkanBuffer> m_indexBufferWide;

  std::unique_ptr<ZVulkanDescriptorPool> m_descPool;
  // Descriptor set layouts
  std::optional<vk::raii::DescriptorSetLayout> m_set0Dummy;
  std::optional<vk::raii::DescriptorSetLayout> m_set0Texture;
  std::optional<vk::raii::DescriptorSetLayout> m_set1Lighting;
  std::optional<vk::raii::DescriptorSetLayout> m_set2Transforms;

  // Descriptor sets (set = 1, 2)
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTexture; // set=0

  // UBO buffers for set=1 and set=2 (binding 0 and 1)
  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;

  // CPU-side input data
  std::vector<glm::vec3>* m_linesPt = nullptr;
  std::vector<glm::vec4>* m_lineColorsPt = nullptr;
  std::vector<glm::vec4>* m_linePickingColorsPt = nullptr;

  // CPU-side wide-line expansion
  struct WideVertex { glm::vec3 p0; glm::vec3 p1; glm::vec4 c0; glm::vec4 c1; float flags; float _pad; };
  std::vector<WideVertex> m_wideVerticesCPU;
  std::vector<uint32_t> m_indicesCPU;

  // State
  bool m_isLineStrip = false;
  bool m_useSmoothLine = true;
  bool m_roundCap = true;
  bool m_screenAligned = false;
  float m_srcLineWidth = 1.0f;
  std::vector<float> m_lineWidthArray; // optional per-segment widths
  bool m_useTextureColor = false;
  std::unique_ptr<ZVulkanTexture> m_tex1D;
  std::optional<vk::raii::Sampler> m_sampler;

  // dirties
  bool m_dirtyCPU = false;     // CPU wide geometry needs rebuild
  bool m_dirtyGPU = false;     // GPU buffers need upload
  bool m_dirtyPC = true;       // push constants change (line width/size)
  bool m_dirtyPipeline = false;// specialization constants change
};

} // namespace nim
