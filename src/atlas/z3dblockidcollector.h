#ifndef Z3DBLOCKIDCOLLECTOR_H
#define Z3DBLOCKIDCOLLECTOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nim {

enum class Z3DBlockIdSortOrder
{
  Ascending,
  Descending
};

struct Z3DBlockIdCollectionStats
{
  size_t buffersProcessed = 0;
  size_t wordsProcessed = 0;
  size_t uniqueBlockIds = 0;
  size_t uniqueIdsIncludingSentinels = 0;
  bool sawZero = false;
  bool sawInvalid = false;
  bool lastBufferAllZero = true;
};

enum class Z3DBlockIdCollectorMethod
{
  ProductionDefault,
  DenseBitset,
  CurrentTbbConcurrentUnorderedSet,
  FilteredTbbConcurrentUnorderedSet,
  BoostConcurrentFlatSet,
  ThreadLocalVectorParallelSort,
  ThreadLocalAbslFlatHashSetMerge,
  ThreadLocalFollyF14FastSetMerge,
  SequentialIntegerSort
};

struct Z3DBlockIdCollectorMethodInfo
{
  Z3DBlockIdCollectorMethod method;
  std::string_view name;
};

struct Z3DBlockIdCollectorTiming
{
  std::string name;
  double milliseconds = 0.0;
  size_t uniqueBlockIds = 0;
};

using Z3DBlockIdBufferList = std::vector<std::vector<uint32_t>>;

[[nodiscard]] bool shouldBenchmarkZ3DBlockIdCollectors();
[[nodiscard]] int z3DBlockIdCollectorBenchmarkRuns();
[[nodiscard]] std::span<const Z3DBlockIdCollectorMethodInfo> z3DBlockIdCollectorMethods();

[[nodiscard]] std::vector<uint32_t> collectZ3DBlockIdsForBenchmark(const Z3DBlockIdBufferList& buffers,
                                                                   uint32_t maxBlockId,
                                                                   Z3DBlockIdSortOrder order,
                                                                   Z3DBlockIdCollectorMethod method);

[[nodiscard]] std::vector<Z3DBlockIdCollectorTiming> benchmarkZ3DBlockIdCollectors(const Z3DBlockIdBufferList& buffers,
                                                                                   uint32_t maxBlockId,
                                                                                   Z3DBlockIdSortOrder order,
                                                                                   int runs);

[[nodiscard]] std::string formatZ3DBlockIdCollectorTimings(std::span<const Z3DBlockIdCollectorTiming> timings);

class Z3DBlockIdCollector
{
public:
  Z3DBlockIdCollector();
  explicit Z3DBlockIdCollector(uint32_t maxBlockId);
  ~Z3DBlockIdCollector();

  void reset(uint32_t maxBlockId);
  void addBuffer(std::span<const uint32_t> blockIds);

  [[nodiscard]] const Z3DBlockIdCollectionStats& stats() const;

  void fillSortedBlockIds(std::vector<uint32_t>& out, Z3DBlockIdSortOrder order) const;

  void benchmarkCollectors(std::string_view label, Z3DBlockIdSortOrder order) const;

private:
  struct State;

  uint32_t m_maxBlockId = 0;
  std::unique_ptr<State> m_state;
  bool m_recordBenchmarkBuffers = false;
  Z3DBlockIdBufferList m_benchmarkBuffers;
};

} // namespace nim

#endif // Z3DBLOCKIDCOLLECTOR_H
