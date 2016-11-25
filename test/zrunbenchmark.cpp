#include "zrunbenchmark.h"
#include <benchmark/benchmark.h>

#include "zlog.h"
#include "zrandom.h"
#include "zsaturateoperation.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <tbb/mutex.h>
#include <QReadWriteLock>
#include <QMutexLocker>
#include <mutex>

namespace nim {

template<typename Real>
void BM_stdRound(benchmark::State& state)
{
  std::vector<Real> vec(state.range(0));
  for (auto& d : vec)
    d = ZRandom::instance().randReal<Real>(255);
  while (state.KeepRunning()) {
    for (auto d : vec)
      benchmark::DoNotOptimize(static_cast<uint8_t>(std::round(d)));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

template<typename Real>
void BM_PositiveRound(benchmark::State& state)
{
  std::vector<Real> vec(state.range(0));
  for (auto& d : vec)
    d = ZRandom::instance().randReal<Real>(255);
  while (state.KeepRunning()) {
    for (auto d : vec)
      benchmark::DoNotOptimize(static_cast<uint8_t>(d + Real(0.5)));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addRoundBench()
{
  BENCHMARK_TEMPLATE(BM_stdRound, float)->Range(8, 8 << 10);
  BENCHMARK_TEMPLATE(BM_PositiveRound, float)->Range(8, 8 << 10);
  BENCHMARK_TEMPLATE(BM_stdRound, double)->Range(8, 8 << 10);
  BENCHMARK_TEMPLATE(BM_PositiveRound, double)->Range(8, 8 << 10);
}

inline int64_t saturate_mul_boost(int64_t x, int64_t y)
{
  using namespace boost::multiprecision;

  int128_t res = static_cast<int128_t>(x) * static_cast<int128_t>(y);
  return res <= static_cast<int128_t>(INT64_MIN) ? INT64_MIN :
         res >= static_cast<int128_t>(INT64_MAX) ? INT64_MAX :
         static_cast<int64_t>(res);
}

#ifdef __GNUG__ // clang or gcc
inline int64_t saturate_mul_buildin128(int64_t x, int64_t y)
{
  static_assert((static_cast<__int128_t>(-1) >> 1) == static_cast<__int128_t>(-1), "need arithmetic right shift.");
  static_assert((static_cast<int64_t>(-1) >> 1) == static_cast<int64_t>(-1), "need arithmetic right shift.");

  __int128_t res = static_cast<__int128_t>(x) * static_cast<__int128_t>(y);
  if (static_cast<int64_t>(res >> 64) != (static_cast<int64_t>(res) >> 63))
    res = (static_cast<uint64_t>(x ^ y) >> 63) + INT64_MAX;
  return res;
}

static void BM_saturate_mul_i64_buildin128(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1)
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  for (auto& v2 : vec2)
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i)
      benchmark::DoNotOptimize(saturate_mul_buildin128(vec1[i], vec2[i]));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
#endif // __GNUG__

static void BM_saturate_mul_i64(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1)
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  for (auto& v2 : vec2)
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i)
      benchmark::DoNotOptimize(saturate_mul(vec1[i], vec2[i]));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_saturate_mul_i64_boost(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1)
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  for (auto& v2 : vec2)
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i)
      benchmark::DoNotOptimize(saturate_mul_boost(vec1[i], vec2[i]));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_saturate_mul_i32(benchmark::State& state)
{
  std::vector<int32_t> vec1(state.range(0));
  std::vector<int32_t> vec2(state.range(0));
  for (auto& v1 : vec1)
    v1 = ZRandom::instance().randInt<int32_t>(INT32_MAX, INT32_MIN) * 10000;
  for (auto& v2 : vec2)
    v2 = ZRandom::instance().randInt<int32_t>(INT32_MAX, INT32_MIN) * 10000;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i)
      benchmark::DoNotOptimize(saturate_mul(vec1[i], vec2[i]));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_saturate_mul_i16(benchmark::State& state)
{
  std::vector<int16_t> vec1(state.range(0));
  std::vector<int16_t> vec2(state.range(0));
  for (auto& v1 : vec1)
    v1 = ZRandom::instance().randInt<int16_t>(INT16_MAX, INT16_MIN) * 100;
  for (auto& v2 : vec2)
    v2 = ZRandom::instance().randInt<int16_t>(INT16_MAX, INT16_MIN) * 100;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i)
      benchmark::DoNotOptimize(saturate_mul(vec1[i], vec2[i]));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addSaturateMulBench()
{
  BENCHMARK(BM_saturate_mul_i64)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i64_boost)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i64_buildin128)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i32)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i16)->Range(8, 8 << 10);
}

static void BM_vectorLoopRange(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = i;
  while (state.KeepRunning()) {
    for (auto i : v)
      benchmark::DoNotOptimize(i);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopRangeWithIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = i;
  while (state.KeepRunning()) {
    size_t idx = 0;
    for (auto i : v) {
      benchmark::DoNotOptimize(idx++);
      benchmark::DoNotOptimize(i);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopRangeRefWithIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = i;
  while (state.KeepRunning()) {
    size_t idx = 0;
    for (auto& i : v) {
      benchmark::DoNotOptimize(idx++);
      benchmark::DoNotOptimize(i);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopIterator(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = i;
  while (state.KeepRunning()) {
    for (auto it = v.begin(); it != v.end(); ++it)
      benchmark::DoNotOptimize(*it);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = i;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < v.size(); ++i)
      benchmark::DoNotOptimize(v[i]);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addVectorLoopBench()
{
  BENCHMARK(BM_vectorLoopRange)->Range(8, 8 << 12);
  BENCHMARK(BM_vectorLoopRangeWithIndex)->Range(8, 8 << 12);
  BENCHMARK(BM_vectorLoopRangeRefWithIndex)->Range(8, 8 << 12);
  BENCHMARK(BM_vectorLoopIterator)->Range(8, 8 << 12);
  BENCHMARK(BM_vectorLoopIndex)->Range(8, 8 << 12);
}

static void BM_QReadWriteLock(benchmark::State& state)
{
  QReadWriteLock lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i)
      QWriteLocker lk(&lock);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_QMutex(benchmark::State& state)
{
  QMutex lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i)
      QMutexLocker lk(&lock);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_stdMutex(benchmark::State& state)
{
  std::mutex lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i)
      std::lock_guard<std::mutex> lk(lock);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_tbbMutex(benchmark::State& state)
{
  tbb::mutex lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i)
      tbb::mutex::scoped_lock lk(lock);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addLockBench()
{
  BENCHMARK(BM_QReadWriteLock)->Range(8, 8 << 12);
  BENCHMARK(BM_QMutex)->Range(8, 8 << 12);
  BENCHMARK(BM_stdMutex)->Range(8, 8 << 12);
  BENCHMARK(BM_tbbMutex)->Range(8, 8 << 12);
}

int ZRunBenchmark::run()
{
  LOG(INFO) << "Benchmark Start";

  addRoundBench();
  addSaturateMulBench();
  addVectorLoopBench();
  addLockBench();

  char arg0[] = "Atlas_benchmark";
  char* argv[] = {&arg0[0], nullptr};
  int argc = std::extent<decltype(argv)>::value - 1;

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  LOG(INFO) << "Benchmark End";
  return 0;
}

} // namespace nim
