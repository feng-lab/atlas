#include "zvulkanshader.h"

#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <fstream>

namespace nim {

namespace {
static std::vector<uint32_t> readSpirvFile(const std::string& path)
{
  VLOG(2) << "ZVulkanShader: attempting to open SPIR-V file: " << path;
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file) {
    throw ZException(fmt::format("Failed to open SPIR-V file: {}", path));
  }
  const size_t fileSize = static_cast<size_t>(file.tellg());
  if (fileSize % 4 != 0) {
    throw ZException(fmt::format("Invalid SPIR-V size (must be multiple of 4): {}", path));
  }
  std::vector<uint32_t> buffer(fileSize / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
  return buffer;
}
} // namespace

ZVulkanShader::ZVulkanShader(ZVulkanDevice& device)
  : m_device(device)
{}

ZVulkanShader::ZVulkanShader(ZVulkanDevice& device,
                             const std::string& vertexSpvPath,
                             const std::string& fragmentSpvPath,
                             const std::optional<std::string>& geometrySpvPath)
  : m_device(device)
{
  loadFromSPIRVFiles(vertexSpvPath, fragmentSpvPath, geometrySpvPath);
}

vk::raii::ShaderModule ZVulkanShader::createShaderModule(const std::vector<uint32_t>& spirv) const
{
  vk::ShaderModuleCreateInfo createInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()};
  return vk::raii::ShaderModule(m_device.context().device(), createInfo);
}

void ZVulkanShader::addStageFromSPIRVFile(const std::string& spvPath, vk::ShaderStageFlagBits stage)
{
  VLOG(2) << "ZVulkanShader: addStageFromSPIRVFile stage=" << static_cast<int>(stage) << " path=" << spvPath;
  auto spirv = readSpirvFile(spvPath);
  addStageFromSPIRV(spirv, stage);
}

void ZVulkanShader::addStageFromSPIRV(const std::vector<uint32_t>& spirv, vk::ShaderStageFlagBits stage)
{
  switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
      m_vertModule.emplace(createShaderModule(spirv));
      break;
    case vk::ShaderStageFlagBits::eFragment:
      m_fragModule.emplace(createShaderModule(spirv));
      break;
    case vk::ShaderStageFlagBits::eGeometry:
      m_geomModule.emplace(createShaderModule(spirv));
      break;
    default:
      throw ZException("Unsupported shader stage for SPIR-V load");
  }
  VLOG(2) << "ZVulkanShader: created module for stage=" << static_cast<int>(stage) << " words=" << spirv.size()
          << " bytes=" << (spirv.size() * sizeof(uint32_t));

  m_shaderStages.clear();
  if (m_vertModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eVertex,
                                        .module = **m_vertModule,
                                        .pName = "main"};
    if (m_vertSpecInfo) {
      s.pSpecializationInfo = &*m_vertSpecInfo;
    }
    m_shaderStages.push_back(s);
  }
  if (m_geomModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eGeometry,
                                        .module = **m_geomModule,
                                        .pName = "main"};
    if (m_geomSpecInfo) {
      s.pSpecializationInfo = &*m_geomSpecInfo;
    }
    m_shaderStages.push_back(s);
  }
  if (m_fragModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eFragment,
                                        .module = **m_fragModule,
                                        .pName = "main"};
    if (m_fragSpecInfo) {
      s.pSpecializationInfo = &*m_fragSpecInfo;
    }
    m_shaderStages.push_back(s);
  }
}

void ZVulkanShader::loadFromSPIRVFiles(const std::string& vertexSpvPath,
                                       const std::string& fragmentSpvPath,
                                       const std::optional<std::string>& geometrySpvPath)
{
  VLOG(2) << "ZVulkanShader: loadFromSPIRVFiles vert=" << vertexSpvPath
          << (geometrySpvPath ? ", geom=" + *geometrySpvPath : std::string{}) << ", frag=" << fragmentSpvPath;
  addStageFromSPIRVFile(vertexSpvPath, vk::ShaderStageFlagBits::eVertex);
  if (geometrySpvPath && !geometrySpvPath->empty()) {
    addStageFromSPIRVFile(*geometrySpvPath, vk::ShaderStageFlagBits::eGeometry);
  }
  addStageFromSPIRVFile(fragmentSpvPath, vk::ShaderStageFlagBits::eFragment);
  LOG(INFO) << "Loaded SPIR-V shader modules: vert=" << vertexSpvPath
            << (geometrySpvPath ? ", geom=" + *geometrySpvPath : std::string{}) << ", frag=" << fragmentSpvPath;
}

void ZVulkanShader::setSpecializationConstants(vk::ShaderStageFlagBits stage,
                                               const std::vector<vk::SpecializationMapEntry>& entries,
                                               const std::vector<uint8_t>& data)
{
  switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
      m_vertSpecEntries = entries;
      m_vertSpecData = data;
      m_vertSpecInfo.emplace(static_cast<uint32_t>(m_vertSpecEntries.size()),
                             m_vertSpecEntries.data(),
                             m_vertSpecData.size(),
                             m_vertSpecData.data());
      break;
    case vk::ShaderStageFlagBits::eGeometry:
      m_geomSpecEntries = entries;
      m_geomSpecData = data;
      m_geomSpecInfo.emplace(static_cast<uint32_t>(m_geomSpecEntries.size()),
                             m_geomSpecEntries.data(),
                             m_geomSpecData.size(),
                             m_geomSpecData.data());
      break;
    case vk::ShaderStageFlagBits::eFragment:
      m_fragSpecEntries = entries;
      m_fragSpecData = data;
      m_fragSpecInfo.emplace(static_cast<uint32_t>(m_fragSpecEntries.size()),
                             m_fragSpecEntries.data(),
                             m_fragSpecData.size(),
                             m_fragSpecData.data());
      break;
    default:
      throw ZException("Unsupported shader stage for specialization constants");
  }

  m_shaderStages.clear();
  if (m_vertModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eVertex,
                                        .module = **m_vertModule,
                                        .pName = "main",
                                        .pSpecializationInfo = m_vertSpecInfo ? &*m_vertSpecInfo : nullptr};
    m_shaderStages.push_back(s);
  }
  if (m_geomModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eGeometry,
                                        .module = **m_geomModule,
                                        .pName = "main",
                                        .pSpecializationInfo = m_geomSpecInfo ? &*m_geomSpecInfo : nullptr};
    m_shaderStages.push_back(s);
  }
  if (m_fragModule) {
    vk::PipelineShaderStageCreateInfo s{.stage = vk::ShaderStageFlagBits::eFragment,
                                        .module = **m_fragModule,
                                        .pName = "main",
                                        .pSpecializationInfo = m_fragSpecInfo ? &*m_fragSpecInfo : nullptr};
    m_shaderStages.push_back(s);
  }
}

} // namespace nim
