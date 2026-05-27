#include "z3dblockidcollector.h"

#include "zcommandlineflags.h"
#include "zlog.h"

#include <absl/container/flat_hash_set.h>
#include <absl/log/check.h>
#include <boost/unordered/concurrent_flat_set.hpp>
#include <boost/sort/spreadsort/integer_sort.hpp>
#include <folly/container/F14Set.h>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

ABSL_FLAG(bool,
          atlas_benchmark_blockid_collectors,
          false,
          "Replay real OpenGL block-ID readback buffers through all CPU collector candidates and log best-of timings.");

ABSL_FLAG(
  int32_t,
  atlas_benchmark_blockid_collectors_runs,
  3,
  "Number of replay runs per block-ID collector candidate when --atlas_benchmark_blockid_collectors is enabled.");

namespace nim {

namespace {

constexpr uint32_t kZeroBlockId = 0u;
constexpr uint32_t kInvalidBlockId = std::numeric_limits<uint32_t>::max();

void checkMaxBlockId(uint32_t maxBlockId)
{
  CHECK_LT(maxBlockId, kInvalidBlockId) << "Block-ID domain collides with the invalid block-ID sentinel";
}

[[nodiscard]] size_t bitsetWordCount(uint32_t maxBlockId)
{
  return std::max<size_t>(1u, (static_cast<size_t>(maxBlockId) + 64u) / 64u);
}

void sortBlockIds(std::vector<uint32_t>& ids, Z3DBlockIdSortOrder order)
{
  if (order == Z3DBlockIdSortOrder::Descending) {
    std::sort(ids.begin(), ids.end(), std::greater<uint32_t>{});
  } else {
    std::sort(ids.begin(), ids.end());
  }
}

void sortAndUniqueBlockIds(std::vector<uint32_t>& ids, Z3DBlockIdSortOrder order)
{
  boost::sort::spreadsort::integer_sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (order == Z3DBlockIdSortOrder::Descending) {
    std::reverse(ids.begin(), ids.end());
  }
}

constexpr std::array<Z3DBlockIdCollectorMethodInfo, 9> kCollectorMethods = {
  {
   {Z3DBlockIdCollectorMethod::ProductionDefault, "production_default"},
   {Z3DBlockIdCollectorMethod::DenseBitset, "dense_bitset"},
   {Z3DBlockIdCollectorMethod::CurrentTbbConcurrentUnorderedSet, "current_tbb_concurrent_unordered_set"},
   {Z3DBlockIdCollectorMethod::FilteredTbbConcurrentUnorderedSet, "filtered_tbb_concurrent_unordered_set"},
   {Z3DBlockIdCollectorMethod::BoostConcurrentFlatSet, "boost_concurrent_flat_set"},
   {Z3DBlockIdCollectorMethod::ThreadLocalVectorParallelSort, "thread_local_vector_parallel_sort"},
   {Z3DBlockIdCollectorMethod::ThreadLocalAbslFlatHashSetMerge, "thread_local_absl_flat_hash_set_merge"},
   {Z3DBlockIdCollectorMethod::ThreadLocalFollyF14FastSetMerge, "thread_local_folly_f14_fast_set_merge"},
   {Z3DBlockIdCollectorMethod::SequentialIntegerSort, "sequential_integer_sort"},
   }
};

struct BufferScanFlags
{
  bool sawZero = false;
  bool sawInvalid = false;
  bool sawNonZero = false;
};

void recordLocalSentinels(BufferScanFlags localFlags,
                          std::atomic<bool>& sawZero,
                          std::atomic<bool>& sawInvalid,
                          std::atomic<bool>& sawNonZero)
{
  if (localFlags.sawZero) {
    sawZero.store(true, std::memory_order_relaxed);
  }
  if (localFlags.sawInvalid) {
    sawInvalid.store(true, std::memory_order_relaxed);
  }
  if (localFlags.sawNonZero) {
    sawNonZero.store(true, std::memory_order_relaxed);
  }
}

BufferScanFlags loadBufferScanFlags(const std::atomic<bool>& sawZero,
                                    const std::atomic<bool>& sawInvalid,
                                    const std::atomic<bool>& sawNonZero)
{
  return BufferScanFlags{
    .sawZero = sawZero.load(std::memory_order_relaxed),
    .sawInvalid = sawInvalid.load(std::memory_order_relaxed),
    .sawNonZero = sawNonZero.load(std::memory_order_relaxed),
  };
}

void finishBufferStats(Z3DBlockIdCollectionStats& stats,
                       size_t wordsProcessed,
                       size_t uniqueBlockIds,
                       BufferScanFlags bufferFlags)
{
  stats.buffersProcessed += 1;
  stats.wordsProcessed += wordsProcessed;
  stats.uniqueBlockIds = uniqueBlockIds;
  stats.sawZero = stats.sawZero || bufferFlags.sawZero;
  stats.sawInvalid = stats.sawInvalid || bufferFlags.sawInvalid;
  stats.lastBufferAllZero = !bufferFlags.sawNonZero;
  stats.uniqueIdsIncludingSentinels = stats.uniqueBlockIds + (stats.sawZero ? 1u : 0u) + (stats.sawInvalid ? 1u : 0u);
}

[[nodiscard]] size_t wordIndex(uint32_t blockId)
{
  return static_cast<size_t>(blockId) >> 6u;
}

[[nodiscard]] uint64_t wordMask(uint32_t blockId)
{
  return uint64_t{1} << (blockId & 63u);
}

[[nodiscard]] bool setBitIfNew(std::vector<uint64_t>& bits, uint32_t blockId)
{
  const size_t index = wordIndex(blockId);
  CHECK_LT(index, bits.size());
  const uint64_t mask = wordMask(blockId);
  uint64_t& word = bits[index];
  const bool isNew = (word & mask) == 0u;
  word |= mask;
  return isNew;
}

template<typename LocalBits>
[[nodiscard]] size_t mergeLocalBitsets(std::vector<uint64_t>& target, const LocalBits& localBits)
{
  size_t addedIds = 0;
  for (const auto& local : localBits) {
    CHECK_EQ(local.size(), target.size());
    for (size_t i = 0; i < target.size(); ++i) {
      const uint64_t oldWord = target[i];
      const uint64_t newWord = oldWord | local[i];
      if (newWord != oldWord) {
        addedIds += static_cast<size_t>(std::popcount(newWord) - std::popcount(oldWord));
        target[i] = newWord;
      }
    }
  }
  return addedIds;
}

std::vector<uint32_t>
blockIdsFromBitset(std::span<const uint64_t> bits, size_t uniqueBlockIds, Z3DBlockIdSortOrder order)
{
  std::vector<uint32_t> out;
  out.reserve(uniqueBlockIds);
  if (order == Z3DBlockIdSortOrder::Descending) {
    for (size_t word = bits.size(); word > 0; --word) {
      uint64_t wordBits = bits[word - 1];
      while (wordBits != 0u) {
        const uint32_t bit = 63u - static_cast<uint32_t>(std::countl_zero(wordBits));
        out.push_back(static_cast<uint32_t>(((word - 1) << 6u) + bit));
        wordBits &= ~(uint64_t{1} << bit);
      }
    }
  } else {
    for (size_t word = 0; word < bits.size(); ++word) {
      uint64_t wordBits = bits[word];
      while (wordBits != 0u) {
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(wordBits));
        out.push_back(static_cast<uint32_t>((word << 6u) + bit));
        wordBits &= wordBits - 1u;
      }
    }
  }
  CHECK_EQ(out.size(), uniqueBlockIds);
  return out;
}

template<typename F>
Z3DBlockIdCollectorTiming timeCollectorBestOf(std::string name, int runs, const std::vector<uint32_t>& reference, F&& f)
{
  CHECK_GT(runs, 0);

  double bestMilliseconds = std::numeric_limits<double>::infinity();
  size_t uniqueBlockIds = 0;
  for (int run = 0; run < runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint32_t> ids = f();
    const auto stop = std::chrono::steady_clock::now();
    CHECK(ids == reference) << "Block-ID collector benchmark mismatch for " << name << " on run " << run;

    uniqueBlockIds = ids.size();
    bestMilliseconds = std::min(bestMilliseconds, std::chrono::duration<double, std::milli>(stop - start).count());
  }

  return Z3DBlockIdCollectorTiming{std::move(name), bestMilliseconds, uniqueBlockIds};
}

class DenseBitsetCollectorState
{
public:
  DenseBitsetCollectorState()
  {
    reset(0);
  }

  explicit DenseBitsetCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    checkMaxBlockId(maxBlockId);
    m_maxBlockId = maxBlockId;
    m_wordCount = bitsetWordCount(maxBlockId);
    m_bits.assign(m_wordCount, 0u);
    m_localBits = std::make_unique<tbb::enumerable_thread_specific<std::vector<uint64_t>>>([wordCount = m_wordCount]() {
      return std::vector<uint64_t>(wordCount, 0u);
    });
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    CHECK(m_localBits != nullptr);
    for (auto& local : *m_localBits) {
      if (local.size() != m_wordCount) {
        local.assign(m_wordCount, 0u);
      } else {
        std::fill(local.begin(), local.end(), 0u);
      }
    }

    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      auto& local = m_localBits->local();
      if (local.size() != m_wordCount) {
        local.assign(m_wordCount, 0u);
      }

      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t blockId = blockIds[i];
        if (blockId == kZeroBlockId) {
          localFlags.sawZero = true;
          continue;
        }
        localFlags.sawNonZero = true;
        if (blockId == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          continue;
        }

        CHECK_LE(blockId, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
        local[wordIndex(blockId)] |= wordMask(blockId);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    m_stats.uniqueBlockIds += mergeLocalBitsets(m_bits, *m_localBits);
    finishBufferStats(m_stats,
                      blockIds.size(),
                      m_stats.uniqueBlockIds,
                      loadBufferScanFlags(sawZero, sawInvalid, sawNonZero));
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    return blockIdsFromBitset(m_bits, m_stats.uniqueBlockIds, order);
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  size_t m_wordCount = 1;
  std::vector<uint64_t> m_bits;
  std::unique_ptr<tbb::enumerable_thread_specific<std::vector<uint64_t>>> m_localBits;
  Z3DBlockIdCollectionStats m_stats;
};

class CurrentTbbConcurrentSetCollectorState
{
public:
  explicit CurrentTbbConcurrentSetCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t id = blockIds[i];
        if (id == kZeroBlockId) {
          localFlags.sawZero = true;
        } else if (id == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          localFlags.sawNonZero = true;
        } else {
          CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
          localFlags.sawNonZero = true;
        }
        m_ids.insert(id);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    const BufferScanFlags bufferFlags = loadBufferScanFlags(sawZero, sawInvalid, sawNonZero);
    const bool sawZeroAfterBuffer = m_stats.sawZero || bufferFlags.sawZero;
    const bool sawInvalidAfterBuffer = m_stats.sawInvalid || bufferFlags.sawInvalid;
    const size_t uniqueBlockIds = m_ids.size() - (sawZeroAfterBuffer ? 1u : 0u) - (sawInvalidAfterBuffer ? 1u : 0u);
    finishBufferStats(m_stats, blockIds.size(), uniqueBlockIds, bufferFlags);
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out;
    out.reserve(m_stats.uniqueBlockIds);
    for (uint32_t id : m_ids) {
      if (id != kZeroBlockId && id != kInvalidBlockId) {
        out.push_back(id);
      }
    }
    sortBlockIds(out, order);
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  tbb::concurrent_unordered_set<uint32_t> m_ids;
  Z3DBlockIdCollectionStats m_stats;
};

class FilteredTbbConcurrentSetCollectorState
{
public:
  explicit FilteredTbbConcurrentSetCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t id = blockIds[i];
        if (id == kZeroBlockId) {
          localFlags.sawZero = true;
          continue;
        }
        localFlags.sawNonZero = true;
        if (id == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          continue;
        }
        CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
        m_ids.insert(id);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    finishBufferStats(m_stats, blockIds.size(), m_ids.size(), loadBufferScanFlags(sawZero, sawInvalid, sawNonZero));
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out;
    out.reserve(m_ids.size());
    out.insert(out.end(), m_ids.begin(), m_ids.end());
    sortBlockIds(out, order);
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  tbb::concurrent_unordered_set<uint32_t> m_ids;
  Z3DBlockIdCollectionStats m_stats;
};

class BoostConcurrentFlatSetCollectorState
{
public:
  explicit BoostConcurrentFlatSetCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t id = blockIds[i];
        if (id == kZeroBlockId) {
          localFlags.sawZero = true;
          continue;
        }
        localFlags.sawNonZero = true;
        if (id == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          continue;
        }
        CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
        m_ids.insert(id);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    finishBufferStats(m_stats, blockIds.size(), m_ids.size(), loadBufferScanFlags(sawZero, sawInvalid, sawNonZero));
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out;
    out.reserve(m_ids.size());
    m_ids.visit_all([&out](const uint32_t& id) {
      out.push_back(id);
    });
    sortBlockIds(out, order);
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  boost::concurrent_flat_set<uint32_t> m_ids;
  Z3DBlockIdCollectionStats m_stats;
};

class ThreadLocalVectorSortCollectorState
{
public:
  explicit ThreadLocalVectorSortCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_seenBits.assign(bitsetWordCount(maxBlockId), 0u);
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    const size_t wordCount = m_seenBits.size();
    tbb::enumerable_thread_specific<std::vector<uint32_t>> localIds;
    tbb::enumerable_thread_specific<std::vector<uint64_t>> localBits([wordCount]() {
      return std::vector<uint64_t>(wordCount, 0u);
    });
    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      auto& ids = localIds.local();
      auto& bits = localBits.local();
      CHECK_EQ(bits.size(), wordCount);

      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t id = blockIds[i];
        if (id == kZeroBlockId) {
          localFlags.sawZero = true;
          continue;
        }
        localFlags.sawNonZero = true;
        if (id == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          continue;
        }
        CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
        ids.push_back(id);
        bits[wordIndex(id)] |= wordMask(id);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    size_t addedWords = 0;
    for (const auto& ids : localIds) {
      addedWords += ids.size();
    }
    m_ids.reserve(m_ids.size() + addedWords);
    for (const auto& ids : localIds) {
      m_ids.insert(m_ids.end(), ids.begin(), ids.end());
    }

    m_stats.uniqueBlockIds += mergeLocalBitsets(m_seenBits, localBits);
    finishBufferStats(m_stats,
                      blockIds.size(),
                      m_stats.uniqueBlockIds,
                      loadBufferScanFlags(sawZero, sawInvalid, sawNonZero));
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out = m_ids;
    tbb::parallel_sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (order == Z3DBlockIdSortOrder::Descending) {
      std::reverse(out.begin(), out.end());
    }
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  std::vector<uint32_t> m_ids;
  std::vector<uint64_t> m_seenBits;
  Z3DBlockIdCollectionStats m_stats;
};

template<typename Set>
class ThreadLocalHashSetMergeCollectorState
{
public:
  explicit ThreadLocalHashSetMergeCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    tbb::enumerable_thread_specific<Set> localIds;
    std::atomic<bool> sawZero{false};
    std::atomic<bool> sawInvalid{false};
    std::atomic<bool> sawNonZero{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, blockIds.size()), [&](const tbb::blocked_range<size_t>& range) {
      auto& ids = localIds.local();

      BufferScanFlags localFlags;
      for (size_t i = range.begin(); i != range.end(); ++i) {
        const uint32_t id = blockIds[i];
        if (id == kZeroBlockId) {
          localFlags.sawZero = true;
          continue;
        }
        localFlags.sawNonZero = true;
        if (id == kInvalidBlockId) {
          localFlags.sawInvalid = true;
          continue;
        }
        CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
        ids.insert(id);
      }
      recordLocalSentinels(localFlags, sawZero, sawInvalid, sawNonZero);
    });

    size_t totalLocalUnique = 0;
    for (const auto& ids : localIds) {
      totalLocalUnique += ids.size();
    }
    m_ids.reserve(m_ids.size() + totalLocalUnique);
    for (const auto& ids : localIds) {
      m_ids.insert(ids.begin(), ids.end());
    }

    finishBufferStats(m_stats, blockIds.size(), m_ids.size(), loadBufferScanFlags(sawZero, sawInvalid, sawNonZero));
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out;
    out.reserve(m_ids.size());
    out.insert(out.end(), m_ids.begin(), m_ids.end());
    sortBlockIds(out, order);
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  Set m_ids;
  Z3DBlockIdCollectionStats m_stats;
};

class SequentialIntegerSortCollectorState
{
public:
  explicit SequentialIntegerSortCollectorState(uint32_t maxBlockId)
  {
    reset(maxBlockId);
  }

  void reset(uint32_t maxBlockId)
  {
    m_maxBlockId = maxBlockId;
    m_seenBits.assign(bitsetWordCount(maxBlockId), 0u);
    m_ids.clear();
    m_stats = {};
  }

  void addBuffer(std::span<const uint32_t> blockIds)
  {
    BufferScanFlags bufferFlags;
    size_t addedIds = 0;
    for (const uint32_t id : blockIds) {
      if (id == kZeroBlockId) {
        bufferFlags.sawZero = true;
        continue;
      }
      bufferFlags.sawNonZero = true;
      if (id == kInvalidBlockId) {
        bufferFlags.sawInvalid = true;
        continue;
      }
      CHECK_LE(id, m_maxBlockId) << "Block-ID shader emitted an ID outside the image block-ID domain";
      m_ids.push_back(id);
      if (setBitIfNew(m_seenBits, id)) {
        ++addedIds;
      }
    }

    m_stats.uniqueBlockIds += addedIds;
    finishBufferStats(m_stats, blockIds.size(), m_stats.uniqueBlockIds, bufferFlags);
  }

  [[nodiscard]] std::vector<uint32_t> blockIds(Z3DBlockIdSortOrder order) const
  {
    std::vector<uint32_t> out = m_ids;
    sortAndUniqueBlockIds(out, order);
    CHECK_EQ(out.size(), m_stats.uniqueBlockIds);
    return out;
  }

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const
  {
    return m_stats;
  }

private:
  uint32_t m_maxBlockId = 0;
  std::vector<uint32_t> m_ids;
  std::vector<uint64_t> m_seenBits;
  Z3DBlockIdCollectionStats m_stats;
};

using ProductionBlockIdCollectorState = DenseBitsetCollectorState;

template<typename State>
std::vector<uint32_t>
collectWithState(const Z3DBlockIdBufferList& buffers, uint32_t maxBlockId, Z3DBlockIdSortOrder order)
{
  checkMaxBlockId(maxBlockId);
  State state(maxBlockId);
  for (const auto& buffer : buffers) {
    state.addBuffer(buffer);
  }
  return state.blockIds(order);
}

std::vector<uint32_t> collectWithMethodState(const Z3DBlockIdBufferList& buffers,
                                             uint32_t maxBlockId,
                                             Z3DBlockIdSortOrder order,
                                             Z3DBlockIdCollectorMethod method)
{
  switch (method) {
    case Z3DBlockIdCollectorMethod::ProductionDefault:
      return collectWithState<ProductionBlockIdCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::DenseBitset:
      return collectWithState<DenseBitsetCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::CurrentTbbConcurrentUnorderedSet:
      return collectWithState<CurrentTbbConcurrentSetCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::FilteredTbbConcurrentUnorderedSet:
      return collectWithState<FilteredTbbConcurrentSetCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::BoostConcurrentFlatSet:
      return collectWithState<BoostConcurrentFlatSetCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::ThreadLocalVectorParallelSort:
      return collectWithState<ThreadLocalVectorSortCollectorState>(buffers, maxBlockId, order);
    case Z3DBlockIdCollectorMethod::ThreadLocalAbslFlatHashSetMerge:
      return collectWithState<ThreadLocalHashSetMergeCollectorState<absl::flat_hash_set<uint32_t>>>(buffers,
                                                                                                    maxBlockId,
                                                                                                    order);
    case Z3DBlockIdCollectorMethod::ThreadLocalFollyF14FastSetMerge:
      return collectWithState<ThreadLocalHashSetMergeCollectorState<folly::F14FastSet<uint32_t>>>(buffers,
                                                                                                  maxBlockId,
                                                                                                  order);
    case Z3DBlockIdCollectorMethod::SequentialIntegerSort:
      return collectWithState<SequentialIntegerSortCollectorState>(buffers, maxBlockId, order);
    default:
      CHECK(false) << "Unknown block-ID collector method: " << static_cast<int>(method);
  }
  return {};
}

} // namespace

bool shouldBenchmarkZ3DBlockIdCollectors()
{
  return absl::GetFlag(FLAGS_atlas_benchmark_blockid_collectors);
}

int z3DBlockIdCollectorBenchmarkRuns()
{
  return std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_benchmark_blockid_collectors_runs));
}

std::span<const Z3DBlockIdCollectorMethodInfo> z3DBlockIdCollectorMethods()
{
  return std::span<const Z3DBlockIdCollectorMethodInfo>(kCollectorMethods.data(), kCollectorMethods.size());
}

std::vector<uint32_t> collectZ3DBlockIdsForBenchmark(const Z3DBlockIdBufferList& buffers,
                                                     uint32_t maxBlockId,
                                                     Z3DBlockIdSortOrder order,
                                                     Z3DBlockIdCollectorMethod method)
{
  return collectWithMethodState(buffers, maxBlockId, order, method);
}

std::vector<Z3DBlockIdCollectorTiming> benchmarkZ3DBlockIdCollectors(const Z3DBlockIdBufferList& buffers,
                                                                     uint32_t maxBlockId,
                                                                     Z3DBlockIdSortOrder order,
                                                                     int runs)
{
  const int effectiveRuns = std::max(1, runs);
  const std::vector<uint32_t> reference =
    collectZ3DBlockIdsForBenchmark(buffers, maxBlockId, order, Z3DBlockIdCollectorMethod::DenseBitset);

  std::vector<Z3DBlockIdCollectorTiming> timings;
  timings.reserve(kCollectorMethods.size());
  for (const Z3DBlockIdCollectorMethodInfo& methodInfo : kCollectorMethods) {
    timings.push_back(timeCollectorBestOf(std::string(methodInfo.name), effectiveRuns, reference, [&]() {
      return collectZ3DBlockIdsForBenchmark(buffers, maxBlockId, order, methodInfo.method);
    }));
  }

  std::sort(timings.begin(),
            timings.end(),
            [](const Z3DBlockIdCollectorTiming& lhs, const Z3DBlockIdCollectorTiming& rhs) {
              return lhs.milliseconds < rhs.milliseconds;
            });
  return timings;
}

std::string formatZ3DBlockIdCollectorTimings(std::span<const Z3DBlockIdCollectorTiming> timings)
{
  std::ostringstream stream;
  bool first = true;
  for (const auto& timing : timings) {
    if (!first) {
      stream << ", ";
    }
    first = false;
    stream << timing.name << '=' << timing.milliseconds << "ms";
  }
  return stream.str();
}

struct Z3DBlockIdCollector::State
{
  explicit State(uint32_t maxBlockId)
    : collector(maxBlockId)
  {}

  ProductionBlockIdCollectorState collector;
};

Z3DBlockIdCollector::Z3DBlockIdCollector()
{
  reset(0);
}

Z3DBlockIdCollector::Z3DBlockIdCollector(uint32_t maxBlockId)
{
  reset(maxBlockId);
}

Z3DBlockIdCollector::~Z3DBlockIdCollector() = default;

void Z3DBlockIdCollector::reset(uint32_t maxBlockId)
{
  checkMaxBlockId(maxBlockId);
  m_maxBlockId = maxBlockId;
  m_state = std::make_unique<State>(maxBlockId);
  m_recordBenchmarkBuffers = shouldBenchmarkZ3DBlockIdCollectors();
  m_benchmarkBuffers.clear();
}

void Z3DBlockIdCollector::addBuffer(std::span<const uint32_t> blockIds)
{
  if (m_recordBenchmarkBuffers) {
    m_benchmarkBuffers.emplace_back(blockIds.begin(), blockIds.end());
  }

  CHECK(m_state != nullptr);
  m_state->collector.addBuffer(blockIds);
}

const Z3DBlockIdCollectionStats& Z3DBlockIdCollector::stats() const
{
  CHECK(m_state != nullptr);
  return m_state->collector.stats();
}

void Z3DBlockIdCollector::fillSortedBlockIds(std::vector<uint32_t>& out, Z3DBlockIdSortOrder order) const
{
  CHECK(m_state != nullptr);
  out = m_state->collector.blockIds(order);
}

void Z3DBlockIdCollector::benchmarkCollectors(std::string_view label, Z3DBlockIdSortOrder order) const
{
  if (!m_recordBenchmarkBuffers) {
    return;
  }

  std::vector<uint32_t> reference;
  fillSortedBlockIds(reference, order);

  const int runs = z3DBlockIdCollectorBenchmarkRuns();
  std::vector<Z3DBlockIdCollectorTiming> timings =
    benchmarkZ3DBlockIdCollectors(m_benchmarkBuffers, m_maxBlockId, order, runs);
  for (const Z3DBlockIdCollectorTiming& timing : timings) {
    CHECK_EQ(timing.uniqueBlockIds, reference.size())
      << "Block-ID collector benchmark mismatch for " << timing.name << " in " << label;
  }

  const Z3DBlockIdCollectionStats& currentStats = stats();
  LOG(INFO) << "Block-ID collector benchmark [" << label << "]: buffers=" << currentStats.buffersProcessed
            << " words=" << currentStats.wordsProcessed << " maxBlockId=" << m_maxBlockId
            << " unique=" << currentStats.uniqueBlockIds
            << " order=" << (order == Z3DBlockIdSortOrder::Descending ? "desc" : "asc") << " runs=" << runs
            << " timings {" << formatZ3DBlockIdCollectorTimings(timings) << "}";
}

} // namespace nim
