#include "z3dblockidcollector.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace nim {
namespace {

std::vector<uint32_t> uniqueSortedReference(const std::vector<std::vector<uint32_t>>& buffers,
                                            Z3DBlockIdSortOrder order)
{
  std::vector<uint32_t> ids;
  for (const auto& buffer : buffers) {
    for (const uint32_t id : buffer) {
      if (id != 0u && id != std::numeric_limits<uint32_t>::max()) {
        ids.push_back(id);
      }
    }
  }

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (order == Z3DBlockIdSortOrder::Descending) {
    std::reverse(ids.begin(), ids.end());
  }
  return ids;
}

} // namespace

TEST(Z3DBlockIdCollectorTest, IgnoresSentinelsAndSortsBothDirections)
{
  const std::vector<std::vector<uint32_t>> buffers = {
    {0u, 4u, 2u, 2u, std::numeric_limits<uint32_t>::max(), 128u, 65u, 0u}
  };

  Z3DBlockIdCollector collector(128u);
  collector.addBuffer(buffers.front());

  const Z3DBlockIdCollectionStats& stats = collector.stats();
  EXPECT_EQ(stats.buffersProcessed, 1u);
  EXPECT_EQ(stats.wordsProcessed, buffers.front().size());
  EXPECT_EQ(stats.uniqueBlockIds, 4u);
  EXPECT_FALSE(stats.lastBufferAllZero);

  std::vector<uint32_t> ids;
  collector.fillSortedBlockIds(ids, Z3DBlockIdSortOrder::Ascending);
  EXPECT_EQ(ids, uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Ascending));

  collector.fillSortedBlockIds(ids, Z3DBlockIdSortOrder::Descending);
  EXPECT_EQ(ids, uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Descending));
}

TEST(Z3DBlockIdCollectorTest, AccumulatesAcrossBuffersAndTracksLastAllZeroBuffer)
{
  const std::vector<std::vector<uint32_t>> buffers = {
    {1u, 2u, 2u, 0u},
    {0u, 0u, 0u}
  };

  Z3DBlockIdCollector collector(512u);
  for (const auto& buffer : buffers) {
    collector.addBuffer(buffer);
  }

  const Z3DBlockIdCollectionStats& stats = collector.stats();
  EXPECT_EQ(stats.buffersProcessed, buffers.size());
  EXPECT_EQ(stats.wordsProcessed, 7u);
  EXPECT_EQ(stats.uniqueBlockIds, 2u);
  EXPECT_TRUE(stats.lastBufferAllZero);

  std::vector<uint32_t> ids;
  collector.fillSortedBlockIds(ids, Z3DBlockIdSortOrder::Ascending);
  EXPECT_EQ(ids, uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Ascending));
}

TEST(Z3DBlockIdCollectorTest, MatchesReferenceOnGeneratedRendererLikeInputs)
{
  constexpr uint32_t kMaxBlockId = 8192u;
  constexpr size_t kBufferCount = 4u;
  constexpr size_t kBufferSize = 32768u;

  std::mt19937 rng(0xA71A5u);
  std::uniform_int_distribution<uint32_t> idDist(1u, kMaxBlockId);
  std::uniform_int_distribution<uint32_t> eventDist(0u, 99u);

  std::vector<std::vector<uint32_t>> buffers(kBufferCount);
  for (auto& buffer : buffers) {
    buffer.reserve(kBufferSize);
    for (size_t i = 0; i < kBufferSize; ++i) {
      const uint32_t event = eventDist(rng);
      if (event < 55u) {
        buffer.push_back(0u);
      } else if (event == 99u) {
        buffer.push_back(std::numeric_limits<uint32_t>::max());
      } else {
        buffer.push_back(idDist(rng));
      }
    }
  }

  Z3DBlockIdCollector collector(kMaxBlockId);
  for (const auto& buffer : buffers) {
    collector.addBuffer(buffer);
  }

  std::vector<uint32_t> ids;
  collector.fillSortedBlockIds(ids, Z3DBlockIdSortOrder::Ascending);
  EXPECT_EQ(ids, uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Ascending));

  collector.fillSortedBlockIds(ids, Z3DBlockIdSortOrder::Descending);
  EXPECT_EQ(ids, uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Descending));
}

TEST(Z3DBlockIdCollectorTest, AllBenchmarkMethodStatesMatchReference)
{
  constexpr uint32_t kMaxBlockId = 2048u;
  const std::vector<std::vector<uint32_t>> buffers = {
    {0u, 1u, 17u, 17u, 128u, std::numeric_limits<uint32_t>::max(), 2048u},
    {0u, 2u, 128u, 512u, 1024u, 2048u},
    {0u, 0u, 512u, 1024u, 1024u},
  };

  const std::vector<uint32_t> ascendingReference = uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Ascending);
  const std::vector<uint32_t> descendingReference = uniqueSortedReference(buffers, Z3DBlockIdSortOrder::Descending);

  for (const Z3DBlockIdCollectorMethodInfo& method : z3DBlockIdCollectorMethods()) {
    EXPECT_EQ(collectZ3DBlockIdsForBenchmark(buffers, kMaxBlockId, Z3DBlockIdSortOrder::Ascending, method.method),
              ascendingReference)
      << method.name;
    EXPECT_EQ(collectZ3DBlockIdsForBenchmark(buffers, kMaxBlockId, Z3DBlockIdSortOrder::Descending, method.method),
              descendingReference)
      << method.name;
  }
}

} // namespace nim
