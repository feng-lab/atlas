#pragma once

#include "z3drenderervulkanbackend.h"

#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace nim {

template<typename CacheKey, typename CacheEntry, typename PendingUploadBinding>
class ZVulkanStreamCacheCoordinator
{
public:
  using StaticCacheMap = std::map<CacheKey, CacheEntry>;
  using PendingUploadMap = std::map<CacheKey, PendingUploadBinding>;

  void resetFrame()
  {
    m_pendingKeys.clear();
    m_pendingUploads.clear();
  }

  template<typename ReleaseEntryFn>
  void evictStream(uint64_t streamKey, ReleaseEntryFn&& releaseEntry)
  {
    if (streamKey == 0) {
      return;
    }

    for (auto it = m_pendingKeys.begin(); it != m_pendingKeys.end();) {
      if (it->streamKey == streamKey) {
        it = m_pendingKeys.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = m_pendingUploads.begin(); it != m_pendingUploads.end();) {
      if (it->first.streamKey == streamKey) {
        it = m_pendingUploads.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = m_staticCache.begin(); it != m_staticCache.end();) {
      if (it->first.streamKey != streamKey) {
        ++it;
        continue;
      }
      releaseEntry(it->second);
      it = m_staticCache.erase(it);
    }
  }

  [[nodiscard]] CacheEntry* findEntry(const CacheKey& key)
  {
    auto it = m_staticCache.find(key);
    return it != m_staticCache.end() ? &it->second : nullptr;
  }

  [[nodiscard]] const CacheEntry* findEntry(const CacheKey& key) const
  {
    auto it = m_staticCache.find(key);
    return it != m_staticCache.end() ? &it->second : nullptr;
  }

  CacheEntry& ensureEntry(const CacheKey& key)
  {
    return m_staticCache.try_emplace(key).first->second;
  }

  [[nodiscard]] std::pair<CacheEntry&, bool> tryEmplaceEntry(const CacheKey& key)
  {
    auto [it, inserted] = m_staticCache.try_emplace(key);
    return {it->second, inserted};
  }

  [[nodiscard]] bool hasPendingCopy(const CacheKey& key) const
  {
    return m_pendingKeys.contains(key);
  }

  void markPendingCopy(const CacheKey& key)
  {
    m_pendingKeys.insert(key);
  }

  [[nodiscard]] const PendingUploadBinding* findPendingUpload(const CacheKey& key) const
  {
    auto it = m_pendingUploads.find(key);
    return it != m_pendingUploads.end() ? &it->second : nullptr;
  }

  void rememberPendingUploadBinding(const CacheKey& key, const PendingUploadBinding& binding)
  {
    m_pendingUploads[key] = binding;
  }

  template<typename BytesForEntryFn>
  [[nodiscard]] size_t staticBytesForStream(uint64_t streamKey,
                                            Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                            BytesForEntryFn&& bytesForEntry) const
  {
    size_t bytes = 0;
    for (const auto& [key, entry] : m_staticCache) {
      if (key.streamKey != streamKey) {
        continue;
      }
      bytes += bytesForEntry(entry, domain);
    }
    return bytes;
  }

private:
  StaticCacheMap m_staticCache;
  std::set<CacheKey> m_pendingKeys;
  PendingUploadMap m_pendingUploads;
};

class ZVulkanStaticStreamUsageTracker
{
public:
  explicit ZVulkanStaticStreamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner owner)
    : m_owner(owner)
  {}

  void eraseStream(uint64_t streamKey)
  {
    if (streamKey == 0) {
      return;
    }
    m_streamLastUsedEpoch.erase(streamKey);
  }

  void touch(uint64_t streamKey, uint64_t currentEpoch)
  {
    if (streamKey == 0) {
      return;
    }
    m_streamLastUsedEpoch[streamKey] = currentEpoch;
  }

  template<typename BytesForStreamFn>
  [[nodiscard]] std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
  oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                              uint64_t protectedEpoch,
                              BytesForStreamFn&& bytesForStream) const
  {
    std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate> candidate;
    for (const auto& [streamKey, lastUsedEpoch] : m_streamLastUsedEpoch) {
      if (streamKey == 0 || lastUsedEpoch >= protectedEpoch) {
        continue;
      }
      const size_t bytes = bytesForStream(streamKey, domain);
      if (bytes == 0) {
        continue;
      }
      if (!candidate || lastUsedEpoch < candidate->lastUsedEpoch ||
          (lastUsedEpoch == candidate->lastUsedEpoch && bytes > candidate->bytes)) {
        candidate = Z3DRendererVulkanBackend::StaticPressureEvictionCandidate{.owner = m_owner,
                                                                              .streamKey = streamKey,
                                                                              .lastUsedEpoch = lastUsedEpoch,
                                                                              .bytes = bytes};
      }
    }
    return candidate;
  }

private:
  Z3DRendererVulkanBackend::StaticCacheOwner m_owner;
  std::unordered_map<uint64_t, uint64_t> m_streamLastUsedEpoch;
};

} // namespace nim
