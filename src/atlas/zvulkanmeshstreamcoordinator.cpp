#include "zvulkanmeshstreamcoordinator.h"

#include "zvulkanstaticpromotionutils.h"

namespace nim {
namespace {

size_t staticSliceBytes(const Z3DRendererVulkanBackend::StaticSlice& slice)
{
  return slice ? slice.size : 0u;
}

} // namespace

ZVulkanMeshStreamCoordinator::ZVulkanMeshStreamCoordinator(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_streamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner::Mesh)
{}

void ZVulkanMeshStreamCoordinator::resetFrame()
{
  m_streamCache.resetFrame();
}

void ZVulkanMeshStreamCoordinator::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  m_streamUsageTracker.eraseStream(streamKey);
  m_streamCache.evictStream(streamKey, [this](CacheEntry& entry) {
    releaseActiveSlices(entry);
    clearReplacement(entry);
  });
}

void ZVulkanMeshStreamCoordinator::touchStaticStream(uint64_t streamKey)
{
  m_streamUsageTracker.touch(streamKey, m_backend.currentStaticCacheEpoch());
}

size_t ZVulkanMeshStreamCoordinator::staticBytesForStream(uint64_t streamKey,
                                                          Z3DRendererVulkanBackend::StaticPressureDomain domain) const
{
  return m_streamCache.staticBytesForStream(
    streamKey,
    domain,
    [](const CacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
        return staticSliceBytes(entry.vbPos) + staticSliceBytes(entry.vbNorm) + staticSliceBytes(entry.vbColor) +
               staticSliceBytes(entry.vbTex) + staticSliceBytes(entry.replacement.vbPos) +
               staticSliceBytes(entry.replacement.vbNorm) + staticSliceBytes(entry.replacement.vbColor) +
               staticSliceBytes(entry.replacement.vbTex);
      }
      return staticSliceBytes(entry.ib) + staticSliceBytes(entry.replacement.ib);
    });
}

std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
ZVulkanMeshStreamCoordinator::oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                                          uint64_t protectedEpoch) const
{
  return m_streamUsageTracker.oldestEvictableStaticStream(
    domain,
    protectedEpoch,
    [this](uint64_t streamKey, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      return staticBytesForStream(streamKey, cacheDomain);
    });
}

ZVulkanMeshStreamCoordinator::CacheEntry* ZVulkanMeshStreamCoordinator::findEntry(const CacheKey& key)
{
  return m_streamCache.findEntry(key);
}

const ZVulkanMeshStreamCoordinator::CacheEntry* ZVulkanMeshStreamCoordinator::findEntry(const CacheKey& key) const
{
  return m_streamCache.findEntry(key);
}

ZVulkanMeshStreamCoordinator::CacheEntry& ZVulkanMeshStreamCoordinator::ensureEntry(const CacheKey& key)
{
  return m_streamCache.ensureEntry(key);
}

const ZVulkanMeshStreamCoordinator::PendingUploadBinding*
ZVulkanMeshStreamCoordinator::findPendingUpload(const CacheKey& key) const
{
  return m_streamCache.findPendingUpload(key);
}

void ZVulkanMeshStreamCoordinator::rememberPendingUploadBinding(const CacheKey& key,
                                                                const PendingUploadBinding& binding)
{
  m_streamCache.rememberPendingUploadBinding(key, binding);
}

bool ZVulkanMeshStreamCoordinator::stateMatches(const StreamState& lhs, const StreamState& rhs)
{
  return lhs.vertexCount == rhs.vertexCount && lhs.indexCount == rhs.indexCount && lhs.posGen == rhs.posGen &&
         lhs.normGen == rhs.normGen && lhs.colorGen == rhs.colorGen && lhs.texGen == rhs.texGen &&
         lhs.indexGen == rhs.indexGen && lhs.hasTex == rhs.hasTex;
}

ZVulkanMeshStreamCoordinator::StreamState ZVulkanMeshStreamCoordinator::activeStateForEntry(const CacheEntry& entry)
{
  return StreamState{entry.vertexCount,
                     entry.indexCount,
                     entry.posGen,
                     entry.normGen,
                     entry.colorGen,
                     entry.texGen,
                     entry.indexGen,
                     entry.hasTex};
}

ZVulkanMeshStreamCoordinator::StreamState ZVulkanMeshStreamCoordinator::latestStateForEntry(const CacheEntry& entry)
{
  return StreamState{entry.latestVertexCount,
                     entry.latestIndexCount,
                     entry.latestPosGen,
                     entry.latestNormGen,
                     entry.latestColorGen,
                     entry.latestTexGen,
                     entry.latestIndexGen,
                     entry.latestHasTex};
}

ZVulkanMeshStreamCoordinator::StreamState
ZVulkanMeshStreamCoordinator::replacementStateForEntry(const CacheEntry& entry)
{
  return StreamState{entry.replacement.vertexCount,
                     entry.replacement.indexCount,
                     entry.replacement.posGen,
                     entry.replacement.normGen,
                     entry.replacement.colorGen,
                     entry.replacement.texGen,
                     entry.replacement.indexGen,
                     entry.replacement.hasTex};
}

void ZVulkanMeshStreamCoordinator::updateActiveState(CacheEntry& entry, const StreamState& state)
{
  entry.vertexCount = state.vertexCount;
  entry.indexCount = state.indexCount;
  entry.posGen = state.posGen;
  entry.normGen = state.normGen;
  entry.colorGen = state.colorGen;
  entry.texGen = state.texGen;
  entry.indexGen = state.indexGen;
  entry.hasTex = state.hasTex;
}

void ZVulkanMeshStreamCoordinator::updateLatestState(CacheEntry& entry, const StreamState& state)
{
  entry.latestVertexCount = state.vertexCount;
  entry.latestIndexCount = state.indexCount;
  entry.latestPosGen = state.posGen;
  entry.latestNormGen = state.normGen;
  entry.latestColorGen = state.colorGen;
  entry.latestTexGen = state.texGen;
  entry.latestIndexGen = state.indexGen;
  entry.latestHasTex = state.hasTex;
}

bool ZVulkanMeshStreamCoordinator::pendingUploadMatches(const PendingUploadBinding& pending,
                                                        const StreamState& state) const
{
  const bool texLayoutSame = (!state.hasTex && !pending.tex.buffer) || (state.hasTex && pending.tex.buffer);
  const bool buffersOk = pending.pos.buffer && pending.norm.buffer && pending.color.buffer && texLayoutSame &&
                         ((state.indexCount == 0) || pending.index.buffer);
  return pending.vertexCount == state.vertexCount && pending.indexCount == state.indexCount &&
         pending.posGen == state.posGen && pending.normGen == state.normGen && pending.colorGen == state.colorGen &&
         pending.texGen == state.texGen && pending.indexGen == state.indexGen && buffersOk;
}

bool ZVulkanMeshStreamCoordinator::activeEntryMatches(const CacheEntry& entry, const StreamState& state) const
{
  const bool texLayoutSame = (entry.hasTex == state.hasTex) && (!state.hasTex || static_cast<bool>(entry.vbTex));
  const bool buffersOk = static_cast<bool>(entry.vbPos) && static_cast<bool>(entry.vbNorm) &&
                         static_cast<bool>(entry.vbColor) && texLayoutSame &&
                         ((state.indexCount == 0) || static_cast<bool>(entry.ib));
  return entry.promoted && stateMatches(activeStateForEntry(entry), state) && buffersOk;
}

void ZVulkanMeshStreamCoordinator::releaseActiveSlices(CacheEntry& entry)
{
  vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
}

void ZVulkanMeshStreamCoordinator::clearReplacement(CacheEntry& entry)
{
  vulkan::releaseStaticSlices(m_backend,
                              {&entry.replacement.vbPos,
                               &entry.replacement.vbNorm,
                               &entry.replacement.vbColor,
                               &entry.replacement.vbTex,
                               &entry.replacement.ib});
  entry.replacement = {};
}

void ZVulkanMeshStreamCoordinator::activateReplacement(CacheEntry& entry)
{
  CHECK(entry.replacement.readyToActivate) << "activateReplacement called before replacement copy completed";
  const StreamState replacementState = replacementStateForEntry(entry);
  Z3DRendererVulkanBackend::StaticSlice oldVbPos = entry.vbPos;
  Z3DRendererVulkanBackend::StaticSlice oldVbNorm = entry.vbNorm;
  Z3DRendererVulkanBackend::StaticSlice oldVbColor = entry.vbColor;
  Z3DRendererVulkanBackend::StaticSlice oldVbTex = entry.vbTex;
  Z3DRendererVulkanBackend::StaticSlice oldIb = entry.ib;
  entry.vbPos = entry.replacement.vbPos;
  entry.vbNorm = entry.replacement.vbNorm;
  entry.vbColor = entry.replacement.vbColor;
  entry.vbTex = entry.replacement.vbTex;
  entry.ib = entry.replacement.ib;
  updateActiveState(entry, replacementState);
  entry.promoted = true;
  entry.usedStaticOnce = false;
  entry.replacement = {};
  vulkan::releaseStaticSlices(m_backend, {&oldVbPos, &oldVbNorm, &oldVbColor, &oldVbTex, &oldIb});
}

void ZVulkanMeshStreamCoordinator::pinActiveSlicesForActiveSubmission(const CacheEntry& entry)
{
  vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                             {&entry.vbPos, &entry.vbNorm, &entry.vbColor, &entry.vbTex, &entry.ib});
}

} // namespace nim
