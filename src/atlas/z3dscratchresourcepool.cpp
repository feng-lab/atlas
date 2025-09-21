#include "z3dscratchresourcepool.h"

#include "z3dtexture.h"
#include "zlog.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <utility>

namespace {
using namespace nim;

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
Z3DScratchResourcePool::RenderTargetLease makeLeaseFromSlot(Slot* slot, uint32_t attachments)
{
  Z3DScratchResourcePool::RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = attachments;
  lease.releaser = Z3DScratchResourcePool::RenderTargetLease::Releaser::forSlot(slot);
  return lease;
}

} // anonymous namespace

DEFINE_uint32(atlas_blockid_rt_max_attachments, 8, "Max color attachments for block-id FBO");
DEFINE_double(atlas_blockid_rt_scale, 1.0, "Scale factor for block-id FBO size (relative to viewport)");

namespace nim {

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
                         const uint64_t bpp = bytesPerPixelFromInternal(s.colorFormat);
                         const uint64_t bytes = pixels * bpp;
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} layers={} fmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              s.layers,
                                              glbinding::aux::Meta::getString((GLenum)s.colorFormat),
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
                         const uint64_t colorBpp = bytesPerPixelFromInternal(s.colorFormat);
                         const uint64_t depthBpp = bytesPerPixelFromInternal(s.depthFormat);
                         const uint64_t bytes = layerPixels * (colorBpp + depthBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} layers={} colorFmt={} depthFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              s.layers,
                                              glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                                              glbinding::aux::Meta::getString((GLenum)s.depthFormat),
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
                         const uint64_t colorBpp = bytesPerPixelFromInternal(s.colorFormat);
                         const uint64_t depthBpp = bytesPerPixelFromInternal(s.depthFormat);
                         const uint64_t bytes = pixels * (colorBpp + depthBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} colorFmt={} depthFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                                              glbinding::aux::Meta::getString((GLenum)s.depthFormat),
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
                         const uint64_t primaryBpp = bytesPerPixelFromInternal(s.colorFormat);
                         const uint64_t accumBpp = bytesPerPixelFromInternal(s.accumulatorFormat);
                         const uint64_t bytes = pixels * (primaryBpp + accumBpp);
                         std::string line;
                         if (wantDetail) {
                           line = fmt::format("slot={} size={}x{} colorFmt={} accumFmt={} inUse={} bytes={}",
                                              idx,
                                              sz.x,
                                              sz.y,
                                              glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                                              glbinding::aux::Meta::getString((GLenum)s.accumulatorFormat),
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

  if (!detailed) {
    return head;
  }
  return fmt::format("{}\n{}", head, details);
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
  ++m_creationCounter;
  return created;
}

void Z3DScratchResourcePool::growSlotIfNeeded(BlockIdRenderTargetSlot& slot,
                                              const glm::uvec2& exactSize,
                                              uint32_t requiredAttachments)
{
  // Resize and ensure attachments
  if (slot.fbo->size() != exactSize) {
    slot.fbo->resize(exactSize);
    ++m_changeCounter;
  }
  uint32_t before = slot.attachments;
  for (uint32_t i = slot.attachments; i < requiredAttachments; ++i) {
    auto tex = new Z3DTexture(GL_TEXTURE_2D,
                              GLint(GL_RGBA32UI),
                              glm::uvec3(exactSize.x, exactSize.y, 1),
                              GL_RGBA_INTEGER,
                              GL_UNSIGNED_INT,
                              nullptr,
                              GLint(GL_NEAREST),
                              GLint(GL_NEAREST));
    slot.fbo->attachTextureToFBO(tex, GLenum(GL_COLOR_ATTACHMENT0 + i), true);
    ++slot.attachments;
  }
  if (slot.attachments > before) {
    ++m_changeCounter;
  }
  slot.fbo->isFBOComplete();
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireBlockIdRenderTarget(const glm::uvec2& viewport, int requestedAttachments, double scale)
{
  uint32_t requestedAttachmentCount =
    (requestedAttachments > 0) ? static_cast<uint32_t>(requestedAttachments) : blockIdMaxAttachments();

  // compute scaled exact size
  double scaleFactor = (scale > 0.0) ? scale : blockIdScale();
  glm::uvec2 size = viewport;
  if (scaleFactor > 0.0 && scaleFactor != 1.0) {
    size.x = static_cast<uint32_t>(std::ceil(viewport.x * scaleFactor));
    size.y = static_cast<uint32_t>(std::ceil(viewport.y * scaleFactor));
  }
  BlockIdRenderTargetSlot* slot = acquireFreeBlockIdSlot(size, requestedAttachmentCount);
  CHECK(slot != nullptr) << "Failed to acquire Block ID slot";
  growSlotIfNeeded(*slot, size, requestedAttachmentCount);

  slot->inUse = true;
  slot->lastUseTick = ++m_usageTick;
  return makeLeaseFromSlot(slot, std::min<uint32_t>(requestedAttachmentCount, slot->attachments));
}

void Z3DScratchResourcePool::trim()
{
  auto trimCategory = [&](const char* label, auto& vec) {
    size_t kept = 0;
    const size_t freed = std::erase_if(vec, [&](auto& uptr) {
      auto& slot = *uptr;
      if (slot.inUse) {
        ++kept;
        return false; // keep in-use slots
      }
      return true; // erase non-in-use slots
    });
    if (kept || freed) {
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
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireEntryExitRenderTarget(const glm::uvec2& size, uint32_t layers, GLint colorInternalFormat)
{
  // Find a free slot with closest size, only consider matching internal format.
  EntryExitRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_entryExitRenderTargetSlots, size, [&](const EntryExitRenderTargetSlot& s) {
      return s.colorFormat == colorInternalFormat;
    });

  if (slot) {
    // Existing slot path: single-pass resize; grow Z only if XY unchanged.
    const uint32_t prevLayers = slot->layers;
    const bool xyChanged = (slot->fbo->size() != size);
    const uint32_t desiredZ = xyChanged ? layers : std::max<uint32_t>(slot->layers, layers);
    const bool resized = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->inUse = true;
    slot->layers = desiredZ;
    if (resized || desiredZ != prevLayers) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    // Creation path: make a fresh slot and attachments.
    m_entryExitRenderTargetSlots.emplace_back(std::make_unique<EntryExitRenderTargetSlot>());
    slot = m_entryExitRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    // Create color array attachment (inline): GL_TEXTURE_2D_ARRAY with NEAREST filter
    {
      auto t =
        new Z3DTexture(GL_TEXTURE_2D_ARRAY, colorInternalFormat, glm::uvec3(size.x, size.y, layers), GL_RGBA, GL_FLOAT);
      t->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
      slot->fbo->attachTextureToFBO(t, GL_COLOR_ATTACHMENT0, true);
    }
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->layers = layers;
    slot->colorFormat = colorInternalFormat;
  }

  return makeLeaseFromSlot(slot, 1);
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireLayerArrayRenderTarget(const glm::uvec2& size,
                                                      uint32_t layers,
                                                      GLint colorInternalFormat,
                                                      GLint depthInternalFormat)
{
  // Find a free slot with closest size; only consider matching color+depth formats.
  LayerArrayRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_layerArrayRenderTargetSlots, size, [&](const LayerArrayRenderTargetSlot& s) {
      return s.colorFormat == colorInternalFormat && s.depthFormat == depthInternalFormat;
    });

  if (slot) {
    // Existing slot path: single-pass resize; grow Z only if XY unchanged.
    const uint32_t prevLayers = slot->layers;
    const bool xyChanged = (slot->fbo->size() != size);
    const uint32_t desiredZ = xyChanged ? layers : std::max<uint32_t>(slot->layers, layers);
    const bool resized = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->inUse = true;
    slot->layers = desiredZ;
    if (resized || desiredZ != prevLayers) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    // Creation path: make a fresh slot and attachments.
    m_layerArrayRenderTargetSlots.emplace_back(std::make_unique<LayerArrayRenderTargetSlot>());
    slot = m_layerArrayRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    // Create color array attachment (inline)
    {
      auto t =
        new Z3DTexture(GL_TEXTURE_2D_ARRAY, colorInternalFormat, glm::uvec3(size.x, size.y, layers), GL_RGBA, GL_FLOAT);
      slot->fbo->attachTextureToFBO(t, GL_COLOR_ATTACHMENT0, true);
    }

    // Create depth array attachment (inline)
    {
      auto t = new Z3DTexture(GL_TEXTURE_2D_ARRAY,
                              depthInternalFormat,
                              glm::uvec3(size.x, size.y, layers),
                              GL_DEPTH_COMPONENT,
                              GL_FLOAT);
      slot->fbo->attachTextureToFBO(t, GL_DEPTH_ATTACHMENT, true);
    }

    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->layers = layers;
    slot->colorFormat = colorInternalFormat;
    slot->depthFormat = depthInternalFormat;
  }

  return makeLeaseFromSlot(slot, 1);
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireRaycastAccumulatorRenderTarget(const glm::uvec2& size)
{
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
    slot->inUse = true;
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    m_raycastAccumulatorSlots.emplace_back(std::make_unique<RaycastAccumulatorSlot>());
    slot = m_raycastAccumulatorSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    auto* colorTex = new Z3DTexture(GLint(GL_RGBA16),
                                    glm::uvec3(size.x, size.y, 1),
                                    GL_RGBA,
                                    GL_FLOAT,
                                    nullptr,
                                    GLint(GL_NEAREST),
                                    GLint(GL_NEAREST));
    auto* accumTex = new Z3DTexture(GLint(GL_RG32F),
                                    glm::uvec3(size.x, size.y, 1),
                                    GL_RG,
                                    GL_FLOAT,
                                    nullptr,
                                    GLint(GL_NEAREST),
                                    GLint(GL_NEAREST));

    slot->fbo->attachTextureToFBO(colorTex, GL_COLOR_ATTACHMENT0, true);
    slot->fbo->attachTextureToFBO(accumTex, GL_COLOR_ATTACHMENT1, true);
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->colorFormat = GLint(GL_RGBA16);
    slot->accumulatorFormat = GLint(GL_RG32F);
  }

  return makeLeaseFromSlot(slot, 2);
}

Z3DScratchResourcePool::RenderTargetLease Z3DScratchResourcePool::acquireTempRenderTarget2D(const glm::uvec2& size,
                                                                                            GLint colorInternalFormat,
                                                                                            GLint depthInternalFormat)
{
  // Find a free slot with closest size; only consider matching color+depth formats.
  Temp2DRenderTargetSlot* slot =
    findClosestFreeSlotIf(m_temp2DRenderTargetSlots, size, [&](const Temp2DRenderTargetSlot& s) {
      return s.colorFormat == colorInternalFormat && s.depthFormat == depthInternalFormat;
    });

  if (slot) {
    // Existing slot path: adjust size if needed and retain formats.
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->inUse = true;
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    // Creation path: make a fresh slot and attachments.
    m_temp2DRenderTargetSlots.emplace_back(std::make_unique<Temp2DRenderTargetSlot>());
    slot = m_temp2DRenderTargetSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    // Create 2D color and depth attachments (inline)
    {
      auto ct = new Z3DTexture(GL_TEXTURE_2D, colorInternalFormat, glm::uvec3(size.x, size.y, 1), GL_RGBA, GL_FLOAT);
      slot->fbo->attachTextureToFBO(ct, GL_COLOR_ATTACHMENT0, true);
      auto dt =
        new Z3DTexture(GL_TEXTURE_2D, depthInternalFormat, glm::uvec3(size.x, size.y, 1), GL_DEPTH_COMPONENT, GL_FLOAT);
      slot->fbo->attachTextureToFBO(dt, GL_DEPTH_ATTACHMENT, true);
    }
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->colorFormat = colorInternalFormat;
    slot->depthFormat = depthInternalFormat;
  }

  return makeLeaseFromSlot(slot, 1);
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireDualDepthPeelRenderTarget(const glm::uvec2& size)
{
  DualDepthPeelSlot* slot = findClosestFreeSlot(m_dualDepthPeelSlots, size);

  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->inUse = true;
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    m_dualDepthPeelSlots.emplace_back(std::make_unique<DualDepthPeelSlot>());
    slot = m_dualDepthPeelSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    auto attachTexture = [&](GLint internalFormat, GLenum format, GLenum type, GLenum attachment) {
      auto* tex = new Z3DTexture(internalFormat,
                                 glm::uvec3(size.x, size.y, 1),
                                 format,
                                 type,
                                 nullptr,
                                 GLint(GL_NEAREST),
                                 GLint(GL_NEAREST));
      slot->fbo->attachTextureToFBO(tex, attachment, true);
    };

    attachTexture(GLint(GL_RG32F), GL_RG, GL_FLOAT, GL_COLOR_ATTACHMENT0);
    attachTexture(GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT, GL_COLOR_ATTACHMENT1);
    attachTexture(GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT, GL_COLOR_ATTACHMENT2);
    attachTexture(GLint(GL_RG32F), GL_RG, GL_FLOAT, GL_COLOR_ATTACHMENT3);
    attachTexture(GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT, GL_COLOR_ATTACHMENT4);
    attachTexture(GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT, GL_COLOR_ATTACHMENT5);
    attachTexture(GLint(GL_RGBA16), GL_RGBA, GL_UNSIGNED_SHORT, GL_COLOR_ATTACHMENT6);
    attachTexture(GLint(GL_R32F), GL_RED, GL_FLOAT, GL_COLOR_ATTACHMENT7);

    slot->fbo->isFBOComplete();
    slot->inUse = true;
  }

  return makeLeaseFromSlot(slot, 8);
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireWeightedAverageRenderTarget(const glm::uvec2& size)
{
  WeightedAverageSlot* slot = findClosestFreeSlot(m_weightedAverageSlots, size);

  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->inUse = true;
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    m_weightedAverageSlots.emplace_back(std::make_unique<WeightedAverageSlot>());
    slot = m_weightedAverageSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    auto* accum = new Z3DTexture(GLint(GL_RGBA32F),
                                 glm::uvec3(size.x, size.y, 1),
                                 GL_RGBA,
                                 GL_FLOAT,
                                 nullptr,
                                 GLint(GL_NEAREST),
                                 GLint(GL_NEAREST));
    auto* reveal = new Z3DTexture(GLint(GL_RG32F),
                                  glm::uvec3(size.x, size.y, 1),
                                  GL_RG,
                                  GL_FLOAT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST));
    slot->fbo->attachTextureToFBO(accum, GL_COLOR_ATTACHMENT0, true);
    slot->fbo->attachTextureToFBO(reveal, GL_COLOR_ATTACHMENT1, true);
    slot->fbo->isFBOComplete();
    slot->inUse = true;
  }

  return makeLeaseFromSlot(slot, 2);
}

Z3DScratchResourcePool::RenderTargetLease
Z3DScratchResourcePool::acquireWeightedBlendedRenderTarget(const glm::uvec2& size)
{
  WeightedBlendedSlot* slot = findClosestFreeSlot(m_weightedBlendedSlots, size);

  if (slot) {
    bool changed = false;
    if (slot->fbo->size() != size) {
      slot->fbo->resize(size);
      changed = true;
    }
    slot->inUse = true;
    if (changed) {
      slot->fbo->isFBOComplete();
      ++m_changeCounter;
    }
  } else {
    m_weightedBlendedSlots.emplace_back(std::make_unique<WeightedBlendedSlot>());
    slot = m_weightedBlendedSlots.back().get();
    slot->fbo = std::make_unique<Z3DRenderTarget>(size);
    ++m_creationCounter;

    auto* accum = new Z3DTexture(GLint(GL_RGBA16F),
                                 glm::uvec3(size.x, size.y, 1),
                                 GL_RGBA,
                                 GL_FLOAT,
                                 nullptr,
                                 GLint(GL_NEAREST),
                                 GLint(GL_NEAREST));
    auto* transmittance = new Z3DTexture(GLint(GL_R16F),
                                         glm::uvec3(size.x, size.y, 1),
                                         GL_RED,
                                         GL_FLOAT,
                                         nullptr,
                                         GLint(GL_NEAREST),
                                         GLint(GL_NEAREST));
    slot->fbo->attachTextureToFBO(accum, GL_COLOR_ATTACHMENT0, true);
    slot->fbo->attachTextureToFBO(transmittance, GL_COLOR_ATTACHMENT1, true);
    slot->fbo->isFBOComplete();
    slot->inUse = true;
  }

  return makeLeaseFromSlot(slot, 2);
}

} // namespace nim
