#include "z3dscratchresourcepool.h"

#include "z3dtexture.h"
#include "zlog.h"
#include <cmath>
#include <limits>
#include <algorithm>

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
  uint64_t total = 0;
  uint64_t blockIdBytes = 0;
  uint64_t entryExitBytes = 0;
  uint64_t layerArrayBytes = 0;
  uint64_t temp2DBytes = 0;
  uint64_t raycastBytes = 0;
  uint64_t dualDepthBytes = 0;
  uint64_t weightedAvgBytes = 0;
  uint64_t weightedBlendBytes = 0;

  std::string details;

  // Block ID slots
  for (size_t i = 0; i < m_blockIdRenderTargetSlots.size(); ++i) {
    const auto& s = *m_blockIdRenderTargetSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    const uint64_t bpp = 16; // GL_RGBA32UI = 4 * 32-bit
    const uint64_t bytes = pixels * bpp * s.attachments;
    blockIdBytes += bytes;
    if (detailed) {
      details += fmt::format("[BlockID] slot={} size={}x{} attachments={} inUse={} bytes={}\n",
                             i,
                             sz.x,
                             sz.y,
                             s.attachments,
                             s.inUse ? 1 : 0,
                             bytes);
    }
  }

  // Entry/Exit slots (single color array)
  for (size_t i = 0; i < m_entryExitRenderTargetSlots.size(); ++i) {
    const auto& s = *m_entryExitRenderTargetSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y * std::max<uint32_t>(1, s.layers);
    const uint64_t bpp = bytesPerPixelFromInternal(s.colorFormat);
    const uint64_t bytes = pixels * bpp;
    entryExitBytes += bytes;
    if (detailed) {
      details += fmt::format("[EntryExit] slot={} size={}x{} layers={} fmt={} inUse={} bytes={}\n",
                             i,
                             sz.x,
                             sz.y,
                             s.layers,
                             glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                             s.inUse ? 1 : 0,
                             bytes);
    }
  }

  // Layer Array slots (color + depth arrays)
  for (size_t i = 0; i < m_layerArrayRenderTargetSlots.size(); ++i) {
    const auto& s = *m_layerArrayRenderTargetSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t layerPixels = static_cast<uint64_t>(sz.x) * sz.y * std::max<uint32_t>(1, s.layers);
    const uint64_t colorBpp = bytesPerPixelFromInternal(s.colorFormat);
    const uint64_t depthBpp = bytesPerPixelFromInternal(s.depthFormat);
    const uint64_t bytes = layerPixels * (colorBpp + depthBpp);
    layerArrayBytes += bytes;
    if (detailed) {
      details += fmt::format("[LayerArray] slot={} size={}x{} layers={} colorFmt={} depthFmt={} inUse={} bytes={}\n",
                             i,
                             sz.x,
                             sz.y,
                             s.layers,
                             glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                             glbinding::aux::Meta::getString((GLenum)s.depthFormat),
                             s.inUse ? 1 : 0,
                             bytes);
    }
  }

  // Temp 2D slots (color + depth 2D)
  for (size_t i = 0; i < m_temp2DRenderTargetSlots.size(); ++i) {
    const auto& s = *m_temp2DRenderTargetSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    const uint64_t colorBpp = bytesPerPixelFromInternal(s.colorFormat);
    const uint64_t depthBpp = bytesPerPixelFromInternal(s.depthFormat);
    const uint64_t bytes = pixels * (colorBpp + depthBpp);
    temp2DBytes += bytes;
    if (detailed) {
      details += fmt::format("[Temp2D] slot={} size={}x{} colorFmt={} depthFmt={} inUse={} bytes={}\n",
                             i,
                             sz.x,
                             sz.y,
                             glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                             glbinding::aux::Meta::getString((GLenum)s.depthFormat),
                             s.inUse ? 1 : 0,
                             bytes);
    }
  }

  // Raycast accumulator slots (dual color attachments)
  for (size_t i = 0; i < m_raycastAccumulatorSlots.size(); ++i) {
    const auto& s = *m_raycastAccumulatorSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    const uint64_t primaryBpp = bytesPerPixelFromInternal(s.colorFormat);
    const uint64_t accumBpp = bytesPerPixelFromInternal(s.accumulatorFormat);
    const uint64_t bytes = pixels * (primaryBpp + accumBpp);
    raycastBytes += bytes;
    if (detailed) {
      details += fmt::format("[RaycastAccum] slot={} size={}x{} colorFmt={} accumFmt={} inUse={} bytes={}\n",
                             i,
                             sz.x,
                             sz.y,
                             glbinding::aux::Meta::getString((GLenum)s.colorFormat),
                             glbinding::aux::Meta::getString((GLenum)s.accumulatorFormat),
                             s.inUse ? 1 : 0,
                             bytes);
    }
  }

  // Dual-depth peeling slots (8 color attachments)
  for (size_t i = 0; i < m_dualDepthPeelSlots.size(); ++i) {
    const auto& s = *m_dualDepthPeelSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    uint64_t bytes = 0;
    std::string attachmentFormats;
    for (int att = 0; att < 8; ++att) {
      if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
        bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
        if (!attachmentFormats.empty()) {
          attachmentFormats += ", ";
        }
        attachmentFormats += fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
      }
    }
    dualDepthBytes += bytes;
    if (detailed) {
      details += fmt::format("[DualDepthPeel] slot={} size={}x{} inUse={} bytes={} attachments=[{}]\n",
                             i,
                             sz.x,
                             sz.y,
                             s.inUse ? 1 : 0,
                             bytes,
                             attachmentFormats);
    }
  }

  // Weighted-average slots (two color attachments)
  for (size_t i = 0; i < m_weightedAverageSlots.size(); ++i) {
    const auto& s = *m_weightedAverageSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    uint64_t bytes = 0;
    std::string attachmentFormats;
    for (int att = 0; att < 2; ++att) {
      if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
        bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
        if (!attachmentFormats.empty()) {
          attachmentFormats += ", ";
        }
        attachmentFormats += fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
      }
    }
    weightedAvgBytes += bytes;
    if (detailed) {
      details += fmt::format("[WeightedAverage] slot={} size={}x{} inUse={} bytes={} attachments=[{}]\n",
                             i,
                             sz.x,
                             sz.y,
                             s.inUse ? 1 : 0,
                             bytes,
                             attachmentFormats);
    }
  }

  // Weighted-blended slots (two color attachments)
  for (size_t i = 0; i < m_weightedBlendedSlots.size(); ++i) {
    const auto& s = *m_weightedBlendedSlots[i];
    const glm::uvec2 sz = s.fbo->size();
    const uint64_t pixels = static_cast<uint64_t>(sz.x) * sz.y;
    uint64_t bytes = 0;
    std::string attachmentFormats;
    for (int att = 0; att < 2; ++att) {
      if (const Z3DTexture* tex = s.fbo->attachment(GLenum(GL_COLOR_ATTACHMENT0 + att))) {
        bytes += pixels * bytesPerPixelFromInternal(tex->internalFormat());
        if (!attachmentFormats.empty()) {
          attachmentFormats += ", ";
        }
        attachmentFormats += fmt::format("{}={}", att, glbinding::aux::Meta::getString((GLenum)tex->internalFormat()));
      }
    }
    weightedBlendBytes += bytes;
    if (detailed) {
      details += fmt::format("[WeightedBlended] slot={} size={}x{} inUse={} bytes={} attachments=[{}]\n",
                             i,
                             sz.x,
                             sz.y,
                             s.inUse ? 1 : 0,
                             bytes,
                             attachmentFormats);
    }
  }

  total = blockIdBytes + entryExitBytes + layerArrayBytes + temp2DBytes + raycastBytes + dualDepthBytes +
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = std::min<uint32_t>(requestedAttachmentCount, slot->attachments);
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
}

void Z3DScratchResourcePool::trim()
{
  auto trimCategory = [](auto& vec, size_t& kept, size_t& freed) {
    auto it = std::remove_if(vec.begin(), vec.end(), [&](auto& uptr) {
      auto& slot = *uptr;
      if (slot.inUse) {
        ++kept;
        return false; // keep in-use slots
      }
      // Remove free slots; every remaining slot owns an FBO by construction.
      ++freed;
      return true; // erase non-in-use slots
    });
    vec.erase(it, vec.end());
  };

  size_t keptBlock = 0, freedBlock = 0;
  trimCategory(m_blockIdRenderTargetSlots, keptBlock, freedBlock);
  if (keptBlock || freedBlock) {
    LOG(INFO) << fmt::format("trim(): BlockID kept_in_use={} freed={}", keptBlock, freedBlock);
  }

  size_t keptEE = 0, freedEE = 0;
  trimCategory(m_entryExitRenderTargetSlots, keptEE, freedEE);
  if (keptEE || freedEE) {
    LOG(INFO) << fmt::format("trim(): EntryExit kept_in_use={} freed={}", keptEE, freedEE);
  }

  size_t keptLayer = 0, freedLayer = 0;
  trimCategory(m_layerArrayRenderTargetSlots, keptLayer, freedLayer);
  if (keptLayer || freedLayer) {
    LOG(INFO) << fmt::format("trim(): LayerArray kept_in_use={} freed={}", keptLayer, freedLayer);
  }

  size_t keptTemp = 0, freedTemp = 0;
  trimCategory(m_temp2DRenderTargetSlots, keptTemp, freedTemp);
  if (keptTemp || freedTemp) {
    LOG(INFO) << fmt::format("trim(): Temp2D kept_in_use={} freed={}", keptTemp, freedTemp);
  }

  size_t keptRaycast = 0, freedRaycast = 0;
  trimCategory(m_raycastAccumulatorSlots, keptRaycast, freedRaycast);
  if (keptRaycast || freedRaycast) {
    LOG(INFO) << fmt::format("trim(): RaycastAccum kept_in_use={} freed={}", keptRaycast, freedRaycast);
  }

  size_t keptDDP = 0, freedDDP = 0;
  trimCategory(m_dualDepthPeelSlots, keptDDP, freedDDP);
  if (keptDDP || freedDDP) {
    LOG(INFO) << fmt::format("trim(): DualDepthPeel kept_in_use={} freed={}", keptDDP, freedDDP);
  }

  size_t keptWA = 0, freedWA = 0;
  trimCategory(m_weightedAverageSlots, keptWA, freedWA);
  if (keptWA || freedWA) {
    LOG(INFO) << fmt::format("trim(): WeightedAverage kept_in_use={} freed={}", keptWA, freedWA);
  }

  size_t keptWB = 0, freedWB = 0;
  trimCategory(m_weightedBlendedSlots, keptWB, freedWB);
  if (keptWB || freedWB) {
    LOG(INFO) << fmt::format("trim(): WeightedBlended kept_in_use={} freed={}", keptWB, freedWB);
  }
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
    uint32_t desiredZ = layers;
    if (!xyChanged) {
      desiredZ = std::max<uint32_t>(slot->layers, layers);
    }
    bool changed = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    Z3DTexture* colorTex = slot->fbo->attachment(GL_COLOR_ATTACHMENT0);
    slot->layers = colorTex ? static_cast<uint32_t>(colorTex->dimension().z) : layers;
    if (slot->layers != prevLayers) {
      changed = true;
    }
    if (changed) {
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
    Z3DTexture* colorTex = slot->fbo->attachment(GL_COLOR_ATTACHMENT0);
    slot->layers = static_cast<uint32_t>(colorTex->dimension().z);
    slot->colorFormat = colorInternalFormat;
  }

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 1;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
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
    uint32_t desiredZ = layers;
    if (!xyChanged) {
      desiredZ = std::max<uint32_t>(slot->layers, layers);
    }
    bool changed = slot->fbo->resize(glm::uvec3(size.x, size.y, desiredZ));
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    Z3DTexture* colorTex = slot->fbo->attachment(GL_COLOR_ATTACHMENT0);
    slot->layers = colorTex ? static_cast<uint32_t>(colorTex->dimension().z) : layers;
    if (slot->layers != prevLayers) {
      changed = true;
    }
    if (changed) {
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
    const Z3DTexture* colorTex = slot->fbo->attachment(GL_COLOR_ATTACHMENT0);
    slot->layers = colorTex ? static_cast<uint32_t>(colorTex->dimension().z) : layers;
    slot->colorFormat = colorInternalFormat;
    slot->depthFormat = depthInternalFormat;
  }

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 1;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
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
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->colorFormat = GLint(GL_RGBA16);
    slot->accumulatorFormat = GLint(GL_RG32F);
    if (changed) {
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 2;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
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
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    slot->colorFormat = colorInternalFormat;
    slot->depthFormat = depthInternalFormat;
    if (changed) {
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 1;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };

  return lease;
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
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    if (changed) {
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 8;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
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
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    if (changed) {
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 2;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
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
    slot->fbo->isFBOComplete();
    slot->inUse = true;
    if (changed) {
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

  RenderTargetLease lease;
  lease.renderTarget = slot->fbo.get();
  lease.attachments = 2;
  lease.releaser = [slot]() {
    slot->inUse = false;
  };
  return lease;
}

} // namespace nim
