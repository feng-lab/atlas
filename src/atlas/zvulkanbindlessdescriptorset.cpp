#include "zvulkanbindlessdescriptorset.h"

#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zvulkanbindings.h"

#include <array>

namespace nim {

size_t ZVulkanBindlessDescriptorSet::KeyHash::operator()(const Key& k) const noexcept
{
  // Hash stable identities: texture pointer + descriptor tuple.
  const auto h1 = std::hash<const void*>{}(k.texture);
  const auto h2 = std::hash<uint32_t>{}(static_cast<uint32_t>(k.layout));
  const auto h3 = std::hash<uint32_t>{}(static_cast<uint32_t>(static_cast<VkImageAspectFlags>(k.aspect)));
  // Basic hash combine
  size_t seed = h1;
  seed ^= (h2 + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
  seed ^= (h3 + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
  return seed;
}

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
  CHECK(static_cast<VkDescriptorSet>(m_descriptorSet) != VK_NULL_HANDLE)
    << "ZVulkanBindlessDescriptorSet constructed with a null descriptor set";
}

uint32_t ZVulkanBindlessDescriptorSet::capacity(Kind kind) const
{
  return tableForKind(kind).capacity;
}

uint32_t ZVulkanBindlessDescriptorSet::used(Kind kind) const
{
  return tableForKind(kind).nextIndex;
}

ZVulkanBindlessDescriptorSet::Table ZVulkanBindlessDescriptorSet::makeTable(uint32_t binding, uint32_t capacity)
{
  Table t{};
  t.binding = binding;
  t.capacity = capacity;
  t.nextIndex = 0;
  t.entries.resize(capacity);
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
  Table& table = tableForKind(request.kind);
  return registerInTable(table, request);
}

std::optional<uint32_t> ZVulkanBindlessDescriptorSet::lookupTexture(const RegisterRequest& request) const
{
  const Table& table = tableForKind(request.kind);
  return lookupInTable(table, request);
}

ZVulkanBindlessDescriptorSet::DebugEntryState
ZVulkanBindlessDescriptorSet::debugEntryState(const RegisterRequest& request) const
{
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

  vk::DescriptorImageInfo info = texture.descriptorInfo(request.layoutOverride, request.aspectOverride);
  // Bindless sampled-image tables use VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; sampler
  // state is provided separately via immutable samplers in the set 0 layout.
  info.sampler = vk::Sampler{};

  Key key{};
  key.texture = &texture;
  key.layout = info.imageLayout;
  key.aspect = (request.aspectOverride == vk::ImageAspectFlags{}) ? texture.descriptorAspect() : request.aspectOverride;

  auto it = table.map.find(key);
  if (it != table.map.end()) {
    const uint32_t idx = it->second;
    CHECK(idx < table.entries.size()) << "Bindless table index out of bounds";
    EntryState& entry = table.entries[idx];
    CHECK(entry.valid) << "Bindless table entry missing state (idx=" << idx << ")";

    const bool same = entry.info.imageView == info.imageView && entry.info.imageLayout == info.imageLayout;
    const uint64_t gen = texture.imageGeneration();
    if (same && entry.imageGeneration == gen) {
      return idx;
    }

    // Texture was re-created or descriptor override changed (layout/aspect/sampler): rewrite.
    writeDescriptor(table.binding, idx, info);
    entry.info = info;
    entry.imageGeneration = gen;
    m_generation++;
    return idx;
  }

  if (table.nextIndex >= table.capacity) {
    CHECK(false) << "Bindless " << kindName(request.kind) << " table capacity exceeded: need index " << table.nextIndex
                 << " but capacity is " << table.capacity
                 << ". Increase bindless descriptor capacity flags (see docs/DEVELOPER_GUIDE.md).";
  }

  const uint32_t idx = table.nextIndex++;
  CHECK(idx < table.entries.size()) << "Bindless table entry vector size mismatch";

  // Write descriptor first (so a failed write doesn't mutate the map).
  writeDescriptor(table.binding, idx, info);

  table.map.emplace(key, idx);
  EntryState& entry = table.entries[idx];
  entry.valid = true;
  entry.info = info;
  entry.imageGeneration = texture.imageGeneration();
  m_generation++;
  return idx;
}

std::optional<uint32_t> ZVulkanBindlessDescriptorSet::lookupInTable(const Table& table,
                                                                    const RegisterRequest& request) const
{
  if (request.texture == nullptr) {
    return std::nullopt;
  }
  const ZVulkanTexture& texture = *request.texture;
  vk::DescriptorImageInfo info = texture.descriptorInfo(request.layoutOverride, request.aspectOverride);
  info.sampler = vk::Sampler{};

  Key key{};
  key.texture = &texture;
  key.layout = info.imageLayout;
  key.aspect = (request.aspectOverride == vk::ImageAspectFlags{}) ? texture.descriptorAspect() : request.aspectOverride;

  auto it = table.map.find(key);
  if (it == table.map.end()) {
    return std::nullopt;
  }
  return it->second;
}

void ZVulkanBindlessDescriptorSet::writeDescriptor(uint32_t binding,
                                                   uint32_t index,
                                                   const vk::DescriptorImageInfo& info)
{
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
