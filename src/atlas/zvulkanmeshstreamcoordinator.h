#pragma once

#include "z3drendercommands.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanstreamcachecoordinator.h"

#include <optional>
#include <tuple>

namespace nim {

class ZVulkanMeshStreamCoordinator
{
public:
  struct CacheKey
  {
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;

    auto tie() const
    {
      return std::tuple(streamKey, streamSegmentOrdinal, static_cast<int>(colorSource));
    }

    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct StreamState
  {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t posGen = 0;
    uint32_t normGen = 0;
    uint32_t colorGen = 0;
    uint32_t texGen = 0;
    uint32_t indexGen = 0;
    bool hasTex = false;
  };

  struct PendingUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice pos{};
    Z3DRendererVulkanBackend::UploadSlice norm{};
    Z3DRendererVulkanBackend::UploadSlice color{};
    Z3DRendererVulkanBackend::UploadSlice tex{};
    Z3DRendererVulkanBackend::UploadSlice index{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t posGen = 0;
    uint32_t normGen = 0;
    uint32_t colorGen = 0;
    uint32_t texGen = 0;
    uint32_t indexGen = 0;
  };

  struct CacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbPos{};
    Z3DRendererVulkanBackend::StaticSlice vbNorm{};
    Z3DRendererVulkanBackend::StaticSlice vbColor{};
    Z3DRendererVulkanBackend::StaticSlice vbTex{};
    bool hasTex = false;
    Z3DRendererVulkanBackend::StaticSlice ib{};

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t posGen = 0;
    uint32_t normGen = 0;
    uint32_t colorGen = 0;
    uint32_t texGen = 0;
    uint32_t indexGen = 0;

    uint32_t latestVertexCount = 0;
    uint32_t latestIndexCount = 0;
    uint32_t latestPosGen = 0;
    uint32_t latestNormGen = 0;
    uint32_t latestColorGen = 0;
    uint32_t latestTexGen = 0;
    uint32_t latestIndexGen = 0;
    bool latestHasTex = false;

    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;

    struct PendingReplacement
    {
      Z3DRendererVulkanBackend::StaticSlice vbPos{};
      Z3DRendererVulkanBackend::StaticSlice vbNorm{};
      Z3DRendererVulkanBackend::StaticSlice vbColor{};
      Z3DRendererVulkanBackend::StaticSlice vbTex{};
      bool hasTex = false;
      Z3DRendererVulkanBackend::StaticSlice ib{};

      uint32_t vertexCount = 0;
      uint32_t indexCount = 0;
      uint32_t posGen = 0;
      uint32_t normGen = 0;
      uint32_t colorGen = 0;
      uint32_t texGen = 0;
      uint32_t indexGen = 0;

      size_t posCopiedBytes = 0;
      size_t normCopiedBytes = 0;
      size_t colorCopiedBytes = 0;
      size_t texCopiedBytes = 0;
      size_t indexCopiedBytes = 0;

      bool allocated = false;
      bool readyToActivate = false;
    } replacement;
  };

  explicit ZVulkanMeshStreamCoordinator(Z3DRendererVulkanBackend& backend);

  void resetFrame();
  void evictStream(uint64_t streamKey);
  void touchStaticStream(uint64_t streamKey);

  [[nodiscard]] size_t staticBytesForStream(uint64_t streamKey,
                                            Z3DRendererVulkanBackend::StaticPressureDomain domain) const;
  [[nodiscard]] std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
  oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain, uint64_t protectedEpoch) const;

  [[nodiscard]] CacheEntry* findEntry(const CacheKey& key);
  [[nodiscard]] const CacheEntry* findEntry(const CacheKey& key) const;
  CacheEntry& ensureEntry(const CacheKey& key);

  [[nodiscard]] const PendingUploadBinding* findPendingUpload(const CacheKey& key) const;
  void rememberPendingUploadBinding(const CacheKey& key, const PendingUploadBinding& binding);

  static bool stateMatches(const StreamState& lhs, const StreamState& rhs);
  static StreamState activeStateForEntry(const CacheEntry& entry);
  static StreamState latestStateForEntry(const CacheEntry& entry);
  static StreamState replacementStateForEntry(const CacheEntry& entry);
  static void updateActiveState(CacheEntry& entry, const StreamState& state);
  static void updateLatestState(CacheEntry& entry, const StreamState& state);

  [[nodiscard]] bool pendingUploadMatches(const PendingUploadBinding& pending, const StreamState& state) const;
  [[nodiscard]] bool activeEntryMatches(const CacheEntry& entry, const StreamState& state) const;

  void releaseActiveSlices(CacheEntry& entry);
  void clearReplacement(CacheEntry& entry);
  void activateReplacement(CacheEntry& entry);
  void pinActiveSlicesForActiveSubmission(const CacheEntry& entry);

private:
  Z3DRendererVulkanBackend& m_backend;
  ZVulkanStreamCacheCoordinator<CacheKey, CacheEntry, PendingUploadBinding> m_streamCache;
  ZVulkanStaticStreamUsageTracker m_streamUsageTracker;
};

} // namespace nim
