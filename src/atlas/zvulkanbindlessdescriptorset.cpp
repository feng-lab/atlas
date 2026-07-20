#include "zvulkanbindlessdescriptorset.h"

#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zvulkanbindings.h"

#include <array>
#include <limits>

namespace nim {

ZVulkanBindlessDescriptorSet::ZVulkanBindlessDescriptorSet(ZVulkanDevice& device,
                                                           vk::DescriptorSet descriptorSet,
                                                           uint32_t texture2DCapacity,
                                                           uint32_t texture2DArrayCapacity,
                                                           uint32_t texture3DCapacity,
                                                           uint32_t uTexture2DCapacity,
                                                           uint32_t uTexture3DCapacity)
  : m_device(device)
  , m_descriptorSet(descriptorSet)
  , m_texture2D(makeTable(vkbind::kBindingBindlessTexture2D, texture2DCapacity))
  , m_texture2DArray(makeTable(vkbind::kBindingBindlessTexture2DArray, texture2DArrayCapacity))
  , m_texture3D(makeTable(vkbind::kBindingBindlessTexture3D, texture3DCapacity))
  , m_uTexture2D(makeTable(vkbind::kBindingBindlessUTexture2D, uTexture2DCapacity))
  , m_uTexture3D(makeTable(vkbind::kBindingBindlessUTexture3D, uTexture3DCapacity))
{
  m_device.checkOwnerThread("construct bindless descriptor table");
  CHECK(static_cast<VkDescriptorSet>(m_descriptorSet) != VK_NULL_HANDLE)
    << "ZVulkanBindlessDescriptorSet constructed with a null descriptor set";
  const uint64_t totalCapacity = static_cast<uint64_t>(texture2DCapacity) + texture2DArrayCapacity + texture3DCapacity +
                                 uTexture2DCapacity + uTexture3DCapacity;
  CHECK_LE(totalCapacity, std::numeric_limits<size_t>::max());
  m_validRetirementHandles.reserve(static_cast<size_t>(totalCapacity));
  m_retirementWrites.reserve(static_cast<size_t>(totalCapacity));
}

vk::DescriptorSet ZVulkanBindlessDescriptorSet::descriptorSet() const
{
  m_device.checkOwnerThread("access bindless descriptor-set handle");
  return m_descriptorSet;
}

uint64_t ZVulkanBindlessDescriptorSet::commandBufferCompatibilityGeneration() const
{
  m_device.checkOwnerThread("query bindless command-buffer compatibility generation");
  if (m_device.context().supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
    return 0u;
  }
  return m_descriptorMutationGeneration;
}

uint32_t ZVulkanBindlessDescriptorSet::capacity(Kind kind) const
{
  m_device.checkOwnerThread("query bindless descriptor capacity");
  return tableForKind(kind).capacity;
}

uint32_t ZVulkanBindlessDescriptorSet::used(Kind kind) const
{
  m_device.checkOwnerThread("query bindless descriptor usage");
  const Table& table = tableForKind(kind);
  CHECK_LE(table.freeIndices.size(), static_cast<size_t>(table.nextIndex));
  return table.nextIndex - static_cast<uint32_t>(table.freeIndices.size());
}

void ZVulkanBindlessDescriptorSet::beginRegistrationEpoch()
{
  m_device.checkOwnerThread("begin bindless registration epoch");
  CHECK_LT(m_registrationEpoch, std::numeric_limits<uint64_t>::max())
    << "Bindless descriptor registration epoch exhausted";
  ++m_registrationEpoch;

  const std::array<Table*, 5> tables{&m_texture2D, &m_texture2DArray, &m_texture3D, &m_uTexture2D, &m_uTexture3D};
  for (Table* table : tables) {
    CHECK(table != nullptr);
    CHECK_GT(table->nextIndex, 0u) << "Bindless registration epoch started before placeholder priming";
    CHECK(!table->entries.empty());
    CHECK(table->entries[0].valid) << "Bindless placeholder entry is not valid";
    table->entries[0].lastTouchedEpoch = m_registrationEpoch;
  }
}

ZVulkanBindlessDescriptorSet::Table ZVulkanBindlessDescriptorSet::makeTable(uint32_t binding, uint32_t capacity)
{
  Table t{};
  t.binding = binding;
  t.capacity = capacity;
  t.nextIndex = 0;
  t.map.reserve(capacity);
  t.entries.resize(capacity);
  t.freeIndices.reserve(capacity);
  return t;
}

ZVulkanBindlessDescriptorSet::Table& ZVulkanBindlessDescriptorSet::tableForKind(Kind kind)
{
  switch (kind) {
    case Kind::Texture2D:
      return m_texture2D;
    case Kind::Texture2DArray:
      return m_texture2DArray;
    case Kind::Texture3D:
      return m_texture3D;
    case Kind::UTexture2D:
      return m_uTexture2D;
    case Kind::UTexture3D:
      return m_uTexture3D;
  }
  CHECK(false) << "Unknown bindless kind";
  return m_texture2D;
}

const ZVulkanBindlessDescriptorSet::Table& ZVulkanBindlessDescriptorSet::tableForKind(Kind kind) const
{
  return const_cast<ZVulkanBindlessDescriptorSet*>(this)->tableForKind(kind);
}

size_t ZVulkanBindlessDescriptorSet::indexForKind(Kind kind)
{
  switch (kind) {
    case Kind::Texture2D:
      return 0u;
    case Kind::Texture2DArray:
      return 1u;
    case Kind::Texture3D:
      return 2u;
    case Kind::UTexture2D:
      return 3u;
    case Kind::UTexture3D:
      return 4u;
  }
  CHECK(false) << "Unknown bindless kind";
  return 0u;
}

const char* ZVulkanBindlessDescriptorSet::kindName(Kind kind)
{
  switch (kind) {
    case Kind::Texture2D:
      return "texture2D";
    case Kind::Texture2DArray:
      return "texture2DArray";
    case Kind::Texture3D:
      return "texture3D";
    case Kind::UTexture2D:
      return "utexture2D";
    case Kind::UTexture3D:
      return "utexture3D";
  }
  return "<unknown>";
}

uint32_t ZVulkanBindlessDescriptorSet::registerTexture(const RegisterRequest& request)
{
  m_device.checkOwnerThread("register bindless texture");
  Table& table = tableForKind(request.kind);
  return registerInTable(table, request);
}

std::optional<uint32_t> ZVulkanBindlessDescriptorSet::lookupTexture(const RegisterRequest& request) const
{
  m_device.checkOwnerThread("look up bindless texture");
  const Table& table = tableForKind(request.kind);
  return lookupInTable(table, request);
}

std::optional<ZVulkanBindlessDescriptorSet::EntryHandle>
ZVulkanBindlessDescriptorSet::retirementHandle(Kind kind,
                                               const ZVulkanTexture* textureAddress,
                                               uint64_t textureIdentity) const
{
  m_device.checkOwnerThread("capture bindless retirement handle");
  CHECK(textureAddress != nullptr) << "Bindless retirement lookup requires a non-null texture address";
  CHECK_NE(textureIdentity, 0u) << "Bindless retirement lookup requires a valid texture identity";
  const Table& table = tableForKind(kind);
  const auto it = table.map.find(textureAddress);
  if (it == table.map.end()) {
    return std::nullopt;
  }
  const uint32_t index = it->second;
  CHECK_LT(index, table.entries.size()) << "Bindless retirement lookup index out of bounds";
  const EntryState& entry = table.entries[index];
  CHECK(entry.valid) << "Bindless retirement lookup referenced an invalid entry";
  CHECK_EQ(entry.texture, textureAddress) << "Bindless retirement pointer map is inconsistent";
  if (entry.textureIdentity != textureIdentity) {
    // A newer object can legitimately reuse the same CPU address. Its stable
    // identity prevents an older object's destruction from capturing it.
    return std::nullopt;
  }
  return EntryHandle{.kind = kind, .index = index, .address = textureAddress, .identity = textureIdentity};
}

void ZVulkanBindlessDescriptorSet::retireEntries(std::span<const EntryHandle> handles,
                                                 const PlaceholderDescriptorInfos& placeholderInfos)
{
  m_device.checkOwnerThread("retire bindless descriptor entries");
  if (handles.empty()) {
    return;
  }
  CHECK(m_device.descriptorSetWritesAllowed())
    << "Bindless descriptor retirement attempted while a Vulkan backend is recording commands";
  CHECK(m_validRetirementHandles.empty()) << "Bindless retirement handle scratch reused recursively";
  CHECK(m_retirementWrites.empty()) << "Bindless retirement write scratch reused recursively";
  CHECK_LE(handles.size(), m_validRetirementHandles.capacity())
    << "Bindless retirement batch exceeded descriptor-backed handle scratch capacity";
  CHECK_LE(handles.size(), m_retirementWrites.capacity())
    << "Bindless retirement batch exceeded descriptor-backed scratch capacity";

  for (const EntryHandle& handle : handles) {
    CHECK(handle.address != nullptr) << "Bindless retirement handle has a null texture address";
    CHECK_NE(handle.identity, 0u) << "Bindless retirement handle has no texture identity";
    CHECK_NE(handle.index, 0u) << "Bindless placeholder entry cannot be retired";
    Table& table = tableForKind(handle.kind);
    CHECK_LT(handle.index, table.nextIndex) << "Bindless retirement handle index out of range";
    const EntryState& entry = table.entries[handle.index];
    if (entry.valid) {
      CHECK(entry.texture != nullptr) << "Valid bindless entry has no texture";
      const auto liveMapIt = table.map.find(entry.texture);
      CHECK(liveMapIt != table.map.end() && liveMapIt->second == handle.index)
        << "Bindless entry and pointer map disagree during retirement";
    }
    const bool stillOwnsEntry =
      entry.valid && entry.texture == handle.address && entry.textureIdentity == handle.identity;
    if (!stillOwnsEntry) {
      // A completion-safe rewrite or cold eviction can supersede a queued
      // destruction token. The deferred holder still keeps the old Vulkan
      // handles alive until this drain; it must not sanitize the newer entry.
      continue;
    }

    const vk::DescriptorImageInfo& placeholderInfo = placeholderInfos[indexForKind(handle.kind)];
    CHECK(placeholderInfo.imageView != vk::ImageView{}) << "Bindless retirement placeholder has no image view";
    vk::WriteDescriptorSet write{};
    write.dstSet = m_descriptorSet;
    write.dstBinding = table.binding;
    write.dstArrayElement = handle.index;
    write.descriptorCount = 1u;
    write.descriptorType = vk::DescriptorType::eSampledImage;
    write.pImageInfo = &placeholderInfo;
    m_validRetirementHandles.push_back(handle);
    m_retirementWrites.push_back(write);
  }

  if (m_retirementWrites.empty()) {
    CHECK(m_validRetirementHandles.empty());
    return;
  }

  m_device.context().device().updateDescriptorSets(m_retirementWrites, {});

  for (const EntryHandle& handle : m_validRetirementHandles) {
    Table& table = tableForKind(handle.kind);
    EntryState& entry = table.entries[handle.index];
    const size_t erased = table.map.erase(handle.address);
    CHECK_EQ(erased, 1u) << "Bindless retirement pointer map changed during batch commit";
    CHECK_LT(table.freeIndices.size(), static_cast<size_t>(table.capacity));
    table.freeIndices.push_back(handle.index);
    entry.valid = false;
    entry.texture = nullptr;
    entry.info = placeholderInfos[indexForKind(handle.kind)];
    entry.textureIdentity = 0u;
    entry.imageGeneration = 0u;
    entry.lastTouchedEpoch = 0u;
  }
  m_validRetirementHandles.clear();
  m_retirementWrites.clear();
  CHECK_LT(m_descriptorMutationGeneration, std::numeric_limits<uint64_t>::max());
  ++m_descriptorMutationGeneration;
}

ZVulkanBindlessDescriptorSet::DebugEntryState
ZVulkanBindlessDescriptorSet::debugEntryState(const RegisterRequest& request) const
{
  m_device.checkOwnerThread("inspect bindless descriptor entry");
  DebugEntryState out{};
  if (request.texture == nullptr) {
    return out;
  }

  const Table& table = tableForKind(request.kind);
  const auto idxOpt = lookupInTable(table, request);
  if (!idxOpt.has_value()) {
    return out;
  }

  out.found = true;
  out.index = *idxOpt;
  if (out.index < table.entries.size()) {
    const EntryState& entry = table.entries[out.index];
    out.entryValid = entry.valid;
    out.entryInfo = entry.info;
    out.entryImageGeneration = entry.imageGeneration;
  }
  return out;
}

uint32_t ZVulkanBindlessDescriptorSet::registerInTable(Table& table, const RegisterRequest& request)
{
  CHECK(request.texture != nullptr) << "Bindless registerTexture requires a texture"
                                    << (request.debugLabel.empty() ? "" : " (") << request.debugLabel
                                    << (request.debugLabel.empty() ? "" : ")");
  CHECK(static_cast<VkDescriptorSet>(m_descriptorSet) != VK_NULL_HANDLE) << "Bindless descriptor set missing";

  ZVulkanTexture& texture = *request.texture;
  CHECK(texture.resident()) << "Attempting to register a non-resident Vulkan texture in bindless table"
                            << (request.debugLabel.empty() ? "" : " (") << request.debugLabel
                            << (request.debugLabel.empty() ? "" : ")");
  texture.ensureBindlessRetirementResources();

  vk::DescriptorImageInfo info = texture.descriptorInfo();
  // Bindless sampled-image tables use VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; sampler
  // state is provided separately via immutable samplers in the set 0 layout.
  info.sampler = vk::Sampler{};

  auto it = table.map.find(&texture);
  if (it != table.map.end()) {
    const uint32_t idx = it->second;
    CHECK(idx < table.entries.size()) << "Bindless table index out of bounds";
    EntryState& entry = table.entries[idx];
    CHECK(entry.valid) << "Bindless table entry missing state (idx=" << idx << ")";
    CHECK_EQ(entry.texture, &texture) << "Bindless texture pointer map is inconsistent";

    const bool same = entry.textureIdentity == texture.descriptorIdentity() && entry.info.imageView == info.imageView &&
                      entry.info.imageLayout == info.imageLayout;
    const uint64_t gen = texture.imageGeneration();
    if (same && entry.imageGeneration == gen) {
      entry.lastTouchedEpoch = m_registrationEpoch;
      return idx;
    }

    // The texture object was re-created in place or its canonical descriptor
    // changed. Keep its stable table index and rewrite only the descriptor.
    writeDescriptor(table.binding, idx, info);
    entry.info = info;
    entry.textureIdentity = texture.descriptorIdentity();
    entry.imageGeneration = gen;
    entry.lastTouchedEpoch = m_registrationEpoch;
    CHECK_LT(m_descriptorMutationGeneration, std::numeric_limits<uint64_t>::max());
    ++m_descriptorMutationGeneration;
    return idx;
  }

  const bool reuseRetiredIndex = !table.freeIndices.empty();
  const bool growTable = !reuseRetiredIndex && table.nextIndex < table.capacity;
  uint32_t idx = 0u;
  const ZVulkanTexture* evictedTexture = nullptr;
  if (reuseRetiredIndex) {
    idx = table.freeIndices.back();
    CHECK_NE(idx, 0u) << "Bindless placeholder index entered the retirement free list";
    CHECK_LT(idx, table.entries.size());
    CHECK(!table.entries[idx].valid) << "Bindless free-list entry is still valid";
  } else if (growTable) {
    idx = table.nextIndex;
  } else {
    uint64_t oldestEpoch = std::numeric_limits<uint64_t>::max();
    bool foundColdEntry = false;
    for (uint32_t candidate = 1u; candidate < table.nextIndex; ++candidate) {
      const EntryState& entry = table.entries[candidate];
      if (!entry.valid || entry.lastTouchedEpoch == m_registrationEpoch || entry.lastTouchedEpoch > oldestEpoch) {
        continue;
      }
      idx = candidate;
      oldestEpoch = entry.lastTouchedEpoch;
      foundColdEntry = true;
    }
    CHECK(foundColdEntry) << "Bindless " << kindName(request.kind)
                          << " current-submission working set exceeds table capacity " << table.capacity
                          << ". Increase the corresponding bindless descriptor capacity flag.";
    evictedTexture = table.entries[idx].texture;
    CHECK(evictedTexture != nullptr);
  }

  CHECK(idx < table.entries.size()) << "Bindless table entry vector size mismatch";

  if (evictedTexture != nullptr) {
    // Keep the full-table replacement allocation-free: update the descriptor
    // first, then re-key the existing unordered-map node in place.
    writeDescriptor(table.binding, idx, info);
    auto node = table.map.extract(evictedTexture);
    CHECK(!node.empty()) << "Bindless cold-entry eviction map state was inconsistent";
    CHECK_EQ(node.mapped(), idx);
    node.key() = &texture;
    const auto insertResult = table.map.insert(std::move(node));
    CHECK(insertResult.inserted) << "Bindless cold-entry replacement texture unexpectedly existed";
  } else {
    const bool inserted = table.map.emplace(&texture, idx).second;
    CHECK(inserted) << "Bindless texture unexpectedly existed during index allocation";
    writeDescriptor(table.binding, idx, info);
  }

  if (reuseRetiredIndex) {
    table.freeIndices.pop_back();
  } else if (growTable) {
    ++table.nextIndex;
  }

  EntryState& entry = table.entries[idx];
  entry.valid = true;
  entry.texture = &texture;
  entry.info = info;
  entry.textureIdentity = texture.descriptorIdentity();
  entry.imageGeneration = texture.imageGeneration();
  entry.lastTouchedEpoch = m_registrationEpoch;
  CHECK_LT(m_descriptorMutationGeneration, std::numeric_limits<uint64_t>::max());
  ++m_descriptorMutationGeneration;
  return idx;
}

std::optional<uint32_t> ZVulkanBindlessDescriptorSet::lookupInTable(const Table& table,
                                                                    const RegisterRequest& request) const
{
  if (request.texture == nullptr) {
    return std::nullopt;
  }
  const ZVulkanTexture& texture = *request.texture;
  vk::DescriptorImageInfo info = texture.descriptorInfo();
  info.sampler = vk::Sampler{};

  auto it = table.map.find(&texture);
  if (it == table.map.end()) {
    return std::nullopt;
  }
  const uint32_t index = it->second;
  CHECK_LT(index, table.entries.size()) << "Bindless table lookup index out of bounds";
  const EntryState& entry = table.entries[index];
  if (!entry.valid || entry.texture != &texture || entry.textureIdentity != texture.descriptorIdentity() ||
      entry.imageGeneration != texture.imageGeneration() || entry.info.imageView != info.imageView ||
      entry.info.imageLayout != info.imageLayout || entry.lastTouchedEpoch != m_registrationEpoch) {
    return std::nullopt;
  }
  return index;
}

void ZVulkanBindlessDescriptorSet::writeDescriptor(uint32_t binding,
                                                   uint32_t index,
                                                   const vk::DescriptorImageInfo& info)
{
  CHECK(m_device.descriptorSetWritesAllowed())
    << "Bindless descriptor write attempted while a Vulkan backend is recording commands";
  vk::WriteDescriptorSet write{};
  write.dstSet = m_descriptorSet;
  write.dstBinding = binding;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eSampledImage;
  write.pImageInfo = &info;

  std::array<vk::WriteDescriptorSet, 1> writes{write};
  m_device.context().device().updateDescriptorSets(writes, {});
}

} // namespace nim
