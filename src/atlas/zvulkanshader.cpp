#include "zvulkanshader.h"

#include "zioutils.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"
#include "zsysteminfo.h"

#include <QDir>

namespace nim {

QString ZVulkanShader::spirvResourcePath(const QString& spvName)
{
  QString relativePath = QStringLiteral("shader/vulkan/spv/");
  relativePath += spvName;
  return QDir(ZSystemInfo::resourcesDirPath()).filePath(relativePath);
}

std::vector<uint32_t> ZVulkanShader::readSPIRVFile(const QString& spvPath)
{
  VLOG(2) << "opening SPIR-V file: " << spvPath;
  std::ifstream file = openIFStream(spvPath, std::ios::ate | std::ios::binary);
  const std::streampos end = file.tellg();
  if (end < std::streampos{0}) {
    throw ZException(fmt::format("Failed to determine SPIR-V file size: {}", spvPath));
  }
  const size_t fileSize = static_cast<size_t>(end);
  if (fileSize % 4 != 0) {
    throw ZException(fmt::format("Invalid SPIR-V size (must be multiple of 4): {}", spvPath));
  }
  std::vector<uint32_t> buffer(fileSize / 4);
  if (!buffer.empty()) {
    file.seekg(0, std::ios::beg);
    readStream(file, buffer.data(), fileSize);
  }
  return buffer;
}

ZVulkanShader::ZVulkanShader(ZVulkanDevice& device)
  : m_device(device)
{}

ZVulkanShader::ZVulkanShader(ZVulkanDevice& device,
                             const QString& vertexSpvPath,
                             const QString& fragmentSpvPath,
                             const std::optional<QString>& geometrySpvPath)
  : m_device(device)
{
  loadFromSPIRVFiles(vertexSpvPath, fragmentSpvPath, geometrySpvPath);
}

vk::raii::ShaderModule ZVulkanShader::createShaderModule(const std::vector<uint32_t>& spirv) const
{
  vk::ShaderModuleCreateInfo createInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()};
  return vk::raii::ShaderModule(m_device.context().device(), createInfo);
}

void ZVulkanShader::addStageFromSPIRVFile(const QString& spvPath, vk::ShaderStageFlagBits stage)
{
  VLOG(2) << "add SPIR-V stage=" << static_cast<int>(stage) << " path=" << spvPath;
  auto spirv = readSPIRVFile(spvPath);
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
  VLOG(2) << "created shader module stage=" << static_cast<int>(stage) << " words=" << spirv.size()
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

void ZVulkanShader::loadFromSPIRVFiles(const QString& vertexSpvPath,
                                       const QString& fragmentSpvPath,
                                       const std::optional<QString>& geometrySpvPath)
{
  VLOG(2) << "load SPIR-V files vert=" << vertexSpvPath
          << (geometrySpvPath ? QStringLiteral(", geom=") + *geometrySpvPath : QString{})
          << ", frag=" << fragmentSpvPath;
  addStageFromSPIRVFile(vertexSpvPath, vk::ShaderStageFlagBits::eVertex);
  if (geometrySpvPath && !geometrySpvPath->isEmpty()) {
    addStageFromSPIRVFile(*geometrySpvPath, vk::ShaderStageFlagBits::eGeometry);
  }
  addStageFromSPIRVFile(fragmentSpvPath, vk::ShaderStageFlagBits::eFragment);
  LOG(INFO) << "Loaded SPIR-V modules: vert=" << vertexSpvPath
            << (geometrySpvPath ? QStringLiteral(", geom=") + *geometrySpvPath : QString{})
            << ", frag=" << fragmentSpvPath;
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
