#include <benchmark/benchmark.h>

#include "zbioformatsbridgeclient.h"
#include "zcpuinfo.h"
#include "zimgbioformats.h"
#include "zimginit.h"
#include "zimgregion.h"
#include "zlog.h"
#include "zrandom.h"
#include "zsaturateoperation.h"
#include <boost/multiprecision/cpp_int.hpp>
#if __has_include(<absl/numeric/int128.h> )
#define ATLAS_HAS_ABSL
#include <absl/numeric/int128.h>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QReadWriteLock>
#include <QString>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <gflags/gflags.h>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

DECLARE_bool(atlas_bioformats_bridge_use_grpc);
DECLARE_bool(atlas_bioformats_bridge_diagnostics);
DECLARE_int32(atlas_bioformats_bridge_io_timeout_ms);
DECLARE_int32(v);

namespace nim {

void applyGlogVerbosityFlagForBenchmark(int argc, char* argv[])
{
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i] == nullptr ? "" : argv[i]);
    const std::string prefix = "--v=";
    try {
      if (arg.rfind(prefix, 0) == 0) {
        ::FLAGS_v = std::stoi(arg.substr(prefix.size()));
      } else if (arg == "--v" && i + 1 < argc) {
        ::FLAGS_v = std::stoi(argv[i + 1]);
      }
    }
    catch (const std::exception&) {
      return;
    }
  }
}

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
      std::scoped_lock lk(lock);
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

constexpr int kBioFormatsBenchmarkIoTimeoutMs = 10 * 60 * 1000;
constexpr const char* kDefaultBioFormatsBenchmarkFile =
  "/Users/feng/Documents/omeimages/Ventana/openslide/Ventana-1.bif";
constexpr uint64_t kBioFormatsBenchmarkTileSize = 1024;
constexpr uint32_t kBioFormatsBenchmarkAxisSamples = 4;

QString platformJavaExecutableName()
{
#ifdef _WIN32
  return QStringLiteral("java.exe");
#else
  return QStringLiteral("java");
#endif
}

QString javaExecutableInJreDir(const QString& jreDir)
{
  return QDir(jreDir).filePath(QStringLiteral("bin/") + platformJavaExecutableName());
}

std::optional<QString> normalizeBundledJreDir(const QString& path)
{
  QDir dir(path);
  if (!dir.exists()) {
    return std::nullopt;
  }

  if (dir.exists(QStringLiteral("bin/") + platformJavaExecutableName())) {
    return dir.absolutePath();
  }

  const QString macHome = dir.filePath(QStringLiteral("Contents/Home"));
  QDir macHomeDir(macHome);
  if (macHomeDir.exists(QStringLiteral("bin/") + platformJavaExecutableName())) {
    return macHomeDir.absolutePath();
  }

  return std::nullopt;
}

std::optional<QString> bundledJreDir()
{
  const QDir thirdPartyBuild(QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
  const QString jreName = ZCpuInfo::instance().isX86_64 ? QStringLiteral("jre") : QStringLiteral("jre-arm");
  return normalizeBundledJreDir(thirdPartyBuild.filePath(jreName));
}

QString bioFormatsBenchmarkFile()
{
  const char* env = std::getenv("ATLAS_BIOFORMATS_BENCHMARK_FILE");
  if (env != nullptr && env[0] != '\0') {
    return QString::fromLocal8Bit(env);
  }
  return QString::fromUtf8(kDefaultBioFormatsBenchmarkFile);
}

const std::optional<std::string>& bioFormatsBenchmarkRuntimeError()
{
  static std::once_flag once;
  static std::optional<std::string> error;
  std::call_once(once, []() {
    ::FLAGS_atlas_bioformats_bridge_use_grpc = true;
    if (::FLAGS_atlas_bioformats_bridge_io_timeout_ms <= 0) {
      ::FLAGS_atlas_bioformats_bridge_io_timeout_ms = kBioFormatsBenchmarkIoTimeoutMs;
    }

    const QDir thirdPartyBuild(QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
    const QDir jarsDir(thirdPartyBuild.filePath(QStringLiteral("jars")));
    if (!jarsDir.exists("bioformats_package.jar")) {
      error = fmt::format("missing {}", jarsDir.filePath("bioformats_package.jar"));
      return;
    }
    if (!jarsDir.exists("atlas-bioformats-bridge.jar")) {
      error = fmt::format("missing {}", jarsDir.filePath("atlas-bioformats-bridge.jar"));
      return;
    }
    const std::optional<QString> jreDir = bundledJreDir();
    if (!jreDir) {
      error = fmt::format("missing bundled JRE under {}", QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
      return;
    }

    ZLogInit::instance("zbenchmark");
    LOG(INFO) << "Bio-Formats benchmark runtime: file=" << bioFormatsBenchmarkFile()
              << ", java=" << javaExecutableInJreDir(*jreDir) << ", jarsDIR=" << jarsDir.absolutePath()
              << ", useGrpc=" << ::FLAGS_atlas_bioformats_bridge_use_grpc
              << ", bridgeDiagnostics=" << ::FLAGS_atlas_bioformats_bridge_diagnostics;

    try {
      ZImgInit::instance("", *jreDir, jarsDir.absolutePath(), false);
      if (!ZImgBioFormats().supportRead()) {
        error = "Bio-Formats runtime support is not available";
      }
    }
    catch (const std::exception& e) {
      error = fmt::format("Bio-Formats runtime initialization failed: {}", e.what());
    }
  });
  return error;
}

index_t checkedBenchmarkIndex(uint64_t value, const char* name)
{
  if (value > static_cast<uint64_t>(std::numeric_limits<index_t>::max())) {
    throw std::runtime_error(fmt::format("{} exceeds Atlas index range: {}", name, value));
  }
  return static_cast<index_t>(value);
}

int64_t checkedBenchmarkBytes(uint64_t value)
{
  if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw std::runtime_error(fmt::format("benchmark byte count exceeds int64 range: {}", value));
  }
  return static_cast<int64_t>(value);
}

std::vector<index_t> evenlySpacedStarts(uint64_t size, uint64_t tileSize)
{
  CHECK(tileSize > 0);
  std::vector<index_t> starts;
  if (size <= tileSize) {
    starts.push_back(0);
    return starts;
  }

  const long double maxStart = static_cast<long double>(size - tileSize);
  for (uint32_t i = 0; i < kBioFormatsBenchmarkAxisSamples; ++i) {
    const long double ratio =
      kBioFormatsBenchmarkAxisSamples == 1 ? 0.0L : static_cast<long double>(i) / (kBioFormatsBenchmarkAxisSamples - 1);
    const auto start = static_cast<uint64_t>(std::llround(maxStart * ratio));
    const index_t atlasStart = checkedBenchmarkIndex(start, "region start");
    if (starts.empty() || starts.back() != atlasStart) {
      starts.push_back(atlasStart);
    }
  }
  return starts;
}

struct BioFormatsBenchmarkDataset
{
  QString path;
  ZBioFormatsDatasetInfo info;
  uint32_t series = 0;
  uint32_t resolution = 0;
  std::vector<ZImgRegion> regions;
  int64_t bytesPerIteration = 0;
};

BioFormatsBenchmarkDataset makeBioFormatsBenchmarkDataset()
{
  if (const auto& error = bioFormatsBenchmarkRuntimeError(); error.has_value()) {
    throw std::runtime_error(*error);
  }

  BioFormatsBenchmarkDataset dataset;
  dataset.path = bioFormatsBenchmarkFile();
  const QFileInfo file(dataset.path);
  if (!file.isFile()) {
    throw std::runtime_error(fmt::format("benchmark file does not exist: {}", dataset.path));
  }

  dataset.info = ZBioFormatsBridgeClient::instance().readDatasetInfo(dataset.path);
  if (dataset.info.series.empty()) {
    throw std::runtime_error("benchmark dataset has no Bio-Formats series");
  }

  const ZBioFormatsSeriesInfo& series = dataset.info.series.front();
  if (series.sizeX == 0 || series.sizeY == 0 || series.sizeZ == 0 || series.sizeT == 0 || series.effectiveSizeC == 0 ||
      series.bytesPerPixel == 0) {
    throw std::runtime_error("benchmark dataset has invalid zero-sized metadata");
  }

  const uint64_t tileWidth = std::min<uint64_t>(kBioFormatsBenchmarkTileSize, series.sizeX);
  const uint64_t tileHeight = std::min<uint64_t>(kBioFormatsBenchmarkTileSize, series.sizeY);
  const uint64_t channelCount = series.effectiveSizeC * std::max<uint32_t>(1, series.rgbChannelCount);
  const std::vector<index_t> xStarts = evenlySpacedStarts(series.sizeX, tileWidth);
  const std::vector<index_t> yStarts = evenlySpacedStarts(series.sizeY, tileHeight);

  uint64_t bytesPerIteration = 0;
  for (const index_t y : yStarts) {
    for (const index_t x : xStarts) {
      dataset.regions.emplace_back(x,
                                   x + checkedBenchmarkIndex(tileWidth, "tile width"),
                                   y,
                                   y + checkedBenchmarkIndex(tileHeight, "tile height"),
                                   0,
                                   1,
                                   0,
                                   checkedBenchmarkIndex(channelCount, "channel count"),
                                   0,
                                   1);
      bytesPerIteration += tileWidth * tileHeight * channelCount * series.bytesPerPixel;
    }
  }

  if (dataset.regions.empty()) {
    throw std::runtime_error("benchmark dataset produced no regions");
  }
  dataset.bytesPerIteration = checkedBenchmarkBytes(bytesPerIteration);
  return dataset;
}

uint64_t readBioFormatsRegionsSequential(const BioFormatsBenchmarkDataset& dataset)
{
  uint64_t totalBytes = 0;
  for (const ZImgRegion& region : dataset.regions) {
    std::vector<uint8_t> pixels =
      ZBioFormatsBridgeClient::instance().readRegion(dataset.path, dataset.series, dataset.resolution, region);
    totalBytes += pixels.size();
    benchmark::DoNotOptimize(pixels.data());
  }
  return totalBytes;
}

uint64_t readBioFormatsRegionsConcurrent(const BioFormatsBenchmarkDataset& dataset, int concurrency)
{
  CHECK(concurrency > 0);
  uint64_t totalBytes = 0;
  for (size_t first = 0; first < dataset.regions.size();) {
    std::vector<std::future<std::vector<uint8_t>>> futures;
    futures.reserve(static_cast<size_t>(concurrency));
    for (int i = 0; i < concurrency && first < dataset.regions.size(); ++i, ++first) {
      const ZImgRegion region = dataset.regions[first];
      futures.push_back(std::async(std::launch::async, [&dataset, region]() {
        return ZBioFormatsBridgeClient::instance().readRegion(dataset.path, dataset.series, dataset.resolution, region);
      }));
    }
    for (auto& future : futures) {
      std::vector<uint8_t> pixels = future.get();
      totalBytes += pixels.size();
      benchmark::DoNotOptimize(pixels.data());
    }
  }
  return totalBytes;
}

static void BM_BioFormatsVentanaDatasetInfo(benchmark::State& state)
{
  try {
    if (const auto& error = bioFormatsBenchmarkRuntimeError(); error.has_value()) {
      state.SkipWithError(error->c_str());
      return;
    }
    const QString path = bioFormatsBenchmarkFile();
    if (!QFileInfo(path).isFile()) {
      const std::string message = fmt::format("benchmark file does not exist: {}", path);
      state.SkipWithError(message.c_str());
      return;
    }

    std::ignore = ZBioFormatsBridgeClient::instance().readDatasetInfo(path);
    for (auto _ : state) {
      QElapsedTimer timer;
      timer.start();
      const ZBioFormatsDatasetInfo info = ZBioFormatsBridgeClient::instance().readDatasetInfo(path);
      state.SetIterationTime(timer.nsecsElapsed() / 1.0e9);
      benchmark::DoNotOptimize(info.series.size());
    }
    state.SetItemsProcessed(state.iterations());
  }
  catch (const std::exception& e) {
    state.SkipWithError(e.what());
  }
}

static void BM_BioFormatsVentanaSequentialRegions(benchmark::State& state)
{
  try {
    const BioFormatsBenchmarkDataset dataset = makeBioFormatsBenchmarkDataset();
    readBioFormatsRegionsSequential(dataset);

    uint64_t totalBytes = 0;
    for (auto _ : state) {
      QElapsedTimer timer;
      timer.start();
      totalBytes += readBioFormatsRegionsSequential(dataset);
      state.SetIterationTime(timer.nsecsElapsed() / 1.0e9);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(dataset.regions.size()));
    state.SetBytesProcessed(checkedBenchmarkBytes(totalBytes));
    state.counters["regions_per_iteration"] = static_cast<double>(dataset.regions.size());
    state.counters["bytes_per_iteration"] = static_cast<double>(dataset.bytesPerIteration);
  }
  catch (const std::exception& e) {
    state.SkipWithError(e.what());
  }
}

static void BM_BioFormatsVentanaConcurrentRegions(benchmark::State& state)
{
  try {
    const int concurrency = static_cast<int>(state.range(0));
    const BioFormatsBenchmarkDataset dataset = makeBioFormatsBenchmarkDataset();
    readBioFormatsRegionsConcurrent(dataset, concurrency);

    uint64_t totalBytes = 0;
    for (auto _ : state) {
      QElapsedTimer timer;
      timer.start();
      totalBytes += readBioFormatsRegionsConcurrent(dataset, concurrency);
      state.SetIterationTime(timer.nsecsElapsed() / 1.0e9);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(dataset.regions.size()));
    state.SetBytesProcessed(checkedBenchmarkBytes(totalBytes));
    state.counters["concurrency"] = concurrency;
    state.counters["regions_per_iteration"] = static_cast<double>(dataset.regions.size());
    state.counters["bytes_per_iteration"] = static_cast<double>(dataset.bytesPerIteration);
  }
  catch (const std::exception& e) {
    state.SkipWithError(e.what());
  }
}

static void addBioFormatsBench()
{
  BENCHMARK(BM_BioFormatsVentanaDatasetInfo)->Unit(benchmark::kMillisecond)->UseManualTime()->Iterations(5);
  BENCHMARK(BM_BioFormatsVentanaSequentialRegions)->Unit(benchmark::kMillisecond)->UseManualTime()->Iterations(3);
  BENCHMARK(BM_BioFormatsVentanaConcurrentRegions)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime()
    ->Iterations(3);
}

} // namespace nim

int main(int argc, char* argv[])
{
  using namespace nim;

  applyGlogVerbosityFlagForBenchmark(argc, argv);

  addLogMessageFormatBench();
  addRoundBench();
  addSaturateMulBench();
  addVectorLoopBench();
  addLockBench();
  addBioFormatsBench();

  benchmark::Initialize(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
