#include "zvulkandescriptorset.h"
#include "zvulkandevice.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <vector>

namespace nim {

ZVulkanDescriptorSet::ZVulkanDescriptorSet(ZVulkanDevice& device,
                                           vk::DescriptorSet descriptorSet,
                                           bool isOverrideTransient)
  : m_device(device)
  , m_descriptorSet(descriptorSet)
  , m_isOverrideTransient(isOverrideTransient)
{}

void ZVulkanDescriptorSet::updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = m_initializedBindings.find(binding) != m_initializedBindings.end();
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (uniform buffer) at binding " << binding;
    }
  }
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = buffer.size()};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
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
  m_initializedBindings.insert(binding);
  VLOG(3) << "Updated uniform buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = m_initializedBindings.find(binding) != m_initializedBindings.end();
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture implicit sampler) at binding " << binding;
    }
  }
  auto imageInfo = texture.descriptorInfo();
  if (imageInfo.sampler == vk::Sampler{}) {
    throw ZException("Texture descriptor requires a valid sampler");
  }

  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
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
  m_initializedBindings.insert(binding);
  VLOG(2) << "Updated texture descriptor (sampler-owned) at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = m_initializedBindings.find(binding) != m_initializedBindings.end();
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture) at binding " << binding;
    }
  }
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format(
      "DS update b={} set=0x{:x} img=0x{:x} fmt={} texLayout={} texDescrLayout={} (sampler provided)",
      binding,
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(m_descriptorSet)),
      reinterpret_cast<uintptr_t>(static_cast<VkImage>(texture.image())),
      enumOrUnderlying(texture.format(), 16),
      enumOrUnderlying(texture.layout(), 16),
      enumOrUnderlying(texture.descriptorLayout(), 16));
  }
  auto imageInfo = texture.descriptorInfo();
  imageInfo.sampler = sampler;
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
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
  m_initializedBindings.insert(binding);
  VLOG(2) << "Updated texture descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding,
                                         ZVulkanTexture& texture,
                                         vk::Sampler sampler,
                                         vk::ImageLayout layoutOverride,
                                         vk::ImageAspectFlags aspectOverride)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = m_initializedBindings.find(binding) != m_initializedBindings.end();
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture with override) at binding " << binding;
    }
  }
  if (layoutOverride == vk::ImageLayout::eDepthReadOnlyOptimal ||
      layoutOverride == vk::ImageLayout::eDepthAttachmentOptimal ||
      layoutOverride == vk::ImageLayout::eDepthStencilAttachmentOptimal ||
      layoutOverride == vk::ImageLayout::eDepthStencilReadOnlyOptimal) {
    // Depth-oriented layouts must not be used with color formats.
    CHECK(texture.descriptorAspect() != vk::ImageAspectFlagBits::eColor)
      << "Attempting to bind color texture with depth/stencil layout at binding " << binding
      << ", fmt=" << enumOrUnderlying(texture.format(), 16)
      << ", layoutOverride=" << enumOrUnderlying(layoutOverride, 16);
  }
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format(
      "DS update (override) b={} set=0x{:x} img=0x{:x} fmt={} layoutOverride={} aspectOverride=0x{:x} texLayout={} texDescrLayout={}",
      binding,
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(m_descriptorSet)),
      reinterpret_cast<uintptr_t>(static_cast<VkImage>(texture.image())),
      enumOrUnderlying(texture.format(), 16),
      enumOrUnderlying(layoutOverride, 16),
      static_cast<unsigned int>(static_cast<uint32_t>(aspectOverride)),
      enumOrUnderlying(texture.layout(), 16),
      enumOrUnderlying(texture.descriptorLayout(), 16));
  }
  auto imageInfo = texture.descriptorInfo(layoutOverride, aspectOverride);
  if (imageInfo.sampler == vk::Sampler{}) {
    imageInfo.sampler = sampler;
  } else {
    imageInfo.sampler = sampler;
  }
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
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
  m_initializedBindings.insert(binding);
  VLOG(2) << "Updated texture descriptor (override) at binding " << binding;
}

bool ZVulkanDescriptorSet::writeUniformBufferOnce(uint32_t binding, ZVulkanBuffer& buffer)
{
  if ((m_initializedBindings.find(binding) != m_initializedBindings.end()) && !m_isOverrideTransient) {
    return false;
  }
  updateUniformBuffer(binding, buffer);
  return true;
}

bool ZVulkanDescriptorSet::writeTextureOnce(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  if ((m_initializedBindings.find(binding) != m_initializedBindings.end()) && !m_isOverrideTransient) {
    return false;
  }
  updateTexture(binding, texture, sampler);
  return true;
}

void ZVulkanDescriptorSet::updateStorageBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = m_initializedBindings.find(binding) != m_initializedBindings.end();
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (storage buffer) at binding " << binding;
    }
  }
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = buffer.size()};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
                                         .dstBinding = binding,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                                         .pImageInfo = nullptr,
                                         .pBufferInfo = &bufferInfo,
                                         .pTexelBufferView = nullptr};
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  m_initializedBindings.insert(binding);
  VLOG(2) << "Updated storage buffer descriptor at binding " << binding;
}

} // namespace nim
