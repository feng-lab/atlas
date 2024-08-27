#include <benchmark/benchmark.h>

#include "zlog.h"
#include "zrandom.h"
#include "zsaturateoperation.h"
#include <boost/multiprecision/cpp_int.hpp>
#if __has_include(<absl/numeric/int128.h> )
#define ATLAS_HAS_ABSL
#include <absl/numeric/int128.h>
#endif
#include <QReadWriteLock>
#include <QMutexLocker>
#include <mutex>

namespace nim {

__forceinline std::string formatLogMessageNoCompile(google::LogSeverity severity,
                                                    const char* file,
                                                    int line,
                                                    const google::LogMessageTime& time,
                                                    const char* message,
                                                    size_t message_len)
{
  return fmt::format("{}{:%Y%m%d %H:%M:%S} {} {}:{}] {}",
                     google::GetLogSeverityName(severity)[0],
                     time.when(),
                     std::this_thread::get_id(),
                     file,
                     line,
                     fmt::string_view(message, message_len));
}

__forceinline std::string formatLogMessageNoTimeFormat(google::LogSeverity severity,
                                                       const char* file,
                                                       int line,
                                                       const google::LogMessageTime& time,
                                                       const char* message,
                                                       size_t message_len)
{
  return fmt::format(FMT_COMPILE("{}{} {} {}:{}] {}"),
                     google::GetLogSeverityName(severity)[0],
                     time.when(),
                     std::this_thread::get_id(),
                     file,
                     line,
                     fmt::string_view(message, message_len));
}

static void BM_glogToString(benchmark::State& state)
{
  std::array<const char*, 5> filenames{"aftewfw", "sfwtwtwfr", "fwtwofwojwt", "fwofjwofjwo", "sfabbb"};
  std::vector<const char*> filename(state.range(0));
  std::vector<int> line(state.range(0));
  std::vector<google::LogMessageTime> time(state.range(0));
  for (size_t i = 0; i < filename.size(); ++i) {
    filename[i] = filenames[ZRandom::instance().randInt<int>(4)];
    line[i] = ZRandom::instance().randInt<int>(1000);
    time[i] = google::LogMessageTime(std::chrono::system_clock::now());
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < filename.size(); ++i) {
      auto a = google::LogSink::ToString(google::GLOG_INFO, filename[i], line[i], time[i], filename[i], 4);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_formatLogMessageToString(benchmark::State& state)
{
  std::array<const char*, 5> filenames{"aftewfw", "sfwtwtwfr", "fwtwofwojwt", "fwofjwofjwo", "sfabbb"};
  std::vector<const char*> filename(state.range(0));
  std::vector<int> line(state.range(0));
  std::vector<google::LogMessageTime> time(state.range(0));
  for (size_t i = 0; i < filename.size(); ++i) {
    filename[i] = filenames[ZRandom::instance().randInt<int>(4)];
    line[i] = ZRandom::instance().randInt<int>(1000);
    time[i] = google::LogMessageTime(std::chrono::system_clock::now());
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < filename.size(); ++i) {
      auto a = formatLogMessage(google::GLOG_INFO, filename[i], line[i], time[i], filename[i], 4);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_formatLogMessageToStringNoCompile(benchmark::State& state)
{
  std::array<const char*, 5> filenames{"aftewfw", "sfwtwtwfr", "fwtwofwojwt", "fwofjwofjwo", "sfabbb"};
  std::vector<const char*> filename(state.range(0));
  std::vector<int> line(state.range(0));
  std::vector<google::LogMessageTime> time(state.range(0));
  for (size_t i = 0; i < filename.size(); ++i) {
    filename[i] = filenames[ZRandom::instance().randInt<int>(4)];
    line[i] = ZRandom::instance().randInt<int>(1000);
    time[i] = google::LogMessageTime(std::chrono::system_clock::now());
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < filename.size(); ++i) {
      auto a = formatLogMessageNoCompile(google::GLOG_INFO, filename[i], line[i], time[i], filename[i], 4);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_formatLogMessageToStringNoTimeFormat(benchmark::State& state)
{
  std::array<const char*, 5> filenames{"aftewfw", "sfwtwtwfr", "fwtwofwojwt", "fwofjwofjwo", "sfabbb"};
  std::vector<const char*> filename(state.range(0));
  std::vector<int> line(state.range(0));
  std::vector<google::LogMessageTime> time(state.range(0));
  for (size_t i = 0; i < filename.size(); ++i) {
    filename[i] = filenames[ZRandom::instance().randInt<int>(4)];
    line[i] = ZRandom::instance().randInt<int>(1000);
    time[i] = google::LogMessageTime(std::chrono::system_clock::now());
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < filename.size(); ++i) {
      auto a = formatLogMessageNoTimeFormat(google::GLOG_INFO, filename[i], line[i], time[i], filename[i], 4);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addLogMessageFormatBench()
{
  BENCHMARK(BM_glogToString)->Range(8, 8 << 10);
  BENCHMARK(BM_formatLogMessageToString)->Range(8, 8 << 10);
  BENCHMARK(BM_formatLogMessageToStringNoCompile)->Range(8, 8 << 10);
  BENCHMARK(BM_formatLogMessageToStringNoTimeFormat)->Range(8, 8 << 10);
}

template<typename Real>
void BM_stdRound(benchmark::State& state)
{
  std::vector<Real> vec(state.range(0));
  for (auto& d : vec) {
    d = ZRandom::instance().randReal<Real>(255);
  }
  while (state.KeepRunning()) {
    for (auto d : vec) {
      auto a = static_cast<uint8_t>(std::round(d));
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

template<typename Real>
void BM_PositiveRound(benchmark::State& state)
{
  std::vector<Real> vec(state.range(0));
  for (auto& d : vec) {
    d = ZRandom::instance().randReal<Real>(255);
  }
  while (state.KeepRunning()) {
    for (auto d : vec) {
      auto a = static_cast<uint8_t>(d + Real(0.5));
      benchmark::DoNotOptimize(a);
    }
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
  return res <= static_cast<int128_t>(INT64_MIN)   ? INT64_MIN
         : res >= static_cast<int128_t>(INT64_MAX) ? INT64_MAX
                                                   : static_cast<int64_t>(res);
}

#ifdef ATLAS_HAS_ABSL
inline int64_t saturate_mul_absl(int64_t x, int64_t y)
{
  using namespace absl;

  int128 res = int128(x) * int128(y);
  return res <= int128(INT64_MIN) ? INT64_MIN : res >= int128(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(res);
}
#endif

static void BM_saturate_mul_i64(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1) {
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  for (auto& v2 : vec2) {
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i) {
      auto a = saturate_mul(vec1[i], vec2[i]);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_saturate_mul_i64_boost(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1) {
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  for (auto& v2 : vec2) {
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i) {
      auto a = saturate_mul_boost(vec1[i], vec2[i]);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

#ifdef ATLAS_HAS_ABSL
static void BM_saturate_mul_i64_absl(benchmark::State& state)
{
  std::vector<int64_t> vec1(state.range(0));
  std::vector<int64_t> vec2(state.range(0));
  for (auto& v1 : vec1) {
    v1 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  for (auto& v2 : vec2) {
    v2 = ZRandom::instance().randInt<int64_t>(INT64_MAX, INT64_MIN) * 100000;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i) {
      auto a = saturate_mul_absl(vec1[i], vec2[i]);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
#endif

static void BM_saturate_mul_i32(benchmark::State& state)
{
  std::vector<int32_t> vec1(state.range(0));
  std::vector<int32_t> vec2(state.range(0));
  for (auto& v1 : vec1) {
    v1 = ZRandom::instance().randInt<int32_t>(INT32_MAX, INT32_MIN) * 10000;
  }
  for (auto& v2 : vec2) {
    v2 = ZRandom::instance().randInt<int32_t>(INT32_MAX, INT32_MIN) * 10000;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i) {
      auto a = saturate_mul(vec1[i], vec2[i]);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_saturate_mul_i16(benchmark::State& state)
{
  std::vector<int16_t> vec1(state.range(0));
  std::vector<int16_t> vec2(state.range(0));
  for (auto& v1 : vec1) {
    v1 = ZRandom::instance().randInt<int16_t>(INT16_MAX, INT16_MIN) * 100;
  }
  for (auto& v2 : vec2) {
    v2 = ZRandom::instance().randInt<int16_t>(INT16_MAX, INT16_MIN) * 100;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < vec1.size(); ++i) {
      auto a = saturate_mul(vec1[i], vec2[i]);
      benchmark::DoNotOptimize(a);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addSaturateMulBench()
{
  BENCHMARK(BM_saturate_mul_i64)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i64_boost)->Range(8, 8 << 10);
#ifdef ATLAS_HAS_ABSL
  BENCHMARK(BM_saturate_mul_i64_absl)->Range(8, 8 << 10);
#endif
  BENCHMARK(BM_saturate_mul_i32)->Range(8, 8 << 10);
  BENCHMARK(BM_saturate_mul_i16)->Range(8, 8 << 10);
}

static void BM_vectorLoopRange(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  while (state.KeepRunning()) {
    for (auto i : v) {
      benchmark::DoNotOptimize(i);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopRangeWithIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  while (state.KeepRunning()) {
    size_t idx = 0;
    for (auto i : v) {
      idx++;
      benchmark::DoNotOptimize(idx);
      benchmark::DoNotOptimize(i);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopRangeRefWithIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  while (state.KeepRunning()) {
    size_t idx = 0;
    for (auto& i : v) {
      idx++;
      benchmark::DoNotOptimize(idx);
      benchmark::DoNotOptimize(i);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopIterator(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  while (state.KeepRunning()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      benchmark::DoNotOptimize(*it);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_vectorLoopIndex(benchmark::State& state)
{
  std::vector<int> v(state.range(0));
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  while (state.KeepRunning()) {
    for (size_t i = 0; i < v.size(); ++i) {
      benchmark::DoNotOptimize(v[i]);
    }
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
    for (int i = 0; i < state.range(0); ++i) {
      QWriteLocker lk(&lock);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_QMutex(benchmark::State& state)
{
  QMutex lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i) {
      QMutexLocker lk(&lock);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_stdMutex(benchmark::State& state)
{
  std::mutex lock;
  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i) {
      std::lock_guard<std::mutex> lk(lock);
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void addLockBench()
{
  BENCHMARK(BM_QReadWriteLock)->Range(8, 8 << 12);
  BENCHMARK(BM_QMutex)->Range(8, 8 << 12);
  BENCHMARK(BM_stdMutex)->Range(8, 8 << 12);
}

} // namespace nim

int main(int argc, char* argv[])
{
  using namespace nim;

  addLogMessageFormatBench();
  addRoundBench();
  addSaturateMulBench();
  addVectorLoopBench();
  addLockBench();

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
