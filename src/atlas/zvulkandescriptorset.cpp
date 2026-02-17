#include "zvulkandescriptorset.h"
#include "zvulkandevice.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zlog.h"

#include <vector>

namespace nim {

ZVulkanDescriptorSet::ZVulkanDescriptorSet(ZVulkanDevice& device, vk::DescriptorSet descriptorSet)
  : m_device(device)
  , m_descriptorSet(descriptorSet)
{}

void ZVulkanDescriptorSet::updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    backend->notifyDescriptorWriteWhileRecording(/*rewriteAttempt*/ alreadyInit);
    CHECK(false) << "Descriptor write attempted during recording (uniform buffer) at binding " << binding;
    return;
  }
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = buffer.size()};
  const uint64_t bit = (1ull << binding);
  if ((m_initializedMask & bit) != 0ull) {
    const BindingState& state = m_bindingStates[binding];
    if (state.kind == BindingState::Kind::Buffer && state.type == vk::DescriptorType::eUniformBuffer &&
        state.bufferInfo.buffer == bufferInfo.buffer && state.bufferInfo.offset == bufferInfo.offset &&
        state.bufferInfo.range == bufferInfo.range) {
      return;
    }
  }
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
  m_initializedMask |= bit;
  BindingState& state = m_bindingStates[binding];
  state.kind = BindingState::Kind::Buffer;
  state.type = vk::DescriptorType::eUniformBuffer;
  state.bufferInfo = bufferInfo;
  m_generation++;
  VLOG(3) << "Updated uniform buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateUniformBufferDynamic(uint32_t binding,
                                                      ZVulkanBuffer& buffer,
                                                      vk::DeviceSize range)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    backend->notifyDescriptorWriteWhileRecording(/*rewriteAttempt*/ alreadyInit);
    CHECK(false) << "Descriptor write attempted during recording (uniform buffer dynamic, range) at binding "
                 << binding;
    return;
  }
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = range};
  const uint64_t bit = (1ull << binding);
  if ((m_initializedMask & bit) != 0ull) {
    const BindingState& state = m_bindingStates[binding];
    if (state.kind == BindingState::Kind::Buffer && state.type == vk::DescriptorType::eUniformBufferDynamic &&
        state.bufferInfo.buffer == bufferInfo.buffer && state.bufferInfo.offset == bufferInfo.offset &&
        state.bufferInfo.range == bufferInfo.range) {
      return;
    }
  }
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
  m_initializedMask |= bit;
  BindingState& state = m_bindingStates[binding];
  state.kind = BindingState::Kind::Buffer;
  state.type = vk::DescriptorType::eUniformBufferDynamic;
  state.bufferInfo = bufferInfo;
  m_generation++;
  VLOG(2) << "Updated uniform buffer dynamic descriptor (range) at binding " << binding;
}

bool ZVulkanDescriptorSet::writeUniformBufferOnce(uint32_t binding, ZVulkanBuffer& buffer)
{
  if ((m_initializedMask & (1ull << binding)) != 0ull) {
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
  if ((m_initializedMask & (1ull << binding)) != 0ull) {
    return false;
  }
  updateUniformBufferDynamic(binding, buffer, range);
  return true;
}

bool ZVulkanDescriptorSet::writeStorageBufferOnce(uint32_t binding, ZVulkanBuffer& buffer)
{
  if ((m_initializedMask & (1ull << binding)) != 0ull) {
    return false;
  }
  updateStorageBuffer(binding, buffer);
  return true;
}

void ZVulkanDescriptorSet::updateStorageBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    backend->notifyDescriptorWriteWhileRecording(/*rewriteAttempt*/ alreadyInit);
    CHECK(false) << "Descriptor write attempted during recording (storage buffer) at binding " << binding;
    return;
  }
  // Invariant: storage buffer descriptors require buffers created with STORAGE_BUFFER usage.
  CHECK(static_cast<bool>(buffer.usage() & vk::BufferUsageFlagBits::eStorageBuffer))
    << "Storage buffer bound at binding " << binding << " was not created with VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
  vk::DescriptorBufferInfo bufferInfo{.buffer = buffer.buffer(), .offset = 0, .range = buffer.size()};
  const uint64_t bit = (1ull << binding);
  if ((m_initializedMask & bit) != 0ull) {
    const BindingState& state = m_bindingStates[binding];
    if (state.kind == BindingState::Kind::Buffer && state.type == vk::DescriptorType::eStorageBuffer &&
        state.bufferInfo.buffer == bufferInfo.buffer && state.bufferInfo.offset == bufferInfo.offset &&
        state.bufferInfo.range == bufferInfo.range) {
      return;
    }
  }
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
  m_initializedMask |= bit;
  BindingState& state = m_bindingStates[binding];
  state.kind = BindingState::Kind::Buffer;
  state.type = vk::DescriptorType::eStorageBuffer;
  state.bufferInfo = bufferInfo;
  m_generation++;
  VLOG(2) << "Updated storage buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateStorageImage(uint32_t binding,
                                              ZVulkanTexture& texture,
                                              vk::ImageLayout layoutOverride,
                                              vk::ImageAspectFlags aspectOverride)
{
  if (auto* backend = Z3DRendererVulkanBackend::current(); backend && backend->isRecording()) {
    const bool alreadyInit = (m_initializedMask & (1ull << binding)) != 0ull;
    backend->notifyDescriptorWriteWhileRecording(/*rewriteAttempt*/ alreadyInit);
    CHECK(false) << "Descriptor write attempted during recording (storage image) at binding " << binding;
    return;
  }
  const vk::ImageLayout effectiveLayout =
    (layoutOverride == vk::ImageLayout::eUndefined) ? texture.descriptorLayout() : layoutOverride;
  CHECK(effectiveLayout == vk::ImageLayout::eGeneral)
    << "Storage image descriptor requires VK_IMAGE_LAYOUT_GENERAL: binding=" << binding
    << " requested=" << enumOrUnderlying(effectiveLayout, 16);
  // Build image info for storage image (no sampler)
  auto info = texture.descriptorInfo(layoutOverride, aspectOverride);
  info.sampler = vk::Sampler{};
  const uint64_t bit = (1ull << binding);
  if ((m_initializedMask & bit) != 0ull) {
    const BindingState& state = m_bindingStates[binding];
    if (state.kind == BindingState::Kind::Image && state.type == vk::DescriptorType::eStorageImage &&
        state.imageInfo.sampler == info.sampler && state.imageInfo.imageView == info.imageView &&
        state.imageInfo.imageLayout == info.imageLayout) {
      return;
    }
  }
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
  m_initializedMask |= bit;
  BindingState& state = m_bindingStates[binding];
  state.kind = BindingState::Kind::Image;
  state.type = vk::DescriptorType::eStorageImage;
  state.imageInfo = info;
  m_generation++;
  VLOG(2) << "Updated storage image descriptor at binding " << binding;
}

} // namespace nim
