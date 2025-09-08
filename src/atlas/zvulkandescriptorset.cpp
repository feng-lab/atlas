#include "zvulkandescriptorset.h"
#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zlog.h"

namespace nim {

ZVulkanDescriptorSet::ZVulkanDescriptorSet(ZVulkanDevice& device, vk::raii::DescriptorSet&& descriptorSet)
  : m_device(device)
  , m_descriptorSet(std::move(descriptorSet))
{
  LOG(INFO) << "ZVulkanDescriptorSet created";
}

ZVulkanDescriptorSet::~ZVulkanDescriptorSet()
{
  LOG(INFO) << "Destroying ZVulkanDescriptorSet";
}

void ZVulkanDescriptorSet::updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = buffer.size()};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = *m_descriptorSet,
                                         .dstBinding = binding,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = vk::DescriptorType::eUniformBuffer,
                                         .pImageInfo = nullptr,
                                         .pBufferInfo = &bufferInfo,
                                         .pTexelBufferView = nullptr};
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  LOG(INFO) << "Updated uniform buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  vk::DescriptorImageInfo imageInfo{.sampler = sampler,
                                    .imageView = texture.imageView(),
                                    .imageLayout = texture.layout()};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = *m_descriptorSet,
                                         .dstBinding = binding,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                         .pImageInfo = &imageInfo,
                                         .pBufferInfo = nullptr,
                                         .pTexelBufferView = nullptr};
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  LOG(INFO) << "Updated texture descriptor at binding " << binding;
}

} // namespace nim
