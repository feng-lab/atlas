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
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
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
  m_initializedMask |= (1ull << binding);
  VLOG(3) << "Updated uniform buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateUniformBufferDynamic(uint32_t binding,
                                                      ZVulkanBuffer& buffer,
                                                      vk::DeviceSize range)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (uniform buffer dynamic, range) at binding "
                   << binding;
    }
  }
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = range};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
                                         .dstBinding = binding,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                         .pImageInfo = nullptr,
                                         .pBufferInfo = &bufferInfo,
                                         .pTexelBufferView = nullptr};
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated uniform buffer dynamic descriptor (range) at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture implicit sampler) at binding " << binding;
    }
    // Enforcement: override descriptor sets are updated during recording and
    // are intended for immediate use. Require that the texture has already been
    // transitioned to the layout declared in its descriptor state; missing
    // resource-usage metadata should fail fast rather than produce undefined
    // sampling results.
    CHECK(texture.layout() == texture.descriptorLayout())
      << "Texture bound while recording is not in its descriptor layout: binding=" << binding
      << " fmt=" << enumOrUnderlying(texture.format(), 16) << " current=" << enumOrUnderlying(texture.layout(), 16)
      << " descriptor=" << enumOrUnderlying(texture.descriptorLayout(), 16);
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
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated texture descriptor (sampler-owned) at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture) at binding " << binding;
    }
    CHECK(texture.layout() == texture.descriptorLayout())
      << "Texture bound while recording is not in its descriptor layout: binding=" << binding
      << " fmt=" << enumOrUnderlying(texture.format(), 16) << " current=" << enumOrUnderlying(texture.layout(), 16)
      << " descriptor=" << enumOrUnderlying(texture.descriptorLayout(), 16);
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
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated texture descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding,
                                         ZVulkanTexture& texture,
                                         vk::Sampler sampler,
                                         vk::ImageLayout layoutOverride,
                                         vk::ImageAspectFlags aspectOverride)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (texture with override) at binding " << binding;
    }
    const vk::ImageLayout effectiveLayout =
      (layoutOverride == vk::ImageLayout::eUndefined) ? texture.descriptorLayout() : layoutOverride;
    CHECK(texture.layout() == effectiveLayout)
      << "Texture bound while recording is not in the requested layout override: binding=" << binding
      << " fmt=" << enumOrUnderlying(texture.format(), 16) << " current=" << enumOrUnderlying(texture.layout(), 16)
      << " requested=" << enumOrUnderlying(effectiveLayout, 16)
      << " descriptor=" << enumOrUnderlying(texture.descriptorLayout(), 16);
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
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated texture descriptor (override) at binding " << binding;
}

bool ZVulkanDescriptorSet::writeUniformBufferOnce(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (((m_initializedMask & (1ull << binding)) != 0ull) && !m_isOverrideTransient) {
    return false;
  }
  updateUniformBuffer(binding, buffer);
  return true;
}

// Removed two-parameter writeUniformBufferDynamicOnce; use the range-aware overload.

bool ZVulkanDescriptorSet::writeUniformBufferDynamicOnce(uint32_t binding,
                                                         ZVulkanBuffer& buffer,
                                                         vk::DeviceSize range)
{
  if (((m_initializedMask & (1ull << binding)) != 0ull) && !m_isOverrideTransient) {
    return false;
  }
  updateUniformBufferDynamic(binding, buffer, range);
  return true;
}

bool ZVulkanDescriptorSet::writeTextureOnce(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  if (((m_initializedMask & (1ull << binding)) != 0ull) && !m_isOverrideTransient) {
    return false;
  }
  updateTexture(binding, texture, sampler);
  return true;
}

bool ZVulkanDescriptorSet::writeStorageBufferOnce(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (((m_initializedMask & (1ull << binding)) != 0ull) && !m_isOverrideTransient) {
    return false;
  }
  updateStorageBuffer(binding, buffer);
  return true;
}

void ZVulkanDescriptorSet::updateStorageBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (storage buffer) at binding " << binding;
    }
  }
  // Invariant: storage buffer descriptors require buffers created with STORAGE_BUFFER usage.
  CHECK(static_cast<bool>(buffer.usage() & vk::BufferUsageFlagBits::eStorageBuffer))
    << "Storage buffer bound at binding " << binding << " was not created with VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
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
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated storage buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateStorageImage(uint32_t binding,
                                              ZVulkanTexture& texture,
                                              vk::ImageLayout layoutOverride,
                                              vk::ImageAspectFlags aspectOverride)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    const bool rewriteAttempt = (!m_isOverrideTransient && alreadyInit);
    backend->notifyDescriptorWriteWhileRecording(rewriteAttempt);
    if (!m_isOverrideTransient) {
      CHECK(false) << "Descriptor write attempted during recording (storage image) at binding " << binding;
    }
    const vk::ImageLayout effectiveLayout =
      (layoutOverride == vk::ImageLayout::eUndefined) ? texture.descriptorLayout() : layoutOverride;
    CHECK(effectiveLayout == vk::ImageLayout::eGeneral)
      << "Storage image descriptor requires VK_IMAGE_LAYOUT_GENERAL: binding=" << binding
      << " requested=" << enumOrUnderlying(effectiveLayout, 16);
    CHECK(texture.layout() == effectiveLayout)
      << "Storage image bound while recording is not in VK_IMAGE_LAYOUT_GENERAL: binding=" << binding
      << " fmt=" << enumOrUnderlying(texture.format(), 16) << " current=" << enumOrUnderlying(texture.layout(), 16);
  }
  // Build image info for storage image (no sampler)
  auto info = texture.descriptorInfo(layoutOverride, aspectOverride);
  info.sampler = vk::Sampler{};
  vk::WriteDescriptorSet descriptorWrite{.dstSet = m_descriptorSet,
                                         .dstBinding = binding,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = vk::DescriptorType::eStorageImage,
                                         .pImageInfo = &info,
                                         .pBufferInfo = nullptr,
                                         .pTexelBufferView = nullptr};
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  m_initializedMask |= (1ull << binding);
  VLOG(2) << "Updated storage image descriptor at binding " << binding;
}

} // namespace nim
