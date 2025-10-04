#include "z3dscratchresourcepool.h"

#include "z3dgl.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"
#include <cmath>
#include <limits>
#include <algorithm>
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
  }
  CHECK(false) << "Unhandled ScratchFormat value";
  return {GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT};
}

std::string scratchFormatLabel(ScratchFormat format)
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
      return vk::Format::eD24UnormS8Uint;
  }
  CHECK(false) << "Unhandled ScratchFormat for Vulkan";
  return vk::Format::eR8G8B8A8Unorm;
}

vk::ImageAspectFlags vkAspectMaskFor(ScratchAttachmentKind kind)
{
  switch (kind) {
    case ScratchAttachmentKind::Color:
      return vk::ImageAspectFlagBits::eColor;
    case ScratchAttachmentKind::Depth:
      return vk::ImageAspectFlagBits::eDepth;
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
  const auto usage = vkUsageFor(attachment);
  const auto aspect = vkAspectMaskFor(attachment.kind);
  constexpr auto memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
  constexpr bool createSampler = true;
  const auto descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

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

namespace nim {

} // namespace nim

DEFINE_uint32(atlas_blockid_rt_max_attachments, 8, "Max color attachments for block-id FBO");
DEFINE_double(atlas_blockid_rt_scale, 1.0, "Scale factor for block-id FBO size (relative to viewport)");

namespace nim {

class ZVulkanScratchImage
{
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

private:
  void createAttachments()
  {
    m_colorAttachments.clear();
    m_depthAttachment.reset();

    for (const auto& attachment : m_descriptor.attachments) {
      auto info = makeVulkanTextureInfo(m_descriptor, attachment);
      auto texture = m_device.createTexture(info);
      if (attachment.kind == ScratchAttachmentKind::Color) {
        if (m_colorAttachments.size() <= attachment.index) {
          m_colorAttachments.resize(attachment.index + 1);
        }
        m_colorAttachments[attachment.index] = std::move(texture);
      } else {
        m_depthAttachment = std::move(texture);
      }
    }
  }

  ZVulkanDevice& m_device;
  ScratchImageDescriptor m_descriptor;
  std::vector<std::unique_ptr<ZVulkanTexture>> m_colorAttachments;
  std::unique_ptr<ZVulkanTexture> m_depthAttachment;
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

Z3DScratchResourcePool::Z3DScratchResourcePool() = default;

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

  const uint64_t total = blockIdBytes + entryExitBytes + layerArrayBytes + temp2DBytes + raycastBytes + dualDepthBytes +
                         weightedAvgBytes + weightedBlendBytes;
  const double totalMiB = static_cast<double>(total) / (1024.0 * 1024.0);
  const double blockMiB = static_cast<double>(blockIdBytes) / (1024.0 * 1024.0);
  const double eeMiB = static_cast<double>(entryExitBytes) / (1024.0 * 1024.0);
  const double layerMiB = static_cast<double>(layerArrayBytes) / (1024.0 * 1024.0);
  const double tempMiB = static_cast<double>(temp2DBytes) / (1024.0 * 1024.0);
  const double raycastMiB = static_cast<double>(raycastBytes) / (1024.0 * 1024.0);
  const double ddpMiB = static_cast<double>(dualDepthBytes) / (1024.0 * 1024.0);
  const double waMiB = static_cast<double>(weightedAvgBytes) / (1024.0 * 1024.0);
  const double wbMiB = static_cast<double>(weightedBlendBytes) / (1024.0 * 1024.0);
  auto head = fmt::format(
    "ScratchPool memory: total={} bytes ({:.2f} MiB) (BlockID={} bytes ({:.2f} MiB), "
    "EntryExit={} bytes ({:.2f} MiB), LayerArray={} bytes ({:.2f} MiB), Temp2D={} bytes ({:.2f} MiB), "
    "RaycastAccum={} bytes ({:.2f} MiB), DualDepthPeel={} bytes ({:.2f} MiB), WeightedAvg={} bytes ({:.2f} MiB), "
    "WeightedBlend={} bytes ({:.2f} MiB))",
    total,
    totalMiB,
    blockIdBytes,
    blockMiB,
    entryExitBytes,
    eeMiB,
    layerArrayBytes,
    layerMiB,
    temp2DBytes,
    tempMiB,
    raycastBytes,
    raycastMiB,
    dualDepthBytes,
    ddpMiB,
    weightedAvgBytes,
    waMiB,
    weightedBlendBytes,
    wbMiB);

  std::string result = detailed ? fmt::format("{}\n{}", head, details) : head;

  cache.valid = true;
  cache.creationCounter = m_creationCounter;
  cache.changeCounter = m_changeCounter;
  cache.text = std::move(result);

  return cache.text;
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

  BlockIdRenderTargetSlot* slot = acquireFreeBlockIdSlot(size, requestedAttachmentCount);
  CHECK(slot != nullptr) << "Failed to acquire Block ID slot";
  growSlotIfNeeded(*slot, descriptor);

  markSlotAcquired(*slot);
  uint32_t usableAttachments = std::min<uint32_t>(requestedAttachmentCount, slot->attachments);
  ScratchImageDescriptor leaseDescriptor = slot->descriptor;
  if (usableAttachments != slot->attachments) {
    leaseDescriptor.attachments = makeColorAttachments(usableAttachments, ScratchFormat::RGBA32UI);
  }
  auto lease = makeLeaseFromSlot(slot, usableAttachments, RenderBackend::OpenGL);
  lease.descriptor = std::move(leaseDescriptor);
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
    const size_t freed = std::erase_if(slots, [&](const auto& slot) {
      if (slot->inUse) {
        return false;
      }
      return shouldTrim(slot->lastUseTick);
    });
    if (freed > 0) {
      totalFreed += freed;
      ++m_changeCounter;
    }
  }

  if (!logSummary && totalFreed > 0) {
    VLOG(1) << fmt::format("scratch pool auto trim: freed {} slots at usage tick {}", totalFreed, m_usageTick);
  }

  return totalFreed;
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
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
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
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
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
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
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
  }

  auto lease = makeLeaseFromSlot(slot, 1, RenderBackend::OpenGL);
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
  } else {
    m_dualDepthPeelSlots.emplace_back(std::make_unique<DualDepthPeelSlot>());
    slot = m_dualDepthPeelSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
  }

  auto lease = makeLeaseFromSlot(slot, 8, RenderBackend::OpenGL);
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
  } else {
    m_weightedAverageSlots.emplace_back(std::make_unique<WeightedAverageSlot>());
    slot = m_weightedAverageSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
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
  } else {
    m_weightedBlendedSlots.emplace_back(std::make_unique<WeightedBlendedSlot>());
    slot = m_weightedBlendedSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    slot->descriptor = descriptor;
    ++m_creationCounter;
    attachTexturesForDescriptor(*slot->fbo, descriptor);
    slot->fbo->isFBOComplete();
    markSlotAcquired(*slot);
  }

  auto lease = makeLeaseFromSlot(slot, 2, RenderBackend::OpenGL);
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
  auto& slots = vulkanSlotsForUsage(descriptor.usage);

  VulkanScratchSlot* slot = nullptr;
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
    slot = candidate.get();
    break;
  }

  if (!slot) {
    auto newSlot = std::make_unique<VulkanScratchSlot>();
    newSlot->image = std::make_unique<ZVulkanScratchImage>(dev, descriptor);
    newSlot->descriptor = descriptor;
    markSlotAcquired(*newSlot);
    auto lease = RenderTargetLease{};
    lease.descriptor = descriptor;
    lease.backend = RenderBackend::Vulkan;
    lease.vulkanImage = newSlot->image.get();
    lease.attachments = colorAttachmentCount(descriptor);
    lease.releaser = RenderTargetLease::Releaser::forSlot(newSlot.get());
    slots.emplace_back(std::move(newSlot));
    ++m_creationCounter;
    maybeTrimAfterAcquire();
    return lease;
  }

  slot->descriptor = descriptor;
  markSlotAcquired(*slot);
  auto lease = RenderTargetLease{};
  lease.descriptor = descriptor;
  lease.backend = RenderBackend::Vulkan;
  lease.vulkanImage = slot->image.get();
  lease.attachments = colorAttachmentCount(descriptor);
  lease.releaser = RenderTargetLease::Releaser::forSlot(slot);
  maybeTrimAfterAcquire();
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

} // namespace nim
