#include "z3dscratchresourcepool.h"

#include "z3dgl.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"
#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"
#include "zvulkanresidencymanager.h"
#include "zvulkantexture.h"
#include <glbinding-aux/Meta.h>
#include <folly/ScopeGuard.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace {
using namespace nim;

struct GLTextureParams
{
  GLint internalFormat;
  GLenum format;
  GLenum type;
};

GLenum glTextureTargetFor(ScratchImageDimension dimension)
{
  switch (dimension) {
    case ScratchImageDimension::Tex2D:
      return GL_TEXTURE_2D;
    case ScratchImageDimension::Tex2DArray:
      return GL_TEXTURE_2D_ARRAY;
  }
  CHECK(false) << "Unhandled ScratchImageDimension";
  return GL_TEXTURE_2D;
}

GLTextureParams glTextureParamsFor(ScratchFormat format)
{
  switch (format) {
    case ScratchFormat::RGBA8:
      return {GLint(GL_RGBA8), GL_RGBA, GL_UNSIGNED_BYTE};
    case ScratchFormat::RGBA32UI:
      return {GLint(GL_RGBA32UI), GL_RGBA_INTEGER, GL_UNSIGNED_INT};
    case ScratchFormat::RGBA32F:
      return {GLint(GL_RGBA32F), GL_RGBA, GL_FLOAT};
    case ScratchFormat::RGBA16:
      return {GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT};
    case ScratchFormat::RGBA16F:
      return {GLint(GL_RGBA16F), GL_RGBA, GL_FLOAT};
    case ScratchFormat::RG32F:
      return {GLint(GL_RG32F), GL_RG, GL_FLOAT};
    case ScratchFormat::R32F:
      return {GLint(GL_R32F), GL_RED, GL_FLOAT};
    case ScratchFormat::R16F:
      return {GLint(GL_R16F), GL_RED, GL_FLOAT};
    case ScratchFormat::Depth24:
      return {GLint(GL_DEPTH_COMPONENT24), GL_DEPTH_COMPONENT, GL_UNSIGNED_INT};
    case ScratchFormat::Depth32F:
      return {GLint(GL_DEPTH_COMPONENT32F), GL_DEPTH_COMPONENT, GL_FLOAT};
  }
  CHECK(false) << "Unhandled ScratchFormat value";
  return {GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT};
}

const std::string& scratchFormatLabel(ScratchFormat format)
{
  const auto params = glTextureParamsFor(format);
  return glbinding::aux::Meta::getString(GLenum(params.internalFormat));
}

glm::uvec3 textureExtentFor(const ScratchImageDescriptor& descriptor)
{
  switch (descriptor.dimension) {
    case ScratchImageDimension::Tex2D:
      return glm::uvec3(descriptor.size.x, descriptor.size.y, 1u);
    case ScratchImageDimension::Tex2DArray:
      return glm::uvec3(descriptor.size.x, descriptor.size.y, std::max<uint32_t>(1u, descriptor.layers));
  }
  CHECK(false) << "Unhandled ScratchImageDimension";
  return glm::uvec3(descriptor.size, 1u);
}

void updateSlotDescriptor(ScratchImageDescriptor& slotDescriptor, const ScratchImageDescriptor& descriptor)
{
  slotDescriptor.usage = descriptor.usage;
  slotDescriptor.dimension = descriptor.dimension;
  slotDescriptor.size = descriptor.size;
  slotDescriptor.layers = descriptor.layers;
  slotDescriptor.attachments = descriptor.attachments;
}

std::vector<ScratchAttachmentDesc> makeColorAttachments(uint32_t count, ScratchFormat format)
{
  std::vector<ScratchAttachmentDesc> attachments;
  attachments.reserve(count);
  for (uint32_t idx = 0; idx < count; ++idx) {
    attachments.push_back(ScratchAttachmentDesc{ScratchAttachmentKind::Color, idx, format});
  }
  return attachments;
}

ScratchImageDescriptor makeBlockIdDescriptor(const glm::uvec2& size, uint32_t attachments)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::BlockId;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = makeColorAttachments(attachments, ScratchFormat::RGBA32UI);
  return descriptor;
}

ScratchImageDescriptor makeEntryExitDescriptor(const glm::uvec2& size, uint32_t layers, ScratchFormat format)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::EntryExit;
  descriptor.dimension = ScratchImageDimension::Tex2DArray;
  descriptor.size = size;
  descriptor.layers = layers;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, format}
  };
  return descriptor;
}

ScratchImageDescriptor
makeLayerArrayDescriptor(const glm::uvec2& size, uint32_t layers, ScratchFormat colorFormat, ScratchFormat depthFormat)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::LayerArray;
  descriptor.dimension = ScratchImageDimension::Tex2DArray;
  descriptor.size = size;
  descriptor.layers = layers;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, colorFormat},
    ScratchAttachmentDesc{ScratchAttachmentKind::Depth, 0u, depthFormat}
  };
  return descriptor;
}

ScratchImageDescriptor
makeRaycastAccumulatorDescriptor(const glm::uvec2& size, ScratchFormat colorFormat, ScratchFormat accumulatorFormat)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::RaycastAccumulator;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, colorFormat      },
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 1u, accumulatorFormat}
  };
  return descriptor;
}

ScratchImageDescriptor
makeTemp2DDescriptor(const glm::uvec2& size, ScratchFormat colorFormat, ScratchFormat depthFormat)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::Temp2D;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, colorFormat},
    ScratchAttachmentDesc{ScratchAttachmentKind::Depth, 0u, depthFormat}
  };
  return descriptor;
}

ScratchImageDescriptor makeDualDepthPeelDescriptor(const glm::uvec2& size)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::DualDepthPeel;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, ScratchFormat::RG32F },
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 1u, ScratchFormat::RGBA16},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 2u, ScratchFormat::RGBA16},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 3u, ScratchFormat::RG32F },
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 4u, ScratchFormat::RGBA16},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 5u, ScratchFormat::RGBA16},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 6u, ScratchFormat::RGBA16},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 7u, ScratchFormat::R32F  }
  };
  return descriptor;
}

ScratchImageDescriptor makeWeightedAverageDescriptor(const glm::uvec2& size)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::WeightedAverage;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, ScratchFormat::RGBA32F},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 1u, ScratchFormat::RG32F  }
  };
  return descriptor;
}

ScratchImageDescriptor makeWeightedBlendedDescriptor(const glm::uvec2& size)
{
  ScratchImageDescriptor descriptor;
  descriptor.usage = ScratchImageUsage::WeightedBlended;
  descriptor.dimension = ScratchImageDimension::Tex2D;
  descriptor.size = size;
  descriptor.layers = 1;
  descriptor.attachments = {
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 0u, ScratchFormat::RGBA16F},
    ScratchAttachmentDesc{ScratchAttachmentKind::Color, 1u, ScratchFormat::R16F   }
  };
  return descriptor;
}

vk::Format vkFormatFor(ScratchFormat format)
{
  switch (format) {
    case ScratchFormat::RGBA8:
      return vk::Format::eR8G8B8A8Unorm;
    case ScratchFormat::RGBA32UI:
      return vk::Format::eR32G32B32A32Uint;
    case ScratchFormat::RGBA32F:
      return vk::Format::eR32G32B32A32Sfloat;
    case ScratchFormat::RGBA16:
      return vk::Format::eR16G16B16A16Unorm;
    case ScratchFormat::RGBA16F:
      return vk::Format::eR16G16B16A16Sfloat;
    case ScratchFormat::RG32F:
      return vk::Format::eR32G32Sfloat;
    case ScratchFormat::R32F:
      return vk::Format::eR32Sfloat;
    case ScratchFormat::R16F:
      return vk::Format::eR16Sfloat;
    case ScratchFormat::Depth24:
      // On Vulkan (MoltenVK), prefer D32Sfloat for depth-only rendering to
      // ensure dynamic rendering + gl_FragDepth writes and transfer ops are supported.
      return vk::Format::eD32Sfloat;
    case ScratchFormat::Depth32F:
      return vk::Format::eD32Sfloat;
  }
  CHECK(false) << "Unhandled ScratchFormat for Vulkan";
  return vk::Format::eR8G8B8A8Unorm;
}

vk::ImageAspectFlags vkAspectMaskFor(ScratchAttachmentKind kind, vk::Format format)
{
  switch (kind) {
    case ScratchAttachmentKind::Color:
      return vk::ImageAspectFlagBits::eColor;
    case ScratchAttachmentKind::Depth: {
      // Depth attachments that use a combined depth-stencil format must include both
      // aspects in barriers and views unless separateDepthStencilLayouts is enabled.
      switch (format) {
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
          return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        default:
          return vk::ImageAspectFlagBits::eDepth;
      }
    }
  }
  CHECK(false) << "Unhandled ScratchAttachmentKind";
  return vk::ImageAspectFlagBits::eColor;
}

vk::ImageUsageFlags vkUsageFor(const ScratchAttachmentDesc& attachment)
{
  switch (attachment.kind) {
    case ScratchAttachmentKind::Color:
      return vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    case ScratchAttachmentKind::Depth:
      return vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  }
  CHECK(false) << "Unhandled attachment kind";
  return vk::ImageUsageFlagBits::eColorAttachment;
}

ZVulkanTexture::CreateInfo makeVulkanTextureInfo(const ScratchImageDescriptor& descriptor,
                                                 const ScratchAttachmentDesc& attachment)
{
  const auto format = vkFormatFor(attachment.format);
  vk::ImageUsageFlags usage = vkUsageFor(attachment);
  const auto aspect = vkAspectMaskFor(attachment.kind, format);
  constexpr auto memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
  constexpr bool createSampler = true;
  const auto descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  // For Block-ID render targets, allow storage-image reads in compute compaction.
  if (descriptor.usage == ScratchImageUsage::BlockId && attachment.kind == ScratchAttachmentKind::Color) {
    usage |= vk::ImageUsageFlagBits::eStorage;
  }

  if (descriptor.dimension == ScratchImageDimension::Tex2D) {
    auto info = ZVulkanTexture::CreateInfo::make2D(descriptor.size.x,
                                                   descriptor.size.y,
                                                   format,
                                                   usage,
                                                   memory,
                                                   1u,
                                                   createSampler,
                                                   descriptorLayout);
    info.aspectMask = aspect;
    info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::ScratchBacking;
    info.deferAllocation = true;
    return info;
  }

  auto info = ZVulkanTexture::CreateInfo::make2DArray(descriptor.size.x,
                                                      descriptor.size.y,
                                                      descriptor.layers,
                                                      format,
                                                      usage,
                                                      memory,
                                                      1u,
                                                      createSampler,
                                                      descriptorLayout);
  info.aspectMask = aspect;
  info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::ScratchBacking;
  info.deferAllocation = true;
  return info;
}

uint32_t colorAttachmentCount(const ScratchImageDescriptor& descriptor)
{
  uint32_t count = 0;
  for (const auto& attachment : descriptor.attachments) {
    if (attachment.kind == ScratchAttachmentKind::Color) {
      ++count;
    }
  }
  return count;
}

bool attachmentsCompatibleForReuse(ScratchImageUsage usage,
                                   const ScratchImageDescriptor& candidate,
                                   const ScratchImageDescriptor& requested)
{
  if (candidate.dimension != requested.dimension) {
    return false;
  }

  if (usage != ScratchImageUsage::BlockId) {
    return candidate.attachments == requested.attachments;
  }

  if (candidate.attachments.size() < requested.attachments.size()) {
    return false;
  }
  for (size_t i = 0; i < requested.attachments.size(); ++i) {
    if (candidate.attachments[i] != requested.attachments[i]) {
      return false;
    }
  }
  return true;
}

void attachTexturesForDescriptor(Z3DRenderTarget& fbo, const ScratchImageDescriptor& descriptor)
{
  const auto extent = textureExtentFor(descriptor);
  const auto target = ::glTextureTargetFor(descriptor.dimension);
  for (const auto& attachment : descriptor.attachments) {
    auto params = glTextureParamsFor(attachment.format);
    auto* texture = new Z3DTexture(target,
                                   params.internalFormat,
                                   extent,
                                   params.format,
                                   params.type,
                                   nullptr,
                                   GLint(GL_NEAREST),
                                   GLint(GL_NEAREST));
    GLenum attachmentPoint = (attachment.kind == ScratchAttachmentKind::Color)
                               ? GLenum(GL_COLOR_ATTACHMENT0 + attachment.index)
                               : GL_DEPTH_ATTACHMENT;
    fbo.attachTextureToFBO(texture, attachmentPoint, true);
  }
}

// Variant that only considers slots where predicate(slot) is true.
template<typename Slot, typename Predicate>
Slot* findClosestFreeSlotIf(std::vector<std::unique_ptr<Slot>>& slots, const glm::uvec2& requestedSize, Predicate pred)
{
  Slot* best = nullptr;
  uint64_t bestDelta = std::numeric_limits<uint64_t>::max();
  const uint64_t reqPixels = static_cast<uint64_t>(requestedSize.x) * static_cast<uint64_t>(requestedSize.y);
  for (auto& up : slots) {
    Slot& s = *up;
    if (s.inUse) {
      continue;
    }
    if (!pred(s)) {
      continue;
    }
    // After trim(), all remaining slots have a valid FBO.
    const glm::uvec2 sSize = s.fbo->size();
    const uint64_t sPixels = static_cast<uint64_t>(sSize.x) * static_cast<uint64_t>(sSize.y);
    uint64_t delta = (sPixels > reqPixels) ? (sPixels - reqPixels) : (reqPixels - sPixels);
    if (!best || delta < bestDelta) {
      best = &s;
      bestDelta = delta;
    }
  }
  return best;
}

template<typename Slot>
Slot* findClosestFreeSlot(std::vector<std::unique_ptr<Slot>>& slots, const glm::uvec2& requestedSize)
{
  return findClosestFreeSlotIf(slots, requestedSize, [](const Slot&) {
    return true;
  });
}

template<typename Slots, typename PerSlotFn>
uint64_t
accumulateCategory(const char* label, const Slots& slots, bool detailed, std::string& details, PerSlotFn&& perSlot)
{
  uint64_t categoryBytes = 0;
  for (size_t idx = 0; idx < slots.size(); ++idx) {
    const auto& slot = *slots[idx];
    auto [bytes, line] = perSlot(slot, idx, detailed);
    categoryBytes += bytes;
    if (detailed && !line.empty()) {
      details += fmt::format("[{}] {}\n", label, line);
    }
  }
  return categoryBytes;
}

template<typename Slot>
Z3DScratchResourcePool::RenderTargetLease makeLeaseFromSlot(Slot* slot, uint32_t attachments, RenderBackend backend)
{
  Z3DScratchResourcePool::RenderTargetLease lease;
  lease.descriptor = slot->descriptor;
  lease.backend = backend;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = attachments;
  lease.releaser = Z3DScratchResourcePool::RenderTargetLease::Releaser::forSlot(slot);
  return lease;
}

} // anonymous namespace

namespace nim {} // namespace nim

DEFINE_uint32(atlas_blockid_rt_max_attachments, 8, "Max color attachments for block-id FBO");
DEFINE_double(atlas_blockid_rt_scale, 1.0, "Scale factor for block-id FBO size (relative to viewport)");

namespace nim {

static inline uint64_t bytesPerPixelForScratchFormat(ScratchFormat fmt);
static std::string describeScratchDescriptor(const ScratchImageDescriptor& descriptor);

class VulkanAllocationRecoveryScope final
{
public:
  explicit VulkanAllocationRecoveryScope(ZVulkanDevice& device)
    : m_device(device)
  {
    m_device.enterAllocationRecoveryScope();
  }

  VulkanAllocationRecoveryScope(const VulkanAllocationRecoveryScope&) = delete;
  VulkanAllocationRecoveryScope& operator=(const VulkanAllocationRecoveryScope&) = delete;

  ~VulkanAllocationRecoveryScope()
  {
    m_device.leaveAllocationRecoveryScope();
  }

private:
  ZVulkanDevice& m_device;
};

class ZVulkanScratchImage
{
  struct AttachmentBackup
  {
    std::vector<uint8_t> bytes;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    bool valid = false;
  };

public:
  ZVulkanScratchImage(ZVulkanDevice& device, const ScratchImageDescriptor& descriptor)
    : m_device(device)
    , m_descriptor(descriptor)
  {
    createAttachments();
  }

  const ScratchImageDescriptor& descriptor() const
  {
    return m_descriptor;
  }

  bool resident() const
  {
    bool sawAttachment = false;
    for (const auto& attachment : m_descriptor.attachments) {
      const auto* texture = attachmentTexture(attachment);
      sawAttachment = true;
      if (!texture->resident()) {
        return false;
      }
    }
    return sawAttachment;
  }

  uint64_t estimatedResidentBytes() const
  {
    const uint64_t pixels =
      static_cast<uint64_t>(m_descriptor.size.x) * m_descriptor.size.y * std::max<uint32_t>(1u, m_descriptor.layers);
    uint64_t bytes = 0;
    for (const auto& attachment : m_descriptor.attachments) {
      const auto* texture = attachmentTexture(attachment);
      if (texture->resident()) {
        bytes += pixels * bytesPerPixelForScratchFormat(attachment.format);
      }
    }
    return bytes;
  }

  uint64_t estimatedTotalBytes() const
  {
    const uint64_t pixels =
      static_cast<uint64_t>(m_descriptor.size.x) * m_descriptor.size.y * std::max<uint32_t>(1u, m_descriptor.layers);
    uint64_t bytes = 0;
    for (const auto& attachment : m_descriptor.attachments) {
      bytes += pixels * bytesPerPixelForScratchFormat(attachment.format);
    }
    return bytes;
  }

  bool containsTexture(const ZVulkanTexture* texture) const
  {
    if (texture == nullptr) {
      return false;
    }
    for (const auto& attachment : m_descriptor.attachments) {
      if (attachmentTexture(attachment) == texture) {
        return true;
      }
    }
    return false;
  }

  bool contentAvailable(const ZVulkanTexture* texture) const
  {
    if (texture == nullptr) {
      return false;
    }
    if (texture->resident()) {
      return true;
    }
    const auto* backup = backupForTexture(texture);
    return backup != nullptr && backup->valid && !backup->bytes.empty();
  }

  void releaseDeviceResources()
  {
    for (const auto& attachment : m_descriptor.attachments) {
      attachmentTexture(attachment)->releaseDeviceResources();
    }
  }

  uint64_t releaseDeviceResourcesToHost()
  {
    VulkanAllocationRecoveryScope recoveryScope(m_device);
    uint64_t releasedBytes = estimatedResidentBytes();
    for (const auto& attachment : m_descriptor.attachments) {
      backupAttachmentToHost(attachment);
    }
    releaseDeviceResources();
    return releasedBytes;
  }

  void recreateDeviceResources()
  {
    try {
      for (const auto& attachment : m_descriptor.attachments) {
        auto* texture = attachmentTexture(attachment);
        if (!texture->resident()) {
          texture->recreateDeviceResources();
        }
      }
    }
    catch (...) {
      releaseDeviceResources();
      throw;
    }
  }

  void makeResidentForPass(std::span<const Z3DScratchResourcePool::VulkanScratchTextureUse> uses)
  {
    for (const auto& use : uses) {
      if (!use.contentsRequired || !containsTexture(use.texture) || use.texture == nullptr || use.texture->resident()) {
        continue;
      }
      if (!contentAvailable(use.texture)) {
        throw ZException(fmt::format(
          "Vulkan scratch texture restore failed: contents required but no host backup is available label={} texture=0x{:x}",
          describe(),
          reinterpret_cast<uintptr_t>(use.texture)));
      }
    }

    struct RestoreTarget
    {
      ScratchAttachmentDesc attachment;
      bool wasResident = false;
    };
    std::vector<RestoreTarget> restoreTargets;
    restoreTargets.reserve(m_descriptor.attachments.size());
    for (const auto& attachment : m_descriptor.attachments) {
      auto* texture = attachmentTexture(attachment);
      restoreTargets.push_back(RestoreTarget{.attachment = attachment, .wasResident = texture->resident()});
    }

    recreateDeviceResources();

    try {
      for (const auto& target : restoreTargets) {
        if (target.wasResident) {
          continue;
        }
        auto* texture = attachmentTexture(target.attachment);
        const auto& backup = attachmentBackup(target.attachment);
        if (!backup.valid || backup.bytes.empty()) {
          continue;
        }
        CHECK(texture->resident()) << "Scratch texture restore recreated no resident backing";
        CHECK(backup.layout != vk::ImageLayout::eUndefined)
          << "Scratch host backup cannot restore to undefined image layout";
        texture->uploadData(backup.bytes.data(), backup.bytes.size(), backup.layout);
      }
    }
    catch (...) {
      releaseDeviceResources();
      throw;
    }
  }

  void retarget(const ScratchImageDescriptor& descriptor)
  {
    if (m_descriptor.dimension == descriptor.dimension && m_descriptor.size == descriptor.size &&
        m_descriptor.layers == descriptor.layers && m_descriptor.attachments == descriptor.attachments) {
      return;
    }
    if (m_descriptor.dimension == descriptor.dimension && m_descriptor.attachments == descriptor.attachments) {
      releaseDeviceResources();
      for (const auto& attachment : descriptor.attachments) {
        attachmentTexture(attachment)->resetNonResidentCreateInfo(makeVulkanTextureInfo(descriptor, attachment));
      }
      m_descriptor = descriptor;
      m_colorBackups.clear();
      m_depthBackup = AttachmentBackup{};
      return;
    }
    auto storage = createAttachmentsForDescriptor(descriptor);
    m_descriptor = descriptor;
    m_colorAttachments = std::move(storage.colorAttachments);
    m_depthAttachment = std::move(storage.depthAttachment);
    m_colorBackups.clear();
    m_depthBackup = AttachmentBackup{};
  }

  ZVulkanTexture* colorAttachment(uint32_t index) const
  {
    if (index >= m_colorAttachments.size()) {
      return nullptr;
    }
    return m_colorAttachments[index].get();
  }

  ZVulkanTexture* depthAttachment() const
  {
    return m_depthAttachment.get();
  }

  std::string describe() const
  {
    return describeScratchDescriptor(m_descriptor);
  }

private:
  struct AttachmentStorage
  {
    std::vector<std::unique_ptr<ZVulkanTexture>> colorAttachments;
    std::unique_ptr<ZVulkanTexture> depthAttachment;
  };

  AttachmentStorage createAttachmentsForDescriptor(const ScratchImageDescriptor& descriptor)
  {
    AttachmentStorage storage;
    for (const auto& attachment : descriptor.attachments) {
      auto info = makeVulkanTextureInfo(descriptor, attachment);
      auto texture = m_device.createTexture(info);
      if (attachment.kind == ScratchAttachmentKind::Color) {
        if (storage.colorAttachments.size() <= attachment.index) {
          storage.colorAttachments.resize(attachment.index + 1);
        }
        storage.colorAttachments[attachment.index] = std::move(texture);
      } else {
        storage.depthAttachment = std::move(texture);
      }
    }
    return storage;
  }

  void createAttachments()
  {
    auto storage = createAttachmentsForDescriptor(m_descriptor);
    m_colorAttachments = std::move(storage.colorAttachments);
    m_depthAttachment = std::move(storage.depthAttachment);
  }

  ZVulkanTexture* attachmentTexture(const ScratchAttachmentDesc& attachment)
  {
    return const_cast<ZVulkanTexture*>(std::as_const(*this).attachmentTexture(attachment));
  }

  const ZVulkanTexture* attachmentTexture(const ScratchAttachmentDesc& attachment) const
  {
    if (attachment.kind == ScratchAttachmentKind::Color) {
      CHECK_LT(attachment.index, m_colorAttachments.size()) << "Vulkan scratch color attachment missing";
      const auto& texture = m_colorAttachments[attachment.index];
      CHECK(texture != nullptr) << "Vulkan scratch color attachment is null";
      return texture.get();
    }
    CHECK(m_depthAttachment != nullptr) << "Vulkan scratch depth attachment is null";
    return m_depthAttachment.get();
  }

  uint64_t attachmentByteSize(const ScratchAttachmentDesc& attachment) const
  {
    const uint64_t pixels =
      static_cast<uint64_t>(m_descriptor.size.x) * m_descriptor.size.y * std::max<uint32_t>(1u, m_descriptor.layers);
    return pixels * bytesPerPixelForScratchFormat(attachment.format);
  }

  AttachmentBackup& attachmentBackup(const ScratchAttachmentDesc& attachment)
  {
    if (attachment.kind == ScratchAttachmentKind::Color) {
      if (m_colorBackups.size() <= attachment.index) {
        m_colorBackups.resize(attachment.index + 1u);
      }
      return m_colorBackups[attachment.index];
    }
    return m_depthBackup;
  }

  const AttachmentBackup& attachmentBackup(const ScratchAttachmentDesc& attachment) const
  {
    if (attachment.kind == ScratchAttachmentKind::Color) {
      CHECK_LT(attachment.index, m_colorBackups.size()) << "Vulkan scratch color backup missing";
      return m_colorBackups[attachment.index];
    }
    return m_depthBackup;
  }

  const AttachmentBackup* backupForTexture(const ZVulkanTexture* texture) const
  {
    for (const auto& attachment : m_descriptor.attachments) {
      if (attachmentTexture(attachment) == texture) {
        if (attachment.kind == ScratchAttachmentKind::Color) {
          if (attachment.index >= m_colorBackups.size()) {
            return nullptr;
          }
          return &m_colorBackups[attachment.index];
        }
        return &m_depthBackup;
      }
    }
    return nullptr;
  }

  void backupAttachmentToHost(const ScratchAttachmentDesc& attachment)
  {
    auto* texture = attachmentTexture(attachment);
    auto& backup = attachmentBackup(attachment);
    backup.valid = false;
    backup.layout = vk::ImageLayout::eUndefined;

    if (!texture->resident()) {
      return;
    }

    const uint64_t bytes = attachmentByteSize(attachment);
    CHECK(bytes <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
      << "Vulkan scratch host backup exceeds addressable size";
    if (bytes == 0u || texture->layout() == vk::ImageLayout::eUndefined) {
      backup.bytes.clear();
      return;
    }

    backup.bytes.resize(static_cast<size_t>(bytes));
    backup.layout = texture->layout();
    texture->downloadData(backup.bytes.data(), backup.bytes.size());
    backup.valid = true;
  }

  ZVulkanDevice& m_device;
  ScratchImageDescriptor m_descriptor;
  std::vector<std::unique_ptr<ZVulkanTexture>> m_colorAttachments;
  std::unique_ptr<ZVulkanTexture> m_depthAttachment;
  std::vector<AttachmentBackup> m_colorBackups;
  AttachmentBackup m_depthBackup;
};

ZVulkanTexture* Z3DScratchResourcePool::RenderTargetLease::colorAttachment(uint32_t index) const
{
  if (!vulkanImage) {
    return nullptr;
  }
  return vulkanImage->colorAttachment(index);
}

ZVulkanTexture* Z3DScratchResourcePool::RenderTargetLease::depthAttachmentTexture() const
{
  if (!vulkanImage) {
    return nullptr;
  }
  return vulkanImage->depthAttachment();
}

Z3DScratchResourcePool::Z3DScratchResourcePool(RenderBackend defaultBackend)
  : m_defaultBackend(defaultBackend)
{
  CHECK(defaultBackend == RenderBackend::OpenGL || defaultBackend == RenderBackend::Vulkan)
    << "Z3DScratchResourcePool constructed with invalid RenderBackend value";
}

Z3DScratchResourcePool::~Z3DScratchResourcePool() = default;

uint32_t Z3DScratchResourcePool::blockIdMaxAttachments() const
{
  return std::max<uint32_t>(1, FLAGS_atlas_blockid_rt_max_attachments);
}

double Z3DScratchResourcePool::blockIdScale() const
{
  return std::max(0.0, FLAGS_atlas_blockid_rt_scale);
}

static inline uint64_t bytesPerPixelFromInternal(GLint internal)
{
  return static_cast<uint64_t>(Z3DTexture::bypePerPixel(internal));
}

static inline uint64_t bytesPerPixelForScratchFormat(ScratchFormat fmt)
{
  switch (fmt) {
    case ScratchFormat::RGBA8:
      return 4u;
    case ScratchFormat::RGBA32UI:
    case ScratchFormat::RGBA32F:
      return 16u; // 4 channels * 4 bytes
    case ScratchFormat::RGBA16:
    case ScratchFormat::RGBA16F:
      return 8u; // 4 channels * 2 bytes
    case ScratchFormat::RG32F:
      return 8u; // 2 channels * 4 bytes
    case ScratchFormat::R32F:
      return 4u;
    case ScratchFormat::R16F:
      return 2u;
    case ScratchFormat::Depth24:
      return 4u; // D24S8 packed as 32-bit
    case ScratchFormat::Depth32F:
      return 4u; // 32-bit float depth
  }
  return 0u;
}

static const char* scratchUsageLabel(ScratchImageUsage usage)
{
  switch (usage) {
    case ScratchImageUsage::BlockId:
      return "BlockID";
    case ScratchImageUsage::EntryExit:
      return "EntryExit";
    case ScratchImageUsage::LayerArray:
      return "LayerArray";
    case ScratchImageUsage::RaycastAccumulator:
      return "RaycastAccum";
    case ScratchImageUsage::Temp2D:
      return "Temp2D";
    case ScratchImageUsage::DualDepthPeel:
      return "DualDepthPeel";
    case ScratchImageUsage::WeightedAverage:
      return "WeightedAvg";
    case ScratchImageUsage::WeightedBlended:
      return "WeightedBlend";
  }
  return "Unknown";
}

static uint64_t estimatedBytesForDescriptor(const ScratchImageDescriptor& descriptor)
{
  const uint64_t pixels =
    static_cast<uint64_t>(descriptor.size.x) * descriptor.size.y * std::max<uint32_t>(1u, descriptor.layers);
  uint64_t bytes = 0;
  for (const auto& attachment : descriptor.attachments) {
    bytes += pixels * bytesPerPixelForScratchFormat(attachment.format);
  }
  return bytes;
}

static std::string describeScratchDescriptor(const ScratchImageDescriptor& descriptor)
{
  std::string attachments;
  for (const auto& attachment : descriptor.attachments) {
    if (!attachments.empty()) {
      attachments += ", ";
    }
    attachments +=
      fmt::format("{}{}={}",
                  attachment.kind == ScratchAttachmentKind::Color ? "c" : "depth",
                  attachment.kind == ScratchAttachmentKind::Color ? std::to_string(attachment.index) : std::string{},
                  scratchFormatLabel(attachment.format));
  }
  return fmt::format("usage={} size={}x{} layers={} attachments=[{}] estimated_bytes={}",
                     scratchUsageLabel(descriptor.usage),
                     descriptor.size.x,
                     descriptor.size.y,
                     descriptor.layers,
                     attachments,
                     estimatedBytesForDescriptor(descriptor));
}

std::string Z3DScratchResourcePool::describeMemoryUsage(bool detailed) const
{
  auto& cache = m_descriptionCache[detailed ? 1 : 0];
  if (cache.valid && cache.creationCounter == m_creationCounter && cache.changeCounter == m_changeCounter) {
    return cache.text;
  }

  std::string details;
  const uint64_t blockIdBytes =
    accumulateCategory("BlockID",
                       m_blockIdRenderTargetSlots,
                       detailed,
                       details,
                       [](const BlockIdRenderTargetSlot& s, size_t idx, bool wantDetail) {
                         const glm::uvec2 sz = s.fbo->size();
                         const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
                         const uint64_t bpp = 16; // GL_RGBA32UI = 4 * 32-bit
                         const uint64_t bytes = pixels * bpp * s.attachments;
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} attachments={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              s.attachments,
                                              s.inUse ? 1 : 0,
                                              bytes);
                         }
                         return std::pair{bytes, std::move(line)};
                       });

  const uint64_t entryExitBytes =
    accumulateCategory("EntryExit",
                       m_entryExitRenderTargetSlots,
                       detailed,
                       details,
                       [](const EntryExitRenderTargetSlot& s, size_t idx, bool wantDetail) {
                         const glm::uvec2 sz = s.fbo->size();
                         const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y * std::max<uint32_t>(1, s.layers);
                         const auto params = glTextureParamsFor(s.colorFormat);
                         const uint64_t bpp = bytesPerPixelFromInternal(params.internalFormat);
                         const uint64_t bytes = pixels * bpp;
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} layers={} fmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              s.layers,
                                              scratchFormatLabel(s.colorFormat),
                                              s.inUse ? 1 : 0,
                                              bytes);
                         }
                         return std::pair{bytes, std::move(line)};
                       });

  const uint64_t layerArrayBytes =
    accumulateCategory("LayerArray",
                       m_layerArrayRenderTargetSlots,
                       detailed,
                       details,
                       [](const LayerArrayRenderTargetSlot& s, size_t idx, bool wantDetail) {
                         const glm::uvec2 sz = s.fbo->size();
                         const uint64_t layerPixels =
                           static_cast<uint64_t>(sz.x) * sz.y * std::max<uint32_t>(1, s.layers);
                         const auto colorParams = glTextureParamsFor(s.colorFormat);
                         const auto depthParams = glTextureParamsFor(s.depthFormat);
                         const uint64_t colorBpp = bytesPerPixelFromInternal(colorParams.internalFormat);
                         const uint64_t depthBpp = bytesPerPixelFromInternal(depthParams.internalFormat);
                         const uint64_t bytes = layerPixels * (colorBpp + depthBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} layers={} colorFmt={} depthFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              s.layers,
                                              scratchFormatLabel(s.colorFormat),
                                              scratchFormatLabel(s.depthFormat),
                                              s.inUse ? 1 : 0,
                                              bytes);
                         }
                         return std::pair{bytes, std::move(line)};
                       });

  const uint64_t temp2DBytes =
    accumulateCategory("Temp2D",
                       m_temp2DRenderTargetSlots,
                       detailed,
                       details,
                       [](const Temp2DRenderTargetSlot& s, size_t idx, bool wantDetail) {
                         const glm::uvec2 sz = s.fbo->size();
                         const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
                         const auto colorParams = glTextureParamsFor(s.colorFormat);
                         const auto depthParams = glTextureParamsFor(s.depthFormat);
                         const uint64_t colorBpp = bytesPerPixelFromInternal(colorParams.internalFormat);
                         const uint64_t depthBpp = bytesPerPixelFromInternal(depthParams.internalFormat);
                         const uint64_t bytes = pixels * (colorBpp + depthBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} colorFmt={} depthFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              scratchFormatLabel(s.colorFormat),
                                              scratchFormatLabel(s.depthFormat),
                                              s.inUse ? 1 : 0,
                                              bytes);
                         }
                         return std::pair{bytes, std::move(line)};
                       });

  const uint64_t raycastBytes =
    accumulateCategory("RaycastAccum",
                       m_raycastAccumulatorSlots,
                       detailed,
                       details,
                       [](const RaycastAccumulatorSlot& s, size_t idx, bool wantDetail) {
                         const glm::uvec2 sz = s.fbo->size();
                         const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
                         const auto primaryParams = glTextureParamsFor(s.colorFormat);
                         const auto accumParams = glTextureParamsFor(s.accumulatorFormat);
                         const uint64_t primaryBpp = bytesPerPixelFromInternal(primaryParams.internalFormat);
                         const uint64_t accumBpp = bytesPerPixelFromInternal(accumParams.internalFormat);
                         const uint64_t bytes = pixels * (primaryBpp + accumBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} colorFmt={} accumFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              scratchFormatLabel(s.colorFormat),
                                              scratchFormatLabel(s.accumulatorFormat),
                                              s.inUse ? 1 : 0,
                                              bytes);
                         }
                         return std::pair{bytes, std::move(line)};
                       });

  const uint64_t dualDepthBytes = accumulateCategory(
    "DualDepthPeel",
    m_dualDepthPeelSlots,
    detailed,
    details,
    [](const DualDepthPeelSlot& s, size_t idx, bool wantDetail) {
      const glm::uvec2 sz = s.fbo->size();
      const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
      uint64_t bytes = 0;
      std::string attachmentFormats;
      if (wantDetail) {
        attachmentFormats.reserve(64);
      }
      for (int att = 0; att < 8; ++att) {
        if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
          bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
          if (wantDetail) {
            if (!attachmentFormats.empty()) {
              attachmentFormats += ", ";
            }
            attachmentFormats +=
              fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
          }
        }
      }
      std::string line;
      if (wantDetail) {
        line = fmt::format("slot={} size={}x{} inUse={} bytes={} attachments=[{}]",
                           idx,
                           sz.x,
                           sz.y,
                           s.inUse ? 1 : 0,
                           bytes,
                           attachmentFormats);
      }
      return std::pair{bytes, std::move(line)};
    });

  const uint64_t weightedAvgBytes = accumulateCategory(
    "WeightedAverage",
    m_weightedAverageSlots,
    detailed,
    details,
    [](const WeightedAverageSlot& s, size_t idx, bool wantDetail) {
      const glm::uvec2 sz = s.fbo->size();
      const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
      uint64_t bytes = 0;
      std::string attachmentFormats;
      if (wantDetail) {
        attachmentFormats.reserve(32);
      }
      for (int att = 0; att < 2; ++att) {
        if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
          bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
          if (wantDetail) {
            if (!attachmentFormats.empty()) {
              attachmentFormats += ", ";
            }
            attachmentFormats +=
              fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
          }
        }
      }
      std::string line;
      if (wantDetail) {
        line = fmt::format("slot={} size={}x{} inUse={} bytes={} attachments=[{}]",
                           idx,
                           sz.x,
                           sz.y,
                           s.inUse ? 1 : 0,
                           bytes,
                           attachmentFormats);
      }
      return std::pair{bytes, std::move(line)};
    });

  const uint64_t weightedBlendBytes = accumulateCategory(
    "WeightedBlended",
    m_weightedBlendedSlots,
    detailed,
    details,
    [](const WeightedBlendedSlot& s, size_t idx, bool wantDetail) {
      const glm::uvec2 sz = s.fbo->size();
      const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
      uint64_t bytes = 0;
      std::string attachmentFormats;
      if (wantDetail) {
        attachmentFormats.reserve(32);
      }
      for (int att = 0; att < 2; ++att) {
        if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
          bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
          if (wantDetail) {
            if (!attachmentFormats.empty()) {
              attachmentFormats += ", ";
            }
            attachmentFormats +=
              fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
          }
        }
      }
      std::string line;
      if (wantDetail) {
        line = fmt::format("slot={} size={}x{} inUse={} bytes={} attachments=[{}]",
                           idx,
                           sz.x,
                           sz.y,
                           s.inUse ? 1 : 0,
                           bytes,
                           attachmentFormats);
      }
      return std::pair{bytes, std::move(line)};
    });

  // Add Vulkan-backed scratch images to accounting
  auto appendDetail = [&](const std::string& line) {
    if (detailed) {
      if (!details.empty()) {
        details += '\n';
      }
      details += line;
    }
  };

  auto accumulateVulkan = [&](ScratchImageUsage usage) -> uint64_t {
    const auto index = static_cast<size_t>(usage);
    uint64_t bytes = 0;
    if (index >= m_vulkanSlots.size()) {
      return 0;
    }
    const auto& slots = m_vulkanSlots[index];
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto& slot = *slots[i];
      const auto& desc = slot.descriptor;
      const uint64_t slotBytes = slot.image ? slot.image->estimatedResidentBytes() : 0;
      bool protectedSlot = false;
      if (slot.image) {
        for (auto* texture : m_vulkanResidencyProtectedTextures) {
          if (slot.image->containsTexture(texture)) {
            protectedSlot = true;
            break;
          }
        }
      }
      std::string attachmentFormats;
      if (detailed) {
        attachmentFormats.reserve(64);
      }
      for (const auto& att : desc.attachments) {
        if (detailed) {
          if (!attachmentFormats.empty()) {
            attachmentFormats += ", ";
          }
          // Reuse scratchFormatLabel for readability even in Vulkan mode.
          attachmentFormats +=
            fmt::format("{}={}",
                        (att.kind == ScratchAttachmentKind::Color) ? "c" + std::to_string(att.index) : "depth",
                        scratchFormatLabel(att.format));
        }
      }
      bytes += slotBytes;
      if (detailed) {
        appendDetail(fmt::format(
          "[Vulkan/{}] slot={} size={}x{} layers={} inUse={} releasePending={} protected={} resident={} bytes={} attachments=[{}]",
          scratchUsageLabel(usage),
          i,
          desc.size.x,
          desc.size.y,
          desc.layers,
          slot.inUse ? 1 : 0,
          slot.releasePending ? 1 : 0,
          protectedSlot ? 1 : 0,
          (slot.image && slot.image->resident()) ? 1 : 0,
          slotBytes,
          attachmentFormats));
      }
    }
    return bytes;
  };

  const uint64_t vkBlockIdBytes = accumulateVulkan(ScratchImageUsage::BlockId);
  const uint64_t vkEntryExitBytes = accumulateVulkan(ScratchImageUsage::EntryExit);
  const uint64_t vkLayerArrayBytes = accumulateVulkan(ScratchImageUsage::LayerArray);
  const uint64_t vkRaycastBytes = accumulateVulkan(ScratchImageUsage::RaycastAccumulator);
  const uint64_t vkTemp2DBytes = accumulateVulkan(ScratchImageUsage::Temp2D);
  const uint64_t vkDualDepthBytes = accumulateVulkan(ScratchImageUsage::DualDepthPeel);
  const uint64_t vkWeightedAvgBytes = accumulateVulkan(ScratchImageUsage::WeightedAverage);
  const uint64_t vkWeightedBlendBytes = accumulateVulkan(ScratchImageUsage::WeightedBlended);

  const uint64_t totalBlockIdBytes = blockIdBytes + vkBlockIdBytes;
  const uint64_t totalEntryExitBytes = entryExitBytes + vkEntryExitBytes;
  const uint64_t totalLayerArrayBytes = layerArrayBytes + vkLayerArrayBytes;
  const uint64_t totalTemp2DBytes = temp2DBytes + vkTemp2DBytes;
  const uint64_t totalRaycastBytes = raycastBytes + vkRaycastBytes;
  const uint64_t totalDualDepthBytes = dualDepthBytes + vkDualDepthBytes;
  const uint64_t totalWeightedAvgBytes = weightedAvgBytes + vkWeightedAvgBytes;
  const uint64_t totalWeightedBlendBytes = weightedBlendBytes + vkWeightedBlendBytes;

  const uint64_t total = totalBlockIdBytes + totalEntryExitBytes + totalLayerArrayBytes + totalTemp2DBytes +
                         totalRaycastBytes + totalDualDepthBytes + totalWeightedAvgBytes + totalWeightedBlendBytes;
  const double totalMiB = static_cast<double>(total) / (1024.0 * 1024.0);
  const double blockMiB = static_cast<double>(totalBlockIdBytes) / (1024.0 * 1024.0);
  const double eeMiB = static_cast<double>(totalEntryExitBytes) / (1024.0 * 1024.0);
  const double layerMiB = static_cast<double>(totalLayerArrayBytes) / (1024.0 * 1024.0);
  const double tempMiB = static_cast<double>(totalTemp2DBytes) / (1024.0 * 1024.0);
  const double raycastMiB = static_cast<double>(totalRaycastBytes) / (1024.0 * 1024.0);
  const double ddpMiB = static_cast<double>(totalDualDepthBytes) / (1024.0 * 1024.0);
  const double waMiB = static_cast<double>(totalWeightedAvgBytes) / (1024.0 * 1024.0);
  const double wbMiB = static_cast<double>(totalWeightedBlendBytes) / (1024.0 * 1024.0);
  auto head = fmt::format(
    "ScratchPool memory: total={} bytes ({:.2f} MiB) (BlockID={} bytes ({:.2f} MiB), "
    "EntryExit={} bytes ({:.2f} MiB), LayerArray={} bytes ({:.2f} MiB), Temp2D={} bytes ({:.2f} MiB), "
    "RaycastAccum={} bytes ({:.2f} MiB), DualDepthPeel={} bytes ({:.2f} MiB), WeightedAvg={} bytes ({:.2f} MiB), "
    "WeightedBlend={} bytes ({:.2f} MiB))",
    total,
    totalMiB,
    totalBlockIdBytes,
    blockMiB,
    totalEntryExitBytes,
    eeMiB,
    totalLayerArrayBytes,
    layerMiB,
    totalTemp2DBytes,
    tempMiB,
    totalRaycastBytes,
    raycastMiB,
    totalDualDepthBytes,
    ddpMiB,
    totalWeightedAvgBytes,
    waMiB,
    totalWeightedBlendBytes,
    wbMiB);

  std::string result = detailed ? fmt::format("{}\n{}", head, details) : head;

  cache.valid = true;
  cache.creationCounter = m_creationCounter;
  cache.changeCounter = m_changeCounter;
  cache.text = std::move(result);

  return cache.text;
}

Z3DScratchResourcePool::ScratchUsageLiveCounts Z3DScratchResourcePool::scratchLiveCounts(RenderBackend backend,
                                                                                         ScratchImageUsage usage) const
{
  auto countGlSlots = [](const auto& slots) {
    ScratchUsageLiveCounts counts;
    for (const auto& slotPtr : slots) {
      const auto& slot = *slotPtr;
      const uint64_t textures = slot.descriptor.attachments.size();
      counts.slots++;
      counts.textures += textures;
      counts.residentSlots++;
      counts.residentTextures += textures;
      if (slot.inUse) {
        counts.inUseSlots++;
        counts.inUseTextures += textures;
      }
    }
    return counts;
  };

  if (backend == RenderBackend::OpenGL) {
    switch (usage) {
      case ScratchImageUsage::BlockId:
        return countGlSlots(m_blockIdRenderTargetSlots);
      case ScratchImageUsage::EntryExit:
        return countGlSlots(m_entryExitRenderTargetSlots);
      case ScratchImageUsage::LayerArray:
        return countGlSlots(m_layerArrayRenderTargetSlots);
      case ScratchImageUsage::RaycastAccumulator:
        return countGlSlots(m_raycastAccumulatorSlots);
      case ScratchImageUsage::Temp2D:
        return countGlSlots(m_temp2DRenderTargetSlots);
      case ScratchImageUsage::DualDepthPeel:
        return countGlSlots(m_dualDepthPeelSlots);
      case ScratchImageUsage::WeightedAverage:
        return countGlSlots(m_weightedAverageSlots);
      case ScratchImageUsage::WeightedBlended:
        return countGlSlots(m_weightedBlendedSlots);
    }
  }

  ScratchUsageLiveCounts counts;
  const auto index = static_cast<size_t>(usage);
  CHECK_LT(index, m_vulkanSlots.size()) << "Invalid Vulkan scratch usage index";
  for (const auto& slotPtr : m_vulkanSlots[index]) {
    const auto& slot = *slotPtr;
    const uint64_t textures = slot.descriptor.attachments.size();
    const bool resident = slot.image && slot.image->resident();
    counts.slots++;
    counts.textures += textures;
    if (resident) {
      counts.residentSlots++;
      counts.residentTextures += textures;
    }
    if (slot.inUse) {
      counts.inUseSlots++;
      counts.inUseTextures += textures;
    }
  }
  return counts;
}

void Z3DScratchResourcePool::recordScratchAcquire(RenderBackend backend,
                                                  const ScratchImageDescriptor& descriptor,
                                                  ScratchAcquireKind kind,
                                                  bool residentRecreated)
{
  const auto index = static_cast<size_t>(descriptor.usage);
  CHECK_LT(index, m_glReuseStats.size()) << "Invalid scratch usage index";
  auto& stats = (backend == RenderBackend::Vulkan) ? m_vulkanReuseStats[index] : m_glReuseStats[index];

  stats.acquisitions++;
  switch (kind) {
    case ScratchAcquireKind::ExactReuse:
      stats.exactReuses++;
      break;
    case ScratchAcquireKind::CompatibleReuse:
      stats.compatibleReuses++;
      break;
    case ScratchAcquireKind::RetargetReuse:
      stats.retargetReuses++;
      break;
    case ScratchAcquireKind::NewSlot:
      stats.newSlots++;
      break;
  }
  if (residentRecreated) {
    stats.residentRecreates++;
  }

  const auto counts = scratchLiveCounts(backend, descriptor.usage);
  stats.peakSlots = std::max(stats.peakSlots, counts.slots);
  stats.peakTextures = std::max(stats.peakTextures, counts.textures);
  stats.peakInUseSlots = std::max(stats.peakInUseSlots, counts.inUseSlots);
  stats.peakInUseTextures = std::max(stats.peakInUseTextures, counts.inUseTextures);
  stats.peakResidentSlots = std::max(stats.peakResidentSlots, counts.residentSlots);
  stats.peakResidentTextures = std::max(stats.peakResidentTextures, counts.residentTextures);
  ++m_reuseStatsCounter;
}

void Z3DScratchResourcePool::recordVulkanAllocationRecovery(ScratchImageUsage usage)
{
  const auto index = static_cast<size_t>(usage);
  CHECK_LT(index, m_vulkanReuseStats.size()) << "Invalid Vulkan scratch usage index";
  m_vulkanReuseStats[index].allocationRecoveries++;
  ++m_reuseStatsCounter;
}

void Z3DScratchResourcePool::recordVulkanBudgetTrim(ScratchImageUsage usage, uint64_t slots)
{
  const auto index = static_cast<size_t>(usage);
  CHECK_LT(index, m_vulkanReuseStats.size()) << "Invalid Vulkan scratch usage index";
  auto& stats = m_vulkanReuseStats[index];
  stats.budgetTrimEvents++;
  stats.budgetTrimSlots += slots;
  ++m_reuseStatsCounter;
}

void Z3DScratchResourcePool::recordVulkanEvictAll(ScratchImageUsage usage, uint64_t slots)
{
  const auto index = static_cast<size_t>(usage);
  CHECK_LT(index, m_vulkanReuseStats.size()) << "Invalid Vulkan scratch usage index";
  auto& stats = m_vulkanReuseStats[index];
  stats.evictAllEvents++;
  stats.evictAllSlots += slots;
  ++m_reuseStatsCounter;
}

std::string Z3DScratchResourcePool::describeReuseStats(bool detailed) const
{
  auto appendBackendSummary = [&](RenderBackend backend, const char* label, std::string& out) {
    const auto& allStats = (backend == RenderBackend::Vulkan) ? m_vulkanReuseStats : m_glReuseStats;
    ScratchUsageReuseStats total;
    for (const auto& stats : allStats) {
      total.acquisitions += stats.acquisitions;
      total.exactReuses += stats.exactReuses;
      total.compatibleReuses += stats.compatibleReuses;
      total.retargetReuses += stats.retargetReuses;
      total.newSlots += stats.newSlots;
      total.residentRecreates += stats.residentRecreates;
      total.allocationRecoveries += stats.allocationRecoveries;
      total.budgetTrimEvents += stats.budgetTrimEvents;
      total.budgetTrimSlots += stats.budgetTrimSlots;
      total.evictAllEvents += stats.evictAllEvents;
      total.evictAllSlots += stats.evictAllSlots;
      total.peakSlots += stats.peakSlots;
      total.peakTextures += stats.peakTextures;
      total.peakInUseSlots += stats.peakInUseSlots;
      total.peakInUseTextures += stats.peakInUseTextures;
      total.peakResidentSlots += stats.peakResidentSlots;
      total.peakResidentTextures += stats.peakResidentTextures;
    }
    if (!out.empty()) {
      out += " | ";
    }
    out += fmt::format(
      "{} acq={} new={} exact={} compat={} retarget={} re_resident={} recover={} trim_events={} trim_slots={} evict_all_events={} evict_all_slots={} peak_in_use_slots={} peak_in_use_tex={} peak_slots={} peak_tex={} peak_resident_slots={} peak_resident_tex={}",
      label,
      total.acquisitions,
      total.newSlots,
      total.exactReuses,
      total.compatibleReuses,
      total.retargetReuses,
      total.residentRecreates,
      total.allocationRecoveries,
      total.budgetTrimEvents,
      total.budgetTrimSlots,
      total.evictAllEvents,
      total.evictAllSlots,
      total.peakInUseSlots,
      total.peakInUseTextures,
      total.peakSlots,
      total.peakTextures,
      total.peakResidentSlots,
      total.peakResidentTextures);
  };

  std::string summary;
  appendBackendSummary(RenderBackend::OpenGL, "GL", summary);
  appendBackendSummary(RenderBackend::Vulkan, "Vulkan", summary);

  std::string result = "ScratchPool reuse stats: " + summary;

  if (!detailed) {
    return result;
  }

  auto appendDetail = [&](RenderBackend backend, const char* backendLabel) {
    const auto& allStats = (backend == RenderBackend::Vulkan) ? m_vulkanReuseStats : m_glReuseStats;
    for (size_t i = 0; i < allStats.size(); ++i) {
      const auto& stats = allStats[i];
      if (stats.acquisitions == 0 && stats.peakSlots == 0 && stats.allocationRecoveries == 0 &&
          stats.budgetTrimEvents == 0 && stats.evictAllEvents == 0) {
        continue;
      }
      const auto usage = static_cast<ScratchImageUsage>(i);
      result += '\n';
      result += fmt::format(
        "[{}/{}] acq={} new={} exact={} compat={} retarget={} re_resident={} recover={} trim_events={} trim_slots={} evict_all_events={} evict_all_slots={} peak_in_use_slots={} peak_in_use_tex={} peak_slots={} peak_tex={} peak_resident_slots={} peak_resident_tex={}",
        backendLabel,
        scratchUsageLabel(usage),
        stats.acquisitions,
        stats.newSlots,
        stats.exactReuses,
        stats.compatibleReuses,
        stats.retargetReuses,
        stats.residentRecreates,
        stats.allocationRecoveries,
        stats.budgetTrimEvents,
        stats.budgetTrimSlots,
        stats.evictAllEvents,
        stats.evictAllSlots,
        stats.peakInUseSlots,
        stats.peakInUseTextures,
        stats.peakSlots,
        stats.peakTextures,
        stats.peakResidentSlots,
        stats.peakResidentTextures);
    }
  };

  appendDetail(RenderBackend::OpenGL, "GL");
  appendDetail(RenderBackend::Vulkan, "Vulkan");
  return result;
}

Z3DScratchResourcePool::BlockIdRenderTargetSlot* Z3DScratchResourcePool::acquireFreeBlockIdSlot(const glm::uvec2& size,
                                                                                                uint32_t)
{
  if (auto* best = findClosestFreeSlot(m_blockIdRenderTargetSlots, size)) {
    return best;
  }
  m_blockIdRenderTargetSlots.emplace_back(std::make_unique<BlockIdRenderTargetSlot>());
  auto* created = m_blockIdRenderTargetSlots.back().get();
  created->fbo = std::make_unique<Z3DRenderTarget>(size);
  created->attachments = 0;
  updateSlotDescriptor(created->descriptor, makeBlockIdDescriptor(size, 0));
  ++m_creationCounter;
  return created;
}

void Z3DScratchResourcePool::growSlotIfNeeded(BlockIdRenderTargetSlot& slot, const ScratchImageDescriptor& descriptor)
{
  // Resize and ensure attachments
  if (slot.fbo->size() != descriptor.size) {
    slot.fbo->resize(descriptor.size);
    ++m_changeCounter;
  }
  uint32_t before = slot.attachments;
  const auto& descriptorAttachments = descriptor.attachments;
  for (uint32_t i = slot.attachments; i < descriptorAttachments.size(); ++i) {
    const auto& attachmentDesc = descriptorAttachments[i];
    CHECK(attachmentDesc.kind == ScratchAttachmentKind::Color) << "Block ID slot only supports color attachments";
    auto params = glTextureParamsFor(attachmentDesc.format);
    auto tex = new Z3DTexture(::glTextureTargetFor(descriptor.dimension),
                              params.internalFormat,
                              textureExtentFor(descriptor),
                              params.format,
                              params.type,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    slot.fbo->attachTextureToFBO(tex, GLenum(GL_COLOR_ATTACHMENT0 + i), true);
    ++slot.attachments;
  }
  if (slot.attachments > before) {
    ++m_changeCounter;
  }
  ScratchImageDescriptor slotDescriptor = descriptor;
  slotDescriptor.attachments = makeColorAttachments(slot.attachments, ScratchFormat::RGBA32UI);
  updateSlotDescriptor(slot.descriptor, slotDescriptor);
  slot.fbo->isFBOComplete();
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireBlockIdRenderTarget(const glm::uvec2& viewport,
                                                   int requestedAttachments,
                                                   double scale,
                                                   std::optional<RenderBackend> backend)
{
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);
  uint32_t requestedAttachmentCount =
    (requestedAttachments > 0) ? static_cast<uint32_t>(requestedAttachments) : blockIdMaxAttachments();

  // compute scaled exact size
  double scaleFactor = (scale > 0.0) ? scale : blockIdScale();
  glm::uvec2 size = viewport;
  if (scaleFactor > 0.0 && scaleFactor != 1.0) {
    size.x = static_cast<uint32_t>(std::ceil(viewport.x * scaleFactor));
    size.y = static_cast<uint32_t>(std::ceil(viewport.y * scaleFactor));
  }
  ScratchImageDescriptor descriptor = makeBlockIdDescriptor(size, requestedAttachmentCount);

  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }

  const uint64_t createBefore = m_creationCounter;
  BlockIdRenderTargetSlot* slot = acquireFreeBlockIdSlot(size, requestedAttachmentCount);
  CHECK(slot != nullptr) << "Failed to acquire Block ID slot";
  const bool createdSlot = (m_creationCounter != createBefore);
  const bool needsResize = slot->fbo->size() != size;
  const bool needsAttachmentGrow = slot->attachments < requestedAttachmentCount;
  growSlotIfNeeded(*slot, descriptor);

  markSlotAcquired(*slot);
  uint32_t usableAttachments = std::min<uint32_t>(requestedAttachmentCount, slot->attachments);
  ScratchImageDescriptor leaseDescriptor = slot->descriptor;
  if (usableAttachments != slot->attachments) {
    leaseDescriptor.attachments = makeColorAttachments(usableAttachments, ScratchFormat::RGBA32UI);
  }
  auto lease = makeLeaseFromSlot(slot, usableAttachments, RenderBackend::OpenGL);
  lease.descriptor = std::move(leaseDescriptor);
  const ScratchAcquireKind acquireKind =
    createdSlot
      ? ScratchAcquireKind::NewSlot
      : ((needsResize || needsAttachmentGrow) ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

void Z3DScratchResourcePool::maybeTrimAfterAcquire()
{
  if constexpr (kTrimAcquireInterval == 0) {
    return;
  }

  static_assert((kTrimAcquireInterval & (kTrimAcquireInterval - 1)) == 0,
                "Trim interval must be a power of two for bitmask check");

  if ((m_usageTick & (kTrimAcquireInterval - 1)) != 0) {
    return;
  }

  performTrim(kTrimAgeTicks, false);
}

size_t Z3DScratchResourcePool::performTrim(uint64_t ageThreshold, bool logSummary)
{
  const auto shouldTrim = [&](uint64_t lastUseTick) {
    if (ageThreshold == 0) {
      return true;
    }
    if (lastUseTick == 0 || m_usageTick <= lastUseTick) {
      return ageThreshold == 0;
    }
    return (m_usageTick - lastUseTick) >= ageThreshold;
  };

  size_t totalFreed = 0;

  auto trimCategory = [&](const char* label, auto& vec) {
    size_t kept = 0;
    const size_t freed = std::erase_if(vec, [&](auto& uptr) {
      auto& slot = *uptr;
      if (slot.inUse) {
        ++kept;
        return false;
      }
      return shouldTrim(slot.lastUseTick);
    });
    if (freed > 0) {
      totalFreed += freed;
      ++m_changeCounter;
    }
    if (logSummary && (kept || freed)) {
      LOG(INFO) << fmt::format("trim(): {} kept_in_use={} freed={}", label, kept, freed);
    }
  };

  trimCategory("BlockID", m_blockIdRenderTargetSlots);
  trimCategory("EntryExit", m_entryExitRenderTargetSlots);
  trimCategory("LayerArray", m_layerArrayRenderTargetSlots);
  trimCategory("Temp2D", m_temp2DRenderTargetSlots);
  trimCategory("RaycastAccum", m_raycastAccumulatorSlots);
  trimCategory("DualDepthPeel", m_dualDepthPeelSlots);
  trimCategory("WeightedAverage", m_weightedAverageSlots);
  trimCategory("WeightedBlended", m_weightedBlendedSlots);

  for (size_t usageIndex = 0; usageIndex < m_vulkanSlots.size(); ++usageIndex) {
    auto& slots = m_vulkanSlots[usageIndex];
    size_t evicted = 0;
    for (auto& slot : slots) {
      if (slot->inUse || !slot->image || !shouldTrim(slot->lastUseTick)) {
        continue;
      }
      const uint64_t slotBytes = slot->image->estimatedResidentBytes();
      if (slotBytes == 0) {
        continue;
      }
      slot->image->releaseDeviceResources();
      recordVulkanBudgetTrim(static_cast<ScratchImageUsage>(usageIndex), 1);
      ++evicted;
    }
    if (evicted > 0) {
      totalFreed += evicted;
      ++m_changeCounter;
    }
  }

  if (!logSummary && totalFreed > 0) {
    VLOG(1) << fmt::format("scratch pool auto trim: freed {} slots at usage tick {}", totalFreed, m_usageTick);
  }

  return totalFreed;
}

void Z3DScratchResourcePool::pumpVulkanScratchReleases(VulkanScratchReclaimMode mode)
{
  if (m_vulkanMemoryPressureHandler) {
    m_vulkanMemoryPressureHandler(mode);
    return;
  }

  if (mode != VulkanScratchReclaimMode::PollCompleted && m_externalVkDevice != nullptr) {
    m_externalVkDevice->frameExecutor().waitForAllInFlight();
  }
}

Z3DScratchResourcePool::VulkanScratchBackingReclaimStats
Z3DScratchResourcePool::evictAllFreeVulkanSlotsWithStats(const VulkanScratchSlot* protectedSlot,
                                                         bool logSummary,
                                                         uint64_t targetBytes)
{
  struct EvictCandidate
  {
    VulkanScratchSlot* slot = nullptr;
    ScratchImageUsage usage = ScratchImageUsage::BlockId;
    uint64_t bytes = 0;
    uint64_t lastUseTick = 0;
  };

  std::vector<EvictCandidate> candidates;
  for (size_t usageIndex = 0; usageIndex < m_vulkanSlots.size(); ++usageIndex) {
    auto& slots = m_vulkanSlots[usageIndex];
    for (auto& slot : slots) {
      if (slot.get() == protectedSlot || slot->inUse || !slot->image) {
        continue;
      }
      const uint64_t slotBytes = slot->image->estimatedResidentBytes();
      if (slotBytes == 0) {
        continue;
      }
      candidates.push_back(EvictCandidate{.slot = slot.get(),
                                          .usage = static_cast<ScratchImageUsage>(usageIndex),
                                          .bytes = slotBytes,
                                          .lastUseTick = slot->lastUseTick});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const EvictCandidate& a, const EvictCandidate& b) {
    if (a.lastUseTick != b.lastUseTick) {
      return a.lastUseTick < b.lastUseTick;
    }
    if (a.bytes != b.bytes) {
      return a.bytes > b.bytes;
    }
    return static_cast<uint8_t>(a.usage) < static_cast<uint8_t>(b.usage);
  });

  VulkanScratchBackingReclaimStats stats{};
  for (const EvictCandidate& candidate : candidates) {
    CHECK(candidate.slot != nullptr) << "Vulkan scratch eviction candidate missing slot";
    CHECK(candidate.slot->image != nullptr) << "Vulkan scratch eviction candidate lost image";
    candidate.slot->image->releaseDeviceResources();
    recordVulkanEvictAll(candidate.usage, 1);
    stats.bytesReleased += candidate.bytes;
    stats.slotsEvicted++;
    if (targetBytes > 0u && stats.bytesReleased >= targetBytes) {
      break;
    }
  }

  if (stats.slotsEvicted > 0) {
    ++m_changeCounter;
    if (logSummary) {
      VLOG(1) << fmt::format("scratch pool Vulkan evict-free-backing: evicted {} slots ({:.2f} MiB) target={}B",
                             stats.slotsEvicted,
                             static_cast<double>(stats.bytesReleased) / (1024.0 * 1024.0),
                             targetBytes);
    }
  }
  return stats;
}

size_t Z3DScratchResourcePool::evictAllFreeVulkanSlots(const VulkanScratchSlot* protectedSlot, bool logSummary)
{
  return evictAllFreeVulkanSlotsWithStats(protectedSlot, logSummary).slotsEvicted;
}

void Z3DScratchResourcePool::reclaimVulkanScratchMemory(VulkanScratchReclaimMode mode)
{
  pumpVulkanScratchReleases(mode);
}

Z3DScratchResourcePool::VulkanScratchBackingReclaimStats
Z3DScratchResourcePool::reclaimFreeVulkanScratchBacking(std::string_view reason, uint64_t targetBytes)
{
  const auto stats = evictAllFreeVulkanSlotsWithStats(nullptr, true, targetBytes);
  if (stats.slotsEvicted > 0u) {
    VLOG(1) << fmt::format("scratch pool Vulkan broker reclaim: reason='{}' slots={} bytes={}B target={}B",
                           reason.empty() ? "<unspecified>" : std::string(reason),
                           stats.slotsEvicted,
                           stats.bytesReleased,
                           targetBytes);
  }
  return stats;
}

std::vector<Z3DScratchResourcePool::VulkanScratchBackingCandidate>
Z3DScratchResourcePool::vulkanScratchBackingCandidates() const
{
  std::vector<VulkanScratchBackingCandidate> candidates;
  const bool canHostBackLeasedScratch =
    m_externalVkDevice != nullptr && m_externalVkDevice->frameExecutor().inFlightCount() == 0u;
  for (size_t usageIndex = 0; usageIndex < m_vulkanSlots.size(); ++usageIndex) {
    const auto usage = static_cast<ScratchImageUsage>(usageIndex);
    const auto& slots = m_vulkanSlots[usageIndex];
    for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
      const auto& slot = slots[slotIndex];
      if (!slot || !slot->image) {
        continue;
      }
      const uint64_t residentBytes = slot->image->estimatedResidentBytes();
      if (residentBytes == 0u) {
        continue;
      }

      uint32_t pinCount = slot->releasePending || (slot->inUse && !canHostBackLeasedScratch) ? 1u : 0u;
      for (auto* texture : m_vulkanResidencyProtectedTextures) {
        if (slot->image->containsTexture(texture)) {
          CHECK(pinCount < std::numeric_limits<uint32_t>::max()) << "Vulkan scratch candidate pin count overflow";
          ++pinCount;
        }
      }

      candidates.push_back(VulkanScratchBackingCandidate{
        .usage = usage,
        .slotIndex = slotIndex,
        .residentBytes = residentBytes,
        .lastUseTick = slot->lastUseTick,
        .pinCount = pinCount,
        .inUse = slot->inUse,
        .releasePending = slot->releasePending,
        .label = fmt::format("scratch_{} slot={} inUse={}", scratchUsageLabel(usage), slotIndex, slot->inUse ? 1 : 0)});
    }
  }
  return candidates;
}

Z3DScratchResourcePool::VulkanScratchBackingReclaimStats
Z3DScratchResourcePool::reclaimVulkanScratchBackingCandidate(ScratchImageUsage usage,
                                                             size_t slotIndex,
                                                             std::string_view reason)
{
  VulkanScratchBackingReclaimStats stats{};
  const size_t usageIndex = static_cast<size_t>(usage);
  CHECK_LT(usageIndex, m_vulkanSlots.size()) << "Invalid Vulkan scratch backing candidate usage";
  auto& slots = m_vulkanSlots[usageIndex];
  if (slotIndex >= slots.size()) {
    return stats;
  }

  auto& slot = slots[slotIndex];
  if (!slot || !slot->image || slot->releasePending) {
    return stats;
  }
  if (slot->inUse && (m_externalVkDevice == nullptr || m_externalVkDevice->frameExecutor().inFlightCount() != 0u)) {
    return stats;
  }
  for (auto* texture : m_vulkanResidencyProtectedTextures) {
    if (slot->image->containsTexture(texture)) {
      return stats;
    }
  }

  const uint64_t residentBytes = slot->image->estimatedResidentBytes();
  if (residentBytes == 0u) {
    return stats;
  }

  const uint64_t releasedBytes =
    slot->inUse ? slot->image->releaseDeviceResourcesToHost() : (slot->image->releaseDeviceResources(), residentBytes);
  if (releasedBytes == 0u) {
    return stats;
  }

  recordVulkanEvictAll(usage, 1);
  ++m_changeCounter;
  stats.bytesReleased = releasedBytes;
  stats.slotsEvicted = 1u;
  VLOG(1) << fmt::format("scratch pool Vulkan broker candidate evict: usage={} slot={} inUse={} bytes={}B reason='{}'",
                         scratchUsageLabel(usage),
                         slotIndex,
                         slot->inUse ? 1 : 0,
                         releasedBytes,
                         reason.empty() ? "<unspecified>" : std::string(reason));
  return stats;
}

Z3DScratchResourcePool::VulkanScratchProtectionScope::VulkanScratchProtectionScope(
  Z3DScratchResourcePool* pool,
  std::vector<ZVulkanTexture*> textures)
  : m_pool(pool)
  , m_textures(std::move(textures))
{}

Z3DScratchResourcePool::VulkanScratchProtectionScope::~VulkanScratchProtectionScope()
{
  release();
}

Z3DScratchResourcePool::VulkanScratchProtectionScope::VulkanScratchProtectionScope(
  VulkanScratchProtectionScope&& other) noexcept
  : m_pool(other.m_pool)
  , m_textures(std::move(other.m_textures))
{
  other.m_pool = nullptr;
}

Z3DScratchResourcePool::VulkanScratchProtectionScope&
Z3DScratchResourcePool::VulkanScratchProtectionScope::operator=(VulkanScratchProtectionScope&& other) noexcept
{
  if (this != &other) {
    release();
    m_pool = other.m_pool;
    m_textures = std::move(other.m_textures);
    other.m_pool = nullptr;
  }
  return *this;
}

void Z3DScratchResourcePool::VulkanScratchProtectionScope::release()
{
  if (m_pool == nullptr || m_textures.empty()) {
    m_pool = nullptr;
    m_textures.clear();
    return;
  }
  m_pool->releaseVulkanScratchTextureProtections(
    std::span<ZVulkanTexture* const>(m_textures.data(), m_textures.size()));
  m_pool = nullptr;
  m_textures.clear();
}

Z3DScratchResourcePool::VulkanScratchProtectionScope
Z3DScratchResourcePool::protectVulkanScratchTextures(std::span<ZVulkanTexture* const> textures)
{
  auto containsScratchTexture = [&](const ZVulkanTexture* texture) {
    for (const auto& slots : m_vulkanSlots) {
      for (const auto& slot : slots) {
        if (slot->image && slot->image->containsTexture(texture)) {
          return true;
        }
      }
    }
    return false;
  };

  std::vector<ZVulkanTexture*> protectedTextures;
  protectedTextures.reserve(textures.size());
  for (auto* texture : textures) {
    if (texture == nullptr) {
      continue;
    }
    if (!containsScratchTexture(texture)) {
      continue;
    }
    if (std::find(protectedTextures.begin(), protectedTextures.end(), texture) != protectedTextures.end()) {
      continue;
    }
    protectedTextures.push_back(texture);
    m_vulkanResidencyProtectedTextures.push_back(texture);
  }
  if (protectedTextures.empty()) {
    return VulkanScratchProtectionScope{};
  }
  return VulkanScratchProtectionScope(this, std::move(protectedTextures));
}

void Z3DScratchResourcePool::releaseVulkanScratchTextureProtections(std::span<ZVulkanTexture* const> textures)
{
  for (auto* texture : textures) {
    if (texture == nullptr) {
      continue;
    }
    const auto it =
      std::find(m_vulkanResidencyProtectedTextures.begin(), m_vulkanResidencyProtectedTextures.end(), texture);
    CHECK(it != m_vulkanResidencyProtectedTextures.end())
      << "Vulkan scratch protection release without a matching acquire";
    m_vulkanResidencyProtectedTextures.erase(it);
  }
}

Z3DScratchResourcePool::VulkanScratchBackingReclaimStats
Z3DScratchResourcePool::reclaimColdVulkanScratchBacking(std::span<ZVulkanTexture* const> protectedTextures,
                                                        std::string_view reason,
                                                        uint64_t targetBytes)
{
  struct EvictCandidate
  {
    VulkanScratchSlot* slot = nullptr;
    ScratchImageUsage usage = ScratchImageUsage::BlockId;
    uint64_t bytes = 0;
    uint64_t lastUseTick = 0;
    bool inUse = false;
  };

  std::unordered_set<ZVulkanTexture*> protectedSet;
  protectedSet.reserve(protectedTextures.size() + m_vulkanResidencyProtectedTextures.size());
  for (auto* texture : m_vulkanResidencyProtectedTextures) {
    if (texture != nullptr) {
      protectedSet.insert(texture);
    }
  }
  for (auto* texture : protectedTextures) {
    if (texture != nullptr) {
      protectedSet.insert(texture);
    }
  }

  auto slotIsProtected = [&](const VulkanScratchSlot& slot) {
    if (!slot.image) {
      return false;
    }
    for (auto* texture : protectedSet) {
      if (slot.image->containsTexture(texture)) {
        return true;
      }
    }
    return false;
  };

  std::vector<EvictCandidate> candidates;
  for (size_t usageIndex = 0; usageIndex < m_vulkanSlots.size(); ++usageIndex) {
    auto& slots = m_vulkanSlots[usageIndex];
    for (auto& slot : slots) {
      if (!slot->image || slot->releasePending || slotIsProtected(*slot)) {
        continue;
      }
      const uint64_t slotBytes = slot->image->estimatedResidentBytes();
      if (slotBytes == 0u) {
        continue;
      }
      candidates.push_back(EvictCandidate{.slot = slot.get(),
                                          .usage = static_cast<ScratchImageUsage>(usageIndex),
                                          .bytes = slotBytes,
                                          .lastUseTick = slot->lastUseTick,
                                          .inUse = slot->inUse});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const EvictCandidate& a, const EvictCandidate& b) {
    if (a.lastUseTick != b.lastUseTick) {
      return a.lastUseTick < b.lastUseTick;
    }
    if (a.bytes != b.bytes) {
      return a.bytes > b.bytes;
    }
    return static_cast<uint8_t>(a.usage) < static_cast<uint8_t>(b.usage);
  });

  VulkanScratchBackingReclaimStats stats{};
  for (const EvictCandidate& candidate : candidates) {
    CHECK(candidate.slot != nullptr) << "Vulkan scratch cold eviction candidate missing slot";
    CHECK(candidate.slot->image != nullptr) << "Vulkan scratch cold eviction candidate lost image";
    const uint64_t released = candidate.inUse ? candidate.slot->image->releaseDeviceResourcesToHost()
                                              : (candidate.slot->image->releaseDeviceResources(), candidate.bytes);
    if (released == 0u) {
      continue;
    }
    recordVulkanEvictAll(candidate.usage, 1);
    stats.bytesReleased += released;
    stats.slotsEvicted++;
    if (targetBytes > 0u && stats.bytesReleased >= targetBytes) {
      break;
    }
  }

  if (stats.slotsEvicted > 0u) {
    ++m_changeCounter;
    VLOG(1) << fmt::format(
      "scratch pool Vulkan evict-cold-backing: reason='{}' protected_textures={} slots={} bytes={}B target={}B",
      reason.empty() ? "<unspecified>" : std::string(reason),
      protectedSet.size(),
      stats.slotsEvicted,
      stats.bytesReleased,
      targetBytes);
  }
  return stats;
}

void Z3DScratchResourcePool::prepareVulkanScratchTexturesForPass(std::span<const VulkanScratchTextureUse> uses,
                                                                 std::string_view reason)
{
  if (uses.empty()) {
    return;
  }

  ZVulkanDevice& dev = ensureVulkanDevice();
  pumpVulkanScratchReleases(VulkanScratchReclaimMode::PollCompleted);

  std::vector<VulkanScratchTextureUse> dedupedUses;
  dedupedUses.reserve(uses.size());
  for (const auto& use : uses) {
    if (use.texture == nullptr) {
      continue;
    }
    auto it = std::find_if(dedupedUses.begin(), dedupedUses.end(), [&](const VulkanScratchTextureUse& existing) {
      return existing.texture == use.texture;
    });
    if (it == dedupedUses.end()) {
      dedupedUses.push_back(use);
    } else {
      it->contentsRequired = it->contentsRequired || use.contentsRequired;
    }
  }
  if (dedupedUses.empty()) {
    return;
  }

  std::vector<ZVulkanTexture*> protectedTextures;
  protectedTextures.reserve(dedupedUses.size());
  for (const auto& use : dedupedUses) {
    protectedTextures.push_back(use.texture);
  }
  auto protectedGuard =
    protectVulkanScratchTextures(std::span<ZVulkanTexture* const>(protectedTextures.data(), protectedTextures.size()));
  (void)protectedGuard;

  std::vector<ZVulkanScratchImage*> hotImages;
  auto addHotImageForTexture = [&](ZVulkanTexture* texture) {
    for (auto& slots : m_vulkanSlots) {
      for (auto& slot : slots) {
        if (!slot->image || !slot->image->containsTexture(texture)) {
          continue;
        }
        if (std::find(hotImages.begin(), hotImages.end(), slot->image.get()) == hotImages.end()) {
          hotImages.push_back(slot->image.get());
        }
        return;
      }
    }
  };
  for (auto* texture : protectedTextures) {
    addHotImageForTexture(texture);
  }
  if (hotImages.empty()) {
    return;
  }

  uint64_t hotTotalBytes = 0u;
  uint64_t missingBytes = 0u;
  for (auto* image : hotImages) {
    CHECK(image != nullptr);
    const uint64_t total = image->estimatedTotalBytes();
    const uint64_t resident = image->estimatedResidentBytes();
    hotTotalBytes += total;
    if (total > resident) {
      missingBytes += total - resident;
    }
  }

  const uint64_t strictBudget = dev.residencyManager().effectiveBrokerBudgetBytes();
  if (dev.residencyManager().strictBudgetActive() && strictBudget > 0u && hotTotalBytes > strictBudget) {
    throw ZException(fmt::format(
      "Vulkan scratch pass working set exceeds strict residency budget: hot_set={}B budget={}B textures={} slots={} reason='{}'",
      hotTotalBytes,
      strictBudget,
      protectedTextures.size(),
      hotImages.size(),
      reason.empty() ? "<unspecified>" : std::string(reason)));
  }

  auto strictBudgetReclaimBytes = [&](uint64_t incomingBytes) {
    const auto pressure = dev.residencyManager().allocationPressureFor(incomingBytes);
    return pressure.needsReclaim() ? pressure.reclaimBytes : 0u;
  };

  auto reclaimForScratchPassBudget = [&](uint64_t incomingBytes) {
    while (true) {
      uint64_t targetBytes = strictBudgetReclaimBytes(incomingBytes);
      if (targetBytes == 0u) {
        return;
      }
      pumpVulkanScratchReleases(VulkanScratchReclaimMode::WaitForIdle);

      const auto coldScratchStats = reclaimColdVulkanScratchBacking(
        std::span<ZVulkanTexture* const>(protectedTextures.data(), protectedTextures.size()),
        reason,
        targetBytes);
      if (coldScratchStats.bytesReleased > 0u || coldScratchStats.slotsEvicted > 0u) {
        continue;
      }

      const auto stats = dev.residencyManager().reclaimMemory(
        ZVulkanResidencyManager::ReclaimRequest{.requestClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking,
                                                .requestedBytes = targetBytes,
                                                .force = false,
                                                .reason = reason});

      if (stats.resourcesReleased == 0u && stats.bytesReleased == 0u) {
        return;
      }
    }
  };

  reclaimForScratchPassBudget(missingBytes);

  for (auto* image : hotImages) {
    CHECK(image != nullptr);
    if (image->resident()) {
      continue;
    }
    try {
      image->makeResidentForPass(std::span<const VulkanScratchTextureUse>(dedupedUses.data(), dedupedUses.size()));
    }
    catch (const std::exception& e) {
      VLOG(2) << fmt::format("Vulkan scratch pass restore retrying after cold reclaim: reason='{}' error={}",
                             reason.empty() ? "<unspecified>" : std::string(reason),
                             e.what());
      pumpVulkanScratchReleases(VulkanScratchReclaimMode::WaitForIdle);
      (void)dev.residencyManager().reclaimMemory(
        ZVulkanResidencyManager::ReclaimRequest{.requestClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking,
                                                .requestedBytes = image->estimatedTotalBytes(),
                                                .force = false,
                                                .reason = reason});
      try {
        image->makeResidentForPass(std::span<const VulkanScratchTextureUse>(dedupedUses.data(), dedupedUses.size()));
      }
      catch (const std::exception& retryError) {
        throw ZException(
          fmt::format("Vulkan scratch pass restore failed after cold reclaim: reason='{}' error={} scratch={}",
                      reason.empty() ? "<unspecified>" : std::string(reason),
                      retryError.what(),
                      describeMemoryUsage(false)));
      }
    }
  }

  reclaimForScratchPassBudget(0u);

  if (dev.residencyManager().strictBudgetActive()) {
    const auto pressure = dev.residencyManager().allocationPressureFor(0u);
    if (pressure.needsReclaim()) {
      throw ZException(fmt::format(
        "Vulkan scratch pass restore exceeded strict residency budget: usage={}B budget={}B over={}B hot_set={}B reason='{}' memory_by_class=[{}] scratch={}",
        pressure.usageBytes,
        pressure.budgetBytes,
        pressure.reclaimBytes,
        hotTotalBytes,
        reason.empty() ? "<unspecified>" : std::string(reason),
        dev.residencyManager().describeMemoryByClass(),
        describeMemoryUsage(false)));
    }
  }
}

Z3DScratchResourcePool::VulkanScratchBackingReport Z3DScratchResourcePool::vulkanScratchBackingReport() const
{
  VulkanScratchBackingReport report{};
  for (const auto& slots : m_vulkanSlots) {
    for (const auto& slot : slots) {
      if (!slot->image) {
        continue;
      }
      if (slot->inUse) {
        report.inUseSlots++;
      }
      if (slot->releasePending) {
        report.releasePendingSlots++;
      }
      for (auto* texture : m_vulkanResidencyProtectedTextures) {
        if (slot->image->containsTexture(texture)) {
          report.protectedSlots++;
          break;
        }
      }
      const uint64_t bytes = slot->image->estimatedResidentBytes();
      if (bytes == 0) {
        continue;
      }
      report.residentSlots++;
      report.residentBytes += bytes;
    }
  }
  return report;
}

void Z3DScratchResourcePool::trim()
{
  performTrim(0, true);
}

void Z3DScratchResourcePool::reset()
{
  m_blockIdRenderTargetSlots.clear();
  m_entryExitRenderTargetSlots.clear();
  m_layerArrayRenderTargetSlots.clear();
  m_temp2DRenderTargetSlots.clear();
  m_raycastAccumulatorSlots.clear();
  m_dualDepthPeelSlots.clear();
  m_weightedAverageSlots.clear();
  m_weightedBlendedSlots.clear();

  for (auto& slots : m_vulkanSlots) {
    slots.clear();
  }

  for (auto& entry : m_descriptionCache) {
    entry = {};
  }

  m_usageTick = 0;
  m_creationCounter = 0;
  m_changeCounter = 0;
  m_reuseStatsCounter = 0;
  m_glReuseStats = {};
  m_vulkanReuseStats = {};
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireEntryExitRenderTarget(const glm::uvec2& size,
                                                     uint32_t layers,
                                                     ScratchFormat colorFormat,
                                                     std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeEntryExitDescriptor(size, layers, colorFormat);
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);

  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }

  // Find a free slot with closest size, only consider matching internal format.
  EntryExitRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_entryExitRenderTargetSlots, size, [&](const EntryExitRenderTargetSlot& s) {
      return s.colorFormat == colorFormat;
    });

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    // Existing slot path: single-pass resize; grow Z only if XY unchanged.
    const uint32_t prevLayers = slot->layers;
    const bool xyChanged = (slot->fbo->size() != size);
    const uint32_t desiredZ = xyChanged ? layers : std::max<uint32_t>(slot->layers, layers);
    const bool resized = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->layers = desiredZ;
    slot->colorFormat = colorFormat;
    if (resized || desiredZ != prevLayers) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    ScratchImageDescriptor slotDescriptor = descriptor;
    slotDescriptor.layers = desiredZ;
    updateSlotDescriptor(slot->descriptor, slotDescriptor);
    markSlotAcquired(*slot);
    acquireKind = (xyChanged || desiredZ != prevLayers)
                    ? ScratchAcquireKind::RetargetReuse
                    : (prevLayers > layers ? ScratchAcquireKind::CompatibleReuse : ScratchAcquireKind::ExactReuse);
  } else {
    // Creation path: make a fresh slot and attachments.
    m_entryExitRenderTargetSlots.emplace_back(std::make_unique<EntryExitRenderTargetSlot>());
    slot = m_entryExitRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;

    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    slot->layers = layers;
    slot->colorFormat = colorFormat;
    updateSlotDescriptor(slot->descriptor, descriptor);
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireLayerArrayRenderTarget(const glm::uvec2& size,
                                                      uint32_t layers,
                                                      ScratchFormat colorFormat,
                                                      ScratchFormat depthFormat,
                                                      std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeLayerArrayDescriptor(size, layers, colorFormat, depthFormat);
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);

  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }

  // Find a free slot with closest size; only consider matching color+depth formats.
  LayerArrayRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_layerArrayRenderTargetSlots, size, [&](const LayerArrayRenderTargetSlot& s) {
      return s.colorFormat == colorFormat && s.depthFormat == depthFormat;
    });

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    // Existing slot path: single-pass resize; grow Z only if XY unchanged.
    const uint32_t prevLayers = slot->layers;
    const bool xyChanged = (slot->fbo->size() != size);
    const uint32_t desiredZ = xyChanged ? layers : std::max<uint32_t>(slot->layers, layers);
    const bool resized = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->layers = desiredZ;
    slot->colorFormat = colorFormat;
    slot->depthFormat = depthFormat;
    if (resized || desiredZ != prevLayers) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    ScratchImageDescriptor slotDescriptor = descriptor;
    slotDescriptor.layers = desiredZ;
    updateSlotDescriptor(slot->descriptor, slotDescriptor);
    markSlotAcquired(*slot);
    acquireKind = (xyChanged || desiredZ != prevLayers)
                    ? ScratchAcquireKind::RetargetReuse
                    : (prevLayers > layers ? ScratchAcquireKind::CompatibleReuse : ScratchAcquireKind::ExactReuse);
  } else {
    // Creation path: make a fresh slot and attachments.
    m_layerArrayRenderTargetSlots.emplace_back(std::make_unique<LayerArrayRenderTargetSlot>());
    slot = m_layerArrayRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;

    attachTexturesForDescriptor(*slot->fbo, descriptor);

    slot->fbo->isFBOComplete();
    slot->layers = layers;
    slot->colorFormat = colorFormat;
    slot->depthFormat = depthFormat;
    updateSlotDescriptor(slot->descriptor, descriptor);
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireRaycastAccumulatorRenderTarget(const glm::uvec2& size,
                                                              std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor =
    makeRaycastAccumulatorDescriptor(size, ScratchFormat::RGBA16, ScratchFormat::RG32F);

  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);

  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }

  RaycastAccumulatorSlot* slot =
    findClosestFreeSlotIf(m_raycastAccumulatorSlots, size, [](const RaycastAccumulatorSlot&) {
      return true;
    });

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->colorFormat = ScratchFormat::RGBA16;
    slot->accumulatorFormat = ScratchFormat::RG32F;
    updateSlotDescriptor(slot->descriptor, descriptor);
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    markSlotAcquired(*slot);
    acquireKind = changed ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse;
  } else {
    m_raycastAccumulatorSlots.emplace_back(std::make_unique<RaycastAccumulatorSlot>());
    slot = m_raycastAccumulatorSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;

    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    slot->colorFormat = ScratchFormat::RGBA16;
    slot->accumulatorFormat = ScratchFormat::RG32F;
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireTempRenderTarget2D(const glm::uvec2& size,
                                                  ScratchFormat colorFormat,
                                                  ScratchFormat depthFormat,
                                                  std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeTemp2DDescriptor(size, colorFormat, depthFormat);

  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);

  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }

  // Find a free slot with closest size; only consider matching color+depth formats.
  Temp2DRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_temp2DRenderTargetSlots, size, [&](const Temp2DRenderTargetSlot& s) {
      return s.colorFormat == colorFormat && s.depthFormat == depthFormat;
    });

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    // Existing slot path: adjust size if needed and retain formats.
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->colorFormat = colorFormat;
    slot->depthFormat = depthFormat;
    updateSlotDescriptor(slot->descriptor, descriptor);
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    markSlotAcquired(*slot);
    acquireKind = changed ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse;
  } else {
    // Creation path: make a fresh slot and attachments.
    m_temp2DRenderTargetSlots.emplace_back(std::make_unique<Temp2DRenderTargetSlot>());
    slot = m_temp2DRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;

    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    slot->colorFormat = colorFormat;
    slot->depthFormat = depthFormat;
    updateSlotDescriptor(slot->descriptor, descriptor);
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireDualDepthPeelRenderTarget(const glm::uvec2& size, std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeDualDepthPeelDescriptor(size);
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);
  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }
  DualDepthPeelSlot* slot = findClosestFreeSlot(m_dualDepthPeelSlots, size);

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    updateSlotDescriptor(slot->descriptor, descriptor);
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    markSlotAcquired(*slot);
    acquireKind = changed ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse;
  } else {
    m_dualDepthPeelSlots.emplace_back(std::make_unique<DualDepthPeelSlot>());
    slot = m_dualDepthPeelSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 8, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireWeightedAverageRenderTarget(const glm::uvec2& size, std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeWeightedAverageDescriptor(size);
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);
  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }
  WeightedAverageSlot* slot = findClosestFreeSlot(m_weightedAverageSlots, size);

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    updateSlotDescriptor(slot->descriptor, descriptor);
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    markSlotAcquired(*slot);
    acquireKind = changed ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse;
  } else {
    m_weightedAverageSlots.emplace_back(std::make_unique<WeightedAverageSlot>());
    slot = m_weightedAverageSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireWeightedBlendedRenderTarget(const glm::uvec2& size, std::optional<RenderBackend> backend)
{
  ScratchImageDescriptor descriptor = makeWeightedBlendedDescriptor(size);
  const RenderBackend resolvedBackend = backend.value_or(m_defaultBackend);
  if (resolvedBackend == RenderBackend::Vulkan) {
    return acquireVulkanScratchImage(descriptor);
  }
  WeightedBlendedSlot* slot = findClosestFreeSlot(m_weightedBlendedSlots, size);

  ScratchAcquireKind acquireKind = ScratchAcquireKind::ExactReuse;
  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    updateSlotDescriptor(slot->descriptor, descriptor);
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
    markSlotAcquired(*slot);
    acquireKind = changed ? ScratchAcquireKind::RetargetReuse : ScratchAcquireKind::ExactReuse;
  } else {
    m_weightedBlendedSlots.emplace_back(std::make_unique<WeightedBlendedSlot>());
    slot = m_weightedBlendedSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
    acquireKind = ScratchAcquireKind::NewSlot;
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
  recordScratchAcquire(RenderBackend::OpenGL, lease.descriptor, acquireKind, false);
  maybeTrimAfterAcquire();
  return lease;
}

Z3DScratchResourcePool::VulkanEnvironment& Z3DScratchResourcePool::ensureVulkanEnvironment()
{
  if (!m_vulkanEnvironment) {
    auto env = std::make_unique<VulkanEnvironment>();
    // No device/context created here anymore; device must be injected via setVulkanDevice()
    m_vulkanEnvironment = std::move(env);
  }
  return *m_vulkanEnvironment;
}

std::vector<std::unique_ptr<Z3DScratchResourcePool::VulkanScratchSlot>>&
Z3DScratchResourcePool::vulkanSlotsForUsage(ScratchImageUsage usage)
{
  const auto index = static_cast<size_t>(usage);
  CHECK(index < m_vulkanSlots.size()) << "Invalid scratch usage index";
  return m_vulkanSlots[index];
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireVulkanScratchImage(const ScratchImageDescriptor& descriptor)
{
  // Ensure a device is available (external or internal)
  ZVulkanDevice& dev = ensureVulkanDevice();
  pumpVulkanScratchReleases(VulkanScratchReclaimMode::PollCompleted);
  auto& slots = vulkanSlotsForUsage(descriptor.usage);

  auto deviceBudgetText = [&dev]() {
    const auto budget = dev.deviceLocalBudget();
    return fmt::format("device_usage={} device_budget={}", budget.usageBytes, budget.budgetBytes);
  };
  auto recoverBeforeRetry =
    [&](const char* operation, const VulkanScratchSlot* protectedSlot, std::string_view failure) {
      recordVulkanAllocationRecovery(descriptor.usage);
      LOG(WARNING) << fmt::format("Vulkan scratch {} failed before reclaim: {}; {}; {}; scratch={}",
                                  operation,
                                  failure,
                                  describeScratchDescriptor(descriptor),
                                  deviceBudgetText(),
                                  describeMemoryUsage(false));
      pumpVulkanScratchReleases(VulkanScratchReclaimMode::WaitForIdle);
      evictAllFreeVulkanSlots(protectedSlot, true);
    };
  auto retryFailure = [&](const char* operation, std::string_view failure) {
    return ZException(fmt::format("Vulkan scratch {} failed after reclaim: {}; {}; {}; scratch={}",
                                  operation,
                                  failure,
                                  describeScratchDescriptor(descriptor),
                                  deviceBudgetText(),
                                  describeMemoryUsage(false)));
  };
  auto createScratchImage = [&]() {
    try {
      return std::make_unique<ZVulkanScratchImage>(dev, descriptor);
    }
    catch (const std::exception& e) {
      recoverBeforeRetry("create", nullptr, e.what());
    }
    catch (...) {
      recoverBeforeRetry("create", nullptr, "unknown exception");
    }

    try {
      return std::make_unique<ZVulkanScratchImage>(dev, descriptor);
    }
    catch (const std::exception& e) {
      throw retryFailure("create", e.what());
    }
    catch (...) {
      throw retryFailure("create", "unknown exception");
    }
  };
  auto retargetScratchImage = [&](VulkanScratchSlot& target, const ScratchImageDescriptor& retargetDescriptor) {
    try {
      target.image->retarget(retargetDescriptor);
      return;
    }
    catch (const std::exception& e) {
      recoverBeforeRetry("retarget", &target, e.what());
    }
    catch (...) {
      recoverBeforeRetry("retarget", &target, "unknown exception");
    }

    try {
      target.image->retarget(retargetDescriptor);
    }
    catch (const std::exception& e) {
      throw retryFailure("retarget", e.what());
    }
    catch (...) {
      throw retryFailure("retarget", "unknown exception");
    }
  };
  auto findExactFreeSlot = [&]() -> VulkanScratchSlot* {
    for (auto& candidate : slots) {
      if (candidate->inUse) {
        continue;
      }
      if (candidate->descriptor.dimension != descriptor.dimension) {
        continue;
      }
      if (candidate->descriptor.size != descriptor.size) {
        continue;
      }
      if (candidate->descriptor.layers != descriptor.layers) {
        continue;
      }
      if (candidate->descriptor.attachments != descriptor.attachments) {
        continue;
      }
      return candidate.get();
    }
    return nullptr;
  };

  auto findCompatibleFreeSlot = [&](ScratchImageDescriptor& outDescriptor,
                                    ScratchAcquireKind& outAcquireKind,
                                    bool& outRetargeted) -> VulkanScratchSlot* {
    VulkanScratchSlot* reusable = nullptr;
    uint64_t bestDelta = std::numeric_limits<uint64_t>::max();
    const uint64_t desiredPixels =
      static_cast<uint64_t>(descriptor.size.x) * descriptor.size.y * std::max<uint32_t>(1u, descriptor.layers);
    for (auto& candidate : slots) {
      if (candidate->inUse) {
        continue;
      }
      if (candidate->descriptor.dimension != descriptor.dimension) {
        continue;
      }
      if (!attachmentsCompatibleForReuse(descriptor.usage, candidate->descriptor, descriptor)) {
        continue;
      }
      const auto& candDesc = candidate->descriptor;
      const uint64_t candidatePixels =
        static_cast<uint64_t>(candDesc.size.x) * candDesc.size.y * std::max<uint32_t>(1u, candDesc.layers);
      const uint64_t delta = desiredPixels > candidatePixels ? desiredPixels - candidatePixels
                                                             : candidatePixels - desiredPixels;
      if (!reusable || delta < bestDelta) {
        reusable = candidate.get();
        bestDelta = delta;
      }
    }

    if (!reusable) {
      return nullptr;
    }

    const auto& candDesc = reusable->descriptor;
    ScratchImageDescriptor reusableDescriptor = descriptor;
    if (descriptor.usage == ScratchImageUsage::BlockId && candDesc.attachments.size() > descriptor.attachments.size()) {
      reusableDescriptor.attachments = candDesc.attachments;
    }
    const bool needRetarget = candDesc.size != reusableDescriptor.size || candDesc.layers < reusableDescriptor.layers ||
                              candDesc.attachments != reusableDescriptor.attachments;
    if (needRetarget) {
      retargetScratchImage(*reusable, reusableDescriptor);
      outDescriptor = reusableDescriptor;
      outRetargeted = true;
      outAcquireKind = ScratchAcquireKind::RetargetReuse;
    } else {
      // Keep existing descriptor (may have equal/larger layers or, for BlockID,
      // more color attachments than requested).
      outDescriptor = candDesc;
      outRetargeted = false;
      outAcquireKind = ScratchAcquireKind::CompatibleReuse;
    }
    reusable->descriptor = outDescriptor;
    return reusable;
  };

  auto hasPendingReusableRelease = [&]() {
    for (const auto& candidate : slots) {
      if (!candidate->releasePending) {
        continue;
      }
      if (candidate->descriptor.dimension != descriptor.dimension) {
        continue;
      }
      if (attachmentsCompatibleForReuse(descriptor.usage, candidate->descriptor, descriptor)) {
        return true;
      }
    }
    return false;
  };

  VulkanScratchSlot* slot = findExactFreeSlot();
  bool retargeted = false;
  ScratchAcquireKind acquireKind = slot ? ScratchAcquireKind::ExactReuse : ScratchAcquireKind::NewSlot;
  ScratchImageDescriptor targetDescriptor = descriptor;
  if (!slot) {
    slot = findCompatibleFreeSlot(targetDescriptor, acquireKind, retargeted);
  }

  if (!slot && hasPendingReusableRelease()) {
    pumpVulkanScratchReleases(VulkanScratchReclaimMode::PollCompleted);
    slot = findExactFreeSlot();
    retargeted = false;
    acquireKind = slot ? ScratchAcquireKind::ExactReuse : ScratchAcquireKind::NewSlot;
    targetDescriptor = descriptor;
    if (!slot) {
      slot = findCompatibleFreeSlot(targetDescriptor, acquireKind, retargeted);
    }
  }

  if (!slot) {
    auto newSlot = std::make_unique<VulkanScratchSlot>();
    newSlot->image = createScratchImage();
    newSlot->descriptor = descriptor;
    targetDescriptor = descriptor;
    slot = newSlot.get();
    slots.emplace_back(std::move(newSlot));
    ++m_creationCounter;
  }

  slot->descriptor = targetDescriptor;
  if (retargeted) {
    ++m_changeCounter;
  }
  const bool residentRecreated = false;
  markSlotAcquired(*slot);
  auto lease = RenderTargetLease{};
  lease.descriptor = (descriptor.usage == ScratchImageUsage::BlockId) ? descriptor : slot->descriptor;
  lease.backend = RenderBackend::Vulkan;
  lease.vulkanImage = slot->image.get();
  lease.attachments = colorAttachmentCount(descriptor);
  // Always use the Vulkan deferred releaser so releases remain GPU-safe even if
  // the backend installs the scheduler after this lease is acquired (e.g.,
  // persistent output targets allocated before the first beginRender()).
  lease.releaser = RenderTargetLease::Releaser::forVulkanSlotDeferred(this, slot);
  recordScratchAcquire(RenderBackend::Vulkan, lease.descriptor, acquireKind, residentRecreated);
  return lease;
}

ZVulkanDevice* Z3DScratchResourcePool::vulkanDevice()
{
  return m_externalVkDevice;
}

ZVulkanDevice& Z3DScratchResourcePool::ensureVulkanDevice()
{
  CHECK(m_externalVkDevice != nullptr) << "External Vulkan device must be injected before Vulkan scratch allocation";
  return *m_externalVkDevice;
}

void Z3DScratchResourcePool::scheduleDeferredRelease(Z3DScratchResourcePool::VulkanScratchSlot* slot)
{
  if (!slot) {
    return;
  }
  slot->releasePending = true;
  if (m_vulkanReleaseScheduler) {
    // The scheduler executes later on the render thread after the relevant
    // frame-completion safe point, so Vulkan resources can be recycled or freed.
    m_vulkanReleaseScheduler([slot]() {
      slot->inUse = false;
      slot->releasePending = false;
    });
  } else {
    // Fallback: no scheduler is installed (startup/backend-switch edge cases).
    // Be conservative: ensure all in-flight frame-executor submissions have
    // completed before allowing this slot to be reused/retargeted.
    if (m_externalVkDevice) {
      m_externalVkDevice->frameExecutor().waitForAllInFlight();
    }
    slot->inUse = false;
    slot->releasePending = false;
    VLOG(1) << "scratch pool: Vulkan slot release drained in-flight work (no scheduler installed)";
  }
}

} // namespace nim
