#include "zneutubetraceseeder.h"

#include "zneutubestackfitoptions.h"
#include "zneutubetracelocseglabel.h"
#include "zcancellation.h"

#include "zlog.h"

#include <folly/ScopeGuard.h>
#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>
#include <thread>

DEFINE_bool(
  atlas_autotrace_seed_sort_commit_by_score,
  true,
  "Whether auto-trace seed sort commits prepared seeds in descending score order. Disable to preserve the legacy "
  "original-order base-mask commit semantics for parity tests.");

DEFINE_uint32(
  atlas_autotrace_seed_sort_precompute_window_size,
  0,
  "Maximum number of seed-sort fit tasks kept in flight while preparing scores on folly's global CPU executor. "
  "0 uses the global executor's thread count. "
  "Higher values can improve throughput when individual seeds have uneven runtimes, but also increase queued work "
  "that may still finish after cancellation is requested.");

DEFINE_bool(atlas_autotrace_seed_sort_log_cpu_executor_pool_stats,
            true,
            "Log folly global CPU executor pool stats during seed sort prepare progress reports. Useful for diagnosing "
            "whether the executor is idle, saturated, or queueing work.");

namespace nim {

namespace {

constexpr size_t SeedSortProgressReportEvery = 1000;
constexpr double SeedSortZScaleLegacyLike = 1.0;

[[nodiscard]] ZImg makeBaseMaskLike(const ZImg& signal)
{
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);
  ZImgInfo info(signal.width(), signal.height(), signal.depth(), 1, 1, 1, VoxelFormat::Unsigned);
  ZImg out(info);
  return out;
}

[[nodiscard]] int maskValueAt(const ZImg& mask, int x, int y, int z)
{
  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());
  if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
    return 0;
  }

  return imgTypeDispatcher(mask.info(), [&]<typename TVoxel>() -> int {
    return static_cast<int>(*mask.data<TVoxel>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
  });
}

void configureSeedSortFitWorkspace(TraceWorkspace& tw)
{
  tw.fitWorkspace.sws.fs.n = 2;
  tw.fitWorkspace.sws.fs.options[0] = static_cast<int>(StackFitOption::Dot);
  tw.fitWorkspace.sws.fs.options[1] = static_cast<int>(StackFitOption::Corrcoef);
  tw.fitWorkspace.posAdjust = 1;
}

[[nodiscard]] size_t globalCpuExecutorThreadCountOrFallback()
{
  auto cpuExecutor = folly::getGlobalCPUExecutor();
  if (auto* threadPool = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get())) {
    const size_t threads = threadPool->numThreads();
    if (threads > 0) {
      return threads;
    }
  }

  const size_t fallback = static_cast<size_t>(std::thread::hardware_concurrency());
  return std::max<size_t>(1, fallback);
}

[[nodiscard]] size_t seedSortMaxInFlight()
{
  const size_t concurrencyFloor = globalCpuExecutorThreadCountOrFallback() * 8;
  const size_t requested = static_cast<size_t>(FLAGS_atlas_autotrace_seed_sort_precompute_window_size);
  if (requested == 0) {
    return concurrencyFloor;
  }
  return std::max(concurrencyFloor, requested);
}

struct PreparedSeedSortEntry
{
  bool didOptimize = false;
  bool rejectedByTraceMask = false;
  size_t seedOffset = 0;
  LocalNeuroseg locseg{};
  double score = 0.0;
  LocsegFitWorkspace fitWorkspaceAfterOptimize{};
};

[[nodiscard]] folly::coro::Task<PreparedSeedSortEntry>
prepareSeedSortEntryAsync(const Geo3dScalarField& seeds,
                          const ZImg& signal,
                          const TraceWorkspace& tw,
                          size_t seedIndex,
                          size_t width,
                          size_t height,
                          size_t depth,
                          size_t plane,
                          folly::ThreadPoolExecutor* cpuThreadPool,
                          std::atomic<size_t>& preparedCount)
{
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  auto progressGuard = folly::makeGuard([&]() {
    const size_t done = preparedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (VLOG_IS_ON(1) && (((done % SeedSortProgressReportEvery) == 0) || done == seeds.size())) {
      if (FLAGS_atlas_autotrace_seed_sort_log_cpu_executor_pool_stats && cpuThreadPool != nullptr) {
        const auto poolStats = cpuThreadPool->getPoolStats();
        VLOG(1) << fmt::format("Seed sort prepare: {}/{} (pending/total tasks={}/{}, active/idle threads={}/{})",
                               done,
                               seeds.size(),
                               poolStats.pendingTaskCount,
                               poolStats.totalTaskCount,
                               poolStats.activeThreadCount,
                               poolStats.idleThreadCount);
      } else {
        VLOG(1) << fmt::format("Seed sort prepare: {}/{}", done, seeds.size());
      }
    }
  });

  PreparedSeedSortEntry out;

  const int x = static_cast<int>(seeds.points[seedIndex][0]);
  const int y = static_cast<int>(seeds.points[seedIndex][1]);
  const int z = static_cast<int>(seeds.points[seedIndex][2]);

  if (tw.traceMask && maskValueAt(*tw.traceMask, x, y, z) > 0) {
    co_return out;
  }

  double widthValue = seeds.values[seedIndex];
  if (widthValue < 3.0) {
    widthValue += 0.5;
  }

  if (x < 0 || y < 0 || z < 0 || static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height ||
      static_cast<size_t>(z) >= depth) {
    co_return out;
  }

  out.seedOffset = static_cast<size_t>(x) + static_cast<size_t>(y) * width + static_cast<size_t>(z) * plane;
  out.locseg.seg.r1 = widthValue;
  out.locseg.seg.c = 0.0;
  out.locseg.seg.h = NeurosegDefaultHLegacyLike;
  out.locseg.seg.theta = 0.0;
  out.locseg.seg.psi = 0.0;
  out.locseg.seg.curvature = 0.0;
  out.locseg.seg.alpha = 0.0;
  out.locseg.seg.scale = 1.0;

  const std::array<double, 3> cpos = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
  setNeurosegPositionLegacyLike(out.locseg, cpos, NeuroposReferenceLegacyLike::Center);

  LocsegFitWorkspace fitWorkspace = tw.fitWorkspace;
  (void)localNeurosegOptimizeWLegacyLike(out.locseg, signal, SeedSortZScaleLegacyLike, /*option*/ 0, fitWorkspace);
  maybeCancel(cancellationToken);

  out.didOptimize = true;
  out.fitWorkspaceAfterOptimize = fitWorkspace;

  if (tw.traceMask && localNeurosegHitMaskLegacyLike(out.locseg, *tw.traceMask)) {
    out.rejectedByTraceMask = true;
    co_return out;
  }

  out.score = fitWorkspace.sws.fs.scores[1];
  co_return out;
}

} // namespace

SeedSortResultLegacyLike sortSeedsLegacyLike(const Geo3dScalarField& seeds, const ZImg& signal, TraceWorkspace& tw)
{
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);

  configureSeedSortFitWorkspace(tw);
  maybeCancel(tw.cancellationToken);

  VLOG(1) << fmt::format("Seed sort: start (seeds={}, minScore={})", seeds.size(), tw.minScore);
  VLOG(1) << fmt::format(
    "Seed sort: global CPU executor threads={}, requested prepare window={}, max in-flight prepare "
    "tasks={}, commit order={}",
    globalCpuExecutorThreadCountOrFallback(),
    FLAGS_atlas_autotrace_seed_sort_precompute_window_size,
    seedSortMaxInFlight(),
    FLAGS_atlas_autotrace_seed_sort_commit_by_score ? "score" : "legacy");

  SeedSortResultLegacyLike out;
  out.locsegArray.resize(seeds.size());
  out.scoreArray.assign(seeds.size(), 0.0);
  out.baseMask = makeBaseMaskLike(signal);

  const size_t width = out.baseMask.width();
  const size_t height = out.baseMask.height();
  const size_t depth = out.baseMask.depth();
  const size_t plane = width * height;

  auto* baseMaskData = out.baseMask.timeData<uint8_t>(0);

  const bool commitByScore = FLAGS_atlas_autotrace_seed_sort_commit_by_score;
  bool haveLastCommittedFitWorkspace = false;
  LocsegFitWorkspace lastCommittedFitWorkspace = tw.fitWorkspace;

  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto* cpuThreadPool = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get());
  const size_t maxInFlight = seedSortMaxInFlight();
  std::atomic<size_t> preparedCount{0};

  std::vector<folly::coro::TaskWithExecutor<PreparedSeedSortEntry>> prepareTasks;
  prepareTasks.reserve(seeds.size());
  for (size_t seedIndex = 0; seedIndex < seeds.size(); ++seedIndex) {
    maybeCancel(tw.cancellationToken);
    prepareTasks.push_back(folly::coro::co_withExecutor(cpuExecutor,
                                                        prepareSeedSortEntryAsync(seeds,
                                                                                  signal,
                                                                                  tw,
                                                                                  seedIndex,
                                                                                  width,
                                                                                  height,
                                                                                  depth,
                                                                                  plane,
                                                                                  cpuThreadPool,
                                                                                  preparedCount)));
  }

  // Keep the window manager off the CPU thread pool so worker threads stay focused on the per-seed optimize work.
  std::vector<PreparedSeedSortEntry> preparedSeeds = folly::coro::blockingWait(
    folly::coro::co_withCancellation(tw.cancellationToken,
                                     folly::coro::collectAllWindowed(std::move(prepareTasks), maxInFlight)));
  maybeCancel(tw.cancellationToken);

  std::vector<size_t> commitOrder(seeds.size());
  std::iota(commitOrder.begin(), commitOrder.end(), size_t{0});
  if (commitByScore) {
    std::stable_sort(commitOrder.begin(), commitOrder.end(), [&preparedSeeds](size_t lhs, size_t rhs) {
      const PreparedSeedSortEntry& left = preparedSeeds[lhs];
      const PreparedSeedSortEntry& right = preparedSeeds[rhs];
      const bool leftIsCommitCandidate = left.didOptimize && !left.rejectedByTraceMask;
      const bool rightIsCommitCandidate = right.didOptimize && !right.rejectedByTraceMask;
      if (leftIsCommitCandidate != rightIsCommitCandidate) {
        return leftIsCommitCandidate > rightIsCommitCandidate;
      }
      if (leftIsCommitCandidate && (left.score != right.score)) {
        return left.score > right.score;
      }
      return false;
    });
  }

  for (size_t processedIndex = 0; processedIndex < commitOrder.size(); ++processedIndex) {
    maybeCancel(tw.cancellationToken);

    if (VLOG_IS_ON(1) &&
        (((processedIndex % SeedSortProgressReportEvery) == 0) || (processedIndex + 1 == commitOrder.size()))) {
      VLOG(1) << fmt::format("Seed sort commit: {}/{}", processedIndex + 1, commitOrder.size());
    }

    const size_t seedIndex = commitOrder[processedIndex];
    const PreparedSeedSortEntry& prepared = preparedSeeds[seedIndex];
    if (!prepared.didOptimize) {
      continue;
    }

    if (baseMaskData[prepared.seedOffset] > 0) {
      continue;
    }

    if (!commitByScore) {
      lastCommittedFitWorkspace = prepared.fitWorkspaceAfterOptimize;
      haveLastCommittedFitWorkspace = true;
    }

    if (prepared.rejectedByTraceMask) {
      continue;
    }

    if (commitByScore) {
      lastCommittedFitWorkspace = prepared.fitWorkspaceAfterOptimize;
      haveLastCommittedFitWorkspace = true;
    }

    out.locsegArray[seedIndex] = prepared.locseg;
    out.scoreArray[seedIndex] = prepared.score;

    const int labelValue = (out.scoreArray[seedIndex] > tw.minScore) ? 2 : 1;
    localNeurosegLabelGLegacyLike(out.locsegArray[seedIndex],
                                  out.baseMask,
                                  /*flag*/ -1,
                                  labelValue,
                                  SeedSortZScaleLegacyLike);
  }

  if (haveLastCommittedFitWorkspace) {
    tw.fitWorkspace = lastCommittedFitWorkspace;
  }

  VLOG(1) << "Seed sort: done.";
  return out;
}

} // namespace nim
