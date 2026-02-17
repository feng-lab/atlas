#pragma once

#include "zvulkan.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nim {

class ZVulkanDevice;
class ZVulkanTexture;

// Per-frame-slot bindless descriptor table for sampled images.
//
// This is intentionally owned by the Vulkan backend's FrameResources so the
// VkDescriptorSet handle remains stable for the lifetime of a frame-executor
// slot (ActiveFrame::key()).
//
// Contract:
// - Mutations (vkUpdateDescriptorSets) are only allowed when the backend is NOT
//   recording commands for the slot, and only after the slot's previous
//   submission fence is complete (frame completion safe point).
// - Lookups are allowed during recording but must not allocate new indices.
class ZVulkanBindlessDescriptorSet final
{
public:
  enum class Kind : uint8_t
  {
    Texture2D,
    Texture2DArray,
    Texture3D,
    UTexture2D,
    UTexture3D
  };

  struct RegisterRequest
  {
    Kind kind = Kind::Texture2D;
    ZVulkanTexture* texture = nullptr;
    vk::ImageLayout layoutOverride{vk::ImageLayout::eUndefined};
    vk::ImageAspectFlags aspectOverride{};
    // Optional debug label for crash diagnostics
    std::string_view debugLabel{};
  };

  struct DebugEntryState
  {
    bool found = false;
    uint32_t index = 0;
    bool entryValid = false;
    vk::DescriptorImageInfo entryInfo{};
    uint64_t entryImageGeneration = 0;
  };

  ZVulkanBindlessDescriptorSet(ZVulkanDevice& device,
                               vk::DescriptorSet descriptorSet,
                               uint32_t texture2DCapacity,
                               uint32_t texture2DArrayCapacity,
                               uint32_t texture3DCapacity,
                               uint32_t uTexture2DCapacity,
                               uint32_t uTexture3DCapacity);

  vk::DescriptorSet descriptorSet() const
  {
    return m_descriptorSet;
  }

  [[nodiscard]] uint64_t generation() const
  {
    return m_generation;
  }

  [[nodiscard]] uint32_t capacity(Kind kind) const;
  [[nodiscard]] uint32_t used(Kind kind) const;

  // Mutating API: allocate an index (if needed) and write the descriptor.
  // Requires: request.texture != nullptr and the caller has ensured it is safe
  // to update descriptors for this slot.
  [[nodiscard]] uint32_t registerTexture(const RegisterRequest& request);

  // Lookup-only API: returns the previously assigned index for the request,
  // or std::nullopt if missing.
  [[nodiscard]] std::optional<uint32_t> lookupTexture(const RegisterRequest& request) const;

  // Debug: retrieve the currently cached descriptor entry state for the request.
  // Intended for diagnosing stale bindless entries when textures are recreated.
  [[nodiscard]] DebugEntryState debugEntryState(const RegisterRequest& request) const;

private:
  struct Key
  {
    const ZVulkanTexture* texture = nullptr;
    vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    vk::ImageAspectFlags aspect{};

    bool operator==(const Key& o) const noexcept
    {
      return texture == o.texture && layout == o.layout && aspect == o.aspect;
    }
  };

  struct KeyHash
  {
    size_t operator()(const Key& k) const noexcept;
  };

  struct EntryState
  {
    bool valid = false;
    vk::DescriptorImageInfo info{};
    uint64_t imageGeneration = 0;
  };

  struct Table
  {
    uint32_t binding = 0;
    uint32_t capacity = 0;
    uint32_t nextIndex = 0;
    std::unordered_map<Key, uint32_t, KeyHash> map;
    std::vector<EntryState> entries;
  };

  [[nodiscard]] static Table makeTable(uint32_t binding, uint32_t capacity);

  [[nodiscard]] Table& tableForKind(Kind kind);
  [[nodiscard]] const Table& tableForKind(Kind kind) const;
  [[nodiscard]] static const char* kindName(Kind kind);

  [[nodiscard]] uint32_t registerInTable(Table& table, const RegisterRequest& request);
  [[nodiscard]] std::optional<uint32_t> lookupInTable(const Table& table, const RegisterRequest& request) const;

  void writeDescriptor(uint32_t binding, uint32_t index, const vk::DescriptorImageInfo& info);

  ZVulkanDevice& m_device;
  vk::DescriptorSet m_descriptorSet{};
  uint64_t m_generation = 0;

  Table m_texture2D;
  Table m_texture2DArray;
  Table m_texture3D;
  Table m_uTexture2D;
  Table m_uTexture3D;
};

} // namespace nim
