#pragma once

#include "zvulkan.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nim {

class ZVulkanDevice;
class ZVulkanTexture;

// Per-frame-slot bindless descriptor table for sampled images.
//
// This is owned by ZVulkanDevice and shared by every renderer backend using the
// same frame-executor slot. The VkDescriptorSet handle remains stable for the
// device lifetime.
//
// Contract:
// - Mutations (vkUpdateDescriptorSets) are allowed only when no backend is
//   recording on the device and after the slot's previous submission fence is
//   complete (frame completion safe point).
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
    // Optional debug label for crash diagnostics
    std::string_view debugLabel{};
  };

  struct EntryHandle
  {
    Kind kind = Kind::Texture2D;
    uint32_t index = 0u;
    const ZVulkanTexture* address = nullptr;
    uint64_t identity = 0u;
  };

  static constexpr size_t kKindCount = 5u;
  using PlaceholderDescriptorInfos = std::array<vk::DescriptorImageInfo, kKindCount>;

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

  [[nodiscard]] vk::DescriptorSet descriptorSet() const;

  // Updating a legacy descriptor binding invalidates every executable command
  // buffer that bound the set. Update-after-bind descriptors do not, so their
  // command-buffer compatibility token is intentionally constant.
  [[nodiscard]] uint64_t commandBufferCompatibilityGeneration() const;

  [[nodiscard]] uint32_t capacity(Kind kind) const;
  [[nodiscard]] uint32_t used(Kind kind) const;

  // Start the registration epoch for a newly acquired executor slot. Existing
  // entries remain cached, but only entries touched in this epoch may be looked
  // up while recording; untouched entries are eligible for cold eviction.
  void beginRegistrationEpoch();

  // Mutating API: allocate an index (if needed) and write the descriptor.
  // Requires: request.texture != nullptr and the caller has ensured it is safe
  // to update descriptors for this slot.
  [[nodiscard]] uint32_t registerTexture(const RegisterRequest& request);

  // Lookup-only API: returns the previously assigned index for the request,
  // or std::nullopt if missing.
  [[nodiscard]] std::optional<uint32_t> lookupTexture(const RegisterRequest& request) const;

  // Direct completion-safe reclamation used by ZVulkanDevice. Destruction
  // captures exact handles with O(1) per-kind lookups; the slot drain validates
  // address + stable identity, performs one batched descriptor update, then
  // releases every exact index.
  [[nodiscard]] std::optional<EntryHandle>
  retirementHandle(Kind kind, const ZVulkanTexture* textureAddress, uint64_t textureIdentity) const;
  void retireEntries(std::span<const EntryHandle> handles, const PlaceholderDescriptorInfos& placeholderInfos);

  // Debug: retrieve the currently cached descriptor entry state for the request.
  // Intended for diagnosing stale bindless entries when textures are recreated.
  [[nodiscard]] DebugEntryState debugEntryState(const RegisterRequest& request) const;

private:
  struct EntryState
  {
    bool valid = false;
    const ZVulkanTexture* texture = nullptr;
    vk::DescriptorImageInfo info{};
    uint64_t textureIdentity = 0u;
    uint64_t imageGeneration = 0;
    uint64_t lastTouchedEpoch = 0u;
  };

  struct Table
  {
    uint32_t binding = 0;
    uint32_t capacity = 0;
    uint32_t nextIndex = 0;
    std::unordered_map<const ZVulkanTexture*, uint32_t> map;
    std::vector<EntryState> entries;
    std::vector<uint32_t> freeIndices;
  };

  [[nodiscard]] static Table makeTable(uint32_t binding, uint32_t capacity);

  [[nodiscard]] Table& tableForKind(Kind kind);
  [[nodiscard]] const Table& tableForKind(Kind kind) const;
  [[nodiscard]] static size_t indexForKind(Kind kind);
  [[nodiscard]] static const char* kindName(Kind kind);

  [[nodiscard]] uint32_t registerInTable(Table& table, const RegisterRequest& request);
  [[nodiscard]] std::optional<uint32_t> lookupInTable(const Table& table, const RegisterRequest& request) const;

  void writeDescriptor(uint32_t binding, uint32_t index, const vk::DescriptorImageInfo& info);

  ZVulkanDevice& m_device;
  vk::DescriptorSet m_descriptorSet{};
  uint64_t m_descriptorMutationGeneration = 0;
  uint64_t m_registrationEpoch = 0;
  std::vector<EntryHandle> m_validRetirementHandles;
  std::vector<vk::WriteDescriptorSet> m_retirementWrites;

  Table m_texture2D;
  Table m_texture2DArray;
  Table m_texture3D;
  Table m_uTexture2D;
  Table m_uTexture3D;
};

} // namespace nim
