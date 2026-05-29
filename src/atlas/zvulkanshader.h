#pragma once

#include "zvulkan.h"
#include <QString>
#include <optional>
#include <vector>

namespace nim {

class ZVulkanDevice;

class ZVulkanShader
{
public:
  explicit ZVulkanShader(ZVulkanDevice& device);
  ZVulkanShader(ZVulkanDevice& device,
                const QString& vertexSpvPath,
                const QString& fragmentSpvPath,
                const std::optional<QString>& geometrySpvPath = std::nullopt);

  [[nodiscard]] static QString spirvResourcePath(const QString& spvName);
  [[nodiscard]] static std::vector<uint32_t> readSPIRVFile(const QString& spvPath);

  void addStageFromSPIRVFile(const QString& spvPath, vk::ShaderStageFlagBits stage);
  void addStageFromSPIRV(const std::vector<uint32_t>& spirv, vk::ShaderStageFlagBits stage);

  void loadFromSPIRVFiles(const QString& vertexSpvPath,
                          const QString& fragmentSpvPath,
                          const std::optional<QString>& geometrySpvPath = std::nullopt);

  const std::vector<vk::PipelineShaderStageCreateInfo>& shaderStages() const
  {
    return m_shaderStages;
  }

  void setSpecializationConstants(vk::ShaderStageFlagBits stage,
                                  const std::vector<vk::SpecializationMapEntry>& entries,
                                  const std::vector<uint8_t>& data);

private:
  vk::raii::ShaderModule createShaderModule(const std::vector<uint32_t>& spirv) const;

  ZVulkanDevice& m_device;

  std::optional<vk::raii::ShaderModule> m_vertModule;
  std::optional<vk::raii::ShaderModule> m_fragModule;
  std::optional<vk::raii::ShaderModule> m_geomModule;

  std::vector<vk::SpecializationMapEntry> m_vertSpecEntries;
  std::vector<uint8_t> m_vertSpecData;
  std::optional<vk::SpecializationInfo> m_vertSpecInfo;

  std::vector<vk::SpecializationMapEntry> m_geomSpecEntries;
  std::vector<uint8_t> m_geomSpecData;
  std::optional<vk::SpecializationInfo> m_geomSpecInfo;

  std::vector<vk::SpecializationMapEntry> m_fragSpecEntries;
  std::vector<uint8_t> m_fragSpecData;
  std::optional<vk::SpecializationInfo> m_fragSpecInfo;

  std::vector<vk::PipelineShaderStageCreateInfo> m_shaderStages;
};

} // namespace nim
