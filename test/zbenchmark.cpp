#include <benchmark/benchmark.h>

#include "zbioformatsbridgeclient.h"
#include "zconcurrentlrucache.h"
#include "z3dblockidcollector.h"
#include "zimgbioformats.h"
#include "zimage2dutils.h"
#include "zimage3dutils.h"
#include "zimagehwy.h"
#include "zimageresizehwy.h"
#include "zimginit.h"
#include "zimgregion.h"
#include "zlog.h"
#include "zexception.h"
#include "zrandom.h"
#include "zsaturateoperation.h"
#include "ztest.h"
#include <boost/multiprecision/cpp_int.hpp>
#if __has_include(<absl/numeric/int128.h> )
#define ATLAS_HAS_ABSL
#include <absl/numeric/int128.h>
#endif
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QReadWriteLock>
#include <QString>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include "zcommandlineflags.h"
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

ABSL_DECLARE_FLAG(bool, atlas_bioformats_bridge_diagnostics);
ABSL_DECLARE_FLAG(int32_t, atlas_bioformats_bridge_io_timeout_ms);
ABSL_FLAG(bool,
          atlas_resize_3d_exact_benchmark,
          false,
          "Enable exact-size 3D cubic-AA resize benchmarks, which allocate multi-GB working sets.");

namespace nim {

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

enum class SaturateAddSubBenchMethod
{
  ScalarLoop,
  HighwayPath,
};

enum class SaturateAddSubBenchOp
{
  Add,
  Sub,
};

enum class SaturateAddSubBenchRhs
{
  Array,
  Scalar,
};

template<typename T>
T saturateAddSubBenchValue(size_t index)
{
  if constexpr (std::is_signed_v<T>) {
    constexpr T min = std::numeric_limits<T>::lowest();
    constexpr T max = std::numeric_limits<T>::max();
    constexpr std::array<T, 12> values = {min,
                                          static_cast<T>(min + 1),
                                          static_cast<T>(-1000003),
                                          static_cast<T>(-97),
                                          static_cast<T>(-1),
                                          static_cast<T>(0),
                                          static_cast<T>(1),
                                          static_cast<T>(97),
                                          static_cast<T>(1000003),
                                          static_cast<T>(max / 2),
                                          static_cast<T>(max - 1),
                                          max};
    return values[index % values.size()];
  } else {
    constexpr T max = std::numeric_limits<T>::max();
    constexpr std::array<T, 10> values = {static_cast<T>(0),
                                          static_cast<T>(1),
                                          static_cast<T>(7),
                                          static_cast<T>(97),
                                          static_cast<T>(max / 2),
                                          static_cast<T>(max - max / 3),
                                          static_cast<T>(max - 97),
                                          static_cast<T>(max - 7),
                                          static_cast<T>(max - 1),
                                          max};
    return values[index % values.size()];
  }
}

template<typename T>
void initializeSaturateAddSubBenchData(std::vector<T>& lhs, std::vector<T>& rhs)
{
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = saturateAddSubBenchValue<T>(i * 7 + 3);
    rhs[i] = saturateAddSubBenchValue<T>(i * 11 + 5);
  }
}

template<typename T, SaturateAddSubBenchOp Op, SaturateAddSubBenchRhs Rhs>
void runSaturateAddSubScalarLoop(const T* lhs, const T* rhs, T scalar, size_t count, T* result)
{
  if constexpr (Rhs == SaturateAddSubBenchRhs::Array) {
    for (size_t i = 0; i < count; ++i) {
      if constexpr (Op == SaturateAddSubBenchOp::Add) {
        result[i] = saturate_add(lhs[i], rhs[i]);
      } else {
        result[i] = saturate_sub(lhs[i], rhs[i]);
      }
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      if constexpr (Op == SaturateAddSubBenchOp::Add) {
        result[i] = saturate_add(lhs[i], scalar);
      } else {
        result[i] = saturate_sub(lhs[i], scalar);
      }
    }
  }
}

template<typename T, SaturateAddSubBenchOp Op, SaturateAddSubBenchRhs Rhs>
void runSaturateAddSubHighwayPath(const T* lhs, const T* rhs, T scalar, size_t count, T* result)
{
  if constexpr (Rhs == SaturateAddSubBenchRhs::Array) {
    if constexpr (Op == SaturateAddSubBenchOp::Add) {
      saturate_add<T, const T>(lhs, rhs, count, result);
    } else {
      saturate_sub<T, const T>(lhs, rhs, count, result);
    }
  } else {
    if constexpr (Op == SaturateAddSubBenchOp::Add) {
      saturate_add<T, T>(lhs, scalar, count, result);
    } else {
      saturate_sub<T, T>(lhs, scalar, count, result);
    }
  }
}

template<typename T, SaturateAddSubBenchMethod Method, SaturateAddSubBenchOp Op, SaturateAddSubBenchRhs Rhs>
static void BM_saturate_add_sub_array(benchmark::State& state)
{
  const size_t count = static_cast<size_t>(state.range(0));
  constexpr size_t kPadding = 64;
  constexpr size_t kLhsOffset = 3;
  constexpr size_t kRhsOffset = 9;
  constexpr size_t kResultOffset = 5;

  std::vector<T> lhs(count + kPadding);
  std::vector<T> rhs(count + kPadding);
  std::vector<T> expected(count + kPadding);
  std::vector<T> result(count + kPadding);
  initializeSaturateAddSubBenchData(lhs, rhs);
  const T scalar = saturateAddSubBenchValue<T>(17);

  const T* lhsData = lhs.data() + kLhsOffset;
  const T* rhsData = rhs.data() + kRhsOffset;
  T* expectedData = expected.data() + kResultOffset;
  T* resultData = result.data() + kResultOffset;

  runSaturateAddSubScalarLoop<T, Op, Rhs>(lhsData, rhsData, scalar, count, expectedData);
  runSaturateAddSubHighwayPath<T, Op, Rhs>(lhsData, rhsData, scalar, count, resultData);
  if (!std::equal(expected.begin(), expected.end(), result.begin())) {
    state.SkipWithError("scalar-loop and Highway saturate results differ");
    return;
  }

  for (auto _ : state) {
    if constexpr (Method == SaturateAddSubBenchMethod::ScalarLoop) {
      runSaturateAddSubScalarLoop<T, Op, Rhs>(lhsData, rhsData, scalar, count, resultData);
    } else {
      runSaturateAddSubHighwayPath<T, Op, Rhs>(lhsData, rhsData, scalar, count, resultData);
    }
    benchmark::DoNotOptimize(resultData);
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
  const int64_t arraysTouched = Rhs == SaturateAddSubBenchRhs::Array ? 3 : 2;
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(count * sizeof(T)) * arraysTouched);
}

template<typename T, SaturateAddSubBenchOp Op, SaturateAddSubBenchRhs Rhs>
void registerSaturateAddSubBenchPair(const char* typeName, const char* opName, const char* rhsName)
{
  constexpr std::array<int64_t, 12> args = {8, 16, 32, 64, 128, 256, 512, 1024, 4096, 65536, 262144, 1048576};

  auto* scalarBench =
    benchmark::RegisterBenchmark(fmt::format("SaturateAddSub/{}/scalar_loop/{}/{}", typeName, opName, rhsName).c_str(),
                                 &BM_saturate_add_sub_array<T, SaturateAddSubBenchMethod::ScalarLoop, Op, Rhs>);
  auto* highwayBench =
    benchmark::RegisterBenchmark(fmt::format("SaturateAddSub/{}/highway_path/{}/{}", typeName, opName, rhsName).c_str(),
                                 &BM_saturate_add_sub_array<T, SaturateAddSubBenchMethod::HighwayPath, Op, Rhs>);

  for (const int64_t arg : args) {
    scalarBench->Arg(arg);
    highwayBench->Arg(arg);
  }
}

template<typename T>
void registerSaturateAddSubTypeBenches(const char* typeName)
{
  registerSaturateAddSubBenchPair<T, SaturateAddSubBenchOp::Add, SaturateAddSubBenchRhs::Array>(typeName,
                                                                                                "add",
                                                                                                "array_rhs");
  registerSaturateAddSubBenchPair<T, SaturateAddSubBenchOp::Sub, SaturateAddSubBenchRhs::Array>(typeName,
                                                                                                "sub",
                                                                                                "array_rhs");
  registerSaturateAddSubBenchPair<T, SaturateAddSubBenchOp::Add, SaturateAddSubBenchRhs::Scalar>(typeName,
                                                                                                 "add",
                                                                                                 "scalar_rhs");
  registerSaturateAddSubBenchPair<T, SaturateAddSubBenchOp::Sub, SaturateAddSubBenchRhs::Scalar>(typeName,
                                                                                                 "sub",
                                                                                                 "scalar_rhs");
}

static void addSaturateAddSubBench()
{
  registerSaturateAddSubTypeBenches<uint32_t>("uint32");
  registerSaturateAddSubTypeBenches<int32_t>("int32");
  registerSaturateAddSubTypeBenches<uint64_t>("uint64");
  registerSaturateAddSubTypeBenches<int64_t>("int64");
}

struct ImageConvolutionBenchCase
{
  std::string name;
  size_t width;
  size_t height;
  size_t depth;
  size_t kernelWidth;
  size_t kernelHeight;
  size_t kernelDepth;
};

double imageConvolutionBenchValue(size_t index)
{
  const uint64_t mixed = static_cast<uint64_t>(index + 1) * 2862933555777941757ULL + 3037000493ULL;
  return static_cast<double>(mixed % 2003) * (1.0 / 1001.0) - 1.0;
}

std::vector<double> makeImageConvolutionBenchData(size_t count)
{
  std::vector<double> data(count);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = imageConvolutionBenchValue(i);
  }
  return data;
}

std::vector<double> makeImageConvolutionBenchKernel(size_t count)
{
  std::vector<double> kernel(count);
  double total = 0.0;
  for (size_t i = 0; i < kernel.size(); ++i) {
    kernel[i] = static_cast<double>((i * 17 + 11) % 29 + 1);
    total += kernel[i];
  }
  for (double& value : kernel) {
    value /= total;
  }
  return kernel;
}

static void BM_image_convolution_hwy_2d_row(benchmark::State& state, ImageConvolutionBenchCase benchCase)
{
  const size_t padImgWidth = benchCase.width + benchCase.kernelWidth - 1;
  std::vector<double> padImg = makeImageConvolutionBenchData(padImgWidth * benchCase.height);
  std::vector<double> kernel = makeImageConvolutionBenchKernel(benchCase.kernelWidth);
  std::vector<double> imgOut(benchCase.width * benchCase.height);

  for (auto _ : state) {
    Image2DRowFilterForOneBlock_Hwy(padImg.data(),
                                    padImgWidth,
                                    kernel.data(),
                                    benchCase.kernelWidth,
                                    imgOut.data(),
                                    benchCase.width,
                                    0,
                                    benchCase.height);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
}

static void BM_image_convolution_hwy_2d_full(benchmark::State& state, ImageConvolutionBenchCase benchCase)
{
  const size_t padImgWidth = benchCase.width + benchCase.kernelWidth - 1;
  const size_t padImgHeight = benchCase.height + benchCase.kernelHeight - 1;
  std::vector<double> padImg = makeImageConvolutionBenchData(padImgWidth * padImgHeight);
  std::vector<double> kernel = makeImageConvolutionBenchKernel(benchCase.kernelWidth * benchCase.kernelHeight);
  std::vector<double> imgOut(benchCase.width * benchCase.height);

  for (auto _ : state) {
    Image2DFilterForOneBlock_Hwy(padImg.data(),
                                 padImgWidth,
                                 kernel.data(),
                                 benchCase.kernelWidth,
                                 benchCase.kernelHeight,
                                 imgOut.data(),
                                 benchCase.width,
                                 0,
                                 benchCase.height);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
}

static void BM_image_convolution_hwy_3d_row(benchmark::State& state, ImageConvolutionBenchCase benchCase)
{
  const size_t padImgWidth = benchCase.width + benchCase.kernelWidth - 1;
  const size_t padImgHeight = benchCase.height;
  std::vector<double> padImg = makeImageConvolutionBenchData(padImgWidth * padImgHeight * benchCase.depth);
  std::vector<double> kernel = makeImageConvolutionBenchKernel(benchCase.kernelWidth);
  std::vector<double> imgOut(benchCase.width * benchCase.height * benchCase.depth);

  for (auto _ : state) {
    Image3DRowFilterForOneBlock_Hwy(padImg.data(),
                                    padImgWidth,
                                    padImgHeight,
                                    kernel.data(),
                                    benchCase.kernelWidth,
                                    imgOut.data(),
                                    benchCase.width,
                                    benchCase.height,
                                    0,
                                    benchCase.depth);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
}

static void BM_image_convolution_hwy_3d_full(benchmark::State& state, ImageConvolutionBenchCase benchCase)
{
  const size_t padImgWidth = benchCase.width + benchCase.kernelWidth - 1;
  const size_t padImgHeight = benchCase.height + benchCase.kernelHeight - 1;
  const size_t padImgDepth = benchCase.depth + benchCase.kernelDepth - 1;
  std::vector<double> padImg = makeImageConvolutionBenchData(padImgWidth * padImgHeight * padImgDepth);
  std::vector<double> kernel =
    makeImageConvolutionBenchKernel(benchCase.kernelWidth * benchCase.kernelHeight * benchCase.kernelDepth);
  std::vector<double> imgOut(benchCase.width * benchCase.height * benchCase.depth);

  for (auto _ : state) {
    Image3DFilterForOneBlock_Hwy(padImg.data(),
                                 padImgWidth,
                                 padImgHeight,
                                 kernel.data(),
                                 benchCase.kernelWidth,
                                 benchCase.kernelHeight,
                                 benchCase.kernelDepth,
                                 imgOut.data(),
                                 benchCase.width,
                                 benchCase.height,
                                 0,
                                 benchCase.depth);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
}

void registerImageConvolutionBench(const ImageConvolutionBenchCase& benchCase,
                                   void (*benchFn)(benchmark::State&, ImageConvolutionBenchCase))
{
  benchmark::RegisterBenchmark(benchCase.name.c_str(), benchFn, benchCase)->Unit(benchmark::kMicrosecond);
}

static void addImageConvolutionBench()
{
  constexpr std::array<size_t, 4> imageSizes = {128, 256, 512, 1024};
  constexpr std::array<size_t, 4> rowKernelWidths = {3, 5, 11, 31};
  constexpr std::array<size_t, 3> fullKernelWidths = {3, 5, 11};
  constexpr std::array<size_t, 3> full3DKernelWidths = {3, 5, 7};
  constexpr std::array<std::tuple<size_t, size_t, size_t>, 3> row3DCases = {
    std::tuple<size_t, size_t, size_t>{64,  64,  16},
    std::tuple<size_t, size_t, size_t>{128, 128, 16},
    std::tuple<size_t, size_t, size_t>{256, 256, 16}
  };
  constexpr std::array<std::tuple<size_t, size_t, size_t>, 3> full3DCases = {
    std::tuple<size_t, size_t, size_t>{32,  32,  16},
    std::tuple<size_t, size_t, size_t>{64,  64,  16},
    std::tuple<size_t, size_t, size_t>{128, 128, 16}
  };

  for (const size_t size : imageSizes) {
    for (const size_t kernelWidth : rowKernelWidths) {
      registerImageConvolutionBench({fmt::format("ImageConvolutionHighway/2d_row/{}x{}/k{}", size, size, kernelWidth),
                                     size,
                                     size,
                                     1,
                                     kernelWidth,
                                     1,
                                     1},
                                    BM_image_convolution_hwy_2d_row);
    }

    for (const size_t kernelWidth : fullKernelWidths) {
      registerImageConvolutionBench(
        {fmt::format("ImageConvolutionHighway/2d_full/{}x{}/k{}x{}", size, size, kernelWidth, kernelWidth),
         size,
         size,
         1,
         kernelWidth,
         kernelWidth,
         1},
        BM_image_convolution_hwy_2d_full);
    }
  }

  for (const auto& [width, height, depth] : row3DCases) {
    for (const size_t kernelWidth : fullKernelWidths) {
      registerImageConvolutionBench(
        {fmt::format("ImageConvolutionHighway/3d_row/{}x{}x{}/k{}", width, height, depth, kernelWidth),
         width,
         height,
         depth,
         kernelWidth,
         1,
         1},
        BM_image_convolution_hwy_3d_row);
    }
  }

  for (const auto& [width, height, depth] : full3DCases) {
    for (const size_t kernelWidth : full3DKernelWidths) {
      registerImageConvolutionBench({fmt::format("ImageConvolutionHighway/3d_full/{}x{}x{}/k{}x{}x{}",
                                                 width,
                                                 height,
                                                 depth,
                                                 kernelWidth,
                                                 kernelWidth,
                                                 kernelWidth),
                                     width,
                                     height,
                                     depth,
                                     kernelWidth,
                                     kernelWidth,
                                     kernelWidth},
                                    BM_image_convolution_hwy_3d_full);
    }
  }
}

struct ImageResize2DHalfBenchCase
{
  std::string name;
  size_t width;
  size_t height;
};

struct ImageResize3DTargetBenchCase
{
  std::string name;
  size_t width;
  size_t height;
  size_t depth;
  size_t outWidth;
  size_t outHeight;
  size_t outDepth;
};

struct ImageResize3DVisBlockBenchCase
{
  std::string name;
  size_t width;
  size_t height;
  size_t depth;
  size_t outWidth;
  size_t outHeight;
  size_t outDepth;
};

struct ImageResize2DPyramidBenchCase
{
  std::string name;
  size_t width;
  size_t height;
  size_t stopThreshold;
};

struct ImageResize2DLevel
{
  size_t width;
  size_t height;
};

struct ResizeKernelWidths
{
  size_t x = 0;
  size_t y = 0;
  size_t z = 0;
};

template<typename T>
std::vector<T> makeImageResizeBenchData(size_t count)
{
  std::vector<T> data(count);
  for (size_t i = 0; i < data.size(); ++i) {
    const double value = (imageConvolutionBenchValue(i) + 1.0) * 0.5;
    if constexpr (std::is_integral_v<T>) {
      data[i] = static_cast<T>(std::llround(value * static_cast<double>(std::numeric_limits<T>::max())));
    } else {
      data[i] = static_cast<T>(value);
    }
  }
  return data;
}

template<typename T>
T imageResizeBenchConstantValue()
{
  if constexpr (std::is_integral_v<T>) {
    return static_cast<T>(std::numeric_limits<T>::max() / 2);
  } else {
    return static_cast<T>(0.5);
  }
}

template<typename T>
std::vector<T> makeImageResizePyramidBenchData(size_t count)
{
  constexpr size_t kConstantFillThreshold = 1'000'000'000;
  if (count >= kConstantFillThreshold) {
    return std::vector<T>(count, imageResizeBenchConstantValue<T>());
  }
  return makeImageResizeBenchData<T>(count);
}

template<typename T>
std::vector<T> makeImageResize3DVisBlockBenchData(size_t count)
{
  std::vector<T> data(count);
  for (size_t i = 0; i < data.size(); ++i) {
    const uint64_t hash = static_cast<uint64_t>(i) * 11400714819323198485ull + 0x9e3779b97f4a7c15ull;
    if constexpr (std::is_integral_v<T>) {
      constexpr uint64_t range = static_cast<uint64_t>(std::numeric_limits<T>::max()) + 1ull;
      data[i] = static_cast<T>((hash >> 32u) % range);
    } else {
      constexpr double scale = 1.0 / static_cast<double>(0xFFFFFFu);
      data[i] = static_cast<T>(static_cast<double>((hash >> 40u) & 0xFFFFFFu) * scale);
    }
  }
  return data;
}

template<typename T>
double maxResizeAbsDiff(const std::vector<T>& expected, const std::vector<T>& actual)
{
  CHECK(expected.size() == actual.size());
  double maxDiff = 0.0;
  for (size_t i = 0; i < expected.size(); ++i) {
    maxDiff = std::max(maxDiff, std::abs(static_cast<double>(expected[i]) - static_cast<double>(actual[i])));
  }
  return maxDiff;
}

template<typename T>
double resizeValidationTolerance()
{
  if constexpr (std::is_integral_v<T>) {
    return 1.0;
  } else {
    return 1.0e-9;
  }
}

ResizeKernelWidths resize2DKernelWidths(size_t width, size_t height, size_t outWidth, size_t outHeight)
{
  std::vector<double> weights;
  std::vector<size_t> indices;
  bool kernelIsTrivial = false;
  ResizeKernelWidths widths;
  _resizeContributions(width, outWidth, Interpolant::Cubic, true, weights, indices, widths.x, kernelIsTrivial);
  _resizeContributions(height, outHeight, Interpolant::Cubic, true, weights, indices, widths.y, kernelIsTrivial);
  return widths;
}

ResizeKernelWidths
resize3DKernelWidths(size_t width, size_t height, size_t depth, size_t outWidth, size_t outHeight, size_t outDepth)
{
  std::vector<double> weights;
  std::vector<size_t> indices;
  bool kernelIsTrivial = false;
  ResizeKernelWidths widths;
  _resizeContributions(width, outWidth, Interpolant::Cubic, true, weights, indices, widths.x, kernelIsTrivial);
  _resizeContributions(height, outHeight, Interpolant::Cubic, true, weights, indices, widths.y, kernelIsTrivial);
  _resizeContributions(depth, outDepth, Interpolant::Cubic, true, weights, indices, widths.z, kernelIsTrivial);
  return widths;
}

std::vector<ImageResize2DLevel> resize2DPyramidLevels(size_t width, size_t height, size_t stopThreshold)
{
  std::vector<ImageResize2DLevel> levels;
  while (width >= stopThreshold || height >= stopThreshold) {
    width = (width + 1) / 2;
    height = (height + 1) / 2;
    levels.push_back({width, height});
  }
  return levels;
}

size_t totalResize2DPyramidOutputPixels(const std::vector<ImageResize2DLevel>& levels)
{
  size_t count = 0;
  for (const auto& level : levels) {
    count += level.width * level.height;
  }
  return count;
}

size_t totalResize2DPyramidInputPixels(size_t width, size_t height, const std::vector<ImageResize2DLevel>& levels)
{
  size_t count = width * height;
  for (size_t i = 0; i + 1 < levels.size(); ++i) {
    count += levels[i].width * levels[i].height;
  }
  return count;
}

template<typename T>
void validateImageResize2DHalfHwy()
{
  static const bool validated = [] {
    constexpr size_t width = 64;
    constexpr size_t height = 48;
    std::vector<T> img = makeImageResizeBenchData<T>(width * height);
    std::vector<T> expected((width / 2) * (height / 2));
    std::vector<T> actual(expected.size());
    image2DResize(img.data(),
                  width,
                  height,
                  expected.data(),
                  width / 2,
                  height / 2,
                  Interpolant::Cubic,
                  true,
                  false,
                  true);
    image2DResizeHighway(img.data(),
                         width,
                         height,
                         actual.data(),
                         width / 2,
                         height / 2,
                         Interpolant::Cubic,
                         true,
                         false,
                         true);
    CHECK_LE(maxResizeAbsDiff(expected, actual), resizeValidationTolerance<T>());
    return true;
  }();
  CHECK(validated);
}

template<typename T>
void validateImageResize2DPyramidHwy()
{
  static const bool validated = [] {
    constexpr size_t width = 65;
    constexpr size_t height = 73;
    constexpr size_t stopThreshold = 16;
    std::vector<T> current = makeImageResizeBenchData<T>(width * height);
    std::vector<T> hwy = current;
    size_t currentWidth = width;
    size_t currentHeight = height;
    for (const auto& level : resize2DPyramidLevels(width, height, stopThreshold)) {
      std::vector<T> expected(level.width * level.height);
      std::vector<T> actual(level.width * level.height);
      image2DResize(current.data(),
                    currentWidth,
                    currentHeight,
                    expected.data(),
                    level.width,
                    level.height,
                    Interpolant::Cubic,
                    true,
                    false,
                    true);
      image2DResizeHighway(hwy.data(),
                           currentWidth,
                           currentHeight,
                           actual.data(),
                           level.width,
                           level.height,
                           Interpolant::Cubic,
                           true,
                           false,
                           true);
      CHECK_LE(maxResizeAbsDiff(expected, actual), resizeValidationTolerance<T>());
      current = std::move(expected);
      hwy = std::move(actual);
      currentWidth = level.width;
      currentHeight = level.height;
    }
    return true;
  }();
  CHECK(validated);
}

template<typename T>
void validateImageResize3DHwy()
{
  static const bool validated = [] {
    constexpr size_t width = 32;
    constexpr size_t height = 40;
    constexpr size_t depth = 24;
    constexpr size_t outWidth = 20;
    constexpr size_t outHeight = 20;
    constexpr size_t outDepth = 20;
    std::vector<T> img = makeImageResizeBenchData<T>(width * height * depth);
    std::vector<T> expected(outWidth * outHeight * outDepth);
    std::vector<T> actual(expected.size());
    image3DResize(img.data(),
                  width,
                  height,
                  depth,
                  expected.data(),
                  outWidth,
                  outHeight,
                  outDepth,
                  Interpolant::Cubic,
                  true,
                  false,
                  true);
    image3DResizeHighway(img.data(),
                         width,
                         height,
                         depth,
                         actual.data(),
                         outWidth,
                         outHeight,
                         outDepth,
                         Interpolant::Cubic,
                         true,
                         false,
                         true);
    CHECK_LE(maxResizeAbsDiff(expected, actual), resizeValidationTolerance<T>());
    return true;
  }();
  CHECK(validated);
}

template<typename T>
void validateImageResize3DVisBlockHwy()
{
  static const bool validated = [] {
    constexpr size_t width = 48;
    constexpr size_t height = 56;
    constexpr size_t depth = 96;
    constexpr size_t outWidth = 32;
    constexpr size_t outHeight = 32;
    constexpr size_t outDepth = 24;
    std::vector<T> img = makeImageResize3DVisBlockBenchData<T>(width * height * depth);
    std::vector<T> expected(outWidth * outHeight * outDepth);
    std::vector<T> actual(expected.size());
    image3DResize(img.data(),
                  width,
                  height,
                  depth,
                  expected.data(),
                  outWidth,
                  outHeight,
                  outDepth,
                  Interpolant::Cubic,
                  true,
                  false,
                  false);
    image3DResizeHighway(img.data(),
                         width,
                         height,
                         depth,
                         actual.data(),
                         outWidth,
                         outHeight,
                         outDepth,
                         Interpolant::Cubic,
                         true,
                         false,
                         false);
    CHECK_LE(maxResizeAbsDiff(expected, actual), resizeValidationTolerance<T>());
    return true;
  }();
  CHECK(validated);
}

template<typename T>
static void BM_image_resize_2d_half_current_cubic_aa(benchmark::State& state, ImageResize2DHalfBenchCase benchCase)
{
  const size_t outWidth = benchCase.width / 2;
  const size_t outHeight = benchCase.height / 2;
  const ResizeKernelWidths kernelWidths = resize2DKernelWidths(benchCase.width, benchCase.height, outWidth, outHeight);
  std::vector<T> img = makeImageResizeBenchData<T>(benchCase.width * benchCase.height);
  std::vector<T> imgOut(outWidth * outHeight);

  for (auto _ : state) {
    image2DResize(img.data(),
                  benchCase.width,
                  benchCase.height,
                  imgOut.data(),
                  outWidth,
                  outHeight,
                  Interpolant::Cubic,
                  true,
                  false,
                  true);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
}

template<typename T>
static void BM_image_resize_2d_half_hwy_cubic_aa(benchmark::State& state, ImageResize2DHalfBenchCase benchCase)
{
  validateImageResize2DHalfHwy<T>();

  const size_t outWidth = benchCase.width / 2;
  const size_t outHeight = benchCase.height / 2;
  const ResizeKernelWidths kernelWidths = resize2DKernelWidths(benchCase.width, benchCase.height, outWidth, outHeight);
  std::vector<T> img = makeImageResizeBenchData<T>(benchCase.width * benchCase.height);
  std::vector<T> imgOut(outWidth * outHeight);

  for (auto _ : state) {
    image2DResizeHighway(img.data(),
                         benchCase.width,
                         benchCase.height,
                         imgOut.data(),
                         outWidth,
                         outHeight,
                         Interpolant::Cubic,
                         true,
                         false,
                         true);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
}

template<typename T>
static void BM_image_resize_2d_pyramid_current_cubic_aa(benchmark::State& state,
                                                        ImageResize2DPyramidBenchCase benchCase)
{
  const auto levels = resize2DPyramidLevels(benchCase.width, benchCase.height, benchCase.stopThreshold);
  const size_t totalOutputPixels = totalResize2DPyramidOutputPixels(levels);
  const size_t totalInputPixels = totalResize2DPyramidInputPixels(benchCase.width, benchCase.height, levels);
  std::vector<T> base = makeImageResizePyramidBenchData<T>(benchCase.width * benchCase.height);

  for (auto _ : state) {
    std::vector<std::vector<T>> pyramid;
    pyramid.reserve(levels.size());
    const T* src = base.data();
    size_t srcWidth = benchCase.width;
    size_t srcHeight = benchCase.height;
    for (const auto& level : levels) {
      std::vector<T> out(level.width * level.height);
      image2DResize(src,
                    srcWidth,
                    srcHeight,
                    out.data(),
                    level.width,
                    level.height,
                    Interpolant::Cubic,
                    true,
                    false,
                    true);
      pyramid.push_back(std::move(out));
      src = pyramid.back().data();
      srcWidth = level.width;
      srcHeight = level.height;
    }
    benchmark::DoNotOptimize(pyramid.back().data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(totalOutputPixels));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(totalInputPixels * sizeof(T)));
  state.counters["levels"] = static_cast<double>(levels.size());
}

template<typename T>
static void BM_image_resize_2d_pyramid_hwy_cubic_aa(benchmark::State& state, ImageResize2DPyramidBenchCase benchCase)
{
  validateImageResize2DPyramidHwy<T>();

  const auto levels = resize2DPyramidLevels(benchCase.width, benchCase.height, benchCase.stopThreshold);
  const size_t totalOutputPixels = totalResize2DPyramidOutputPixels(levels);
  const size_t totalInputPixels = totalResize2DPyramidInputPixels(benchCase.width, benchCase.height, levels);
  std::vector<T> base = makeImageResizePyramidBenchData<T>(benchCase.width * benchCase.height);

  for (auto _ : state) {
    std::vector<std::vector<T>> pyramid;
    pyramid.reserve(levels.size());
    const T* src = base.data();
    size_t srcWidth = benchCase.width;
    size_t srcHeight = benchCase.height;
    for (const auto& level : levels) {
      std::vector<T> out(level.width * level.height);
      image2DResizeHighway(src,
                           srcWidth,
                           srcHeight,
                           out.data(),
                           level.width,
                           level.height,
                           Interpolant::Cubic,
                           true,
                           false,
                           true);
      pyramid.push_back(std::move(out));
      src = pyramid.back().data();
      srcWidth = level.width;
      srcHeight = level.height;
    }
    benchmark::DoNotOptimize(pyramid.back().data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(totalOutputPixels));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(totalInputPixels * sizeof(T)));
  state.counters["levels"] = static_cast<double>(levels.size());
}

template<typename T>
static void BM_image_resize_3d_target_current_cubic_aa(benchmark::State& state, ImageResize3DTargetBenchCase benchCase)
{
  if (!absl::GetFlag(FLAGS_atlas_resize_3d_exact_benchmark)) {
    state.SkipWithError("pass --atlas_resize_3d_exact_benchmark=true to enable exact-size 3D resize benchmarks");
    return;
  }

  const ResizeKernelWidths kernelWidths = resize3DKernelWidths(benchCase.width,
                                                               benchCase.height,
                                                               benchCase.depth,
                                                               benchCase.outWidth,
                                                               benchCase.outHeight,
                                                               benchCase.outDepth);
  std::vector<T> img = makeImageResizeBenchData<T>(benchCase.width * benchCase.height * benchCase.depth);
  std::vector<T> imgOut(benchCase.outWidth * benchCase.outHeight * benchCase.outDepth);

  for (auto _ : state) {
    image3DResize(img.data(),
                  benchCase.width,
                  benchCase.height,
                  benchCase.depth,
                  imgOut.data(),
                  benchCase.outWidth,
                  benchCase.outHeight,
                  benchCase.outDepth,
                  Interpolant::Cubic,
                  true,
                  false,
                  true);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
  state.counters["z_taps"] = static_cast<double>(kernelWidths.z);
}

template<typename T>
static void BM_image_resize_3d_target_hwy_cubic_aa(benchmark::State& state, ImageResize3DTargetBenchCase benchCase)
{
  if (!absl::GetFlag(FLAGS_atlas_resize_3d_exact_benchmark)) {
    state.SkipWithError("pass --atlas_resize_3d_exact_benchmark=true to enable exact-size 3D resize benchmarks");
    return;
  }
  validateImageResize3DHwy<T>();

  const ResizeKernelWidths kernelWidths = resize3DKernelWidths(benchCase.width,
                                                               benchCase.height,
                                                               benchCase.depth,
                                                               benchCase.outWidth,
                                                               benchCase.outHeight,
                                                               benchCase.outDepth);
  std::vector<T> img = makeImageResizeBenchData<T>(benchCase.width * benchCase.height * benchCase.depth);
  std::vector<T> imgOut(benchCase.outWidth * benchCase.outHeight * benchCase.outDepth);

  for (auto _ : state) {
    image3DResizeHighway(img.data(),
                         benchCase.width,
                         benchCase.height,
                         benchCase.depth,
                         imgOut.data(),
                         benchCase.outWidth,
                         benchCase.outHeight,
                         benchCase.outDepth,
                         Interpolant::Cubic,
                         true,
                         false,
                         true);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
  state.counters["z_taps"] = static_cast<double>(kernelWidths.z);
}

template<typename T>
static void BM_image_resize_3d_vis_block_current_cubic_aa(benchmark::State& state,
                                                          ImageResize3DVisBlockBenchCase benchCase)
{
  const ResizeKernelWidths kernelWidths = resize3DKernelWidths(benchCase.width,
                                                               benchCase.height,
                                                               benchCase.depth,
                                                               benchCase.outWidth,
                                                               benchCase.outHeight,
                                                               benchCase.outDepth);
  std::vector<T> img = makeImageResize3DVisBlockBenchData<T>(benchCase.width * benchCase.height * benchCase.depth);
  std::vector<T> imgOut(benchCase.outWidth * benchCase.outHeight * benchCase.outDepth);

  for (auto _ : state) {
    image3DResize(img.data(),
                  benchCase.width,
                  benchCase.height,
                  benchCase.depth,
                  imgOut.data(),
                  benchCase.outWidth,
                  benchCase.outHeight,
                  benchCase.outDepth,
                  Interpolant::Cubic,
                  true,
                  false,
                  false);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
  state.counters["z_taps"] = static_cast<double>(kernelWidths.z);
}

template<typename T>
static void BM_image_resize_3d_vis_block_hwy_cubic_aa(benchmark::State& state, ImageResize3DVisBlockBenchCase benchCase)
{
  validateImageResize3DVisBlockHwy<T>();

  const ResizeKernelWidths kernelWidths = resize3DKernelWidths(benchCase.width,
                                                               benchCase.height,
                                                               benchCase.depth,
                                                               benchCase.outWidth,
                                                               benchCase.outHeight,
                                                               benchCase.outDepth);
  std::vector<T> img = makeImageResize3DVisBlockBenchData<T>(benchCase.width * benchCase.height * benchCase.depth);
  std::vector<T> imgOut(benchCase.outWidth * benchCase.outHeight * benchCase.outDepth);

  for (auto _ : state) {
    image3DResizeHighway(img.data(),
                         benchCase.width,
                         benchCase.height,
                         benchCase.depth,
                         imgOut.data(),
                         benchCase.outWidth,
                         benchCase.outHeight,
                         benchCase.outDepth,
                         Interpolant::Cubic,
                         true,
                         false,
                         false);
    benchmark::DoNotOptimize(imgOut.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(imgOut.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(img.size() * sizeof(T)));
  state.counters["x_taps"] = static_cast<double>(kernelWidths.x);
  state.counters["y_taps"] = static_cast<double>(kernelWidths.y);
  state.counters["z_taps"] = static_cast<double>(kernelWidths.z);
}

template<typename T>
void registerImageResize2DHalfTypeBenches(std::string_view typeName)
{
  constexpr std::array<size_t, 3> imageSizes = {512, 1024, 2048};
  for (const size_t size : imageSizes) {
    ImageResize2DHalfBenchCase currentBenchCase{
      fmt::format("ImageResize2DHalf/Current/CubicAA/{}/{}x{}_to_{}x{}", typeName, size, size, size / 2, size / 2),
      size,
      size};
    benchmark::RegisterBenchmark(currentBenchCase.name.c_str(),
                                 &BM_image_resize_2d_half_current_cubic_aa<T>,
                                 currentBenchCase)
      ->Unit(benchmark::kMicrosecond);

    ImageResize2DHalfBenchCase hwyBenchCase{fmt::format("ImageResize2DHalf/HighwaySeparable/CubicAA/{}/{}x{}_to_{}x{}",
                                                        typeName,
                                                        size,
                                                        size,
                                                        size / 2,
                                                        size / 2),
                                            size,
                                            size};
    benchmark::RegisterBenchmark(hwyBenchCase.name.c_str(), &BM_image_resize_2d_half_hwy_cubic_aa<T>, hwyBenchCase)
      ->Unit(benchmark::kMicrosecond);
  }
}

template<typename T>
void registerImageResize2DPyramidTypeBenches(std::string_view typeName)
{
  const std::array<std::tuple<std::string_view, size_t, size_t>, 2> cases = {
    std::tuple<std::string_view, size_t, size_t>{"21000x24000_to_lt512",   21000,  24000 },
    std::tuple<std::string_view, size_t, size_t>{"110000x120000_to_lt512", 110000, 120000},
  };

  for (const auto& [caseName, width, height] : cases) {
    ImageResize2DPyramidBenchCase currentBenchCase{
      fmt::format("ImageResize2DPyramid/Current/CubicAA/{}/{}", typeName, caseName),
      width,
      height,
      512};
    benchmark::RegisterBenchmark(currentBenchCase.name.c_str(),
                                 &BM_image_resize_2d_pyramid_current_cubic_aa<T>,
                                 currentBenchCase)
      ->Unit(benchmark::kMillisecond)
      ->Iterations(1);

    ImageResize2DPyramidBenchCase hwyBenchCase{
      fmt::format("ImageResize2DPyramid/HighwaySeparable/CubicAA/{}/{}", typeName, caseName),
      width,
      height,
      512};
    benchmark::RegisterBenchmark(hwyBenchCase.name.c_str(), &BM_image_resize_2d_pyramid_hwy_cubic_aa<T>, hwyBenchCase)
      ->Unit(benchmark::kMillisecond)
      ->Iterations(1);
  }
}

template<typename T>
void registerImageResize3DTargetTypeBenches(std::string_view typeName)
{
  ImageResize3DTargetBenchCase currentBenchCase{
    fmt::format("ImageResize3DTarget/Current/CubicAA/{}/800x1000x600_to_512x512x512", typeName),
    800,
    1000,
    600,
    512,
    512,
    512};
  benchmark::RegisterBenchmark(currentBenchCase.name.c_str(),
                               &BM_image_resize_3d_target_current_cubic_aa<T>,
                               currentBenchCase)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);

  ImageResize3DTargetBenchCase hwyBenchCase{
    fmt::format("ImageResize3DTarget/HighwayPlaneFirst/CubicAA/{}/800x1000x600_to_512x512x512", typeName),
    800,
    1000,
    600,
    512,
    512,
    512};
  benchmark::RegisterBenchmark(hwyBenchCase.name.c_str(), &BM_image_resize_3d_target_hwy_cubic_aa<T>, hwyBenchCase)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);
}

template<typename T>
void registerImageResize3DVisBlockTypeBenches(std::string_view typeName)
{
  const std::array<std::tuple<std::string_view, size_t, size_t, size_t, size_t, size_t, size_t>, 4> cases = {
    std::tuple<std::string_view, size_t, size_t, size_t, size_t, size_t, size_t>{"zonly_block64_z2000",
                                                                                 64,  64,
                                                                                 2000, 64,
                                                                                 64,  64 },
    std::tuple<std::string_view, size_t, size_t, size_t, size_t, size_t, size_t>{"xy2_block64_z2000",
                                                                                 128, 128,
                                                                                 2000, 64,
                                                                                 64,  64 },
    std::tuple<std::string_view, size_t, size_t, size_t, size_t, size_t, size_t>{"zonly_block128_z2000",
                                                                                 128, 128,
                                                                                 2000, 128,
                                                                                 128, 128},
    std::tuple<std::string_view, size_t, size_t, size_t, size_t, size_t, size_t>{"xy2_block128_z2000",
                                                                                 256, 256,
                                                                                 2000, 128,
                                                                                 128, 128},
  };

  for (const auto& [caseName, width, height, depth, outWidth, outHeight, outDepth] : cases) {
    ImageResize3DVisBlockBenchCase currentBenchCase{
      fmt::format("ImageResize3DVisBlock/Current/CubicAA_SingleThread/{}/{}/{}x{}x{}_to_{}x{}x{}",
                  typeName,
                  caseName,
                  width,
                  height,
                  depth,
                  outWidth,
                  outHeight,
                  outDepth),
      width,
      height,
      depth,
      outWidth,
      outHeight,
      outDepth};
    benchmark::RegisterBenchmark(currentBenchCase.name.c_str(),
                                 &BM_image_resize_3d_vis_block_current_cubic_aa<T>,
                                 currentBenchCase)
      ->Unit(benchmark::kMillisecond)
      ->Iterations(1);

    ImageResize3DVisBlockBenchCase hwyBenchCase{
      fmt::format("ImageResize3DVisBlock/HighwayPlaneFirst/CubicAA_SingleThread/{}/{}/{}x{}x{}_to_{}x{}x{}",
                  typeName,
                  caseName,
                  width,
                  height,
                  depth,
                  outWidth,
                  outHeight,
                  outDepth),
      width,
      height,
      depth,
      outWidth,
      outHeight,
      outDepth};
    benchmark::RegisterBenchmark(hwyBenchCase.name.c_str(), &BM_image_resize_3d_vis_block_hwy_cubic_aa<T>, hwyBenchCase)
      ->Unit(benchmark::kMillisecond)
      ->Iterations(1);
  }
}

static void addImageResizeBench()
{
  registerImageResize2DHalfTypeBenches<uint8_t>("uint8");
  registerImageResize2DHalfTypeBenches<uint16_t>("uint16");
  registerImageResize2DHalfTypeBenches<double>("double");
  registerImageResize2DPyramidTypeBenches<uint8_t>("uint8");
  registerImageResize2DPyramidTypeBenches<uint16_t>("uint16");
  registerImageResize3DTargetTypeBenches<uint8_t>("uint8");
  registerImageResize3DTargetTypeBenches<uint16_t>("uint16");
  registerImageResize3DVisBlockTypeBenches<uint8_t>("uint8");
  registerImageResize3DVisBlockTypeBenches<uint16_t>("uint16");
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

constexpr int kCacheBenchmarkOperationsPerThread = 1000000;
constexpr int kCacheBenchmarkKeyRange = 100000;
constexpr size_t kCacheBenchmarkMaxSize = 1000000;

int cacheBenchmarkThreadCount()
{
  const unsigned int hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads == 0) {
    return 4;
  }
  return static_cast<int>(hardwareThreads);
}

static void BM_ConcurrentLRUCacheMixed(benchmark::State& state)
{
  const size_t numSegments = static_cast<size_t>(state.range(0));
  const int threadCount = static_cast<int>(state.range(1));
  const int64_t operationsPerRun = static_cast<int64_t>(threadCount) * kCacheBenchmarkOperationsPerThread;
  int64_t totalHits = 0;

  for (auto _ : state) {
    ZConcurrentLRUCache<int, int> cache(kCacheBenchmarkMaxSize, numSegments);
    std::atomic<int> hits{0};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int threadId = 0; threadId < threadCount; ++threadId) {
      threads.emplace_back([&cache, &hits, threadId]() {
        std::mt19937 rng(threadId);
        std::uniform_int_distribution<int> opDist(0, 2);
        std::uniform_int_distribution<int> keyDist(0, kCacheBenchmarkKeyRange - 1);

        for (int i = 0; i < kCacheBenchmarkOperationsPerThread; ++i) {
          const int key = keyDist(rng);
          const int value = key * 10;
          switch (opDist(rng)) {
            case 0:
              cache.insert(key, value, sizeof(value));
              break;
            case 1:
              if (const auto result = cache.find(key, ZConcurrentLRUCache<int, int>::FindStrategy::MaybeUpdateLRUList);
                  result.has_value()) {
                ++hits;
                int foundValue = result.value();
                benchmark::DoNotOptimize(foundValue);
              }
              break;
            case 2:
              cache.remove(key);
              break;
            default:
              break;
          }
        }
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }
    totalHits += hits.load(std::memory_order_relaxed);
    benchmark::DoNotOptimize(cache.size());
  }

  state.SetItemsProcessed(state.iterations() * operationsPerRun);
  state.counters["hits_per_run"] = static_cast<double>(totalHits) / state.iterations();
  state.counters["ops_per_run"] = static_cast<double>(operationsPerRun);
  state.counters["segments"] = static_cast<double>(numSegments);
  state.counters["threads"] = static_cast<double>(threadCount);
}

static void addConcurrentLRUCacheBench()
{
  const int threadCount = cacheBenchmarkThreadCount();
  BENCHMARK(BM_ConcurrentLRUCacheMixed)
    ->Args({0, threadCount})
    ->Args({8, threadCount})
    ->Args({16, threadCount})
    ->Args({32, threadCount})
    ->Args({64, threadCount})
    ->UseRealTime()
    ->ArgNames({"segments", "threads"});
}

enum class BlockIdSyntheticCase
{
  MostlyEmpty4KDomain3PctReal,
  Mixed64KDomain25PctReal,
  Heavy1MDomain80PctReal,
  AllZero,
  RealLogEmpty13MSingleBuffer,
  RealLogSparse338Unique26MTwoBuffers,
  RealLogSparse1421Unique26MTwoBuffers,
  Count
};

struct BlockIdSyntheticCaseInfo
{
  BlockIdSyntheticCase id;
  std::string_view name;
  uint32_t maxBlockId;
  size_t bufferCount;
  size_t wordsPerBuffer;
  uint32_t randomRealPermille;
  uint32_t randomInvalidPermille;
  uint32_t targetUniqueBlockIds;
  uint32_t occurrencesPerUniqueBlock;
};

struct BlockIdSyntheticDataset
{
  Z3DBlockIdBufferList buffers;
  std::vector<uint32_t> ascendingReference;
  uint32_t maxBlockId = 0;
  size_t wordCount = 0;
  size_t wordsPerBuffer = 0;
  size_t validWordCount = 0;
  size_t invalidWordCount = 0;
};

constexpr size_t kBlockIdSyntheticDefaultReadbackWords = 1024u * 1024u * 4u;
constexpr size_t kBlockIdRealLogReadbackWords = 13008128u;
constexpr uint32_t kBlockIdRealLogMaxBlockId = 551672u;
constexpr std::array<BlockIdSyntheticCaseInfo, static_cast<size_t>(BlockIdSyntheticCase::Count)>
  kBlockIdSyntheticCases = {
    {
     {BlockIdSyntheticCase::MostlyEmpty4KDomain3PctReal,
       "mostly_empty_4k_id_domain_3pct_real",
       4096u,
       1u,
       kBlockIdSyntheticDefaultReadbackWords,
       30u,
       1u,
       0u,
       0u},
     {BlockIdSyntheticCase::Mixed64KDomain25PctReal,
       "mixed_64k_id_domain_25pct_real",
       65536u,
       1u,
       kBlockIdSyntheticDefaultReadbackWords,
       250u,
       1u,
       0u,
       0u},
     {BlockIdSyntheticCase::Heavy1MDomain80PctReal,
       "heavy_1m_id_domain_80pct_real",
       1048576u,
       1u,
       kBlockIdSyntheticDefaultReadbackWords,
       800u,
       1u,
       0u,
       0u},
     {BlockIdSyntheticCase::AllZero, "all_zero", 4096u, 1u, kBlockIdSyntheticDefaultReadbackWords, 0u, 0u, 0u, 0u},
     {BlockIdSyntheticCase::RealLogEmpty13MSingleBuffer,
       "real_log_empty_13m_1buffer",
       kBlockIdRealLogMaxBlockId,
       1u,
       kBlockIdRealLogReadbackWords,
       0u,
       0u,
       0u,
       0u},
     {BlockIdSyntheticCase::RealLogSparse338Unique26MTwoBuffers,
       "real_log_sparse_338_unique_26m_2buffers",
       kBlockIdRealLogMaxBlockId,
       2u,
       kBlockIdRealLogReadbackWords,
       0u,
       0u,
       338u,
       3072u},
     {BlockIdSyntheticCase::RealLogSparse1421Unique26MTwoBuffers,
       "real_log_sparse_1421_unique_26m_2buffers",
       kBlockIdRealLogMaxBlockId,
       2u,
       kBlockIdRealLogReadbackWords,
       0u,
       0u,
       1421u,
       5120u},
     }
};

const BlockIdSyntheticCaseInfo& blockIdSyntheticCaseInfo(BlockIdSyntheticCase id)
{
  const size_t index = static_cast<size_t>(id);
  CHECK_LT(index, kBlockIdSyntheticCases.size());
  CHECK(kBlockIdSyntheticCases[index].id == id);
  return kBlockIdSyntheticCases[index];
}

BlockIdSyntheticDataset makeBlockIdSyntheticDataset(BlockIdSyntheticCase id)
{
  const BlockIdSyntheticCaseInfo& info = blockIdSyntheticCaseInfo(id);
  CHECK_GT(info.bufferCount, 0u);
  CHECK_GT(info.wordsPerBuffer, 0u);
  CHECK_LE(info.randomRealPermille + info.randomInvalidPermille, 1000u);
  CHECK_LE(info.targetUniqueBlockIds, info.maxBlockId);

  BlockIdSyntheticDataset dataset;
  dataset.maxBlockId = info.maxBlockId;
  dataset.wordCount = info.bufferCount * info.wordsPerBuffer;
  dataset.wordsPerBuffer = info.wordsPerBuffer;
  dataset.buffers.resize(info.bufferCount);
  for (auto& buffer : dataset.buffers) {
    buffer.resize(info.wordsPerBuffer);
  }

  auto setFlatWord = [&dataset](size_t flatIndex, uint32_t word) {
    CHECK_LT(flatIndex, dataset.wordCount);
    dataset.buffers[flatIndex / dataset.wordsPerBuffer][flatIndex % dataset.wordsPerBuffer] = word;
  };

  if (info.targetUniqueBlockIds > 0u) {
    const size_t validWordCount =
      static_cast<size_t>(info.targetUniqueBlockIds) * static_cast<size_t>(info.occurrencesPerUniqueBlock);
    CHECK_LE(validWordCount, dataset.wordCount);
    dataset.validWordCount = validWordCount;

    const size_t spacing = std::max<size_t>(1u, dataset.wordCount / validWordCount);
    const size_t start = static_cast<size_t>(static_cast<uint32_t>(id)) % spacing;
    for (size_t occurrence = 0; occurrence < validWordCount; ++occurrence) {
      const size_t flatIndex = start + occurrence * spacing;
      CHECK_LT(flatIndex, dataset.wordCount);
      const uint32_t idOrdinal = static_cast<uint32_t>(occurrence % info.targetUniqueBlockIds);
      const uint32_t blockId =
        1u + static_cast<uint32_t>((static_cast<uint64_t>(idOrdinal) * info.maxBlockId) / info.targetUniqueBlockIds);
      setFlatWord(flatIndex, blockId);
    }
  } else if (info.randomRealPermille > 0u || info.randomInvalidPermille > 0u) {
    std::mt19937 rng(0xA71A500u + static_cast<uint32_t>(static_cast<size_t>(id)));
    std::uniform_int_distribution<uint32_t> idDist(1u, info.maxBlockId);
    std::uniform_int_distribution<uint32_t> eventDist(0u, 999u);
    const uint32_t invalidStart = 1000u - info.randomInvalidPermille;
    for (auto& buffer : dataset.buffers) {
      for (uint32_t& word : buffer) {
        const uint32_t event = eventDist(rng);
        if (event < info.randomRealPermille) {
          word = idDist(rng);
          ++dataset.validWordCount;
        } else if (event >= invalidStart) {
          word = std::numeric_limits<uint32_t>::max();
          ++dataset.invalidWordCount;
        } else {
          word = 0u;
        }
      }
    }
  }

  dataset.ascendingReference = collectZ3DBlockIdsForBenchmark(dataset.buffers,
                                                              dataset.maxBlockId,
                                                              Z3DBlockIdSortOrder::Ascending,
                                                              Z3DBlockIdCollectorMethod::DenseBitset);
  return dataset;
}

const BlockIdSyntheticDataset& blockIdSyntheticDataset(BlockIdSyntheticCase id)
{
  const size_t index = static_cast<size_t>(id);
  CHECK_LT(index, kBlockIdSyntheticCases.size());

  static std::array<std::once_flag, static_cast<size_t>(BlockIdSyntheticCase::Count)> onceFlags;
  static std::array<std::unique_ptr<BlockIdSyntheticDataset>, static_cast<size_t>(BlockIdSyntheticCase::Count)>
    datasets;
  std::call_once(onceFlags[index], [id, index]() {
    datasets[index] = std::make_unique<BlockIdSyntheticDataset>(makeBlockIdSyntheticDataset(id));
  });
  CHECK(datasets[index] != nullptr);
  return *datasets[index];
}

static void BM_BlockIdCollectorSynthetic(benchmark::State& state,
                                         BlockIdSyntheticCase syntheticCase,
                                         Z3DBlockIdCollectorMethod method)
{
  const BlockIdSyntheticDataset& dataset = blockIdSyntheticDataset(syntheticCase);
  const std::vector<uint32_t> warmup =
    collectZ3DBlockIdsForBenchmark(dataset.buffers, dataset.maxBlockId, Z3DBlockIdSortOrder::Ascending, method);
  if (warmup != dataset.ascendingReference) {
    state.SkipWithError("block-ID collector produced a different unique sorted ID list than dense_bitset");
    return;
  }

  for (auto _ : state) {
    std::vector<uint32_t> ids =
      collectZ3DBlockIdsForBenchmark(dataset.buffers, dataset.maxBlockId, Z3DBlockIdSortOrder::Ascending, method);
    benchmark::DoNotOptimize(ids.data());
    benchmark::DoNotOptimize(ids.size());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(dataset.wordCount));
  state.counters["buffers"] = static_cast<double>(dataset.buffers.size());
  state.counters["input_words"] = static_cast<double>(dataset.wordCount);
  state.counters["max_block_id"] = static_cast<double>(dataset.maxBlockId);
  state.counters["valid_percent"] =
    dataset.wordCount == 0u ? 0.0 : 100.0 * static_cast<double>(dataset.validWordCount) / dataset.wordCount;
  state.counters["valid_words"] = static_cast<double>(dataset.validWordCount);
  state.counters["unique_ids"] = static_cast<double>(dataset.ascendingReference.size());
  state.counters["words_per_buffer"] = static_cast<double>(dataset.wordsPerBuffer);
  state.counters["threads"] = static_cast<double>(std::thread::hardware_concurrency());
}

static void addBlockIdCollectorBench()
{
  for (const BlockIdSyntheticCaseInfo& syntheticCase : kBlockIdSyntheticCases) {
    for (const Z3DBlockIdCollectorMethodInfo& method : z3DBlockIdCollectorMethods()) {
      const std::string name = fmt::format("BlockIdCollector/{}/{}", syntheticCase.name, method.name);
      benchmark::RegisterBenchmark(
        name,
        [syntheticCaseId = syntheticCase.id, methodId = method.method](benchmark::State& state) {
          BM_BlockIdCollectorSynthetic(state, syntheticCaseId, methodId);
        })
        ->Unit(benchmark::kMillisecond)
        ->UseRealTime()
        ->Repetitions(3)
        ->ReportAggregatesOnly(true);
    }
  }
}

constexpr int kBioFormatsBenchmarkIoTimeoutMs = 10 * 60 * 1000;
constexpr const char* kDefaultBioFormatsBenchmarkFile =
  "/Users/feng/Documents/omeimages/Ventana/openslide/Ventana-1.bif";
constexpr uint64_t kBioFormatsBenchmarkTileSize = 1024;
constexpr uint32_t kBioFormatsBenchmarkAxisSamples = 4;
constexpr int kBioFormatsBenchmarkMaxConcurrency = 16;

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
    if (absl::GetFlag(FLAGS_atlas_bioformats_bridge_io_timeout_ms) <= 0) {
      absl::SetFlag(&FLAGS_atlas_bioformats_bridge_io_timeout_ms, kBioFormatsBenchmarkIoTimeoutMs);
    }

    const QDir jarsDir = atlasTestJarsDir();
    if (!QFileInfo(atlasTestBioFormatsJarPath()).isFile()) {
      error = fmt::format("missing {}", atlasTestBioFormatsJarPath());
      return;
    }
    if (!QFileInfo(atlasTestBioFormatsBridgeJarPath()).isFile()) {
      error = fmt::format("missing {}", atlasTestBioFormatsBridgeJarPath());
      return;
    }
    const QDir jreDir = atlasTestJreDir();
    if (!QFileInfo(atlasTestJavaExecutablePath()).isExecutable()) {
      error = fmt::format("missing {}", atlasTestJavaExecutablePath());
      return;
    }

    ZLogInit::instance("zbenchmark");
    LOG(INFO) << "Bio-Formats benchmark runtime: file=" << bioFormatsBenchmarkFile()
              << ", java=" << atlasTestJavaExecutablePath() << ", jarsDIR=" << jarsDir.absolutePath()
              << ", bridgeDiagnostics=" << absl::GetFlag(FLAGS_atlas_bioformats_bridge_diagnostics);

    try {
      ZImgInit::instance("", jreDir.absolutePath(), jarsDir.absolutePath(), false);
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
    throw ZException(fmt::format("{} exceeds Atlas index range: {}", name, value));
  }
  return static_cast<index_t>(value);
}

int64_t checkedBenchmarkBytes(uint64_t value)
{
  if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw ZException(fmt::format("benchmark byte count exceeds int64 range: {}", value));
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
    throw ZException(*error);
  }

  BioFormatsBenchmarkDataset dataset;
  dataset.path = bioFormatsBenchmarkFile();
  const QFileInfo file(dataset.path);
  if (!file.isFile()) {
    throw ZException(fmt::format("benchmark file does not exist: {}", dataset.path));
  }

  dataset.info = ZBioFormatsBridgeClient::instance().readDatasetInfo(dataset.path);
  if (dataset.info.series.empty()) {
    throw ZException("benchmark dataset has no Bio-Formats series");
  }

  const ZBioFormatsSeriesInfo& series = dataset.info.series.front();
  if (series.sizeX == 0 || series.sizeY == 0 || series.sizeZ == 0 || series.sizeT == 0 || series.effectiveSizeC == 0 ||
      series.bytesPerPixel == 0) {
    throw ZException("benchmark dataset has invalid zero-sized metadata");
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
    throw ZException("benchmark dataset produced no regions");
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

uint64_t readBioFormatsRegionsConcurrent(const BioFormatsBenchmarkDataset& dataset,
                                         const std::vector<ZImgRegion>& regions,
                                         int concurrency)
{
  CHECK(concurrency > 0);
  uint64_t totalBytes = 0;
  for (size_t first = 0; first < regions.size();) {
    std::vector<std::future<std::vector<uint8_t>>> futures;
    futures.reserve(static_cast<size_t>(concurrency));
    for (int i = 0; i < concurrency && first < regions.size(); ++i, ++first) {
      const ZImgRegion region = regions[first];
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

uint64_t readBioFormatsRegionsConcurrent(const BioFormatsBenchmarkDataset& dataset, int concurrency)
{
  return readBioFormatsRegionsConcurrent(dataset, dataset.regions, concurrency);
}

std::vector<ZImgRegion> splitBioFormatsRegionsByChannel(const BioFormatsBenchmarkDataset& dataset)
{
  std::vector<ZImgRegion> regions;
  for (const ZImgRegion& region : dataset.regions) {
    for (index_t c = region.start.c; c < region.end.c; ++c) {
      ZImgRegion channelRegion = region;
      channelRegion.start.c = c;
      channelRegion.end.c = c + 1;
      regions.push_back(channelRegion);
    }
  }
  if (regions.empty()) {
    throw ZException("benchmark dataset produced no split-channel regions");
  }
  return regions;
}

static void BM_BioFormatsVentanaDatasetInfo(benchmark::State& state)
{
  try {
    if (const auto& error = bioFormatsBenchmarkRuntimeError(); error.has_value()) {
      state.SkipWithError(*error);
      return;
    }
    const QString path = bioFormatsBenchmarkFile();
    if (!QFileInfo(path).isFile()) {
      const std::string message = fmt::format("benchmark file does not exist: {}", path);
      state.SkipWithError(message);
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

static void BM_BioFormatsSplitChannelConcurrentRegions(benchmark::State& state)
{
  try {
    const int concurrency = static_cast<int>(state.range(0));
    const BioFormatsBenchmarkDataset dataset = makeBioFormatsBenchmarkDataset();
    const std::vector<ZImgRegion> channelRegions = splitBioFormatsRegionsByChannel(dataset);
    readBioFormatsRegionsConcurrent(dataset, channelRegions, concurrency);

    uint64_t totalBytes = 0;
    for (auto _ : state) {
      QElapsedTimer timer;
      timer.start();
      totalBytes += readBioFormatsRegionsConcurrent(dataset, channelRegions, concurrency);
      state.SetIterationTime(timer.nsecsElapsed() / 1.0e9);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(channelRegions.size()));
    state.SetBytesProcessed(checkedBenchmarkBytes(totalBytes));
    state.counters["concurrency"] = concurrency;
    state.counters["source_regions_per_iteration"] = static_cast<double>(dataset.regions.size());
    state.counters["regions_per_iteration"] = static_cast<double>(channelRegions.size());
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
  BENCHMARK(BM_BioFormatsSplitChannelConcurrentRegions)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(kBioFormatsBenchmarkMaxConcurrency)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime()
    ->Iterations(3);
}

} // namespace nim

int main(int argc, char* argv[])
{
  using namespace nim;

  addRoundBench();
  addSaturateMulBench();
  addSaturateAddSubBench();
  addImageConvolutionBench();
  addImageResizeBench();
  addVectorLoopBench();
  addLockBench();
  addConcurrentLRUCacheBench();
  addBlockIdCollectorBench();
  addBioFormatsBench();

  benchmark::Initialize(&argc, argv);
  nim::parseCommandLine(argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
