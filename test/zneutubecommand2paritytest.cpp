#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <iterator>
#include <span>
#include <tuple>
#include <memory>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <optional>

#include "zcommandlineflags.h"
#include "zioutils.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#if !defined(_MSC_VER)
// neurolabi's legacy tz_* code is built as C++ on MSVC (see `tz_cdefs.h`), so
// wrapping these headers in `extern "C"` would force C linkage and break
// linking (unresolved externals for the unmangled symbol names).
extern "C" {
#endif
#include "tz_darray.h"
#include "tz_geo3d_scalar_field.h"
#include "tz_local_neuroseg.h"
#include "tz_locseg_chain.h"
#include "tz_math.h"
#include "tz_neuron_structure.h"
#include "tz_neuroseg.h"
#include "tz_perceptor.h"
#include "tz_stack_bwmorph.h"
#include "tz_stack_lib.h"
#include "tz_stack_math.h"
#include "tz_stack_neighborhood.h"
#include "tz_stack_objlabel.h"
#include "tz_stack_sampling.h"
#include "tz_swc_tree.h"
#include "tz_trace_utils.h"
#include "tz_voxel_graphics.h"
#include "tz_workspace.h"
#if !defined(_MSC_VER)
}
#endif

#include "zstack.hxx"
#include "geometry/zpointarray.h"
#include "zlocalneuroseg.h"
#include "zneurontracer.h"
#include "swc/zswcpruner.h"
#include "swc/zswcresampler.h"

#include "zneutubecompareswc.h"
#include "zneutubedarraymath.h"
#include "zneutubegeo3dscalarfield.h"
#include "zneutubeedt3d.h"
#include "zneutubeimgbwmorph.h"
#include "zneutubeimglocmax.h"
#include "zneutubeimgsampling.h"
#include "zneutubelocalneuroseg.h"
#include "zneutubeneighborhood.h"
#include "zneutubeneuroseg.h"
#include "zneutubeobjlabel.h"
#include "zneutubeperceptor.h"
#include "zneutubetraceworkspace.h"
#include "zneutubetracerecord.h"
#include "zneutubetracelocseglabel.h"
#include "zneutubetraceseeder.h"
#include "zneutubetraceallseeds.h"
#include "zneutubetraceinteractive.h"
#include "zneutubetracescorethresholds.h"
#include "zneutubetraceconfig.h"
#include "zneutubetracezscale.h"
#include "zneutubetracemask.h"
#include "zneutubetracerecover.h"
#include "zneutubeimgbinarizer.h"
#include "zneutubelocsegchainmetrics.h"
#include "zneutubeneuronstructure.h"
#include "zswcops.h"
#include "zswcpostprocess.h"
#include "zswcresampler.h"
#include "zswcwriter.h"
#include "zneutubestackfitscore.h"
#include "zneutubestackfitoptions.h"
#include "zneutubetraceseed.h"
#include "zvoxelvolumedense.h"

#include "zimg.h"
#include "zimgneighborhooditerator.h"
#include "zimginfo.h"
#include "zimginit.h"
#include "zimgregionalextrema.h"
#include "zjson.h"
#include "zlog.h"
#include "zrunneutucommand.h"
#include "zrunneutucommand2.h"
#include "ztest.h"
#include "zswclayertrunkanalyzer.h"
#include "zswclayershollfeatureanalyzer.h"
#include "zswcnodebufferfeatureanalyzer.h"
#include "ztiff.h"
#include "zswctree.h"
#include "zswctreematcher.h"

namespace fs = std::filesystem;

ABSL_DECLARE_FLAG(bool, atlas_autotrace_seed_sort_commit_by_score);
ABSL_DECLARE_FLAG(bool, atlas_trace_use_swc_geometry_mask);
ABSL_DECLARE_FLAG(bool, atlas_trace_enable_legacy_isotropic_chain_canonicalization_for_parity);

namespace {

class LegacySeedSortCommitParityEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    _previousFlag = absl::GetFlag(FLAGS_atlas_autotrace_seed_sort_commit_by_score);
    absl::SetFlag(&FLAGS_atlas_autotrace_seed_sort_commit_by_score, false);
  }

  void TearDown() override
  {
    absl::SetFlag(&FLAGS_atlas_autotrace_seed_sort_commit_by_score, _previousFlag);
  }

private:
  bool _previousFlag = true;
};

[[maybe_unused]] ::testing::Environment* const kLegacySeedSortCommitParityEnvironment =
  ::testing::AddGlobalTestEnvironment(new LegacySeedSortCommitParityEnvironment());

class LegacyTraceMaskParityEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    _previousFlag = absl::GetFlag(FLAGS_atlas_trace_use_swc_geometry_mask);
    absl::SetFlag(&FLAGS_atlas_trace_use_swc_geometry_mask, false);
  }

  void TearDown() override
  {
    absl::SetFlag(&FLAGS_atlas_trace_use_swc_geometry_mask, _previousFlag);
  }

private:
  bool _previousFlag = true;
};

[[maybe_unused]] ::testing::Environment* const kLegacyTraceMaskParityEnvironment =
  ::testing::AddGlobalTestEnvironment(new LegacyTraceMaskParityEnvironment());

class LegacyIsotropicChainCanonicalizationParityEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    _previousFlag = absl::GetFlag(FLAGS_atlas_trace_enable_legacy_isotropic_chain_canonicalization_for_parity);
    absl::SetFlag(&FLAGS_atlas_trace_enable_legacy_isotropic_chain_canonicalization_for_parity, true);
  }

  void TearDown() override
  {
    absl::SetFlag(&FLAGS_atlas_trace_enable_legacy_isotropic_chain_canonicalization_for_parity, _previousFlag);
  }

private:
  bool _previousFlag = false;
};

[[maybe_unused]] ::testing::Environment* const kLegacyIsotropicChainCanonicalizationParityEnvironment =
  ::testing::AddGlobalTestEnvironment(new LegacyIsotropicChainCanonicalizationParityEnvironment());

class ScopedQtCoreApplication
{
public:
  ScopedQtCoreApplication()
  {
    if (QCoreApplication::instance() != nullptr) {
      return;
    }

    static int argc = 1;
    static char arg0[] = "zneutubecommand2paritytest";
    static char* argv[] = {arg0, nullptr};
    _app = std::make_unique<QCoreApplication>(argc, argv);
  }

private:
  std::unique_ptr<QCoreApplication> _app;
};

class ZImgInitEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    _qtApp = std::make_unique<ScopedQtCoreApplication>();
    // Some img ports use folly::getGlobalCPUExecutor(), which aborts until
    // ZImgInit completes Folly singleton registration for the process.
    std::ignore = nim::ZImgInit::instance("", "", "", false);
  }

private:
  std::unique_ptr<ScopedQtCoreApplication> _qtApp;
};

[[maybe_unused]] ::testing::Environment* const kZImgInitEnvironment =
  ::testing::AddGlobalTestEnvironment(new ZImgInitEnvironment());

class ScopedStdoutSilencer
{
public:
  ScopedStdoutSilencer()
  {
#if !defined(_WIN32)
    std::fflush(stdout);
    _saved = dup(fileno(stdout));
    if (_saved < 0) {
      return;
    }
    const int nullFd = open("/dev/null", O_WRONLY);
    if (nullFd < 0) {
      return;
    }
    (void)dup2(nullFd, fileno(stdout));
    (void)close(nullFd);
#endif
  }

  ~ScopedStdoutSilencer()
  {
#if !defined(_WIN32)
    if (_saved >= 0) {
      std::fflush(stdout);
      (void)dup2(_saved, fileno(stdout));
      (void)close(_saved);
    }
#endif
  }

  ScopedStdoutSilencer(const ScopedStdoutSilencer&) = delete;
  ScopedStdoutSilencer& operator=(const ScopedStdoutSilencer&) = delete;
  ScopedStdoutSilencer(ScopedStdoutSilencer&&) = delete;
  ScopedStdoutSilencer& operator=(ScopedStdoutSilencer&&) = delete;

private:
#if !defined(_WIN32)
  int _saved = -1;
#endif
};

void writeTextFile(const fs::path& path, std::string_view text);

struct DevOnlyAutoTraceConfigPaths
{
  fs::path commandConfig;
  fs::path skeletonizeCfg;
  fs::path traceCfg;
};

[[nodiscard]] DevOnlyAutoTraceConfigPaths writeDevOnlyAutoTraceConfigFiles(const fs::path& dir)
{
  DevOnlyAutoTraceConfigPaths out;
  out.commandConfig = dir / "command_config.json";
  out.skeletonizeCfg = dir / "skeletonize.json";
  out.traceCfg = dir / "trace_config.json";

  writeTextFile(out.commandConfig,
                R"json({
  "skeletonize": {
    "include": "skeletonize.json"
  },
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(out.skeletonizeCfg,
                R"json({
  "downsampleInterval": [0, 0, 0],
  "minimalLength": 40,
  "finalMinimalLength": 0,
  "keepingSingleObject": true,
  "rebase": true,
  "fillingHole": true,
  "maximalDistance": 100
}
)json");

  writeTextFile(out.traceCfg,
                R"json({
  "tag": "trace configuration",
  "default": {
    "minimalScoreAuto": 0.3,
    "minimalScoreManual": 0.3,
    "minimalScoreSeed": 0.35,
    "minimalScore2d": 0.5,
    "refit": false,
    "spTest": false,
    "crossoverTest": false,
    "tuneEnd": true,
    "edgePath": false,
    "enhanceMask": false,
    "seedMethod": 1,
    "recover": 1,
    "maxEucDist": 10
  },
  "level": {
    "1": {
      "seedMethod": 2,
      "recover": 0
    },
    "2": {
      "seedMethod": 2,
      "recover": 1
    },
    "3": {
      "seedMethod": 2,
      "spTest": true,
      "recover": 1
    },
    "4": {
      "seedMethod": 2,
      "spTest": true,
      "enhanceMask": true,
      "recover": 1
    },
    "5": {
      "seedMethod": 2,
      "spTest": true,
      "enhanceMask": true,
      "recover": 1
    },
    "6": {
      "seedMethod": 2,
      "spTest": true,
      "enhanceMask": true,
      "recover": 1,
      "refit": true
    }
  }
}
)json");

  return out;
}

TEST(NeutubeImagePortsParity, Bwdist3dSquaredU16_MatchesLegacyC)
{
  const size_t width = 33;
  const size_t height = 19;
  const size_t depth = 7;

  nim::ZImgInfo maskInfo(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg mask(maskInfo);
  mask.fill(0);

  std::mt19937 rng(12345);
  std::bernoulli_distribution bern(0.15);
  const size_t voxelNumber = mask.voxelNumber();
  auto* maskData = mask.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    maskData[i] = bern(rng) ? uint8_t{1} : uint8_t{0};
  }

  const nim::ZImg dist = nim::bwdistSquaredU16LegacyLike(mask, /*pad*/ 0);
  ASSERT_TRUE(dist.isType<uint16_t>()) << dist.info();
  ASSERT_EQ(dist.voxelNumber(), voxelNumber);

  Stack* maskC = Make_Stack(GREY, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(maskC, nullptr);
  std::memcpy(maskC->array, maskData, voxelNumber * sizeof(uint8_t));

  Stack* distC = Stack_Bwdist_L_U16(maskC, nullptr, 0);
  ASSERT_NE(distC, nullptr);
  ASSERT_EQ(distC->kind, GREY16);

  const auto* legacy = reinterpret_cast<const uint16_t*>(distC->array);
  const auto* ported = dist.timeData<uint16_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    ASSERT_EQ(ported[i], legacy[i]) << "i=" << i;
  }

  Kill_Stack(distC);
  Kill_Stack(maskC);
}

TEST(NeutubeImagePortsParity, StackLocalMax_MatchesLegacyC)
{
  const size_t width = 33;
  const size_t height = 19;
  const size_t depth = 7;

  nim::ZImgInfo maskInfo(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg mask(maskInfo);
  mask.fill(0);

  std::mt19937 rng(12345);
  std::bernoulli_distribution bern(0.15);
  const size_t voxelNumber = mask.voxelNumber();
  auto* maskData = mask.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    maskData[i] = bern(rng) ? uint8_t{1} : uint8_t{0};
  }

  const nim::ZImg dist = nim::bwdistSquaredU16LegacyLike(mask, /*pad*/ 0);
  ASSERT_TRUE(dist.isType<uint16_t>()) << dist.info();
  ASSERT_EQ(dist.voxelNumber(), voxelNumber);

  const nim::ZImg portedLocmax = nim::stackLocalMaxMaskLegacyLike(dist, nim::StackLocmaxOptionLegacyLike::Center);
  ASSERT_TRUE(portedLocmax.isType<uint8_t>()) << portedLocmax.info();
  ASSERT_EQ(portedLocmax.voxelNumber(), voxelNumber);

  Stack* distC = Make_Stack(GREY16, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(distC, nullptr);
  std::memcpy(distC->array, dist.timeData<uint16_t>(0), voxelNumber * sizeof(uint16_t));

  Stack* locmaxC = Stack_Local_Max(distC, nullptr, STACK_LOCMAX_CENTER);
  ASSERT_NE(locmaxC, nullptr);
  ASSERT_EQ(locmaxC->kind, GREY);

  const auto* legacy = locmaxC->array;
  const auto* ported = portedLocmax.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    ASSERT_EQ(ported[i], legacy[i]) << "i=" << i;
  }

  Kill_Stack(locmaxC);
  Kill_Stack(distC);
}

TEST(NeutubeImagePortsParity, MajorityFilterR_MatchesLegacyC)
{
  const size_t width = 21;
  const size_t height = 17;
  const size_t depth = 9;

  nim::ZImgInfo maskInfo(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg mask(maskInfo);
  mask.fill(0);

  std::mt19937 rng(12345);
  std::bernoulli_distribution bern(0.15);
  const size_t voxelNumber = mask.voxelNumber();
  auto* maskData = mask.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    maskData[i] = bern(rng) ? uint8_t{1} : uint8_t{0};
  }

  const nim::ZImg ported = nim::majorityFilterBinaryU8RLegacyLike(mask, /*connectivity*/ 26, /*mnbr*/ 4);
  ASSERT_TRUE(ported.isType<uint8_t>()) << ported.info();
  ASSERT_EQ(ported.voxelNumber(), voxelNumber);

  Stack* maskC = Make_Stack(GREY, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(maskC, nullptr);
  std::memcpy(maskC->array, maskData, voxelNumber * sizeof(uint8_t));

  Stack* outC = Stack_Majority_Filter_R(maskC, nullptr, /*conn*/ 26, /*mnbr*/ 4);
  ASSERT_NE(outC, nullptr);
  ASSERT_EQ(outC->kind, GREY);

  const auto* legacy = outC->array;
  const auto* outPorted = ported.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    ASSERT_EQ(outPorted[i], legacy[i]) << "i=" << i;
  }

  Kill_Stack(outC);
  Kill_Stack(maskC);
}

TEST(NeutubeImagePortsParity, ExtractSeedOriginal_MatchesLegacyC)
{
  const size_t width = 33;
  const size_t height = 19;
  const size_t depth = 11;

  nim::ZImgInfo maskInfo(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg mask(maskInfo);
  mask.fill(0);

  std::mt19937 rng(12345);
  std::bernoulli_distribution bern(0.12);
  const size_t voxelNumber = mask.voxelNumber();
  auto* maskData = mask.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    maskData[i] = bern(rng) ? uint8_t{1} : uint8_t{0};
  }

  const nim::Geo3dScalarField ported = nim::extractSeedOriginalLegacyLike(mask);

  Stack* maskC = Make_Stack(GREY, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(maskC, nullptr);
  std::memcpy(maskC->array, maskData, voxelNumber * sizeof(uint8_t));

  Stack* distC = Stack_Bwdist_L_U16(maskC, nullptr, 0);
  ASSERT_NE(distC, nullptr);
  ASSERT_EQ(distC->kind, GREY16);

  Stack* seedsC = Stack_Local_Max(distC, nullptr, STACK_LOCMAX_CENTER);
  ASSERT_NE(seedsC, nullptr);
  ASSERT_EQ(seedsC->kind, GREY);

  Voxel_List* list = Stack_To_Voxel_List(seedsC);
  Pixel_Array* pa = Voxel_List_Sampling(distC, list);
  ASSERT_NE(pa, nullptr);

  Voxel_P* voxel_array = Voxel_List_To_Array(list, 1, nullptr, nullptr);
  ASSERT_NE(voxel_array, nullptr);

  const uint16_t* pa_array = reinterpret_cast<const uint16_t*>(pa->array);

  std::vector<std::array<double, 3>> legacyPoints;
  std::vector<double> legacyValues;
  legacyPoints.reserve(static_cast<size_t>(pa->size));
  legacyValues.reserve(static_cast<size_t>(pa->size));

  for (int i = 0; i < pa->size; ++i) {
    const int x = voxel_array[i]->x;
    const int y = voxel_array[i]->y;
    const int z = voxel_array[i]->z;
    if (IS_IN_OPEN_RANGE3(x, y, z, 0, seedsC->width - 1, 0, seedsC->height - 1, 0, seedsC->depth - 1)) {
      legacyPoints.push_back({static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)});
      legacyValues.push_back(std::sqrt(static_cast<double>(pa_array[i])));
    }
  }

  EXPECT_EQ(ported.points, legacyPoints);
  ASSERT_EQ(ported.values.size(), legacyValues.size());
  for (size_t i = 0; i < legacyValues.size(); ++i) {
    EXPECT_DOUBLE_EQ(ported.values[i], legacyValues[i]) << "i=" << i;
  }

  Kill_Voxel_List(list);
  Kill_Pixel_Array(pa);
  free(voxel_array);
  Kill_Stack(seedsC);
  Kill_Stack(distC);
  Kill_Stack(maskC);
}

TEST(NeutubeTracePortsParity, LocalNeurosegLabelG_MatchesLegacyC)
{
  const size_t width = 64;
  const size_t height = 64;
  const size_t depth = 32;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg ported(info);
  ported.fill(0);

  nim::LocalNeuroseg seg;
  seg.seg.r1 = 3.0;
  seg.seg.c = 0.0;
  seg.seg.h = 11.0;
  seg.seg.theta = 0.0;
  seg.seg.psi = 0.0;
  seg.seg.curvature = 0.0;
  seg.seg.alpha = 0.0;
  seg.seg.scale = 1.0;

  const std::array<double, 3> pos = {30.0, 31.0, 15.0};
  nim::setNeurosegPositionLegacyLike(seg, pos, nim::NeuroposReferenceLegacyLike::Center);

  nim::localNeurosegLabelGLegacyLike(seg, ported, /*flag*/ -1, /*value*/ 2, /*zScale*/ 1.0);

  Stack* legacy = Make_Stack(GREY, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(legacy, nullptr);
  Zero_Stack(legacy);

  Local_Neuroseg segC;
  Set_Neuroseg(&(segC.seg), 3.0, 0.0, NEUROSEG_DEFAULT_H, 0.0, 0.0, 0.0, 0.0, 1.0);
  double cpos[3] = {pos[0], pos[1], pos[2]};
  Set_Neuroseg_Position(&segC, cpos, NEUROSEG_CENTER);

  Local_Neuroseg_Label_G(&segC, legacy, /*flag*/ -1, /*value*/ 2, /*zScale*/ 1.0);

  const auto* portedData = ported.timeData<uint8_t>(0);
  for (size_t i = 0; i < ported.voxelNumber(); ++i) {
    ASSERT_EQ(portedData[i], legacy->array[i]) << "i=" << i;
  }

  Kill_Stack(legacy);
}

TEST(NeutubeTracePortsParity, LocalNeurosegLabelG_AnisotropicMatchesLegacyC)
{
  constexpr size_t width = 64;
  constexpr size_t height = 64;
  constexpr size_t depth = 32;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg ported(info);
  ported.fill(0);

  nim::LocalNeuroseg seg;
  seg.seg.r1 = 3.0;
  seg.seg.c = 0.0;
  seg.seg.h = 11.0;
  seg.seg.theta = 0.4;
  seg.seg.psi = 1.1;
  seg.seg.curvature = 0.0;
  seg.seg.alpha = 0.0;
  seg.seg.scale = 1.0;

  const std::array<double, 3> posTrace = {30.0, 31.0, 3.2};
  nim::setNeurosegPositionLegacyLike(seg, posTrace, nim::NeuroposReferenceLegacyLike::Center);

  nim::localNeurosegLabelGLegacyLike(seg, ported, /*flag*/ -1, /*value*/ 2, zToXYRatio);

  Stack* legacy = Make_Stack(GREY, static_cast<int>(width), static_cast<int>(height), static_cast<int>(depth));
  ASSERT_NE(legacy, nullptr);
  Zero_Stack(legacy);

  Local_Neuroseg segC;
  Set_Neuroseg(&(segC.seg), 3.0, 0.0, NEUROSEG_DEFAULT_H, 0.4, 1.1, 0.0, 0.0, 1.0);
  double cpos[3] = {posTrace[0], posTrace[1], posTrace[2]};
  Set_Neuroseg_Position(&segC, cpos, NEUROSEG_CENTER);

  Local_Neuroseg_Label_G(&segC, legacy, /*flag*/ -1, /*value*/ 2, legacyZScale);

  const auto* portedData = ported.timeData<uint8_t>(0);
  for (size_t i = 0; i < ported.voxelNumber(); ++i) {
    ASSERT_EQ(portedData[i], legacy->array[i]) << "i=" << i;
  }

  Kill_Stack(legacy);
}

TEST(NeutubeLegacyLocalNeuroseg, HitMaskMatchesGuiSweepIsotropic)
{
  constexpr int width = 40;
  constexpr int height = 40;
  constexpr int depth = 16;

  nim::LocalNeuroseg segCpp;
  segCpp.seg.r1 = 2.8;
  segCpp.seg.c = 0.0;
  segCpp.seg.h = 11.0;
  segCpp.seg.theta = 0.55;
  segCpp.seg.psi = 0.95;
  segCpp.seg.curvature = 0.0;
  segCpp.seg.alpha = 0.0;
  segCpp.seg.scale = 1.05;
  nim::setNeurosegPositionLegacyLike(segCpp, {20.0, 20.0, 8.0}, nim::NeuroposReferenceLegacyLike::Center);

  Local_Neuroseg segC;
  Set_Neuroseg(&(segC.seg), 2.8, 0.0, 11.0, 0.55, 0.95, 0.0, 0.0, 1.05);
  double cpos[3] = {20.0, 20.0, 8.0};
  Set_Neuroseg_Position(&segC, cpos, NEUROSEG_CENTER);
  ZLocalNeuroseg guiSeg(&segC, false);

  Stack* legacyMask = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(legacyMask, nullptr);
  Zero_Stack(legacyMask);

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  nim::ZImg portedMask(info);
  portedMask.fill(0);

  auto* portedData = portedMask.timeData<uint8_t>(0);
  const size_t plane = static_cast<size_t>(width) * static_cast<size_t>(height);
  size_t guiHitCount = 0;
  size_t portedHitCount = 0;
  std::optional<std::array<int, 3>> firstMismatch;
  bool firstGuiHit = false;
  bool firstPortedHit = false;
  std::vector<std::array<int, 3>> guiHitCoords;
  std::vector<std::array<int, 3>> portedHitCoords;
  auto coordsToString = [](const std::vector<std::array<int, 3>>& coords) {
    std::string out;
    for (size_t i = 0; i < coords.size(); ++i) {
      if (!out.empty()) {
        out += ", ";
      }
      out += fmt::format("({}, {}, {})", coords[i][0], coords[i][1], coords[i][2]);
    }
    return out;
  };

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t idx =
          static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(z) * plane;
        legacyMask->array[idx] = 1;
        portedData[idx] = 1;

        const bool guiHit = guiSeg.hitMask(legacyMask);
        const bool portedHit = nim::localNeurosegHitMaskLegacyLike(segCpp, portedMask, /*zToXYRatio*/ 1.0);
        guiHitCount += static_cast<size_t>(guiHit);
        portedHitCount += static_cast<size_t>(portedHit);
        if (guiHit) {
          guiHitCoords.push_back({x, y, z});
        }
        if (portedHit) {
          portedHitCoords.push_back({x, y, z});
        }
        if (!firstMismatch.has_value() && portedHit != guiHit) {
          firstMismatch = std::array<int, 3>{x, y, z};
          firstGuiHit = guiHit;
          firstPortedHit = portedHit;
        }

        legacyMask->array[idx] = 0;
        portedData[idx] = 0;
      }
    }
  }

  ASSERT_FALSE(firstMismatch.has_value())
    << fmt::format("single-voxel mismatch at ({}, {}, {}): guiHit={} portedHit={} "
                   "guiHitCount={} portedHitCount={} guiHits={} portedHits={}",
                   (*firstMismatch)[0],
                   (*firstMismatch)[1],
                   (*firstMismatch)[2],
                   firstGuiHit,
                   firstPortedHit,
                   guiHitCount,
                   portedHitCount,
                   coordsToString(guiHitCoords),
                   coordsToString(portedHitCoords));

  Kill_Stack(legacyMask);
}

TEST(NeutubeLegacyLocalNeuroseg, HitMaskHitsOwnLabeledMaskAnisotropic)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 24;
  constexpr double zToXYRatio = 5.0;

  nim::LocalNeuroseg seg;
  seg.seg.r1 = 3.0;
  seg.seg.c = 0.0;
  seg.seg.h = nim::NeurosegDefaultHLegacyLike;
  seg.seg.theta = 0.4;
  seg.seg.psi = 0.7;
  seg.seg.curvature = 0.0;
  seg.seg.alpha = 0.2;
  seg.seg.scale = 1.1;
  nim::setNeurosegPositionLegacyLike(seg, {24.0, 28.0, 8.0 * zToXYRatio}, nim::NeuroposReferenceLegacyLike::Center);

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  nim::ZImg mask(info);
  mask.fill(0);

  EXPECT_FALSE(nim::localNeurosegHitMaskLegacyLike(seg, mask, zToXYRatio));

  nim::localNeurosegLabelGLegacyLike(seg, mask, /*flag*/ -1, /*value*/ 2, zToXYRatio);
  EXPECT_TRUE(nim::localNeurosegHitMaskLegacyLike(seg, mask, zToXYRatio));

  nim::LocalNeuroseg farSeg = seg;
  nim::setNeurosegPositionLegacyLike(farSeg, {48.0, 52.0, 8.0 * zToXYRatio}, nim::NeuroposReferenceLegacyLike::Center);
  EXPECT_FALSE(nim::localNeurosegHitMaskLegacyLike(farSeg, mask, zToXYRatio));
}

TEST(NeutubeTracePortsParity, LocalNeurosegFieldS_MatchesLegacyC)
{
  constexpr double zToXYRatio = 5.0;

  nim::LocalNeuroseg segCpp;
  segCpp.seg.r1 = 2.0;
  segCpp.seg.c = 0.0;
  segCpp.seg.h = 11.0;
  segCpp.seg.theta = 0.7;
  segCpp.seg.psi = 1.1;
  segCpp.seg.curvature = 0.0;
  segCpp.seg.alpha = 0.0;
  segCpp.seg.scale = 1.0;
  segCpp.pos = {24.0, 24.0, 24.0 / zToXYRatio};

  Local_Neuroseg segC;
  Set_Local_Neuroseg(&segC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, 24.0, 24.0, 24.0 / zToXYRatio);

  Geo3d_Scalar_Field* fieldC = Local_Neuroseg_Field_S(&segC, nullptr, nullptr);
  ASSERT_NE(fieldC, nullptr);

  const nim::Geo3dScalarField fieldCpp = nim::localNeurosegFieldSLegacyLike(segCpp, nullptr);
  ASSERT_EQ(fieldCpp.points.size(), static_cast<size_t>(fieldC->size));
  ASSERT_EQ(fieldCpp.values.size(), fieldCpp.points.size());

  for (size_t i = 0; i < fieldCpp.points.size(); ++i) {
    EXPECT_DOUBLE_EQ(fieldCpp.points[i][0], fieldC->points[i][0]) << "i=" << i;
    EXPECT_DOUBLE_EQ(fieldCpp.points[i][1], fieldC->points[i][1]) << "i=" << i;
    EXPECT_DOUBLE_EQ(fieldCpp.points[i][2], fieldC->points[i][2]) << "i=" << i;
    EXPECT_DOUBLE_EQ(fieldCpp.values[i], fieldC->values[i]) << "i=" << i;
  }

  Kill_Geo3d_Scalar_Field(fieldC);
}

class ArgvBuilder
{
public:
  explicit ArgvBuilder(std::vector<std::string> args)
    : _args(std::move(args))
  {
    _argv.reserve(_args.size());
    for (auto& s : _args) {
      _argv.push_back(s.data());
    }
  }

  [[nodiscard]] int argc() const
  {
    return static_cast<int>(_argv.size());
  }

  [[nodiscard]] char** argv()
  {
    return _argv.data();
  }

private:
  std::vector<std::string> _args;
  std::vector<char*> _argv;
};

[[nodiscard]] std::string readTextFile(const fs::path& path)
{
  return nim::readFileIntoString(nim::filesystemPathToQString(path));
}

void writeTextFile(const fs::path& path, std::string_view text)
{
  std::ofstream out =
    nim::openOFStream(nim::filesystemPathToQString(path), std::ios::out | std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] fs::path makeUniqueTempDir()
{
  const fs::path base = fs::temp_directory_path() / "atlas_neutube_ab";
  std::error_code ec;
  fs::create_directories(base, ec);
  CHECK(!ec);

  const auto runTag = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 1000; ++attempt) {
    ec.clear();
    const auto suffix = fmt::format("run_{}_{}", runTag, attempt);
    fs::path dir = base / suffix;
    if (fs::create_directory(dir, ec)) {
      return dir;
    }
  }

  CHECK(false) << "Failed to create a unique temp directory under " << base.string();
  return {};
}

void writeSimpleLineTiff(const fs::path& path, size_t w, size_t h, size_t d, uint8_t value)
{
  nim::ZImgInfo info(w, h, d, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  const size_t x = w / 2;
  const size_t y = h / 2;
  for (size_t z = 0; z < d; ++z) {
    *img.data<uint8_t>(x, y, z) = value;
  }

  // The legacy neurolabi TIFF reader used by ZRunNeuTuCommand is not robust
  // against the tiled/LZW files emitted by ZImg::save; use its classic layout.
  nim::ZTiffWriter writer;
  writer.startWriting(nim::filesystemPathToQString(path), nim::Compression::NONE, -1, false);
  for (size_t z = 0; z < d; ++z) {
    writer.writeIFD(img, z, 0, -1, false);
  }
  writer.finishWriting();
}

[[nodiscard]] size_t firstDiffIndex(std::string_view a, std::string_view b)
{
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return i;
    }
  }
  return n;
}

[[nodiscard]] std::pair<size_t, size_t> lineColFromIndex(std::string_view text, size_t index)
{
  size_t line = 1;
  size_t col = 1;
  const size_t n = std::min(index, text.size());
  for (size_t i = 0; i < n; ++i) {
    if (text[i] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
  return {line, col};
}

[[nodiscard]] std::string lineAt(std::string_view text, size_t index)
{
  const size_t n = text.size();
  const size_t i = std::min(index, n);

  size_t begin = i;
  while (begin > 0 && text[begin - 1] != '\n') {
    --begin;
  }

  size_t end = i;
  while (end < n && text[end] != '\n') {
    ++end;
  }

  return std::string(text.substr(begin, end - begin));
}

struct VolumeDigest
{
  size_t width = 0;
  size_t height = 0;
  size_t depth = 0;
  size_t voxelNumber = 0;
  size_t bytesPerVoxel = 0;
  uint64_t hash = 0;
  uint64_t nonzero = 0;
  uint64_t minValue = 0;
  uint64_t maxValue = 0;
};

template<typename T>
[[nodiscard]] VolumeDigest digestVolume(const T* data, size_t w, size_t h, size_t d)
{
  CHECK(data != nullptr);

  VolumeDigest out;
  out.width = w;
  out.height = h;
  out.depth = d;
  out.voxelNumber = w * h * d;
  out.bytesPerVoxel = sizeof(T);

  constexpr uint64_t FnvOffset = 14695981039346656037ull;
  constexpr uint64_t FnvPrime = 1099511628211ull;

  uint64_t h64 = FnvOffset;
  uint64_t minV = std::numeric_limits<uint64_t>::max();
  uint64_t maxV = 0;
  uint64_t nonzero = 0;

  for (size_t i = 0; i < out.voxelNumber; ++i) {
    const uint64_t v = static_cast<uint64_t>(data[i]);
    if (v != 0) {
      ++nonzero;
    }
    minV = std::min(minV, v);
    maxV = std::max(maxV, v);
    h64 ^= v + 0x9e3779b97f4a7c15ull + (h64 << 6) + (h64 >> 2);
    h64 *= FnvPrime;
  }

  out.hash = h64;
  out.nonzero = nonzero;
  out.minValue = (out.voxelNumber == 0) ? 0 : minV;
  out.maxValue = maxV;
  return out;
}

[[nodiscard]] VolumeDigest digestZImgU8OrU16(const nim::ZImg& img)
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  if (img.isType<uint8_t>()) {
    return digestVolume(img.timeData<uint8_t>(0), img.width(), img.height(), img.depth());
  }
  if (img.isType<uint16_t>()) {
    return digestVolume(img.timeData<uint16_t>(0), img.width(), img.height(), img.depth());
  }

  CHECK(false) << "digestZImgU8OrU16: unsupported type: " << img.info();
  return {};
}

[[nodiscard]] VolumeDigest digestLegacyStackU8OrU16(const Stack* stack)
{
  CHECK(stack != nullptr);

  const size_t w = static_cast<size_t>(stack->width);
  const size_t h = static_cast<size_t>(stack->height);
  const size_t d = static_cast<size_t>(stack->depth);

  if (stack->kind == GREY) {
    return digestVolume(stack->array, w, h, d);
  }
  if (stack->kind == GREY16) {
    return digestVolume(reinterpret_cast<const uint16_t*>(stack->array), w, h, d);
  }

  CHECK(false) << "digestLegacyStackU8OrU16: unsupported kind=" << stack->kind;
  return {};
}

struct SwcDigest
{
  size_t nodes = 0;
  uint64_t hash = 0;
};

[[nodiscard]] SwcDigest digestSwc(const nim::ZSwc& swc)
{
  constexpr uint64_t FnvOffset = 14695981039346656037ull;
  constexpr uint64_t FnvPrime = 1099511628211ull;

  uint64_t h64 = FnvOffset;
  size_t nodes = 0;

  auto mixU64 = [&](uint64_t v) {
    h64 ^= v + 0x9e3779b97f4a7c15ull + (h64 << 6) + (h64 >> 2);
    h64 *= FnvPrime;
  };

  auto mixI64 = [&](int64_t v) {
    mixU64(static_cast<uint64_t>(v));
  };

  auto mixDouble = [&](double v) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    mixU64(bits);
  };

  for (auto it = swc.begin(); it != swc.end(); ++it) {
    ++nodes;
    mixI64(it->id);
    mixI64(it->type);
    mixDouble(it->x);
    mixDouble(it->y);
    mixDouble(it->z);
    mixDouble(it->radius);
    mixI64(nim::ZSwc::parentID(it));
  }

  return {.nodes = nodes, .hash = h64};
}

template<typename T>
[[nodiscard]] std::optional<std::tuple<size_t, size_t, size_t, uint64_t, uint64_t>>
firstMismatch(const T* a, const T* b, size_t w, size_t h, size_t d)
{
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  const size_t voxelNumber = w * h * d;
  const size_t plane = w * h;

  for (size_t idx = 0; idx < voxelNumber; ++idx) {
    if (a[idx] == b[idx]) {
      continue;
    }
    const size_t z = idx / plane;
    const size_t rem = idx - z * plane;
    const size_t y = rem / w;
    const size_t x = rem - y * w;
    return std::make_tuple(x, y, z, static_cast<uint64_t>(a[idx]), static_cast<uint64_t>(b[idx]));
  }
  return std::nullopt;
}

struct IntSeed
{
  int x = 0;
  int y = 0;
  int z = 0;
  double value = 0.0;
};

[[nodiscard]] std::vector<IntSeed> toIntSeeds(const Geo3d_Scalar_Field* field)
{
  std::vector<IntSeed> out;
  if (field == nullptr) {
    return out;
  }
  out.reserve(static_cast<size_t>(field->size));
  for (int i = 0; i < field->size; ++i) {
    IntSeed s;
    s.x = static_cast<int>(field->points[i][0]);
    s.y = static_cast<int>(field->points[i][1]);
    s.z = static_cast<int>(field->points[i][2]);
    s.value = field->values[i];
    out.push_back(s);
  }
  return out;
}

[[nodiscard]] std::vector<IntSeed> toIntSeeds(const nim::Geo3dScalarField& field)
{
  std::vector<IntSeed> out;
  out.reserve(field.size());
  for (size_t i = 0; i < field.size(); ++i) {
    IntSeed s;
    s.x = static_cast<int>(field.points[i][0]);
    s.y = static_cast<int>(field.points[i][1]);
    s.z = static_cast<int>(field.points[i][2]);
    s.value = field.values[i];
    out.push_back(s);
  }
  return out;
}

[[nodiscard]] Geo3d_Scalar_Field* extractSeedOriginalLegacyC(const Stack* mask)
{
  CHECK(mask != nullptr);
  CHECK(mask->kind == GREY) << "Legacy seed extraction expects GREY mask, got kind=" << mask->kind;

  Stack* dist = Stack_Bwdist_L_U16(mask, nullptr, /*pad*/ 0);
  CHECK(dist != nullptr);

  Stack* seeds = Stack_Local_Max(dist, nullptr, STACK_LOCMAX_CENTER);
  CHECK(seeds != nullptr);

  Voxel_List* list = Stack_To_Voxel_List(seeds);
  CHECK(list != nullptr);

  Pixel_Array* pa = Voxel_List_Sampling(dist, list);
  CHECK(pa != nullptr);

  Kill_Stack(dist);
  dist = nullptr;

  Voxel_P* voxelArray = Voxel_List_To_Array(list, 1, nullptr, nullptr);
  CHECK(voxelArray != nullptr);

  const auto* paArray = reinterpret_cast<const uint16_t*>(pa->array);

  Geo3d_Scalar_Field* field = Make_Geo3d_Scalar_Field(pa->size);
  CHECK(field != nullptr);
  field->size = 0;

  for (int i = 0; i < pa->size; ++i) {
    if (IS_IN_OPEN_RANGE3(voxelArray[i]->x,
                          voxelArray[i]->y,
                          voxelArray[i]->z,
                          0,
                          seeds->width - 1,
                          0,
                          seeds->height - 1,
                          0,
                          seeds->depth - 1)) {
      field->points[field->size][0] = voxelArray[i]->x;
      field->points[field->size][1] = voxelArray[i]->y;
      field->points[field->size][2] = voxelArray[i]->z;
      field->values[field->size] = std::sqrt(static_cast<double>(paArray[i]));
      field->size++;
    }
  }

  Kill_Voxel_List(list);
  Kill_Pixel_Array(pa);
  free(voxelArray);
  Kill_Stack(seeds);

  return field;
}

struct LegacyRemoveNoisySeedResult
{
  Geo3d_Scalar_Field* seeds = nullptr;
  Stack* mask = nullptr;
  double seedDensity = 0.0;
  int minSeedSize = 0;
};

[[nodiscard]] LegacyRemoveNoisySeedResult
removeNoisySeedLegacyC(Geo3d_Scalar_Field* seeds, Stack* mask, int seedMethod, bool screeningSeed)
{
  LegacyRemoveNoisySeedResult out;
  out.seeds = seeds;
  out.mask = mask;

  if (mask == nullptr || seeds == nullptr) {
    return out;
  }

  const double voxelNumber = static_cast<double>(static_cast<size_t>(mask->width) * mask->height * mask->depth);
  const double seedDensity = (voxelNumber == 0.0) ? 0.0 : static_cast<double>(seeds->size) / voxelNumber;
  const int minSeedSize = screeningSeed ? static_cast<int>(seedDensity * 16000.0) : 0;

  out.seedDensity = seedDensity;
  out.minSeedSize = minSeedSize;

  if (minSeedSize <= 0) {
    return out;
  }

  Stack* tmp = Copy_Stack(mask);
  CHECK(tmp != nullptr);
  mask = Stack_Remove_Small_Object(tmp, mask, minSeedSize, 26);
  Kill_Stack(tmp);

  if (mask->kind != GREY) {
    C_Stack::translate(mask, GREY, 1);
  }

  Kill_Geo3d_Scalar_Field(seeds);
  seeds = nullptr;

  if (seedMethod == 1) {
    seeds = extractSeedOriginalLegacyC(mask);
  } else {
    CHECK(false) << "removeNoisySeedLegacyC: seedMethod=" << seedMethod << " not supported in this diagnostic helper.";
  }

  out.seeds = seeds;
  out.mask = mask;
  return out;
}

struct LegacySeedSortResult
{
  std::vector<Local_Neuroseg> locsegArray;
  std::vector<double> scoreArray;
  Stack* baseMask = nullptr;
};

[[nodiscard]] bool legacyGuiHitMaskWithZScale(const Local_Neuroseg& locseg, const Stack* mask, double legacyZScale)
{
  CHECK(mask != nullptr);
  CHECK(std::isfinite(legacyZScale));
  CHECK(legacyZScale > 0.0);

  ZLocalNeuroseg seg(const_cast<Local_Neuroseg*>(&locseg), false);
  const ZPointArray points = seg.sample(1.0, 1.0);
  for (const ZPoint& pt : points) {
    if (C_Stack::value(mask, iround(pt.x()), iround(pt.y()), iround(pt.z() * legacyZScale)) > 0.0) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] LegacySeedSortResult sortSeedsLegacyGuiLikeWithZScale(const Geo3d_Scalar_Field* seeds,
                                                                    const Stack* signal,
                                                                    double legacyZScale,
                                                                    double minScore,
                                                                    const Stack* traceMask)
{
  CHECK(seeds != nullptr);
  CHECK(signal != nullptr);
  CHECK(std::isfinite(legacyZScale));
  CHECK(legacyZScale > 0.0);

  // Match the ZNeuronTraceSeeder::sortSeed() stage used by zrunneutucommand, but
  // parameterize the stack-facing calls by `legacyZScale` so the same stage can
  // be compared under anisotropic tracing as well.

  LegacySeedSortResult out;
  out.locsegArray.resize(static_cast<size_t>(seeds->size));
  out.scoreArray.assign(static_cast<size_t>(seeds->size), 0.0);
  out.baseMask = C_Stack::make(GREY, signal->width, signal->height, signal->depth);
  CHECK(out.baseMask != nullptr);
  Zero_Stack(out.baseMask);

  Locseg_Fit_Workspace* fws = New_Locseg_Fit_Workspace();
  CHECK(fws != nullptr);
  fws->sws->fs.n = 2;
  fws->sws->fs.options[0] = STACK_FIT_DOT;
  fws->sws->fs.options[1] = STACK_FIT_CORRCOEF;
  fws->pos_adjust = 1;

  const int width = signal->width;
  const int height = signal->height;
  const int depth = signal->depth;

  for (int i = 0; i < seeds->size; ++i) {
    const int x = static_cast<int>(seeds->points[i][0]);
    const int y = static_cast<int>(seeds->points[i][1]);
    const int z = static_cast<int>(seeds->points[i][2]);

    if (traceMask != nullptr && C_Stack::value(traceMask, x, y, z) > 0.0) {
      out.scoreArray[static_cast<size_t>(i)] = 0.0;
      continue;
    }

    double widthValue = seeds->values[i];
    if (widthValue < 3.0) {
      widthValue += 0.5;
    }

    Set_Neuroseg(&(out.locsegArray[static_cast<size_t>(i)].seg),
                 widthValue,
                 0.0,
                 NEUROSEG_DEFAULT_H,
                 0.0,
                 0.0,
                 0.0,
                 0.0,
                 1.0);

    double cpos[3];
    cpos[0] = x;
    cpos[1] = y;
    cpos[2] = z;
    cpos[2] /= legacyZScale;
    Set_Neuroseg_Position(&(out.locsegArray[static_cast<size_t>(i)]), cpos, NEUROSEG_CENTER);

    const int seedOffset = C_Stack::offset(x, y, z, width, height, depth);
    if (out.baseMask->array[seedOffset] > 0) {
      out.scoreArray[static_cast<size_t>(i)] = 0.0;
      continue;
    }

    Local_Neuroseg_Optimize_W(&(out.locsegArray[static_cast<size_t>(i)]), signal, legacyZScale, 0, fws);

    if (traceMask != nullptr &&
        legacyGuiHitMaskWithZScale(out.locsegArray[static_cast<size_t>(i)], traceMask, legacyZScale)) {
      out.scoreArray[static_cast<size_t>(i)] = 0.0;
      continue;
    }

    out.scoreArray[static_cast<size_t>(i)] = fws->sws->fs.scores[1];

    const int labelValue = (out.scoreArray[static_cast<size_t>(i)] > minScore) ? 2 : 1;
    Local_Neuroseg_Label_G(&(out.locsegArray[static_cast<size_t>(i)]), out.baseMask, -1, labelValue, legacyZScale);
  }

  Kill_Locseg_Fit_Workspace(fws);
  return out;
}

[[nodiscard]] std::vector<uint32_t> toU32Labels(const nim::ZImg& img)
{
  const size_t voxelNumber = img.voxelNumber();
  std::vector<uint32_t> out;
  out.resize(voxelNumber);

  if (img.isType<uint8_t>()) {
    const auto* p = img.timeData<uint8_t>(0);
    for (size_t i = 0; i < voxelNumber; ++i) {
      out[i] = p[i];
    }
    return out;
  }

  if (img.isType<uint16_t>()) {
    const auto* p = img.timeData<uint16_t>(0);
    for (size_t i = 0; i < voxelNumber; ++i) {
      out[i] = p[i];
    }
    return out;
  }

  CHECK(false) << "Unsupported label image type: " << img.info();
  return out;
}

struct LegacyCLabeledStack
{
  std::vector<uint32_t> labels;
  int kind = 0;
  int numLargeObjects = 0;
};

[[nodiscard]] LegacyCLabeledStack labelLargeObjectsLegacyC(const nim::ZImg& img,
                                                           const nim::LabelLargeObjectsParams& params)
{
  using namespace nim;

  CHECK(img.isType<uint8_t>()) << "Test helper currently expects uint8 input, got: " << img.info();
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const int width = static_cast<int>(img.width());
  const int height = static_cast<int>(img.height());
  const int depth = static_cast<int>(img.depth());

  Stack* stack = Make_Stack(GREY, width, height, depth);
  CHECK(stack != nullptr);

  const size_t voxelNumber = img.voxelNumber();
  std::memcpy(stack->array, img.timeData<uint8_t>(0), voxelNumber);

  const int numLargeObjects =
    Stack_Label_Large_Objects_N(stack, nullptr, params.flag, params.smallLabel, params.minSize, params.connectivity);

  LegacyCLabeledStack res;
  res.kind = stack->kind;
  res.numLargeObjects = numLargeObjects;
  res.labels.resize(voxelNumber);

  if (stack->kind == GREY) {
    for (size_t i = 0; i < voxelNumber; ++i) {
      res.labels[i] = stack->array[i];
    }
  } else if (stack->kind == GREY16) {
    const auto* a16 = reinterpret_cast<const uint16_t*>(stack->array);
    for (size_t i = 0; i < voxelNumber; ++i) {
      res.labels[i] = a16[i];
    }
  } else {
    CHECK(false) << "Unexpected legacy output kind: " << stack->kind;
  }

  Kill_Stack(stack);

  return res;
}

[[nodiscard]] double compareSwcLikeLegacy(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher)
{
  double score = 0.0;

  if (tree1 != nullptr && tree2 != nullptr) {
    constexpr double SampleStep = 200.0;
    constexpr int MatchingLevel = 2;

    std::unique_ptr<ZSwcTree> tree1ForMatch(tree1->clone());
    tree1ForMatch->resample(SampleStep);

    std::unique_ptr<ZSwcTree> tree2ForMatch(tree2->clone());
    tree2ForMatch->resample(SampleStep);

    matcher.matchAllG(*tree1ForMatch, *tree2ForMatch, MatchingLevel);
    score = matcher.matchingScore();
  }

  return score;
}

[[nodiscard]] std::string legacyCompareSwcPairs(const std::vector<std::string>& inputPaths, double scale)
{
  std::vector<std::unique_ptr<ZSwcTree>> treeArray;
  treeArray.reserve(inputPaths.size());

  for (const auto& p : inputPaths) {
    auto tree = std::make_unique<ZSwcTree>();
    tree->load(p);
    if (scale != 1.0) {
      tree->rescale(scale, scale, scale);
    }
    treeArray.push_back(std::move(tree));
  }

  ZSwcTreeMatcher matcher;
  ZSwcLayerTrunkAnalyzer trunkAnalyzer;
  trunkAnalyzer.setStep(200.0);

  ZSwcLayerShollFeatureAnalyzer helperAnalyzer;
  helperAnalyzer.setLayerScale(4000.0);
  helperAnalyzer.setLayerMargin(100.0);

  ZSwcNodeBufferFeatureAnalyzer analyzer;
  analyzer.setHelper(&helperAnalyzer);

  matcher.setTrunkAnalyzer(&trunkAnalyzer);
  matcher.setFeatureAnalyzer(&analyzer);

  std::vector<double> selfScore(treeArray.size(), 0.0);
  for (size_t i = 0; i < treeArray.size(); ++i) {
    selfScore[i] = compareSwcLikeLegacy(treeArray[i].get(), treeArray[i].get(), matcher);
  }

  fmt::memory_buffer buffer;
  for (size_t i = 0; i < treeArray.size(); ++i) {
    for (size_t j = i + 1; j < treeArray.size(); ++j) {
      double score = compareSwcLikeLegacy(treeArray[i].get(), treeArray[j].get(), matcher);
      score /= std::max(selfScore[i], selfScore[j]);
      fmt::format_to(std::back_inserter(buffer), "{}-{}: {:g}\n", i, j, score);
    }
  }

  return fmt::to_string(buffer);
}

} // namespace

TEST(NeutubeLegacyNeighborhood, OffsetsMatchLegacyTables)
{
  const int connectivities[] = {4, 8, 6, 10, 18, 26};
  for (const int conn : connectivities) {
    const nim::ZNeighborhood& nb = nim::neighborhoodLegacyOrder(conn);
    ASSERT_EQ(nb.size(), static_cast<size_t>(conn)) << "conn=" << conn;

    const int* xOff = Stack_Neighbor_X_Offset(conn);
    const int* yOff = Stack_Neighbor_Y_Offset(conn);
    const int* zOff = Stack_Neighbor_Z_Offset(conn);
    ASSERT_NE(xOff, nullptr);
    ASSERT_NE(yOff, nullptr);
    ASSERT_NE(zOff, nullptr);

    for (int i = 0; i < conn; ++i) {
      const auto& off = nb.offset(static_cast<size_t>(i));
      EXPECT_EQ(off.x, static_cast<nim::index_t>(xOff[i])) << "conn=" << conn << " i=" << i;
      EXPECT_EQ(off.y, static_cast<nim::index_t>(yOff[i])) << "conn=" << conn << " i=" << i;
      EXPECT_EQ(off.z, static_cast<nim::index_t>(zOff[i])) << "conn=" << conn << " i=" << i;
    }
  }
}

TEST(NeutubeLegacyNeighborhood, WorksWithZImgNeighborhoodIterators)
{
  using namespace nim;

  ZImgInfo info(3, 3, 3);
  info.bytesPerVoxel = 1;
  ZImg img(info);

  ZImgRegion region(0, 3, 0, 3, 0, 3);
  region.resolveRegionEnd(info);

  ZImgNeighborhoodConstIterator<uint8_t> it(nim::neighborhoodLegacyOrder(6), img, region);
  ASSERT_TRUE(it.isAtBegin());
  ASSERT_EQ(ZVoxelCoordinate(0, 0, 0), it.coord());

  // Legacy 6-neighborhood ordering is:
  //  (-1,0,0),(1,0,0),(0,-1,0),(0,1,0),(0,0,-1),(0,0,1)
  EXPECT_FALSE(it.isInBound(0));
  EXPECT_TRUE(it.isInBound(1));
  EXPECT_FALSE(it.isInBound(2));
  EXPECT_TRUE(it.isInBound(3));
  EXPECT_FALSE(it.isInBound(4));
  EXPECT_TRUE(it.isInBound(5));
}

TEST(NeutubeObjLabel, LabelsMatchLegacy_SmallExample)
{
  using namespace nim;

  ZImgInfo info(16, 16, 8, 1, 1, 1, VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();

  ZImg img(info);
  img.fill(0);

  // Object A: small (size 3) near origin.
  *img.data<uint8_t>(1, 1, 0) = 1;
  *img.data<uint8_t>(1, 2, 0) = 1;
  *img.data<uint8_t>(2, 2, 0) = 1;

  // Object B: large (size 6) separated in X/Y.
  *img.data<uint8_t>(10, 10, 0) = 1;
  *img.data<uint8_t>(11, 10, 0) = 1;
  *img.data<uint8_t>(12, 10, 0) = 1;
  *img.data<uint8_t>(10, 11, 0) = 1;
  *img.data<uint8_t>(11, 11, 0) = 1;
  *img.data<uint8_t>(12, 11, 0) = 1;

  // Object C: large (size 5) separated in Z.
  for (size_t z = 0; z < 5; ++z) {
    *img.data<uint8_t>(5, 14, z) = 1;
  }

  nim::LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = 5;
  params.connectivity = 26;

  const auto ported = nim::labelLargeObjectsLegacy(img, params);
  EXPECT_EQ(ported.numLargeObjects, static_cast<size_t>(2));

  const auto legacy = labelLargeObjectsLegacyC(img, params);
  EXPECT_EQ(legacy.numLargeObjects, 2);

  ASSERT_EQ(ported.labels.width(), img.width());
  ASSERT_EQ(ported.labels.height(), img.height());
  ASSERT_EQ(ported.labels.depth(), img.depth());

  const auto a = toU32Labels(ported.labels);
  ASSERT_EQ(a.size(), legacy.labels.size());
  EXPECT_EQ(a, legacy.labels);
}

TEST(NeutubeObjLabel, PromotesToUint16LikeLegacy_WhenLabelsExceed255)
{
  using namespace nim;

  // Place 300 isolated single-voxel objects (26-connectivity) on an even grid.
  // With minSize=1, all are "large", so labels will exceed 255 and legacy promotes GREY->GREY16.
  ZImgInfo info(20, 20, 6, 1, 1, 1, VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();

  ZImg img(info);
  img.fill(0);

  int count = 0;
  for (size_t z = 0; z < info.depth; z += 2) {
    for (size_t y = 0; y < info.height; y += 2) {
      for (size_t x = 0; x < info.width; x += 2) {
        if (count >= 300) {
          break;
        }
        *img.data<uint8_t>(x, y, z) = 1;
        ++count;
      }
      if (count >= 300) {
        break;
      }
    }
    if (count >= 300) {
      break;
    }
  }
  ASSERT_EQ(count, 300);

  nim::LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = 1;
  params.connectivity = 26;

  const auto ported = nim::labelLargeObjectsLegacy(img, params);
  EXPECT_EQ(ported.numLargeObjects, static_cast<size_t>(300));
  EXPECT_TRUE(ported.labels.isType<uint16_t>()) << ported.labels.info();

  const auto legacy = labelLargeObjectsLegacyC(img, params);
  EXPECT_EQ(legacy.numLargeObjects, 300);
  EXPECT_EQ(legacy.kind, GREY16);

  const auto a = toU32Labels(ported.labels);
  ASSERT_EQ(a.size(), legacy.labels.size());
  EXPECT_EQ(a, legacy.labels);
}

TEST(NeutubeLegacySampling, PointSamplingMatchesLegacy)
{
  using namespace nim;

  constexpr size_t W = 8;
  constexpr size_t H = 9;
  constexpr size_t D = 10;

  ZImgInfo info(W, H, D, 1, 1, 1, VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();

  ZImg img(info);
  img.fill(0);

  Stack* stack = Make_Stack(GREY, static_cast<int>(W), static_cast<int>(H), static_cast<int>(D));
  ASSERT_NE(stack, nullptr);
  ASSERT_EQ(stack->kind, GREY);

  for (size_t z = 0; z < D; ++z) {
    for (size_t y = 0; y < H; ++y) {
      for (size_t x = 0; x < W; ++x) {
        const uint8_t v = static_cast<uint8_t>((x * 3 + y * 5 + z * 7) & 0xFF);
        *img.data<uint8_t>(x, y, z) = v;
        stack->array[z * W * H + y * W + x] = v;
      }
    }
  }

  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> distX(-1.0, static_cast<double>(W) + 1.0);
  std::uniform_real_distribution<double> distY(-1.0, static_cast<double>(H) + 1.0);
  std::uniform_real_distribution<double> distZ(-1.0, static_cast<double>(D) + 1.0);

  for (int i = 0; i < 5000; ++i) {
    const double x = distX(rng);
    const double y = distY(rng);
    const double z = distZ(rng);

    const double legacy = Stack_Point_Sampling(stack, x, y, z);
    const double ported = nim::pointSampleLegacyLike(img, x, y, z);

    if (std::isnan(legacy)) {
      EXPECT_TRUE(std::isnan(ported)) << "x=" << x << " y=" << y << " z=" << z;
    } else {
      EXPECT_DOUBLE_EQ(ported, legacy) << "x=" << x << " y=" << y << " z=" << z;
    }
  }

  Kill_Stack(stack);
}

TEST(NeutubeLegacyTraceWorkspace, MaskValueMatchesLegacy)
{
  using namespace nim;

  constexpr int W = 10;
  constexpr int H = 12;
  constexpr int D = 5;

  Trace_Workspace* legacyTw = New_Trace_Workspace();
  ASSERT_NE(legacyTw, nullptr);

  legacyTw->trace_mask = Make_Stack(GREY16, W, H, D);
  ASSERT_NE(legacyTw->trace_mask, nullptr);
  ASSERT_EQ(legacyTw->trace_mask->kind, GREY16);

  auto* legacyMask16 = reinterpret_cast<uint16_t*>(legacyTw->trace_mask->array);
  std::memset(legacyMask16,
              0,
              static_cast<size_t>(W) * static_cast<size_t>(H) * static_cast<size_t>(D) * sizeof(uint16_t));
  legacyMask16[static_cast<size_t>(2) * static_cast<size_t>(W) * static_cast<size_t>(H) +
               static_cast<size_t>(4) * static_cast<size_t>(W) + static_cast<size_t>(3)] = 42;

  double legacyPos[3] = {3.0, 4.0, 2.0};
  const int legacyValue = Trace_Workspace_Mask_Value(legacyTw, legacyPos);

  const std::array<double, 3> portedPos = {3.0, 4.0, 2.0};
  {
    nim::TraceWorkspace portedTw;
    ZImgInfo
      info(static_cast<size_t>(W), static_cast<size_t>(H), static_cast<size_t>(D), 1, 1, 1, VoxelFormat::Unsigned);
    info.setVoxelFormat<uint8_t>();
    info.createDefaultDescriptions();

    portedTw.traceMask = std::make_unique<ZImg>(info);
    portedTw.traceMask->fill(0);
    *portedTw.traceMask->data<uint8_t>(3, 4, 2) = 42;

    const int portedValue = nim::traceWorkspaceMaskValueLegacyLike(portedTw, portedPos);
    EXPECT_EQ(portedValue, legacyValue);

    // Check rounding parity (legacy uses iround() which maps to round() in this build).
    legacyPos[0] = 3.49;
    legacyPos[1] = 4.51;
    legacyPos[2] = 2.50;
    const int legacyRounded = Trace_Workspace_Mask_Value(legacyTw, legacyPos);
    const std::array<double, 3> portedRoundedPos = {3.49, 4.51, 2.50};
    const int portedRounded = nim::traceWorkspaceMaskValueLegacyLike(portedTw, portedRoundedPos);
    EXPECT_EQ(portedRounded, legacyRounded);

    // Out of bounds returns 0.
    legacyPos[0] = -1.0;
    legacyPos[1] = 4.0;
    legacyPos[2] = 2.0;
    EXPECT_EQ(Trace_Workspace_Mask_Value(legacyTw, legacyPos), 0);
    EXPECT_EQ(nim::traceWorkspaceMaskValueLegacyLike(portedTw, {-1.0, 4.0, 2.0}), 0);
  }

  {
    // We still support uint16 masks in the ported implementation because some callers may
    // choose to store non-binary labels.
    nim::TraceWorkspace portedTw;
    ZImgInfo
      info(static_cast<size_t>(W), static_cast<size_t>(H), static_cast<size_t>(D), 1, 1, 1, VoxelFormat::Unsigned);
    info.setVoxelFormat<uint16_t>();
    info.createDefaultDescriptions();

    portedTw.traceMask = std::make_unique<ZImg>(info);
    portedTw.traceMask->fill(0);
    *portedTw.traceMask->data<uint16_t>(3, 4, 2) = 42;

    EXPECT_EQ(nim::traceWorkspaceMaskValueLegacyLike(portedTw, portedPos), legacyValue);
  }

  Kill_Trace_Workspace(legacyTw);
}

TEST(NeutubeLegacyTraceWorkspace, MaskValueZMatchesLegacy)
{
  using namespace nim;

  constexpr int W = 10;
  constexpr int H = 12;
  constexpr int D = 5;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  Trace_Workspace* legacyTw = New_Trace_Workspace();
  ASSERT_NE(legacyTw, nullptr);

  legacyTw->trace_mask = Make_Stack(GREY16, W, H, D);
  ASSERT_NE(legacyTw->trace_mask, nullptr);
  ASSERT_EQ(legacyTw->trace_mask->kind, GREY16);

  auto* legacyMask16 = reinterpret_cast<uint16_t*>(legacyTw->trace_mask->array);
  std::memset(legacyMask16,
              0,
              static_cast<size_t>(W) * static_cast<size_t>(H) * static_cast<size_t>(D) * sizeof(uint16_t));
  legacyMask16[static_cast<size_t>(2) * static_cast<size_t>(W) * static_cast<size_t>(H) +
               static_cast<size_t>(4) * static_cast<size_t>(W) + static_cast<size_t>(3)] = 42;

  nim::TraceWorkspace portedTw;
  ZImgInfo info(static_cast<size_t>(W), static_cast<size_t>(H), static_cast<size_t>(D), 1, 1, 1, VoxelFormat::Unsigned);
  info.setVoxelFormat<uint16_t>();
  info.createDefaultDescriptions();
  portedTw.traceMask = std::make_unique<ZImg>(info);
  portedTw.traceMask->fill(0);
  *portedTw.traceMask->data<uint16_t>(3, 4, 2) = 42;

  std::vector<std::array<double, 3>> tracePositions = {
    {3.0,  4.0,  10.0        },
    {3.49, 4.51, 10.0        },
    {3.0,  4.0,  10.0 + 1e-12},
    {3.0,  4.0,  10.0 - 1e-12},
    {-1.0, 4.0,  10.0        },
  };

  for (const auto& posTrace : tracePositions) {
    double legacyPos[3] = {posTrace[0], posTrace[1], posTrace[2]};
    const int legacyValue = Trace_Workspace_Mask_Value_Z(legacyTw, legacyPos, legacyZScale);
    const int portedValue = nim::traceWorkspaceMaskValueZLegacyLike(portedTw, posTrace, zToXYRatio);
    EXPECT_EQ(portedValue, legacyValue) << "x=" << posTrace[0] << " y=" << posTrace[1] << " z=" << posTrace[2];
  }

  Kill_Trace_Workspace(legacyTw);
}

TEST(NeutubeLegacyTraceWorkspace, PointInBoundZMatchesLegacy)
{
  using namespace nim;

  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  Trace_Workspace* legacyTw = New_Trace_Workspace();
  ASSERT_NE(legacyTw, nullptr);
  legacyTw->trace_range[0] = 0.0;
  legacyTw->trace_range[1] = 0.0;
  legacyTw->trace_range[2] = 0.0;
  legacyTw->trace_range[3] = 100.0;
  legacyTw->trace_range[4] = 100.0;
  legacyTw->trace_range[5] = 20.0;

  nim::TraceWorkspace portedTw;
  portedTw.traceRange = {0.0, 0.0, 0.0, 100.0, 100.0, 20.0};

  std::vector<std::array<double, 3>> tracePositions = {
    {1.0,   1.0, 10.0         },
    {1.0,   1.0, 100.0        },
    {1.0,   1.0, 100.0 + 1e-12},
    {1.0,   1.0, -1e-12       },
    {101.0, 1.0, 10.0         },
  };

  for (const auto& posTrace : tracePositions) {
    double legacyPos[3] = {posTrace[0], posTrace[1], posTrace[2]};
    const int legacyInBound = Trace_Workspace_Point_In_Bound_Z(legacyTw, legacyPos, legacyZScale);
    const bool portedInBound = nim::traceWorkspacePointInBoundZLegacyLike(portedTw, posTrace, zToXYRatio);
    EXPECT_EQ(portedInBound, legacyInBound != 0) << "x=" << posTrace[0] << " y=" << posTrace[1] << " z=" << posTrace[2];
  }

  Kill_Trace_Workspace(legacyTw);
}

TEST(NeutubeLegacyTraceRecord, SettersMatchLegacy)
{
  using namespace nim;

  Trace_Record* legacy = New_Trace_Record();
  ASSERT_NE(legacy, nullptr);

  nim::TraceRecord ported;
  nim::traceRecordReset(ported);

  EXPECT_EQ(ported.mask, legacy->mask);
  EXPECT_EQ(ported.hitRegion, legacy->hit_region);
  EXPECT_EQ(ported.index, legacy->index);
  EXPECT_EQ(ported.refit, legacy->refit);
  EXPECT_EQ(ported.fitHeight[0], legacy->fit_height[0]);
  EXPECT_EQ(ported.fitHeight[1], legacy->fit_height[1]);
  EXPECT_EQ(static_cast<int>(ported.direction), static_cast<int>(legacy->direction));
  EXPECT_DOUBLE_EQ(ported.fixPoint, legacy->fix_point);

  Trace_Record_Set_Fix_Point(legacy, 0.0);
  Trace_Record_Set_Direction(legacy, DL_BOTHDIR);
  nim::traceRecordSetFixPoint(ported, 0.0);
  nim::traceRecordSetDirection(ported, nim::TraceDirection::BothDir);

  EXPECT_EQ(ported.mask, legacy->mask);
  EXPECT_TRUE(nim::traceRecordHasFixPoint(ported));
  EXPECT_TRUE(Trace_Record_Has_Fix_Point(legacy));
  EXPECT_DOUBLE_EQ(nim::traceRecordFixPoint(ported), Trace_Record_Fix_Point(legacy));
  EXPECT_EQ(static_cast<int>(nim::traceRecordDirection(ported)), static_cast<int>(Trace_Record_Direction(legacy)));

  Trace_Record_Set_Index(legacy, 7);
  nim::traceRecordSetIndex(ported, 7);
  EXPECT_EQ(ported.mask, legacy->mask);
  EXPECT_EQ(nim::traceRecordIndex(ported), Trace_Record_Index(legacy));

  Trace_Record_Disable_Fix_Point(legacy);
  nim::traceRecordDisableFixPoint(ported);
  EXPECT_EQ(ported.mask, legacy->mask);
  EXPECT_FALSE(nim::traceRecordHasFixPoint(ported));
  EXPECT_FALSE(Trace_Record_Has_Fix_Point(legacy));
  EXPECT_DOUBLE_EQ(nim::traceRecordFixPoint(ported), Trace_Record_Fix_Point(legacy));

  Delete_Trace_Record(legacy);
}

TEST(NeutubeLegacyDarrayMath, DotSumMeanCorrcoefMatchLegacy)
{
  using namespace nim;

  std::vector<double> a(64, 0.0);
  std::vector<double> b(64, 0.0);

  std::mt19937 rng(7);
  std::uniform_real_distribution<double> dist(-10.0, 10.0);
  for (size_t i = 0; i < a.size(); ++i) {
    a[i] = dist(rng);
    b[i] = dist(rng);
  }

  // Sprinkle NaNs to exercise legacy *_n semantics.
  a[0] = std::numeric_limits<double>::quiet_NaN();
  b[3] = std::numeric_limits<double>::quiet_NaN();
  a[10] = std::numeric_limits<double>::quiet_NaN();
  b[10] = std::numeric_limits<double>::quiet_NaN();

  const std::span<const double> aspan(a.data(), a.size());
  const std::span<const double> bspan(b.data(), b.size());

  EXPECT_DOUBLE_EQ(nim::darrayDotNLegacyLike(aspan, bspan), darray_dot_n(a.data(), b.data(), a.size()));
  {
    const double legacy = darray_dot_nw(a.data(), b.data(), a.size());
    const double ported = nim::darrayDotNWLegacyLike(aspan, bspan);
    if (std::isnan(legacy)) {
      EXPECT_TRUE(std::isnan(ported));
    } else {
      EXPECT_DOUBLE_EQ(ported, legacy);
    }
  }
  EXPECT_DOUBLE_EQ(nim::darraySumNLegacyLike(aspan), darray_sum_n(a.data(), a.size()));
  EXPECT_DOUBLE_EQ(nim::darrayMeanNLegacyLike(aspan), darray_mean_n(a.data(), a.size()));
  EXPECT_DOUBLE_EQ(nim::darrayCorrcoefNLegacyLike(aspan, bspan), darray_corrcoef_n(a.data(), b.data(), a.size()));

  size_t legacyIdx = 0;
  size_t portedIdx = 0;
  const double legacyMax = darray_max(a.data(), a.size(), &legacyIdx);
  const double portedMax = nim::darrayMaxLegacyLike(aspan, &portedIdx);

  if (std::isnan(legacyMax)) {
    EXPECT_TRUE(std::isnan(portedMax));
  } else {
    EXPECT_DOUBLE_EQ(portedMax, legacyMax);
  }
  EXPECT_EQ(portedIdx, legacyIdx);
}

TEST(NeutubeLegacyStackFitScore, BasicOptionsMatchLegacy)
{
  using namespace nim;

  std::vector<double> field(64, 0.0);
  std::vector<double> signal(64, 0.0);

  std::mt19937 rng(9);
  std::uniform_real_distribution<double> dist(-5.0, 5.0);
  for (size_t i = 0; i < field.size(); ++i) {
    field[i] = dist(rng);
    signal[i] = dist(rng);
  }

  // Force a few NaNs to exercise *_n semantics.
  signal[1] = std::numeric_limits<double>::quiet_NaN();
  field[2] = std::numeric_limits<double>::quiet_NaN();

  nim::StackFitScore fs;
  fs.n = 1;

  fs.options[0] = static_cast<int>(nim::StackFitOption::Dot);
  const double dotScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(field.data(), field.size()),
                                                               std::span<const double>(signal.data(), signal.size()),
                                                               &fs);
  EXPECT_DOUBLE_EQ(dotScore, darray_dot_n(field.data(), signal.data(), field.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);
  const double corrScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(field.data(), field.size()),
                                                                std::span<const double>(signal.data(), signal.size()),
                                                                &fs);
  EXPECT_DOUBLE_EQ(corrScore, darray_corrcoef_n(field.data(), signal.data(), field.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::Edot);
  const double edotScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(field.data(), field.size()),
                                                                std::span<const double>(signal.data(), signal.size()),
                                                                &fs);
  EXPECT_DOUBLE_EQ(edotScore,
                   darray_dot_n(field.data(), signal.data(), field.size()) +
                     darray_sum_n(signal.data(), signal.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::CorrcoefSc);
  const double corrScScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(field.data(), field.size()),
                                                                  std::span<const double>(signal.data(), signal.size()),
                                                                  &fs);
  const double legacyCorrSc =
    darray_corrcoef_n(field.data(), signal.data(), field.size()) * darray_max(signal.data(), signal.size(), nullptr);
  if (std::isnan(legacyCorrSc)) {
    EXPECT_TRUE(std::isnan(corrScScore));
  } else {
    EXPECT_DOUBLE_EQ(corrScScore, legacyCorrSc);
  }

  // nullptr fs behaves like legacy default: dot_n
  EXPECT_DOUBLE_EQ(nim::computeStackFitScoresLegacyLike(std::span<const double>(field.data(), field.size()),
                                                        std::span<const double>(signal.data(), signal.size()),
                                                        nullptr),
                   darray_dot_n(field.data(), signal.data(), field.size()));
}

TEST(NeutubeLegacyStackFitScore, StatMatchesLegacyGeo3dScore)
{
  using namespace nim;

  constexpr int Width = 16;
  constexpr int Height = 12;
  constexpr int Depth = 10;
  constexpr size_t FieldSize = 200;

  Stack* stack = Make_Stack(GREY, Width, Height, Depth);
  CHECK(stack != nullptr);

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> distU8(0, 255);
  std::uniform_int_distribution<int> distX(1, Width - 2);
  std::uniform_int_distribution<int> distY(1, Height - 2);
  std::uniform_int_distribution<int> distZ(1, Depth - 2);
  std::uniform_real_distribution<double> distFilter(-1.0, 1.0);

  for (size_t i = 0; i < static_cast<size_t>(Width * Height * Depth); ++i) {
    stack->array[i] = static_cast<uint8_t>(distU8(rng));
  }

  auto pointsFlat = std::make_unique<double[]>(FieldSize * 3);
  auto* points = reinterpret_cast<coordinate_3d_t*>(pointsFlat.get());

  std::vector<double> filter(FieldSize, 0.0);
  std::vector<double> signal(FieldSize, 0.0);

  const size_t area = static_cast<size_t>(Width) * static_cast<size_t>(Height);
  for (size_t i = 0; i < FieldSize; ++i) {
    const int x = distX(rng);
    const int y = distY(rng);
    const int z = distZ(rng);

    points[i][0] = static_cast<double>(x);
    points[i][1] = static_cast<double>(y);
    points[i][2] = static_cast<double>(z);

    filter[i] = distFilter(rng);

    const size_t offset =
      static_cast<size_t>(z) * area + static_cast<size_t>(y) * static_cast<size_t>(Width) + static_cast<size_t>(x);
    signal[i] = static_cast<double>(stack->array[offset]);
  }

  Geo3d_Scalar_Field fieldC;
  fieldC.size = static_cast<int>(FieldSize);
  fieldC.points = points;
  fieldC.values = filter.data();

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_STAT;
  const double legacyScore = Geo3d_Scalar_Field_Stack_Score(&fieldC, stack, 1.0, &fsC);

  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::Stat);
  const double portedScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(filter.data(), filter.size()),
                                                                  std::span<const double>(signal.data(), signal.size()),
                                                                  &fsCpp);

  if (std::isnan(legacyScore)) {
    EXPECT_TRUE(std::isnan(portedScore));
  } else {
    EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  }

  Kill_Stack(stack);
}

TEST(NeutubeLegacyStackFitScore, DotCenterMatchesLegacyGeo3dScore)
{
  using namespace nim;

  constexpr int Width = 16;
  constexpr int Height = 12;
  constexpr int Depth = 10;
  constexpr size_t FieldSize = 200;

  Stack* stack = Make_Stack(GREY, Width, Height, Depth);
  CHECK(stack != nullptr);

  std::mt19937 rng(67890);
  std::uniform_int_distribution<int> distU8(0, 255);
  std::uniform_int_distribution<int> distX(1, Width - 2);
  std::uniform_int_distribution<int> distY(1, Height - 2);
  std::uniform_int_distribution<int> distZ(1, Depth - 2);
  std::uniform_real_distribution<double> distFilter(-1.0, 1.0);

  for (size_t i = 0; i < static_cast<size_t>(Width * Height * Depth); ++i) {
    stack->array[i] = static_cast<uint8_t>(distU8(rng));
  }

  auto pointsFlat = std::make_unique<double[]>(FieldSize * 3);
  auto* points = reinterpret_cast<coordinate_3d_t*>(pointsFlat.get());

  std::vector<double> filter(FieldSize, 0.0);
  std::vector<double> signal(FieldSize, 0.0);

  const size_t area = static_cast<size_t>(Width) * static_cast<size_t>(Height);
  for (size_t i = 0; i < FieldSize; ++i) {
    const int x = distX(rng);
    const int y = distY(rng);
    const int z = distZ(rng);

    points[i][0] = static_cast<double>(x);
    points[i][1] = static_cast<double>(y);
    points[i][2] = static_cast<double>(z);

    filter[i] = distFilter(rng);

    const size_t offset =
      static_cast<size_t>(z) * area + static_cast<size_t>(y) * static_cast<size_t>(Width) + static_cast<size_t>(x);
    signal[i] = static_cast<double>(stack->array[offset]);
  }

  Geo3d_Scalar_Field fieldC;
  fieldC.size = static_cast<int>(FieldSize);
  fieldC.points = points;
  fieldC.values = filter.data();

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_DOT_CENTER;
  const double legacyScore = Geo3d_Scalar_Field_Stack_Score(&fieldC, stack, 1.0, &fsC);

  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::DotCenter);
  const double portedScore = nim::computeStackFitScoresLegacyLike(std::span<const double>(filter.data(), filter.size()),
                                                                  std::span<const double>(signal.data(), signal.size()),
                                                                  &fsCpp);

  if (std::isnan(legacyScore)) {
    EXPECT_TRUE(std::isnan(portedScore));
  } else {
    EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  }

  Kill_Stack(stack);
}

TEST(NeutubeLegacyGeo3dScalarField, SamplingAndScoreMatchLegacy)
{
  using namespace nim;

  constexpr int Width = 32;
  constexpr int Height = 24;
  constexpr int Depth = 20;
  constexpr size_t FieldSize = 500;

  ZImgInfo info(static_cast<size_t>(Width),
                static_cast<size_t>(Height),
                static_cast<size_t>(Depth),
                1,
                1,
                1,
                nim::VoxelFormat::Unsigned);
  ZImg stackImg(info);
  ZImg maskImg(info);

  std::mt19937 rng(24680);
  std::uniform_int_distribution<int> distU8(0, 255);
  std::uniform_int_distribution<int> distMask(0, 1);
  std::uniform_real_distribution<double> distX(-2.0, static_cast<double>(Width) + 1.0);
  std::uniform_real_distribution<double> distY(-2.0, static_cast<double>(Height) + 1.0);
  std::uniform_real_distribution<double> distZ(-2.0, static_cast<double>(Depth) + 1.0);
  std::uniform_real_distribution<double> distFilter(-1.0, 1.0);

  const size_t voxelNumber = stackImg.voxelNumber();
  {
    auto* stack = stackImg.timeData<uint8_t>(0);
    auto* mask = maskImg.timeData<uint8_t>(0);
    for (size_t i = 0; i < voxelNumber; ++i) {
      stack[i] = static_cast<uint8_t>(distU8(rng));
      mask[i] = static_cast<uint8_t>(distMask(rng));
    }
  }

  Stack* stackC = Make_Stack(GREY, Width, Height, Depth);
  CHECK(stackC != nullptr);
  std::memcpy(stackC->array, stackImg.timeData<uint8_t>(0), voxelNumber);

  Stack* maskC = Make_Stack(GREY, Width, Height, Depth);
  CHECK(maskC != nullptr);
  std::memcpy(maskC->array, maskImg.timeData<uint8_t>(0), voxelNumber);

  auto pointsFlat = std::make_unique<double[]>(FieldSize * 3);
  auto* points = reinterpret_cast<coordinate_3d_t*>(pointsFlat.get());

  nim::Geo3dScalarField fieldCpp;
  fieldCpp.points.resize(FieldSize);
  fieldCpp.values.resize(FieldSize);

  for (size_t i = 0; i < FieldSize; ++i) {
    const double x = distX(rng);
    const double y = distY(rng);
    const double z = distZ(rng);

    points[i][0] = x;
    points[i][1] = y;
    points[i][2] = z;

    fieldCpp.points[i] = {x, y, z};
    fieldCpp.values[i] = distFilter(rng);
  }

  Geo3d_Scalar_Field fieldC;
  fieldC.size = static_cast<int>(FieldSize);
  fieldC.points = points;
  fieldC.values = fieldCpp.values.data();

  const std::array<double, 3> zToXYRatios = {1.0, 2.0, 5.0};
  for (double zToXYRatio : zToXYRatios) {
    const double legacyZScale = 1.0 / zToXYRatio;
    auto expectSame = [&](const std::string& what, size_t i, double ported, double legacy) {
      if (std::isnan(legacy)) {
        EXPECT_TRUE(std::isnan(ported)) << what << " zToXYRatio=" << zToXYRatio << " legacyZScale=" << legacyZScale
                                        << " i=" << i;
      } else {
        EXPECT_DOUBLE_EQ(ported, legacy) << what << " zToXYRatio=" << zToXYRatio << " legacyZScale=" << legacyZScale
                                         << " i=" << i;
      }
    };

    // ------------------------------------------------------------
    // Sampling parity
    // ------------------------------------------------------------
    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling(&fieldC, stackC, legacyZScale, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported = nim::geo3dScalarFieldStackSamplingLegacyLike(fieldCpp, stackImg, zToXYRatio);

      ASSERT_EQ(ported.size(), FieldSize);
      for (size_t i = 0; i < FieldSize; ++i) {
        expectSame("Sampling", i, ported[i], legacy[i]);
      }
      free(legacy);
    }

    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling_W(&fieldC, stackC, legacyZScale, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported =
        nim::geo3dScalarFieldStackSamplingWeightedLegacyLike(fieldCpp, stackImg, zToXYRatio);

      ASSERT_EQ(ported.size(), FieldSize);
      for (size_t i = 0; i < FieldSize; ++i) {
        expectSame("Sampling_W", i, ported[i], legacy[i]);
      }
      free(legacy);
    }

    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling_M(&fieldC, stackC, legacyZScale, maskC, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported =
        nim::geo3dScalarFieldStackSamplingMaskedLegacyLike(fieldCpp, stackImg, zToXYRatio, maskImg);

      ASSERT_EQ(ported.size(), FieldSize);
      for (size_t i = 0; i < FieldSize; ++i) {
        expectSame("Sampling_M", i, ported[i], legacy[i]);
      }
      free(legacy);
    }

    // ------------------------------------------------------------
    // Score parity
    // ------------------------------------------------------------
    Stack_Fit_Score fsLegacy;
    fsLegacy.n = 10;
    fsLegacy.options[0] = STACK_FIT_DOT;
    fsLegacy.options[1] = STACK_FIT_CORRCOEF;
    fsLegacy.options[2] = STACK_FIT_EDOT;
    fsLegacy.options[3] = STACK_FIT_STAT;
    fsLegacy.options[4] = STACK_FIT_PDOT;
    fsLegacy.options[5] = STACK_FIT_MEAN_SIGNAL;
    fsLegacy.options[6] = STACK_FIT_LOW_MEAN_SIGNAL;
    fsLegacy.options[7] = STACK_FIT_CORRCOEF_SC;
    fsLegacy.options[8] = STACK_FIT_OUTER_SIGNAL;
    fsLegacy.options[9] = STACK_FIT_VALID_SIGNAL_RATIO;

    const double legacyScore = Geo3d_Scalar_Field_Stack_Score(&fieldC, stackC, legacyZScale, &fsLegacy);

    nim::StackFitScore fsPorted;
    fsPorted.n = fsLegacy.n;
    for (int j = 0; j < fsLegacy.n; ++j) {
      fsPorted.options[static_cast<size_t>(j)] = fsLegacy.options[j];
    }

    const double portedScore = nim::geo3dScalarFieldStackScoreLegacyLike(fieldCpp, stackImg, zToXYRatio, &fsPorted);
    expectSame("Score", 0, portedScore, legacyScore);

    for (int j = 0; j < fsLegacy.n; ++j) {
      expectSame(fmt::format("Score[{}] opt={}", j, fsLegacy.options[j]),
                 static_cast<size_t>(j),
                 fsPorted.scores[static_cast<size_t>(j)],
                 fsLegacy.scores[j]);
    }

    Stack_Fit_Score fsLegacyM;
    fsLegacyM.n = 10;
    fsLegacyM.options[0] = STACK_FIT_DOT;
    fsLegacyM.options[1] = STACK_FIT_CORRCOEF;
    fsLegacyM.options[2] = STACK_FIT_EDOT;
    fsLegacyM.options[3] = STACK_FIT_STAT;
    fsLegacyM.options[4] = STACK_FIT_PDOT;
    fsLegacyM.options[5] = STACK_FIT_MEAN_SIGNAL;
    fsLegacyM.options[6] = STACK_FIT_CORRCOEF_SC;
    fsLegacyM.options[7] = STACK_FIT_DOT_CENTER;
    fsLegacyM.options[8] = STACK_FIT_OUTER_SIGNAL;
    fsLegacyM.options[9] = STACK_FIT_VALID_SIGNAL_RATIO;

    const double legacyScoreM = Geo3d_Scalar_Field_Stack_Score_M(&fieldC, stackC, legacyZScale, maskC, &fsLegacyM);

    nim::StackFitScore fsPortedM;
    fsPortedM.n = fsLegacyM.n;
    for (int j = 0; j < fsLegacyM.n; ++j) {
      fsPortedM.options[static_cast<size_t>(j)] = fsLegacyM.options[j];
    }

    const double portedScoreM =
      nim::geo3dScalarFieldStackScoreMaskedLegacyLike(fieldCpp, stackImg, zToXYRatio, maskImg, &fsPortedM);
    expectSame("Score_M", 0, portedScoreM, legacyScoreM);

    for (int j = 0; j < fsLegacyM.n; ++j) {
      expectSame(fmt::format("Score_M[{}] opt={}", j, fsLegacyM.options[j]),
                 static_cast<size_t>(j),
                 fsPortedM.scores[static_cast<size_t>(j)],
                 fsLegacyM.scores[j]);
    }
  }

  Kill_Stack(stackC);
  Kill_Stack(maskC);
}

TEST(NeutubeLegacyOptimizer, FitPerceptorMatchesLegacy)
{
  auto scoreFunc = +[](const double* var, const void* /*param*/) -> double {
    const double dx = var[0] - 1.0;
    const double dy = var[1] - 2.0;
    return -(dx * dx + dy * dy);
  };

  auto validate = +[](double* var, const double* varMin, const double* varMax, const void* /*param*/) {
    for (int i = 0; i < 2; ++i) {
      if (var[i] < varMin[i]) {
        var[i] = varMin[i];
      } else if (var[i] > varMax[i]) {
        var[i] = varMax[i];
      }
    }
  };

  double varMin[2] = {-10.0, -10.0};
  double varMax[2] = {10.0, 10.0};

  double delta[2] = {0.5, 0.5};
  double weight[2] = {0.3, 1.7};

  int varIndex[2] = {0, 1};

  // Legacy C optimizer.
  double legacyVar[2] = {0.0, 0.0};
  ::Variable_Set vsC;
  vsC.var = legacyVar;
  vsC.var_index = varIndex;
  vsC.link = nullptr;
  vsC.nvar = 2;

  ::Continuous_Function cfC;
  cfC.f = scoreFunc;
  cfC.v = validate;
  cfC.var_min = varMin;
  cfC.var_max = varMax;

  ::Perceptor pC;
  pC.vs = &vsC;
  pC.arg = nullptr;
  pC.s = &cfC;
  pC.min_gradient = 1e-3;
  pC.delta = delta;
  pC.weight = weight;

  const double legacyScore = ::Fit_Perceptor(&pC, nullptr);

  // Ported C++ optimizer.
  double portedVar[2] = {0.0, 0.0};
  nim::VariableSet vsCpp;
  vsCpp.var = portedVar;
  vsCpp.varIndex = varIndex;
  vsCpp.link = nullptr;
  vsCpp.nvar = 2;

  nim::ContinuousFunction cfCpp;
  cfCpp.f = scoreFunc;
  cfCpp.v = validate;
  cfCpp.varMin = varMin;
  cfCpp.varMax = varMax;

  nim::Perceptor pCpp;
  pCpp.vs = &vsCpp;
  pCpp.arg = nullptr;
  pCpp.s = &cfCpp;
  pCpp.minGradient = 1e-3;
  pCpp.delta = delta;
  pCpp.weight = weight;

  const double portedScore = nim::fitPerceptorLegacyLike(pCpp, nullptr);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  EXPECT_DOUBLE_EQ(portedVar[0], legacyVar[0]);
  EXPECT_DOUBLE_EQ(portedVar[1], legacyVar[1]);
}

TEST(NeutubeLegacyNeuroseg, FieldSFastMatchesLegacy)
{
  struct Case
  {
    double r1;
    double c;
    double h;
    double theta;
    double psi;
    double curvature;
    double alpha;
    double scale;
  };

  const std::vector<Case> cases = {
    {3.0, 0.0, 11.0, TZ_PI_4, 0.0, 0.0, 0.0, 1.0},
    {2.5, 0.1, 7.0,  0.3,     1.1, 0.5, 0.2, 1.3},
  };

  for (const auto& c : cases) {
    ::Neuroseg segC;
    ::Set_Neuroseg(&segC, c.r1, c.c, c.h, c.theta, c.psi, c.curvature, c.alpha, c.scale);

    ::Geo3d_Scalar_Field* fieldC = ::Neuroseg_Field_S_Fast(&segC, nullptr, nullptr);
    ASSERT_NE(fieldC, nullptr);

    nim::Neuroseg segCpp;
    segCpp.r1 = c.r1;
    segCpp.c = c.c;
    segCpp.h = c.h;
    segCpp.theta = c.theta;
    segCpp.psi = c.psi;
    segCpp.curvature = c.curvature;
    segCpp.alpha = c.alpha;
    segCpp.scale = c.scale;

    const nim::Geo3dScalarField fieldCpp = nim::neurosegFieldSFastLegacyLike(segCpp, nullptr);

    ASSERT_EQ(fieldCpp.points.size(), static_cast<size_t>(fieldC->size));
    ASSERT_EQ(fieldCpp.values.size(), fieldCpp.points.size());

    for (size_t i = 0; i < fieldCpp.points.size(); ++i) {
      EXPECT_DOUBLE_EQ(fieldCpp.points[i][0], fieldC->points[i][0]) << "i=" << i;
      EXPECT_DOUBLE_EQ(fieldCpp.points[i][1], fieldC->points[i][1]) << "i=" << i;
      EXPECT_DOUBLE_EQ(fieldCpp.points[i][2], fieldC->points[i][2]) << "i=" << i;
      EXPECT_DOUBLE_EQ(fieldCpp.values[i], fieldC->values[i]) << "i=" << i;
    }

    ::Kill_Geo3d_Scalar_Field(fieldC);
  }
}

TEST(NeutubeLegacyLocalNeuroseg, FitWMatchesLegacy)
{
  constexpr int width = 48;
  constexpr int height = 48;
  constexpr int depth = 48;
  constexpr double zScale = 1.0;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  const size_t voxelNumber = img.voxelNumber();
  std::memset(img.timeData<uint8_t>(0), 0, voxelNumber);

  const int cx = width / 2;
  const int cy = height / 2;
  const int cz = depth / 2;
  for (int z = 0; z < depth; ++z) {
    *img.data<uint8_t>(static_cast<size_t>(cx), static_cast<size_t>(cy), static_cast<size_t>(z)) = 255;
  }

  // Legacy C stack.
  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), voxelNumber);

  // Legacy C locseg + fit.
  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC,
                     2.0,
                     0.0,
                     11.0,
                     0.7,
                     1.1,
                     0.0,
                     0.0,
                     1.0,
                     static_cast<double>(cx),
                     static_cast<double>(cy),
                     static_cast<double>(cz));

  Locseg_Fit_Workspace* wsC = New_Locseg_Fit_Workspace();
  ASSERT_NE(wsC, nullptr);
  Default_Locseg_Fit_Workspace(wsC);

  const double legacyInitialScore = Local_Neuroseg_Score_W(locsegC, stackC, zScale, wsC->sws);

  const double legacyScore = Fit_Local_Neuroseg_W(locsegC, stackC, zScale, wsC);

  // Ported C++ locseg + fit.
  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(cz)};

  nim::LocsegFitWorkspace wsCpp;
  nim::defaultLocsegFitWorkspaceLegacyLike(wsCpp);

  const double portedInitialScore = nim::localNeurosegScoreWLegacyLike(locsegCpp, img, zScale, wsCpp.sws);
  EXPECT_DOUBLE_EQ(portedInitialScore, legacyInitialScore);

  ASSERT_EQ(wsCpp.nvar, wsC->nvar);
  for (int i = 0; i < wsCpp.nvar; ++i) {
    EXPECT_EQ(wsCpp.varIndex[static_cast<size_t>(i)], wsC->var_index[i]) << "i=" << i;
  }
  for (int i = 0; i < 12; ++i) {
    EXPECT_DOUBLE_EQ(wsCpp.varMin[static_cast<size_t>(i)], wsC->var_min[i]) << "i=" << i;
    EXPECT_DOUBLE_EQ(wsCpp.varMax[static_cast<size_t>(i)], wsC->var_max[i]) << "i=" << i;
  }

  const double portedScore = nim::fitLocalNeurosegWLegacyLike(locsegCpp, img, zScale, wsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.c, locsegC->seg.c);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.h, locsegC->seg.h);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.curvature, locsegC->seg.curvature);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.alpha, locsegC->seg.alpha);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Locseg_Fit_Workspace(wsC);
  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, FitWAnisotropicMatchesLegacy)
{
  constexpr int width = 48;
  constexpr int height = 48;
  constexpr int depth = 48;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  const size_t voxelNumber = img.voxelNumber();
  std::memset(img.timeData<uint8_t>(0), 0, voxelNumber);

  const int cx = width / 2;
  const int cy = height / 2;
  const int czImage = depth / 2;
  for (int z = 0; z < depth; ++z) {
    *img.data<uint8_t>(static_cast<size_t>(cx), static_cast<size_t>(cy), static_cast<size_t>(z)) = 255;
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), voxelNumber);

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC,
                     2.0,
                     0.0,
                     11.0,
                     0.7,
                     1.1,
                     0.0,
                     0.0,
                     1.0,
                     static_cast<double>(cx),
                     static_cast<double>(cy),
                     static_cast<double>(czImage) / zToXYRatio);

  Locseg_Fit_Workspace* wsC = New_Locseg_Fit_Workspace();
  ASSERT_NE(wsC, nullptr);
  Default_Locseg_Fit_Workspace(wsC);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(czImage) / zToXYRatio};

  nim::LocsegFitWorkspace wsCpp;
  nim::defaultLocsegFitWorkspaceLegacyLike(wsCpp);

  const double legacyScore = Fit_Local_Neuroseg_W(locsegC, stackC, legacyZScale, wsC);
  const double portedScore = nim::fitLocalNeurosegWLegacyLike(locsegCpp, img, zToXYRatio, wsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.c, locsegC->seg.c);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.h, locsegC->seg.h);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.curvature, locsegC->seg.curvature);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.alpha, locsegC->seg.alpha);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Locseg_Fit_Workspace(wsC);
  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, PositionAdjustMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zScale = 1.0;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = x + 2 * y + 3 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const int cz = depth / 2;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, cz);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(cz)};

  Local_Neuroseg_Position_Adjust(locsegC, stackC, zScale);
  nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zScale);

  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, PositionAdjustAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 2.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = x + 2 * y + 3 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 16.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, czTrace);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  Local_Neuroseg_Position_Adjust(locsegC, stackC, legacyZScale);
  nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zToXYRatio);

  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, TopSampleAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 2.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = x + 5 * y + 9 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 16.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, czTrace);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  const double legacy = Local_Neuroseg_Top_Sample(locsegC, stackC, legacyZScale);
  const double ported = nim::localNeurosegTopSampleLegacyLike(locsegCpp, img, zToXYRatio);
  EXPECT_DOUBLE_EQ(ported, legacy);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, OrientationSearchCMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zScale = 1.0;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = x + 7 * y + 11 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const int cz = depth / 2;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.5, 0.0, 11.0, 0.4, 0.9, 0.0, 0.0, 1.0, cx, cy, cz);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.5;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.4;
  locsegCpp.seg.psi = 0.9;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(cz)};

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_CORRCOEF;

  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);

  const double legacyScore = Local_Neuroseg_Orientation_Search_C(locsegC, stackC, zScale, &fsC);
  const double portedScore = nim::localNeurosegOrientationSearchCLegacyLike(locsegCpp, img, zScale, fsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);

  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, OrientationSearchCAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = x + 7 * y + 11 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 6.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.5, 0.0, 11.0, 0.4, 0.9, 0.0, 0.0, 1.0, cx, cy, czTrace);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.5;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.4;
  locsegCpp.seg.psi = 0.9;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_CORRCOEF;

  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);

  const double legacyScore = Local_Neuroseg_Orientation_Search_C(locsegC, stackC, legacyZScale, &fsC);
  const double portedScore = nim::localNeurosegOrientationSearchCLegacyLike(locsegCpp, img, zToXYRatio, fsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, RScaleSearchMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zScale = 1.0;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = 13 * x + 3 * y + 5 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const int cz = depth / 2;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 3.0, 0.0, 11.0, 0.6, 0.2, 0.0, 0.0, 1.2, cx, cy, cz);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 3.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.6;
  locsegCpp.seg.psi = 0.2;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.2;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(cz)};

  const double legacyScore =
    Local_Neuroseg_R_Scale_Search(locsegC, stackC, zScale, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr);

  const double portedScore =
    nim::localNeurosegRScaleSearchLegacyLike(locsegCpp, img, zScale, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, RScaleSearchAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = 13 * x + 3 * y + 5 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 6.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 3.0, 0.0, 11.0, 0.6, 0.2, 0.0, 0.0, 1.2, cx, cy, czTrace);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 3.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.6;
  locsegCpp.seg.psi = 0.2;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.2;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  const double legacyScore =
    Local_Neuroseg_R_Scale_Search(locsegC, stackC, legacyZScale, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr);
  const double portedScore =
    nim::localNeurosegRScaleSearchLegacyLike(locsegCpp, img, zToXYRatio, 1.0, 10.0, 1.0, 0.5, 5.0, 0.5, nullptr);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, OptimizeWMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zScale = 1.0;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = 17 * x + 19 * y + 23 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const int cz = depth / 2;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, cz);

  Locseg_Fit_Workspace* wsC = New_Locseg_Fit_Workspace();
  ASSERT_NE(wsC, nullptr);
  Default_Locseg_Fit_Workspace(wsC);

  const double legacyScore = Local_Neuroseg_Optimize_W(locsegC, stackC, zScale, 0, wsC);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(cz)};

  nim::LocsegFitWorkspace wsCpp;
  nim::defaultLocsegFitWorkspaceLegacyLike(wsCpp);

  const double portedScore = nim::localNeurosegOptimizeWLegacyLike(locsegCpp, img, zScale, 0, wsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.c, locsegC->seg.c);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.h, locsegC->seg.h);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.curvature, locsegC->seg.curvature);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.alpha, locsegC->seg.alpha);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Locseg_Fit_Workspace(wsC);
  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, OptimizeWAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = 17 * x + 19 * y + 23 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 6.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, czTrace);

  Locseg_Fit_Workspace* wsC = New_Locseg_Fit_Workspace();
  ASSERT_NE(wsC, nullptr);
  Default_Locseg_Fit_Workspace(wsC);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  nim::LocsegFitWorkspace wsCpp;
  nim::defaultLocsegFitWorkspaceLegacyLike(wsCpp);

  for (int i = 0; i < wsC->pos_adjust; ++i) {
    Local_Neuroseg_Position_Adjust(locsegC, stackC, legacyZScale);
  }
  for (int i = 0; i < wsCpp.posAdjust; ++i) {
    nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zToXYRatio);
  }

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_CORRCOEF;
  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);

  (void)Local_Neuroseg_Orientation_Search_C(locsegC, stackC, legacyZScale, &fsC);
  (void)nim::localNeurosegOrientationSearchCLegacyLike(locsegCpp, img, zToXYRatio, fsCpp);

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  for (int i = 0; i < 3; ++i) {
    Local_Neuroseg_Position_Adjust(locsegC, stackC, legacyZScale);
    nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zToXYRatio);
  }

  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  const double legacyScore = Fit_Local_Neuroseg_W(locsegC, stackC, legacyZScale, wsC);
  const double portedScore = nim::fitLocalNeurosegWLegacyLike(locsegCpp, img, zToXYRatio, wsCpp);

  EXPECT_DOUBLE_EQ(portedScore, legacyScore);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.r1, locsegC->seg.r1);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.c, locsegC->seg.c);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.h, locsegC->seg.h);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.theta, locsegC->seg.theta);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.psi, locsegC->seg.psi);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.curvature, locsegC->seg.curvature);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.alpha, locsegC->seg.alpha);
  EXPECT_DOUBLE_EQ(locsegCpp.seg.scale, locsegC->seg.scale);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Locseg_Fit_Workspace(wsC);
  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocalNeuroseg, PositionAdjustAfterOrientationSearchAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 64;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  nim::ZImg img(info);

  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int v = 17 * x + 19 * y + 23 * z;
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)) =
          static_cast<uint8_t>(v & 0xFF);
      }
    }
  }

  Stack* stackC = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(stackC, nullptr);
  std::memcpy(stackC->array, img.timeData<uint8_t>(0), img.voxelNumber());

  const int cx = width / 2;
  const int cy = height / 2;
  const double czTrace = 6.0;

  Local_Neuroseg* locsegC = New_Local_Neuroseg();
  ASSERT_NE(locsegC, nullptr);
  Set_Local_Neuroseg(locsegC, 2.0, 0.0, 11.0, 0.7, 1.1, 0.0, 0.0, 1.0, cx, cy, czTrace);

  nim::LocalNeuroseg locsegCpp;
  locsegCpp.seg.r1 = 2.0;
  locsegCpp.seg.c = 0.0;
  locsegCpp.seg.h = 11.0;
  locsegCpp.seg.theta = 0.7;
  locsegCpp.seg.psi = 1.1;
  locsegCpp.seg.curvature = 0.0;
  locsegCpp.seg.alpha = 0.0;
  locsegCpp.seg.scale = 1.0;
  locsegCpp.pos = {static_cast<double>(cx), static_cast<double>(cy), czTrace};

  Stack_Fit_Score fsC;
  fsC.n = 1;
  fsC.options[0] = STACK_FIT_CORRCOEF;

  nim::StackFitScore fsCpp;
  fsCpp.n = 1;
  fsCpp.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);

  Local_Neuroseg_Position_Adjust(locsegC, stackC, legacyZScale);
  nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zToXYRatio);

  (void)Local_Neuroseg_Orientation_Search_C(locsegC, stackC, legacyZScale, &fsC);
  (void)nim::localNeurosegOrientationSearchCLegacyLike(locsegCpp, img, zToXYRatio, fsCpp);

  Local_Neuroseg_Position_Adjust(locsegC, stackC, legacyZScale);
  nim::localNeurosegPositionAdjustLegacyLike(locsegCpp, img, zToXYRatio);

  EXPECT_DOUBLE_EQ(locsegCpp.pos[0], locsegC->pos[0]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[1], locsegC->pos[1]);
  EXPECT_DOUBLE_EQ(locsegCpp.pos[2], locsegC->pos[2]);

  Kill_Local_Neuroseg(locsegC);
  Kill_Stack(stackC);
}

TEST(NeutubeLegacyLocsegChain, LabelWAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 32;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  Stack* legacyMask = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(legacyMask, nullptr);
  Zero_Stack(legacyMask);

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  nim::ZImg portedMask(info);
  portedMask.fill(0);

  Locseg_Chain* legacyChain = New_Locseg_Chain();
  ASSERT_NE(legacyChain, nullptr);

  nim::LocsegChain portedChain;

  auto appendNode = [&](double r1,
                        double c,
                        double h,
                        double theta,
                        double psi,
                        double curvature,
                        double alpha,
                        double scale,
                        double x,
                        double y,
                        double z) {
    Local_Neuroseg* segC = New_Local_Neuroseg();
    ASSERT_NE(segC, nullptr);
    Set_Local_Neuroseg(segC, r1, c, h, theta, psi, curvature, alpha, scale, x, y, z);
    Locseg_Chain_Add(legacyChain, segC, nullptr, DL_TAIL);

    nim::LocsegNode node;
    node.locseg.seg.r1 = r1;
    node.locseg.seg.c = c;
    node.locseg.seg.h = h;
    node.locseg.seg.theta = theta;
    node.locseg.seg.psi = psi;
    node.locseg.seg.curvature = curvature;
    node.locseg.seg.alpha = alpha;
    node.locseg.seg.scale = scale;
    node.locseg.pos = {x, y, z};
    (void)portedChain.addNode(std::move(node), nim::LocsegChainEndLegacyLike::Tail);
  };

  appendNode(2.2, 0.0, 11.0, 0.55, 0.85, 0.0, 0.0, 1.10, 18.0, 20.0, 10.0);
  appendNode(2.1, 0.0, 11.0, 0.60, 0.90, 0.0, 0.0, 1.08, 20.0, 22.5, 10.8);
  appendNode(2.0, 0.0, 11.0, 0.62, 0.95, 0.0, 0.0, 1.05, 22.0, 25.0, 11.6);

  Locseg_Label_Workspace* legacyWs = New_Locseg_Label_Workspace();
  ASSERT_NE(legacyWs, nullptr);
  legacyWs->option = 1;
  legacyWs->flag = -1;
  legacyWs->value = 1;
  legacyWs->sratio = 1.5;
  legacyWs->sdiff = 0.0;
  legacyWs->slimit = 3.0;

  nim::LocsegLabelWorkspaceLegacyLike portedWs;
  portedWs.option = 1;
  portedWs.flag = -1;
  portedWs.value = 1;
  portedWs.sratio = 1.5;
  portedWs.sdiff = 0.0;
  portedWs.slimit = 3.0;

  Locseg_Chain_Label_W(legacyChain, legacyMask, legacyZScale, 0, Locseg_Chain_Length(legacyChain) - 1, legacyWs);
  nim::locsegChainLabelWLegacyLike(portedChain, portedMask, zToXYRatio, 0, portedChain.length() - 1, portedWs);

  const VolumeDigest legacyDigest = digestLegacyStackU8OrU16(legacyMask);
  const VolumeDigest portedDigest = digestZImgU8OrU16(portedMask);
  EXPECT_EQ(portedDigest.hash, legacyDigest.hash);
  EXPECT_EQ(portedDigest.nonzero, legacyDigest.nonzero);

  Kill_Locseg_Label_Workspace(legacyWs);
  Kill_Locseg_Chain(legacyChain);
  Kill_Stack(legacyMask);
}

TEST(NeutubeLegacyLocalNeuroseg, LabelWAnisotropicMatchesLegacy)
{
  constexpr int width = 64;
  constexpr int height = 64;
  constexpr int depth = 32;
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  Stack* legacyMask = Make_Stack(GREY, width, height, depth);
  ASSERT_NE(legacyMask, nullptr);
  Zero_Stack(legacyMask);

  nim::ZImgInfo info(width, height, depth, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  nim::ZImg portedMask(info);
  portedMask.fill(0);

  Local_Neuroseg* segC = New_Local_Neuroseg();
  ASSERT_NE(segC, nullptr);
  Set_Local_Neuroseg(segC, 2.2, 0.0, 11.0, 0.55, 0.85, 0.0, 0.0, 1.10, 18.0, 20.0, 10.0);

  nim::LocalNeuroseg segCpp;
  segCpp.seg.r1 = 2.2;
  segCpp.seg.c = 0.0;
  segCpp.seg.h = 11.0;
  segCpp.seg.theta = 0.55;
  segCpp.seg.psi = 0.85;
  segCpp.seg.curvature = 0.0;
  segCpp.seg.alpha = 0.0;
  segCpp.seg.scale = 1.10;
  segCpp.pos = {18.0, 20.0, 10.0};

  Locseg_Label_Workspace* legacyWs = New_Locseg_Label_Workspace();
  ASSERT_NE(legacyWs, nullptr);
  legacyWs->option = 1;
  legacyWs->flag = -1;
  legacyWs->value = 1;
  legacyWs->sratio = 1.5;
  legacyWs->sdiff = 0.0;
  legacyWs->slimit = 3.0;

  nim::LocsegLabelWorkspaceLegacyLike portedWs;
  portedWs.option = 1;
  portedWs.flag = -1;
  portedWs.value = 1;
  portedWs.sratio = 1.5;
  portedWs.sdiff = 0.0;
  portedWs.slimit = 3.0;

  Local_Neuroseg_Label_W(segC, legacyMask, legacyZScale, legacyWs);
  nim::localNeurosegLabelWLegacyLike(segCpp, portedMask, zToXYRatio, portedWs);

  const VolumeDigest legacyDigest = digestLegacyStackU8OrU16(legacyMask);
  const VolumeDigest portedDigest = digestZImgU8OrU16(portedMask);
  EXPECT_EQ(portedDigest.hash, legacyDigest.hash);
  EXPECT_EQ(portedDigest.nonzero, legacyDigest.nonzero);

  Kill_Locseg_Label_Workspace(legacyWs);
  Kill_Local_Neuroseg(segC);
  Kill_Stack(legacyMask);
}

TEST(NeutubeCommand2Parity, SkeletonizeAndTrace_TiffMatchesLegacy)
{
  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path skeletonizeCfg = dir / "skeletonize.json";
  const fs::path traceCfg = dir / "trace_config.json";
  const fs::path inputSkelTiff = dir / "input_skeletonize.tif";
  const fs::path legacySkelSwc = dir / "legacy_skeletonize.swc";
  const fs::path v2SkelSwc = dir / "v2_skeletonize.swc";
  const fs::path inputTraceTiff = dir / "signal_trace.tif";
  const fs::path legacyTraceSwc = dir / "legacy_trace.swc";
  const fs::path v2TraceSwc = dir / "v2_trace.swc";

  writeTextFile(commandConfig,
                R"json({
  "skeletonize": {
    "include": "skeletonize.json"
  },
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  // Keep parameters close to neuTube defaults, but allow tiny synthetic inputs to produce an SWC.
  writeTextFile(skeletonizeCfg,
                R"json({
  "downsampleInterval": [0, 0, 0],
  "minimalLength": 0,
  "finalMinimalLength": 0,
  "keepingSingleObject": true,
  "rebase": true,
  "fillingHole": true,
  "maximalDistance": 100
}
)json");

  // Minimal config: rely on ZNeuronTracerConfig defaults, but ensure the legacy loader accepts it.
  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  // ============================================================
  // Skeletonize parity
  // ============================================================
  writeSimpleLineTiff(inputSkelTiff, 32, 32, 128, 1);

  // Legacy runner: Atlas --command --skeletonize <input> -o <out> --config <command_config.json>
  {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--skeletonize",
      inputSkelTiff.string(),
      "-o",
      legacySkelSwc.string(),
      "--config",
      commandConfig.string(),
    });
    const int rc = nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
    EXPECT_EQ(rc, 0);
  }

  // V2 runner: Atlas --command --skeletonize <input> -o <out> --config <command_config.json>
  {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--skeletonize",
      inputSkelTiff.string(),
      "-o",
      v2SkelSwc.string(),
      "--config",
      commandConfig.string(),
    });
    const int rc = nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
    EXPECT_EQ(rc, 0);
  }

  ASSERT_TRUE(fs::exists(legacySkelSwc)) << legacySkelSwc.string();
  ASSERT_TRUE(fs::exists(v2SkelSwc)) << v2SkelSwc.string();
  EXPECT_EQ(readTextFile(legacySkelSwc), readTextFile(v2SkelSwc));

  // ============================================================
  // Trace parity
  // ============================================================
  writeSimpleLineTiff(inputTraceTiff, 32, 32, 128, 255);

  json::object in;
  in["signal"] = inputTraceTiff.string();
  json::array pos;
  pos.emplace_back(16);
  pos.emplace_back(16);
  pos.emplace_back(64);
  in["position"] = std::move(pos);
  const std::string inputJson = json::serialize(in);

  const int legacyRc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      legacyTraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();

  const int v2Rc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      v2TraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();

  EXPECT_EQ(legacyRc, v2Rc);

  if (legacyRc == 0) {
    ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
    ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
    EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_WithHostSwc_MatchesLegacy)
{
  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";
  const fs::path inputTraceTiff = dir / "signal_trace.tif";
  const fs::path hostSwc = dir / "host.swc";
  const fs::path legacyTraceSwc = dir / "legacy_trace_host.swc";
  const fs::path v2TraceSwc = dir / "v2_trace_host.swc";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  writeSimpleLineTiff(inputTraceTiff, 128, 32, 32, 255);

  writeTextFile(hostSwc,
                R"swc(
1 0 64 16 10 1 -1
2 0 64 16 14 1 1
3 0 64 16 18 1 2
)swc");

  json::object in;
  in["signal"] = inputTraceTiff.string();
  in["swc"] = hostSwc.string();
  json::array pos;
  pos.emplace_back(64);
  pos.emplace_back(16);
  pos.emplace_back(16);
  in["position"] = std::move(pos);
  const std::string inputJson = json::serialize(in);

  const int legacyRc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      legacyTraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();

  const int v2Rc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      v2TraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();

  EXPECT_EQ(legacyRc, v2Rc);
  EXPECT_EQ(legacyRc, 0);

  ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
  ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
  EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_WithHostSwc_NoConnection_MatchesLegacy)
{
  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";
  const fs::path inputTraceTiff = dir / "signal_trace.tif";
  const fs::path hostSwc = dir / "host_far.swc";
  const fs::path legacyTraceSwc = dir / "legacy_trace_host_far.swc";
  const fs::path v2TraceSwc = dir / "v2_trace_host_far.swc";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  writeSimpleLineTiff(inputTraceTiff, 128, 32, 32, 255);

  writeTextFile(hostSwc,
                R"swc(
1 0 0 0 0 1 -1
2 0 0 0 1 1 1
3 0 0 0 2 1 2
)swc");

  json::object in;
  in["signal"] = inputTraceTiff.string();
  in["swc"] = hostSwc.string();
  json::array pos;
  pos.emplace_back(64);
  pos.emplace_back(16);
  pos.emplace_back(16);
  in["position"] = std::move(pos);
  const std::string inputJson = json::serialize(in);

  const int legacyRc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      legacyTraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();

  const int v2Rc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      v2TraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();

  EXPECT_EQ(legacyRc, v2Rc);
  EXPECT_EQ(legacyRc, 0);

  ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
  ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
  EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_DiagnosisSeeded_MatchesLegacy)
{
  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";
  const fs::path inputTraceTiff = dir / "signal_trace.tif";
  const fs::path legacyTraceSwc = dir / "legacy_trace_diag.swc";
  const fs::path v2TraceSwc = dir / "v2_trace_diag.swc";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json",
    "diagnosis": true
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  writeSimpleLineTiff(inputTraceTiff, 32, 32, 128, 255);

  json::object in;
  in["signal"] = inputTraceTiff.string();
  json::array pos;
  pos.emplace_back(16);
  pos.emplace_back(16);
  pos.emplace_back(64);
  in["position"] = std::move(pos);
  const std::string inputJson = json::serialize(in);

  const int legacyRc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      legacyTraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();

  const int v2Rc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      v2TraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();

  EXPECT_EQ(legacyRc, v2Rc);

  if (legacyRc == 0) {
    ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
    ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
    EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_DiagnosisWithHostSwc_MatchesLegacy)
{
  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";
  const fs::path inputTraceTiff = dir / "signal_trace.tif";
  const fs::path hostSwc = dir / "host.swc";
  const fs::path legacyTraceSwc = dir / "legacy_trace_host_diag.swc";
  const fs::path v2TraceSwc = dir / "v2_trace_host_diag.swc";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json",
    "diagnosis": true
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  writeSimpleLineTiff(inputTraceTiff, 128, 32, 32, 255);

  writeTextFile(hostSwc,
                R"swc(
1 0 64 16 10 1 -1
2 0 64 16 14 1 1
3 0 64 16 18 1 2
)swc");

  json::object in;
  in["signal"] = inputTraceTiff.string();
  in["swc"] = hostSwc.string();
  json::array pos;
  pos.emplace_back(64);
  pos.emplace_back(16);
  pos.emplace_back(16);
  in["position"] = std::move(pos);
  const std::string inputJson = json::serialize(in);

  const int legacyRc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      legacyTraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();

  const int v2Rc = [&]() {
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      v2TraceSwc.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();

  EXPECT_EQ(legacyRc, v2Rc);

  if (legacyRc == 0) {
    ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
    ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
    EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_Auto_FromTestData_MatchesLegacy)
{
  const std::vector<QString> candidateRelativePaths = {
    // "benchmark/fake_neuron.tif",
    "benchmark/fake_neuron2.tif",
    "benchmark/fake_neuron3.tif",
    "benchmark/fake_neuron4.tif",
    "benchmark/fake_spine.tif",
    "benchmark/faint_fiber.tif",
    "benchmark/sharp_turn.tif",
    "benchmark/fork2/fork2.tif",
    "benchmark/line.tif",
    // "benchmark/gaussians.tif",
    // 2D edge cases (depth==1) exercise local-max / neighborhood guards.
    // "benchmark/bline_2d_1.tif",
    // "benchmark/bline_2d_2.tif",
    // "benchmark/bline_2d.tif",
    // "benchmark/bfork_2d.tif",
    // Excluded for now: the legacy in-process auto-trace path crashes on this
    // fixture in the parity harness on Windows (access violation).
    // "benchmark/fork_2d.tif",
    "benchmark/btrig_2d.tif",
    // "benchmark/btrig2_2d.tif",
    "benchmark/crossover_2d.tif",
    // Excluded for now: the legacy in-process auto-trace path crashes on this
    // fixture in the parity harness on Windows (access violation).
    // "benchmark/crossover2_2d.tif",
    "benchmark/cross_45_10.tif",
    "benchmark/stack_graph/fork/fork.tif",
    "benchmark/stack_graph/neuroseg/cross_60_8.tif",
  };

  std::vector<QString> inputPaths;
  inputPaths.reserve(candidateRelativePaths.size());
  for (const QString& rel : candidateRelativePaths) {
    const QString path = nim::getTestDataDir().filePath(rel);
    if (QFileInfo::exists(path)) {
      inputPaths.push_back(path);
    }
  }

  if (inputPaths.empty()) {
    GTEST_SKIP() << "Missing auto-trace benchmark inputs under: " << nim::getTestDataDir().absolutePath();
  }

  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
  }
)json");

  for (const QString& inputPath : inputPaths) {
    SCOPED_TRACE(inputPath.toStdString());

    const std::string stem = QFileInfo(inputPath).completeBaseName().toStdString();
    const fs::path legacyTraceSwc = dir / fmt::format("legacy_trace_auto_{}.swc", stem);
    const fs::path v2TraceSwc = dir / fmt::format("v2_trace_auto_{}.swc", stem);

    std::error_code ec;
    fs::remove(legacyTraceSwc, ec);
    fs::remove(v2TraceSwc, ec);

    const int legacyRc = [&]() {
      ArgvBuilder argv({
        "Atlas",
        "--command",
        "--trace",
        inputPath.toStdString(),
        "-o",
        legacyTraceSwc.string(),
        "--config",
        commandConfig.string(),
      });
      return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
    }();

    const int v2Rc = [&]() {
      ArgvBuilder argv({
        "Atlas",
        "--command",
        "--trace",
        inputPath.toStdString(),
        "-o",
        v2TraceSwc.string(),
        "--config",
        commandConfig.string(),
      });
      return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
    }();

    EXPECT_EQ(legacyRc, v2Rc);

    if (legacyRc == 0) {
      ASSERT_TRUE(fs::exists(legacyTraceSwc)) << legacyTraceSwc.string();
      ASSERT_TRUE(fs::exists(v2TraceSwc)) << v2TraceSwc.string();
      EXPECT_EQ(readTextFile(legacyTraceSwc), readTextFile(v2TraceSwc));
    } else {
      EXPECT_FALSE(fs::exists(legacyTraceSwc));
      EXPECT_FALSE(fs::exists(v2TraceSwc));
    }
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_SequentialSeeds_Fork_MatchesLegacy)
{
  const QString forkTiff = nim::getTestDataDir().filePath("benchmark/stack_graph/fork/fork.tif");
  if (!QFileInfo::exists(forkTiff)) {
    GTEST_SKIP() << "Missing fork trace benchmark input: " << forkTiff;
  }

  const std::vector<std::array<int, 3>> seeds = {
    {63, 64, 51},
    {31, 69, 51},
    {31, 45, 49},
  };

  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  auto runTraceStep = [&](auto&& runner,
                          const std::optional<fs::path>& hostSwcPath,
                          const fs::path& outSwcPath,
                          const std::array<int, 3>& seed) -> int {
    json::object in;
    in["signal"] = forkTiff.toStdString();
    if (hostSwcPath.has_value()) {
      in["swc"] = hostSwcPath->string();
    }
    json::array pos;
    pos.emplace_back(seed[0]);
    pos.emplace_back(seed[1]);
    pos.emplace_back(seed[2]);
    in["position"] = std::move(pos);
    const std::string inputJson = json::serialize(in);

    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      outSwcPath.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return runner(argv.argc(), argv.argv());
  };

  std::optional<fs::path> legacyHost;
  std::optional<fs::path> v2Host;

  for (size_t i = 0; i < seeds.size(); ++i) {
    SCOPED_TRACE(fmt::format("step={} seed=({}, {}, {})", i + 1, seeds[i][0], seeds[i][1], seeds[i][2]));

    const fs::path legacyOut = dir / fmt::format("legacy_fork_step{}.swc", i + 1);
    const fs::path v2Out = dir / fmt::format("v2_fork_step{}.swc", i + 1);

    const int legacyRc = runTraceStep(
      [&](int argc, char** argv) {
        return nim::ZRunNeuTuCommand().run(argc, argv);
      },
      legacyHost,
      legacyOut,
      seeds[i]);

    const int v2Rc = runTraceStep(
      [&](int argc, char** argv) {
        return nim::ZRunNeuTuCommand2().run(argc, argv, QString{});
      },
      v2Host,
      v2Out,
      seeds[i]);

    ASSERT_EQ(legacyRc, v2Rc);
    ASSERT_EQ(legacyRc, 0);

    ASSERT_TRUE(fs::exists(legacyOut)) << legacyOut.string();
    ASSERT_TRUE(fs::exists(v2Out)) << v2Out.string();
    EXPECT_EQ(readTextFile(legacyOut), readTextFile(v2Out));

    legacyHost = legacyOut;
    v2Host = v2Out;
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Trace_SequentialSeeds_Fork2_MatchesLegacy)
{
  const QString fork2Tiff = nim::getTestDataDir().filePath("benchmark/fork2/fork2.tif");
  if (!QFileInfo::exists(fork2Tiff)) {
    GTEST_SKIP() << "Missing fork2 trace benchmark input: " << fork2Tiff;
  }

  const std::vector<std::array<int, 3>> seeds = {
    {50, 40, 37},
    {27, 73, 40},
    {70, 70, 61},
    {39, 62, 57},
    {28, 52, 60},
  };

  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path traceCfg = dir / "trace_config.json";

  writeTextFile(commandConfig,
                R"json({
  "trace": {
    "include": "trace_config.json"
  }
}
)json");

  writeTextFile(traceCfg,
                R"json({
  "tag": "trace config",
  "default": {}
}
)json");

  auto runTraceStep = [&](auto&& runner,
                          const std::optional<fs::path>& hostSwcPath,
                          const fs::path& outSwcPath,
                          const std::array<int, 3>& seed) -> int {
    json::object in;
    in["signal"] = fork2Tiff.toStdString();
    if (hostSwcPath.has_value()) {
      in["swc"] = hostSwcPath->string();
    }
    json::array pos;
    pos.emplace_back(seed[0]);
    pos.emplace_back(seed[1]);
    pos.emplace_back(seed[2]);
    in["position"] = std::move(pos);
    const std::string inputJson = json::serialize(in);

    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      "-o",
      outSwcPath.string(),
      "--config",
      commandConfig.string(),
      "json",
      inputJson,
    });
    return runner(argv.argc(), argv.argv());
  };

  std::optional<fs::path> legacyHost;
  std::optional<fs::path> v2Host;

  for (size_t i = 0; i < seeds.size(); ++i) {
    SCOPED_TRACE(fmt::format("step={} seed=({}, {}, {})", i + 1, seeds[i][0], seeds[i][1], seeds[i][2]));

    const fs::path legacyOut = dir / fmt::format("legacy_fork2_step{}.swc", i + 1);
    const fs::path v2Out = dir / fmt::format("v2_fork2_step{}.swc", i + 1);

    const int legacyRc = runTraceStep(
      [&](int argc, char** argv) {
        return nim::ZRunNeuTuCommand().run(argc, argv);
      },
      legacyHost,
      legacyOut,
      seeds[i]);

    const int v2Rc = runTraceStep(
      [&](int argc, char** argv) {
        return nim::ZRunNeuTuCommand2().run(argc, argv, QString{});
      },
      v2Host,
      v2Out,
      seeds[i]);

    ASSERT_EQ(legacyRc, v2Rc);
    ASSERT_EQ(legacyRc, 0);

    ASSERT_TRUE(fs::exists(legacyOut)) << legacyOut.string();
    ASSERT_TRUE(fs::exists(v2Out)) << v2Out.string();
    EXPECT_EQ(readTextFile(legacyOut), readTextFile(v2Out));

    legacyHost = legacyOut;
    v2Host = v2Out;
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Parity, Skeletonize_FromTestDataBinary_MatchesLegacy)
{
  const std::vector<QString> candidateRelativePaths = {
    "benchmark/sphere_bw.tif",
    "benchmark/sphere_bw_crop.tif",
    "benchmark/isolated_objects.tif",
    "benchmark/btrig2_2d_bw.tif",
    "benchmark/crossover_2d.tif",
    "benchmark/binary/2d/disk_n1.tif",
    "benchmark/binary/3d/diadem_e1.tif",
  };

  std::vector<QString> inputPaths;
  inputPaths.reserve(candidateRelativePaths.size());
  for (const QString& rel : candidateRelativePaths) {
    const QString path = nim::getTestDataDir().filePath(rel);
    if (QFileInfo::exists(path)) {
      inputPaths.push_back(path);
    }
  }

  if (inputPaths.empty()) {
    GTEST_SKIP() << "Missing skeletonize benchmark inputs under: " << nim::getTestDataDir().absolutePath();
  }

  const fs::path dir = makeUniqueTempDir();
  const fs::path commandConfig = dir / "command_config.json";
  const fs::path skeletonizeCfg = dir / "skeletonize.json";

  writeTextFile(commandConfig,
                R"json({
  "skeletonize": {
    "include": "skeletonize.json"
  }
}
)json");

  // Use defaults (legacy-equivalent) for this parity suite; do not tune thresholds.
  writeTextFile(skeletonizeCfg,
                R"json({
  "downsampleInterval": [0, 0, 0]
}
)json");

  for (const QString& inputPath : inputPaths) {
    SCOPED_TRACE(inputPath.toStdString());

    const std::string stem = QFileInfo(inputPath).completeBaseName().toStdString();
    const fs::path legacyOut = dir / fmt::format("legacy_skeletonize_{}.swc", stem);
    const fs::path v2Out = dir / fmt::format("v2_skeletonize_{}.swc", stem);

    std::error_code ec;
    fs::remove(legacyOut, ec);
    fs::remove(v2Out, ec);

    const int legacyRc = [&]() {
      ArgvBuilder argv({
        "Atlas",
        "--command",
        "--skeletonize",
        inputPath.toStdString(),
        "-o",
        legacyOut.string(),
        "--config",
        commandConfig.string(),
      });
      return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
    }();

    const int v2Rc = [&]() {
      ArgvBuilder argv({
        "Atlas",
        "--command",
        "--skeletonize",
        inputPath.toStdString(),
        "-o",
        v2Out.string(),
        "--config",
        commandConfig.string(),
      });
      return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
    }();

    EXPECT_EQ(legacyRc, v2Rc);

    const bool legacyExists = fs::exists(legacyOut);
    const bool v2Exists = fs::exists(v2Out);
    EXPECT_EQ(legacyExists, v2Exists);
    if (legacyExists && v2Exists) {
      EXPECT_EQ(readTextFile(legacyOut), readTextFile(v2Out));
    }
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(NeutubeCommand2Diagnostics, AutoTrace_Slice15_MaskSeedSort_MatchesLegacy_DevOnly)
{
  struct StagePerf
  {
    std::string name;
    int64_t legacyMs = 0;
    int64_t portedMs = 0;
  };

  std::vector<StagePerf> perf;
  perf.reserve(24);

  struct PerfLogger
  {
    std::vector<StagePerf>* perfPtr = nullptr;
    ~PerfLogger()
    {
      if (perfPtr == nullptr || perfPtr->empty()) {
        return;
      }

      int64_t sumLegacy = 0;
      int64_t sumPorted = 0;
      for (const StagePerf& p : *perfPtr) {
        sumLegacy += p.legacyMs;
        sumPorted += p.portedMs;
      }

      LOG(INFO) << "Slice15 stage runtime breakdown (ms):";
      for (const StagePerf& p : *perfPtr) {
        const double ratio =
          (p.legacyMs > 0) ? (static_cast<double>(p.portedMs) / static_cast<double>(p.legacyMs)) : 0.0;
        LOG(INFO)
          << fmt::format("  {:<28} legacy={:>8}  ported={:>8}  ratio={:>6.3f}x", p.name, p.legacyMs, p.portedMs, ratio);
      }

      const double totalRatio =
        (sumLegacy > 0) ? (static_cast<double>(sumPorted) / static_cast<double>(sumLegacy)) : 0.0;
      LOG(INFO) << fmt::format("  {:<28} legacy={:>8}  ported={:>8}  ratio={:>6.3f}x",
                               "TOTAL",
                               sumLegacy,
                               sumPorted,
                               totalRatio);
    }
  } perfLogger{&perf};

  auto timeMs = [](auto&& fn) -> int64_t {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  };

  const fs::path input =
    fs::path(QDir::homePath().toStdString()) / "Dropbox/atlas_test/slice15/slice15_L34_Sum.lsm_ch2.tif";
  if (!fs::exists(input)) {
    GTEST_SKIP() << "Missing dev-only auto-trace input: " << input.string();
  }

  const fs::path dir = makeUniqueTempDir();
  const DevOnlyAutoTraceConfigPaths cfgPaths = writeDevOnlyAutoTraceConfigFiles(dir);

  nim::TraceConfig portedCfg;
  ASSERT_TRUE(nim::loadTraceConfigLegacyLike(nim::filesystemPathToQString(cfgPaths.traceCfg), portedCfg));
  EXPECT_EQ(portedCfg.seedMethod, 1) << "This diagnostic helper currently expects seedMethod=1.";

  // Load raw signal through both legacy (ZStack) and ported (ZImg) IO to ensure we are comparing the same input.
  ZStack legacySignalStack;
  const auto legacyLoadStart = std::chrono::steady_clock::now();
  const bool legacyLoadOk = legacySignalStack.load(input.string());
  const int64_t legacyLoadMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - legacyLoadStart).count();
  ASSERT_TRUE(legacyLoadOk);

  Stack* legacySignal = legacySignalStack.c_stack(0);
  ASSERT_NE(legacySignal, nullptr);

  nim::ZImg portedSignal;
  const auto portedLoadStart = std::chrono::steady_clock::now();
  portedSignal.load(nim::filesystemPathToQString(input));
  const int64_t portedLoadMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - portedLoadStart).count();
  ASSERT_FALSE(portedSignal.isEmpty());
  ASSERT_EQ(portedSignal.numChannels(), 1);
  ASSERT_EQ(portedSignal.numTimes(), 1);

  perf.push_back(StagePerf{.name = "load_signal", .legacyMs = legacyLoadMs, .portedMs = portedLoadMs});

  {
    SCOPED_TRACE("raw_signal");
    const VolumeDigest legacyRaw = digestLegacyStackU8OrU16(legacySignal);
    const VolumeDigest portedRaw = digestZImgU8OrU16(portedSignal);

    ASSERT_EQ(legacyRaw.width, portedRaw.width);
    ASSERT_EQ(legacyRaw.height, portedRaw.height);
    ASSERT_EQ(legacyRaw.depth, portedRaw.depth);
    ASSERT_EQ(legacyRaw.bytesPerVoxel, portedRaw.bytesPerVoxel);

    if (legacyRaw.hash != portedRaw.hash) {
      ADD_FAILURE() << fmt::format("Raw signal mismatch: legacy hash={} ported hash={} (nonzero legacy={} ported={})",
                                   legacyRaw.hash,
                                   portedRaw.hash,
                                   legacyRaw.nonzero,
                                   portedRaw.nonzero);

      if (legacyRaw.bytesPerVoxel == 1) {
        const auto* a = legacySignal->array;
        const auto* b = portedSignal.timeData<uint8_t>(0);
        const auto mismatch = firstMismatch(a, b, legacyRaw.width, legacyRaw.height, legacyRaw.depth);
        if (mismatch) {
          const auto [x, y, z, va, vb] = *mismatch;
          ADD_FAILURE()
            << fmt::format("First raw signal mismatch at ({}, {}, {}): legacy={} ported={}", x, y, z, va, vb);
        }
      } else if (legacyRaw.bytesPerVoxel == 2) {
        const auto* a = reinterpret_cast<const uint16_t*>(legacySignal->array);
        const auto* b = portedSignal.timeData<uint16_t>(0);
        const auto mismatch = firstMismatch(a, b, legacyRaw.width, legacyRaw.height, legacyRaw.depth);
        if (mismatch) {
          const auto [x, y, z, va, vb] = *mismatch;
          ADD_FAILURE()
            << fmt::format("First raw signal mismatch at ({}, {}, {}): legacy={} ported={}", x, y, z, va, vb);
        }
      }
    }
  }

  // Preprocess (subtract background).
  ZNeuronTracerConfig::getInstance().load(cfgPaths.traceCfg.string());

  ZNeuronTracer legacyTracer;
  legacyTracer.setIntensityField(&legacySignalStack);
  legacyTracer.setTraceLevel(/*level*/ 0);
  legacyTracer.initTraceMask(/*clearing*/ false);

  const int64_t legacyPreprocessMs = timeMs([&]() {
    if (legacyTracer._preprocess) {
      legacyTracer._preprocess(legacySignal);
    }
  });
  const int64_t portedPreprocessMs = timeMs([&]() {
    (void)nim::subtractBackgroundLegacyLike(portedSignal, /*minFr*/ 0.5, /*maxIter*/ 3);
  });
  perf.push_back(StagePerf{.name = "preprocess_bgsub", .legacyMs = legacyPreprocessMs, .portedMs = portedPreprocessMs});

  {
    SCOPED_TRACE("preprocessed_signal");
    const VolumeDigest legacyPre = digestLegacyStackU8OrU16(legacySignal);
    const VolumeDigest portedPre = digestZImgU8OrU16(portedSignal);

    ASSERT_EQ(legacyPre.width, portedPre.width);
    ASSERT_EQ(legacyPre.height, portedPre.height);
    ASSERT_EQ(legacyPre.depth, portedPre.depth);
    ASSERT_EQ(legacyPre.bytesPerVoxel, portedPre.bytesPerVoxel);

    if (legacyPre.hash != portedPre.hash) {
      ADD_FAILURE() << fmt::format("Preprocessed signal mismatch: legacy hash={} ported hash={}",
                                   legacyPre.hash,
                                   portedPre.hash);

      if (legacyPre.bytesPerVoxel == 1) {
        const auto* a = legacySignal->array;
        const auto* b = portedSignal.timeData<uint8_t>(0);
        const auto mismatch = firstMismatch(a, b, legacyPre.width, legacyPre.height, legacyPre.depth);
        if (mismatch) {
          const auto [x, y, z, va, vb] = *mismatch;
          ADD_FAILURE()
            << fmt::format("First preprocessed signal mismatch at ({}, {}, {}): legacy={} ported={}", x, y, z, va, vb);
        }
      } else if (legacyPre.bytesPerVoxel == 2) {
        const auto* a = reinterpret_cast<const uint16_t*>(legacySignal->array);
        const auto* b = portedSignal.timeData<uint16_t>(0);
        const auto mismatch = firstMismatch(a, b, legacyPre.width, legacyPre.height, legacyPre.depth);
        if (mismatch) {
          const auto [x, y, z, va, vb] = *mismatch;
          ADD_FAILURE()
            << fmt::format("First preprocessed signal mismatch at ({}, {}, {}): legacy={} ported={}", x, y, z, va, vb);
        }
      }
    }
  }

  // Mask generation.
  Stack* legacyMask = nullptr;
  const int64_t legacyMaskMs = timeMs([&]() {
    legacyMask = legacyTracer.makeMask(legacySignal);
  });
  ASSERT_NE(legacyMask, nullptr);
  ASSERT_EQ(legacyMask->kind, GREY);

  nim::MakeMaskDiagnosticsLegacyLike maskDiag;
  maskDiag.collectTiming = true;
  std::optional<nim::ZImg> portedMaskOpt;
  const int64_t portedMaskMs = timeMs([&]() {
    portedMaskOpt = nim::makeMaskLegacyLike(portedSignal, portedCfg, &maskDiag);
  });
  ASSERT_TRUE(portedMaskOpt.has_value());
  nim::ZImg portedMask = std::move(*portedMaskOpt);

  perf.push_back(StagePerf{.name = "make_mask", .legacyMs = legacyMaskMs, .portedMs = portedMaskMs});
  LOG(INFO) << fmt::format("Slice15 make_mask threshold={}", maskDiag.binarizeThreshold);
  LOG(INFO) << fmt::format(
    "Slice15 make_mask (ported) breakdown: binarize={}ms majority_filter_r={}ms | hist_full={}ms "
    "locmax_hist={}ms (region={}ms seed_select={}ms masked_hist={}ms, region_nonzero={}, seed_nonzero={}) "
    "threshold_select={}ms threshold_binarize={}ms",
    maskDiag.ms_binarize_locmax,
    maskDiag.ms_bwsolid_majority_filter_r,
    maskDiag.binarizeDiag.ms_hist_full,
    maskDiag.binarizeDiag.ms_locmax_hist,
    maskDiag.binarizeDiag.ms_locmax_region_mask,
    maskDiag.binarizeDiag.ms_locmax_seed_select,
    maskDiag.binarizeDiag.ms_locmax_hist_masked,
    maskDiag.binarizeDiag.locmax_region_nonzero,
    maskDiag.binarizeDiag.locmax_seed_nonzero,
    maskDiag.binarizeDiag.ms_threshold,
    maskDiag.binarizeDiag.ms_threshold_binarize);

  // Compare alternative implementation (ZImgRegionalExtrema) for locmax-region mask performance and equivalence.
  {
    constexpr int conn = 18;

    nim::ZImg locmaxLegacyLike;
    const int64_t legacyLikeMs = timeMs([&]() {
      locmaxLegacyLike = nim::stackLocmaxRegionMaskLegacyLike(portedSignal, conn);
    });

    nim::ZImg locmaxRegionalExtrema;
    const int64_t regionalExtremaMs = timeMs([&]() {
      nim::ZImgRegionalExtrema regionalExtrema;
      locmaxRegionalExtrema = regionalExtrema.regionalMax(portedSignal, conn);
    });

    LOG(INFO) << fmt::format("Slice15 locmax_region_mask perf (ported): stackLocmaxRegionMaskLegacyLike={}ms "
                             "ZImgRegionalExtrema::regionalMax={}ms",
                             legacyLikeMs,
                             regionalExtremaMs);

    const VolumeDigest legacyLikeDigest = digestZImgU8OrU16(locmaxLegacyLike);
    const VolumeDigest regionalExtremaDigest = digestZImgU8OrU16(locmaxRegionalExtrema);
    ASSERT_EQ(legacyLikeDigest.width, regionalExtremaDigest.width);
    ASSERT_EQ(legacyLikeDigest.height, regionalExtremaDigest.height);
    ASSERT_EQ(legacyLikeDigest.depth, regionalExtremaDigest.depth);
    ASSERT_EQ(legacyLikeDigest.bytesPerVoxel, 1u);
    ASSERT_EQ(regionalExtremaDigest.bytesPerVoxel, 1u);
    if (legacyLikeDigest.hash != regionalExtremaDigest.hash) {
      ADD_FAILURE() << fmt::format("locmax_region_mask mismatch vs ZImgRegionalExtrema: legacyLike hash={} "
                                   "regionalExtrema hash={}",
                                   legacyLikeDigest.hash,
                                   regionalExtremaDigest.hash);
    }
  }

  {
    SCOPED_TRACE("mask");
    const VolumeDigest legacyMaskDigest = digestLegacyStackU8OrU16(legacyMask);
    const VolumeDigest portedMaskDigest = digestZImgU8OrU16(portedMask);

    ASSERT_EQ(legacyMaskDigest.width, portedMaskDigest.width);
    ASSERT_EQ(legacyMaskDigest.height, portedMaskDigest.height);
    ASSERT_EQ(legacyMaskDigest.depth, portedMaskDigest.depth);
    ASSERT_EQ(legacyMaskDigest.bytesPerVoxel, 1u);
    ASSERT_EQ(portedMaskDigest.bytesPerVoxel, 1u);

    if (legacyMaskDigest.hash != portedMaskDigest.hash) {
      ADD_FAILURE() << fmt::format("Mask mismatch: legacy hash={} ported hash={} (nonzero legacy={} ported={})",
                                   legacyMaskDigest.hash,
                                   portedMaskDigest.hash,
                                   legacyMaskDigest.nonzero,
                                   portedMaskDigest.nonzero);

      const auto* a = legacyMask->array;
      const auto* b = portedMask.timeData<uint8_t>(0);
      const auto mismatch =
        firstMismatch(a, b, legacyMaskDigest.width, legacyMaskDigest.height, legacyMaskDigest.depth);
      if (mismatch) {
        const auto [x, y, z, va, vb] = *mismatch;
        ADD_FAILURE() << fmt::format("First mask mismatch at ({}, {}, {}): legacy={} ported={}", x, y, z, va, vb);
      }
    }
  }

  // Seed extraction (pre-noise-removal).
  Geo3d_Scalar_Field* legacySeeds0 = nullptr;
  const int64_t legacySeedExtract0Ms = timeMs([&]() {
    legacySeeds0 = extractSeedOriginalLegacyC(legacyMask);
  });
  ASSERT_NE(legacySeeds0, nullptr);

  nim::Geo3dScalarField portedSeeds0;
  const int64_t portedSeedExtract0Ms = timeMs([&]() {
    portedSeeds0 = nim::extractSeedOriginalLegacyLike(portedMask);
  });
  perf.push_back(
    StagePerf{.name = "seed_extract_pre", .legacyMs = legacySeedExtract0Ms, .portedMs = portedSeedExtract0Ms});

  {
    SCOPED_TRACE("seed_extract_pre_noise");
    const auto legacySeedsVec = toIntSeeds(legacySeeds0);
    const auto portedSeedsVec = toIntSeeds(portedSeeds0);

    ASSERT_EQ(legacySeedsVec.size(), portedSeedsVec.size());

    for (size_t i = 0; i < legacySeedsVec.size(); ++i) {
      const auto& a = legacySeedsVec[i];
      const auto& b = portedSeedsVec[i];
      if (a.x != b.x || a.y != b.y || a.z != b.z || std::abs(a.value - b.value) > 1e-12) {
        ADD_FAILURE() << fmt::format("Seed mismatch at i={}: legacy=({}, {}, {}, {}) ported=({}, {}, {}, {})",
                                     i,
                                     a.x,
                                     a.y,
                                     a.z,
                                     a.value,
                                     b.x,
                                     b.y,
                                     b.z,
                                     b.value);
        break;
      }
    }
  }

  // Noise removal (may mutate mask + recompute seeds).
  LegacyRemoveNoisySeedResult legacyNoisy;
  const int64_t legacyNoiseMs = timeMs([&]() {
    ScopedStdoutSilencer silence;
    legacyNoisy =
      removeNoisySeedLegacyC(legacySeeds0, legacyMask, /*seedMethod*/ portedCfg.seedMethod, /*screeningSeed*/ true);
  });
  ASSERT_NE(legacyNoisy.seeds, nullptr);
  ASSERT_NE(legacyNoisy.mask, nullptr);

  nim::RemoveNoisySeedDiagnosticsLegacyLike portedNoisyDiag;
  nim::Geo3dScalarField portedSeeds1;
  const int64_t portedNoiseMs = timeMs([&]() {
    portedSeeds1 = nim::removeNoisySeedLegacyLike(std::move(portedSeeds0),
                                                  portedMask,
                                                  portedCfg.seedMethod,
                                                  /*screeningSeed*/ true,
                                                  &portedNoisyDiag);
  });
  perf.push_back(StagePerf{.name = "noise_removal", .legacyMs = legacyNoiseMs, .portedMs = portedNoiseMs});

  EXPECT_NEAR(legacyNoisy.seedDensity, portedNoisyDiag.seedDensity, 0.0);
  EXPECT_EQ(legacyNoisy.minSeedSize, portedNoisyDiag.minSeedSize);

  {
    SCOPED_TRACE("mask_after_noise_removal");
    const VolumeDigest legacyMaskDigest = digestLegacyStackU8OrU16(legacyNoisy.mask);
    const VolumeDigest portedMaskDigest = digestZImgU8OrU16(portedMask);

    ASSERT_EQ(legacyMaskDigest.hash, portedMaskDigest.hash)
      << fmt::format("legacy nonzero={} ported nonzero={}", legacyMaskDigest.nonzero, portedMaskDigest.nonzero);
  }

  {
    SCOPED_TRACE("seed_extract_post_noise");
    const auto legacySeedsVec = toIntSeeds(legacyNoisy.seeds);
    const auto portedSeedsVec = toIntSeeds(portedSeeds1);

    ASSERT_EQ(legacySeedsVec.size(), portedSeedsVec.size());
    for (size_t i = 0; i < legacySeedsVec.size(); ++i) {
      const auto& a = legacySeedsVec[i];
      const auto& b = portedSeedsVec[i];
      if (a.x != b.x || a.y != b.y || a.z != b.z || std::abs(a.value - b.value) > 1e-12) {
        ADD_FAILURE() << fmt::format("Seed mismatch at i={}: legacy=({}, {}, {}, {}) ported=({}, {}, {}, {})",
                                     i,
                                     a.x,
                                     a.y,
                                     a.z,
                                     a.value,
                                     b.x,
                                     b.y,
                                     b.z,
                                     b.value);
        break;
      }
    }
  }

  // Sort seeds.
  const int64_t legacySeedThresholdMs = timeMs([&]() {
    legacyTracer.prepareTraceScoreThreshold(ZNeuronTracer::TRACING_SEED);
  });
  ZNeuronTraceSeeder legacySeeder;
  Stack* legacyBaseMask = nullptr;
  const int64_t legacySortMs = timeMs([&]() {
    ScopedStdoutSilencer silence;
    legacyBaseMask = legacySeeder.sortSeed(legacyNoisy.seeds, legacySignal, legacyTracer.getTraceWorkspace());
  });
  ASSERT_NE(legacyBaseMask, nullptr);

  nim::TraceWorkspace portedTw;
  const int64_t portedTwInitMs = timeMs([&]() {
    nim::locsegChainDefaultTraceWorkspaceLegacyLike(portedTw, portedSignal);
  });
  portedTw.refit = portedCfg.refit;
  portedTw.tuneEnd = portedCfg.tuneEnd;
  portedTw.traceMaskUpdating = true;
  const int64_t portedTraceMaskInitMs = timeMs([&]() {
    nim::traceWorkspaceInitTraceMaskLegacyLike(portedTw, portedSignal, /*clearing*/ false);
  });
  const int64_t portedSeedThresholdMs = timeMs([&]() {
    nim::prepareTraceScoreThresholdLegacyLike(portedSignal, portedCfg, nim::TracingModeLegacyLike::Seed, portedTw);
  });
  nim::SeedSortResultLegacyLike portedSorted;
  const int64_t portedSortMs = timeMs([&]() {
    portedSorted = nim::sortSeedsLegacyLike(portedSeeds1, portedSignal, /*zScale*/ 1.0, portedTw);
  });

  perf.push_back(
    StagePerf{.name = "seed_threshold", .legacyMs = legacySeedThresholdMs, .portedMs = portedSeedThresholdMs});
  perf.push_back(
    StagePerf{.name = "trace_workspace", .legacyMs = 0, .portedMs = portedTwInitMs + portedTraceMaskInitMs});
  perf.push_back(StagePerf{.name = "seed_sort", .legacyMs = legacySortMs, .portedMs = portedSortMs});

  {
    SCOPED_TRACE("base_mask_after_seed_sort");
    const VolumeDigest legacyBaseDigest = digestLegacyStackU8OrU16(legacyBaseMask);
    const VolumeDigest portedBaseDigest = digestZImgU8OrU16(portedSorted.baseMask);
    ASSERT_EQ(legacyBaseDigest.hash, portedBaseDigest.hash)
      << fmt::format("legacy nonzero={} ported nonzero={}", legacyBaseDigest.nonzero, portedBaseDigest.nonzero);
  }

  ASSERT_EQ(legacySeeder.getScoreArray().size(), portedSorted.scoreArray.size());

  for (size_t i = 0; i < legacySeeder.getScoreArray().size(); ++i) {
    const double a = legacySeeder.getScoreArray()[i];
    const double b = portedSorted.scoreArray[i];
    if (std::abs(a - b) > 1e-10) {
      ADD_FAILURE() << fmt::format("Seed score mismatch at i={}: legacy={} ported={}", i, a, b);
      break;
    }
  }

  // Trace all seeds (initial stage) and compare chain counts/lengths.
  const int64_t legacyAutoThresholdMs = timeMs([&]() {
    legacyTracer.prepareTraceScoreThreshold(ZNeuronTracer::TRACING_AUTO);
  });
  const int64_t portedAutoThresholdMs = timeMs([&]() {
    nim::prepareTraceScoreThresholdLegacyLike(portedSignal, portedCfg, nim::TracingModeLegacyLike::Auto, portedTw);
  });
  perf.push_back(
    StagePerf{.name = "auto_threshold", .legacyMs = legacyAutoThresholdMs, .portedMs = portedAutoThresholdMs});

  std::vector<Local_Neuroseg> legacyLocsegs = legacySeeder.getSeedArray();
  std::vector<double> legacyScores = legacySeeder.getScoreArray();

  std::vector<nim::LocalNeuroseg> portedLocsegs = portedSorted.locsegArray;
  std::vector<double> portedScores = portedSorted.scoreArray;

  // Interactive trace perf: reuse sorted seed scores from auto tracing to pick a few seeds, then
  // trace them one-by-one. Compare the ZImg fast path vs ZVoxelVolume interface path.
  {
    std::vector<size_t> seedOrder;
    seedOrder.resize(portedScores.size());
    for (size_t i = 0; i < seedOrder.size(); ++i) {
      seedOrder[i] = i;
    }
    std::sort(seedOrder.begin(), seedOrder.end(), [&](size_t a, size_t b) {
      return portedScores[a] > portedScores[b];
    });

    std::vector<std::array<double, 3>> chosenSeeds;
    chosenSeeds.reserve(5);
    const double minDist = 0.25 * std::min<double>(portedSignal.width(), portedSignal.height());
    const double minDist2 = minDist * minDist;
    for (size_t idx : seedOrder) {
      if (chosenSeeds.size() >= 5) {
        break;
      }
      if (idx >= portedSeeds1.points.size()) {
        continue;
      }
      const auto cand = portedSeeds1.points[idx];

      bool farEnough = true;
      for (const auto& s : chosenSeeds) {
        const double dx = cand[0] - s[0];
        const double dy = cand[1] - s[1];
        const double dz = cand[2] - s[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < minDist2) {
          farEnough = false;
          break;
        }
      }
      if (farEnough) {
        chosenSeeds.push_back(cand);
      }
    }
    if (chosenSeeds.size() < 3) {
      // Fallback: take the top few seeds even if clustered.
      chosenSeeds.clear();
      for (size_t i = 0; i < seedOrder.size() && chosenSeeds.size() < 3; ++i) {
        const size_t idx = seedOrder[i];
        if (idx < portedSeeds1.points.size()) {
          chosenSeeds.push_back(portedSeeds1.points[idx]);
        }
      }
    }

    if (!chosenSeeds.empty()) {
      const double zScale = nim::preferredZToXYRatioFromImgInfoLegacyLike(portedSignal.info());
      nim::ZDenseVoxelVolume denseVol(portedSignal);
      int64_t sumImgMs = 0;
      int64_t sumVolMs = 0;

      for (size_t i = 0; i < chosenSeeds.size(); ++i) {
        const auto seed = chosenSeeds[i];

        nim::SeedTraceResult imgRes;
        const int64_t imgMs = timeMs([&]() {
          imgRes = nim::traceSeedNewSwcLegacyLike(portedSignal, seed, portedCfg, zScale, /*c*/ 0, /*t*/ 0);
        });

        nim::SeedTraceResult volRes;
        const int64_t volMs = timeMs([&]() {
          volRes = nim::traceSeedNewSwcLegacyLike(denseVol, seed, portedCfg, zScale);
        });

        sumImgMs += imgMs;
        sumVolMs += volMs;

        LOG(INFO) << fmt::format("Slice15 interactive trace seed[{}]: img={}ms vol={}ms seed=({:.1f},{:.1f},{:.1f})",
                                 i,
                                 imgMs,
                                 volMs,
                                 seed[0],
                                 seed[1],
                                 seed[2]);

        if (imgRes.swc && volRes.swc) {
          const SwcDigest a = digestSwc(*imgRes.swc);
          const SwcDigest b = digestSwc(*volRes.swc);
          EXPECT_EQ(a.nodes, b.nodes);
          EXPECT_EQ(a.hash, b.hash);
        } else {
          EXPECT_EQ(imgRes.swc != nullptr, volRes.swc != nullptr);
        }
      }

      const double ratio = (sumVolMs > 0) ? (static_cast<double>(sumImgMs) / static_cast<double>(sumVolMs)) : 0.0;
      LOG(INFO) << fmt::format("Slice15 interactive trace total: img={}ms vol={}ms (ratio={:.3f}x)",
                               sumImgMs,
                               sumVolMs,
                               ratio);
    }
  }

  int legacyNchain = 0;
  Locseg_Chain** legacyChainsRaw = nullptr;
  int64_t legacyTraceAllSeedsMs = 0;
  {
    ScopedStdoutSilencer silence;
    legacyTraceAllSeedsMs = timeMs([&]() {
      legacyChainsRaw = Trace_Locseg_S(legacySignal,
                                       /*z_scale*/ 1.0,
                                       legacyLocsegs.data(),
                                       legacyScores.data(),
                                       static_cast<int>(legacyLocsegs.size()),
                                       legacyTracer.getTraceWorkspace(),
                                       &legacyNchain);
    });
  }
  ASSERT_NE(legacyChainsRaw, nullptr);
  ASSERT_GE(legacyNchain, 0);

  std::vector<Locseg_Chain*> legacyChains;
  legacyChains.reserve(static_cast<size_t>(legacyNchain));
  for (int i = 0; i < legacyNchain; ++i) {
    legacyChains.push_back(legacyChainsRaw[i]);
  }
  free(legacyChainsRaw);

  std::vector<std::unique_ptr<nim::LocsegChain>> portedChains;
  int64_t portedTraceAllSeedsMs = 0;
  {
    ScopedStdoutSilencer silence;
    portedTraceAllSeedsMs = timeMs([&]() {
      portedChains = nim::traceAllSeedsLegacyLike(portedSignal, /*zScale*/ 1.0, portedLocsegs, portedScores, portedTw);
    });
  }
  perf.push_back(
    StagePerf{.name = "trace_all_seeds", .legacyMs = legacyTraceAllSeedsMs, .portedMs = portedTraceAllSeedsMs});

  ASSERT_EQ(static_cast<size_t>(legacyNchain), portedChains.size());

  for (size_t i = 0; i < portedChains.size(); ++i) {
    const double a = Locseg_Chain_Geolen(legacyChains[i]);
    const double b = nim::locsegChainGeolenLegacyLike(*portedChains[i]);
    if (std::abs(a - b) > 1e-6) {
      ADD_FAILURE() << fmt::format("Chain geolen mismatch at i={}: legacy={} ported={}", i, a, b);
      break;
    }
  }

  std::vector<Locseg_Chain*> legacyRecoveredChains;
  Stack* legacyRecoverBaseMask = nullptr;
  nim::RecoverResultLegacyLike portedRecover;

  // Recover stage and compare recovered chain counts/lengths.
  if (portedCfg.recover > 0) {
    const auto legacyRecoverStart = std::chrono::steady_clock::now();
    // Legacy leftover computation:
    //   traceMaskBinary = (trace_mask > 0) OR (baseMask == 1)
    //   traceMaskBinary = leftover - dilate(traceMaskBinary, 5)
    //   leftover = removeSmallObjects(traceMaskBinary, 27)
    Stack* legacyLeftover = Copy_Stack(legacyMask);
    ASSERT_NE(legacyLeftover, nullptr);

    Stack* legacyTraceMaskBinary = Make_Stack(GREY, legacyMask->width, legacyMask->height, legacyMask->depth);
    ASSERT_NE(legacyTraceMaskBinary, nullptr);

    const size_t legacyVoxelNumber = static_cast<size_t>(legacyMask->width) * legacyMask->height * legacyMask->depth;
    const auto* legacyTraceMaskLabels =
      reinterpret_cast<const uint16_t*>(legacyTracer.getTraceWorkspace()->trace_mask->array);
    auto* legacyTraceBin = legacyTraceMaskBinary->array;
    const auto* legacyBase = legacyBaseMask->array;
    for (size_t i = 0; i < legacyVoxelNumber; ++i) {
      legacyTraceBin[i] = (legacyTraceMaskLabels[i] > 0 || legacyBase[i] == 1) ? uint8_t{1} : uint8_t{0};
    }

    Stack* legacySubmask = Stack_Z_Dilate(legacyTraceMaskBinary, /*size*/ 5, legacySignal, nullptr);
    ASSERT_NE(legacySubmask, nullptr);
    Stack_Bsub(legacyLeftover, legacySubmask, legacyTraceMaskBinary);
    Kill_Stack(legacySubmask);
    legacySubmask = nullptr;

    Stack_Remove_Small_Object(legacyTraceMaskBinary, legacyLeftover, /*minSize*/ 27, /*connectivity*/ 26);
    C_Stack::translate(legacyLeftover, GREY, 1);
    Kill_Stack(legacyTraceMaskBinary);
    legacyTraceMaskBinary = nullptr;

    if (!Stack_Is_Dark(legacyLeftover)) {
      const double originalMinChainLength = legacyTracer.getTraceWorkspace()->min_chain_length;
      if (legacyTracer.getTraceWorkspace()->refit == _FALSE_) {
        legacyTracer.getTraceWorkspace()->min_chain_length = (NEUROSEG_DEFAULT_H - 1.0) * 2.0 - 1.0;
      } else {
        legacyTracer.getTraceWorkspace()->min_chain_length = (NEUROSEG_DEFAULT_H - 1.0) * 1.5 - 1.0;
      }

      Geo3d_Scalar_Field* legacyRecoverSeeds = extractSeedOriginalLegacyC(legacyLeftover);
      ASSERT_NE(legacyRecoverSeeds, nullptr);
      Kill_Stack(legacyLeftover);
      legacyLeftover = nullptr;

      legacyTracer.prepareTraceScoreThreshold(ZNeuronTracer::TRACING_SEED);
      ZNeuronTraceSeeder legacyRecoverSeeder;
      legacyRecoverBaseMask =
        legacyRecoverSeeder.sortSeed(legacyRecoverSeeds, legacySignal, legacyTracer.getTraceWorkspace());
      ASSERT_NE(legacyRecoverBaseMask, nullptr);
      Kill_Geo3d_Scalar_Field(legacyRecoverSeeds);
      legacyRecoverSeeds = nullptr;

      legacyTracer.prepareTraceScoreThreshold(ZNeuronTracer::TRACING_AUTO);
      std::vector<Local_Neuroseg> legacyRecoverLocsegs = legacyRecoverSeeder.getSeedArray();
      std::vector<double> legacyRecoverScores = legacyRecoverSeeder.getScoreArray();

      int legacyRecoverNchain = 0;
      Locseg_Chain** legacyRecoverChainsRaw = nullptr;
      {
        ScopedStdoutSilencer silence;
        legacyRecoverChainsRaw = Trace_Locseg_S(legacySignal,
                                                /*z_scale*/ 1.0,
                                                legacyRecoverLocsegs.data(),
                                                legacyRecoverScores.data(),
                                                static_cast<int>(legacyRecoverLocsegs.size()),
                                                legacyTracer.getTraceWorkspace(),
                                                &legacyRecoverNchain);
      }
      ASSERT_NE(legacyRecoverChainsRaw, nullptr);
      ASSERT_GE(legacyRecoverNchain, 0);

      legacyRecoveredChains.reserve(static_cast<size_t>(legacyRecoverNchain));
      for (int i = 0; i < legacyRecoverNchain; ++i) {
        legacyRecoveredChains.push_back(legacyRecoverChainsRaw[i]);
      }
      free(legacyRecoverChainsRaw);

      legacyTracer.getTraceWorkspace()->min_chain_length = originalMinChainLength;
    } else {
      Kill_Stack(legacyLeftover);
      legacyLeftover = nullptr;
    }

    const int64_t legacyRecoverMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - legacyRecoverStart)
        .count();

    std::optional<nim::ZImg> portedBaseMaskForRecover = portedSorted.baseMask;
    const double recoverZScale = nim::preferredZToXYRatioFromImgInfoLegacyLike(portedSignal.info());
    const auto portedRecoverStart = std::chrono::steady_clock::now();
    portedRecover = nim::recoverLegacyLike(portedSignal,
                                           portedCfg,
                                           recoverZScale,
                                           portedMask,
                                           std::move(portedBaseMaskForRecover),
                                           portedTw);
    const int64_t portedRecoverMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - portedRecoverStart)
        .count();

    perf.push_back(StagePerf{.name = "recover", .legacyMs = legacyRecoverMs, .portedMs = portedRecoverMs});

    ASSERT_EQ(legacyRecoveredChains.size(), portedRecover.chains.size());

    for (size_t i = 0; i < portedRecover.chains.size(); ++i) {
      const double a = Locseg_Chain_Geolen(legacyRecoveredChains[i]);
      const double b = nim::locsegChainGeolenLegacyLike(*portedRecover.chains[i]);
      if (std::abs(a - b) > 1e-6) {
        ADD_FAILURE() << fmt::format("Recovered chain geolen mismatch at i={}: legacy={} ported={}", i, a, b);
        break;
      }
    }
    if (legacyRecoverBaseMask != nullptr) {
      Kill_Stack(legacyRecoverBaseMask);
    }
  }

  // Combine recovered chains so the reconstruction+SWC postprocessing stages can be compared.
  legacyChains.insert(legacyChains.end(), legacyRecoveredChains.begin(), legacyRecoveredChains.end());
  legacyRecoveredChains.clear();
  for (auto& c : portedRecover.chains) {
    portedChains.push_back(std::move(c));
  }
  portedRecover.chains.clear();

  ZSwcTree* legacyReconTree = nullptr;
  int64_t legacyReconstructMs = 0;
  {
    const auto start = std::chrono::steady_clock::now();
    legacyTracer.initConnectionTestWorkspace();
    Connection_Test_Workspace* legacyCtw = legacyTracer.getConnectionTestWorkspace();
    ASSERT_NE(legacyCtw, nullptr);

    legacyCtw->sp_test = portedCfg.spTest ? _TRUE_ : _FALSE_;
    legacyCtw->crossover_test = portedCfg.crossoverTest ? _TRUE_ : _FALSE_;
    legacyCtw->dist_thre = portedCfg.maxEucDist;
    if (legacyChains.size() > 500) {
      legacyCtw->sp_test = _FALSE_;
    }

    ZNeuronConstructor constructor;
    constructor.setWorkspace(legacyCtw);
    constructor.setSignal(legacySignal);
    legacyReconTree = constructor.reconstruct(legacyChains);

    // Legacy reconstruction frees the Locseg_Chain objects via Clean_Neuron_Component_Array().
    legacyChains.clear();
    legacyReconstructMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  }

  ASSERT_NE(legacyReconTree, nullptr);
  ASSERT_FALSE(legacyReconTree->isEmpty());

  nim::ConnectionTestWorkspaceLegacyLike portedCtw;
  nim::defaultConnectionTestWorkspaceLegacyLike(portedCtw);
  portedCtw.spTest = portedCfg.spTest;
  portedCtw.crossoverTest = portedCfg.crossoverTest;
  portedCtw.distThre = portedCfg.maxEucDist;
  if (portedChains.size() > 500) {
    portedCtw.spTest = false;
  }

  int64_t portedReconstructMs = 0;
  std::unique_ptr<nim::ZSwc> portedReconTree;
  {
    const auto start = std::chrono::steady_clock::now();
    nim::NeuronStructureChainsLegacyLike ns = nim::locsegChainCompNeurostructLegacyLike(portedChains,
                                                                                        &portedSignal,
                                                                                        /*zScale*/ 1.0,
                                                                                        portedCtw);
    nim::processNeuronStructureLegacyLike(ns);
    nim::NeuronStructureCirclesLegacyLike ns2 =
      nim::neuronStructureLocsegChainToCircleSLegacyLike(ns, /*xyScale*/ 1.0, /*zToXYRatio*/ 1.0);
    nim::neuronStructureToTreeLegacyLike(ns2);

    portedReconTree = nim::neuronStructureToSwcTreeCircleZLegacyLike(ns2, /*zScale*/ 1.0);
    portedReconstructMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  }
  ASSERT_TRUE(portedReconTree != nullptr);
  ASSERT_FALSE(portedReconTree->empty());
  perf.push_back(StagePerf{.name = "reconstruct", .legacyMs = legacyReconstructMs, .portedMs = portedReconstructMs});

  // Match `ZNeuronConstructor::reconstruct`: IDs are resorted before SWC post-processing.
  nim::resortId(*portedReconTree);

  bool allStagesMatch = true;
  auto compareStage = [&](std::string_view stageLabel, const ZSwcTree& legacyTree, const nim::ZSwc& portedTree) {
    const fs::path legacyPath = dir / fmt::format("legacy_{}.swc", stageLabel);
    const fs::path portedPath = dir / fmt::format("ported_{}.swc", stageLabel);

    std::unique_ptr<ZSwcTree> legacyCopy(legacyTree.clone());
    legacyCopy->save(legacyPath.string());

    nim::ZSwc portedCopy = portedTree;
    nim::writeSwcLegacyNeuTu(portedCopy, nim::filesystemPathToQString(portedPath), {});

    const std::string legacyText = readTextFile(legacyPath);
    const std::string portedText = readTextFile(portedPath);
    if (legacyText != portedText) {
      const size_t i = firstDiffIndex(legacyText, portedText);
      const auto [line, col] = lineColFromIndex(legacyText, i);
      const std::string legacyLine = lineAt(legacyText, i);
      const std::string portedLine = lineAt(portedText, i);
      ADD_FAILURE() << fmt::format("[{}] SWC mismatch at byte {} (line {}, col {}).\nLegacy: {}\nPorted: {}\n"
                                   "Keeping outputs for inspection under: {}",
                                   stageLabel,
                                   i,
                                   line,
                                   col,
                                   legacyLine,
                                   portedLine,
                                   dir.string());
      allStagesMatch = false;
    }
  };

  compareStage("reconstruct", *legacyReconTree, *portedReconTree);

  if (allStagesMatch) {
    Swc_Tree_Remove_Zigzag(legacyReconTree->data());
    nim::swcTreeRemoveZigzagLegacyLike(*portedReconTree);
    compareStage("remove_zigzag", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Tune_Branch(legacyReconTree->data());
    nim::swcTreeTuneBranchLegacyLike(*portedReconTree);
    compareStage("tune_branch", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Remove_Spur(legacyReconTree->data());
    nim::swcTreeRemoveSpurLegacyLike(*portedReconTree);
    compareStage("remove_spur", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Merge_Close_Node(legacyReconTree->data(), 0.01);
    nim::swcTreeMergeCloseNodeLegacyLike(*portedReconTree, 0.01);
    compareStage("merge_close_node", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Remove_Overshoot(legacyReconTree->data());
    nim::swcTreeRemoveOvershootLegacyLike(*portedReconTree);
    compareStage("remove_overshoot", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    // Match zneurontracer: doResampleAfterTracing=true for auto-trace.
    ZSwcResampler legacyResampler;
    legacyResampler.optimalDownsample(legacyReconTree);

    nim::ZNeutubeSwcResampler portedResampler;
    portedResampler.optimalDownsample(*portedReconTree);
    compareStage("resample", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    ZSwcPruner legacyPruner;
    legacyPruner.setMinLength(0);
    legacyPruner.removeOrphanBlob(legacyReconTree);

    nim::swcTreeRemoveOrphanBlobLegacyLike(*portedReconTree, /*minLength*/ 0.0, /*minOrphanCount*/ 10);
    compareStage("remove_orphan_blob", *legacyReconTree, *portedReconTree);
  }

  delete legacyReconTree;

  Kill_Stack(legacyBaseMask);
  Kill_Geo3d_Scalar_Field(legacyNoisy.seeds);
  Kill_Stack(legacyMask);

  if (allStagesMatch) {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

TEST(NeutubeCommand2Diagnostics, AutoTrace_Slice15_L34_ZToXYRatio5_MatchesLegacy_DevOnly)
{
  constexpr double zToXYRatio = 5.0;
  constexpr double legacyZScale = 1.0 / zToXYRatio;

  const fs::path input =
    fs::path(QDir::homePath().toStdString()) / "Dropbox/atlas_test/slice15/slice15_L34_Sum.lsm_ch2.tif";
  if (!fs::exists(input)) {
    GTEST_SKIP() << "Missing dev-only auto-trace input: " << input.string();
  }

  const fs::path dir = makeUniqueTempDir();
  const DevOnlyAutoTraceConfigPaths cfgPaths = writeDevOnlyAutoTraceConfigFiles(dir);

  nim::TraceConfig portedCfg;
  ASSERT_TRUE(nim::loadTraceConfigLegacyLike(nim::filesystemPathToQString(cfgPaths.traceCfg), portedCfg));
  ASSERT_EQ(portedCfg.seedMethod, 1) << "This diagnostic helper currently expects seedMethod=1.";

  ZStack legacySignalStack;
  ASSERT_TRUE(legacySignalStack.load(input.string()));
  Stack* legacySignal = legacySignalStack.c_stack(0);
  ASSERT_NE(legacySignal, nullptr);

  nim::ZImg portedSignal;
  portedSignal.load(nim::filesystemPathToQString(input));
  ASSERT_FALSE(portedSignal.isEmpty());
  ASSERT_EQ(portedSignal.numChannels(), 1);
  ASSERT_EQ(portedSignal.numTimes(), 1);

  ZNeuronTracerConfig::getInstance().load(cfgPaths.traceCfg.string());

  {
    ScopedStdoutSilencer silence;
    ZNeuronTracer legacyTracer;
    legacyTracer.setIntensityField(&legacySignalStack);
    if (legacyTracer._preprocess) {
      legacyTracer._preprocess(legacySignal);
    }
  }
  (void)nim::subtractBackgroundLegacyLike(portedSignal, /*minFr*/ 0.5, /*maxIter*/ 3);

  Stack* legacyMask = nullptr;
  {
    ScopedStdoutSilencer silence;
    ZNeuronTracer legacyTracer;
    legacyTracer.setIntensityField(&legacySignalStack);
    legacyMask = legacyTracer.makeMask(legacySignal);
  }
  ASSERT_NE(legacyMask, nullptr);

  std::optional<nim::ZImg> portedMaskOpt = nim::makeMaskLegacyLike(portedSignal, portedCfg, nullptr);
  ASSERT_TRUE(portedMaskOpt.has_value());
  nim::ZImg portedMask = std::move(*portedMaskOpt);

  {
    SCOPED_TRACE("mask");
    const VolumeDigest legacyMaskDigest = digestLegacyStackU8OrU16(legacyMask);
    const VolumeDigest portedMaskDigest = digestZImgU8OrU16(portedMask);
    ASSERT_EQ(legacyMaskDigest.hash, portedMaskDigest.hash)
      << fmt::format("legacy nonzero={} ported nonzero={}", legacyMaskDigest.nonzero, portedMaskDigest.nonzero);
  }

  Geo3d_Scalar_Field* legacySeeds0 = extractSeedOriginalLegacyC(legacyMask);
  ASSERT_NE(legacySeeds0, nullptr);
  nim::Geo3dScalarField portedSeeds0 = nim::extractSeedOriginalLegacyLike(portedMask);

  {
    SCOPED_TRACE("seed_extract_pre_noise");
    const auto legacySeedsVec = toIntSeeds(legacySeeds0);
    const auto portedSeedsVec = toIntSeeds(portedSeeds0);
    ASSERT_EQ(legacySeedsVec.size(), portedSeedsVec.size());
    for (size_t i = 0; i < legacySeedsVec.size(); ++i) {
      const auto& a = legacySeedsVec[i];
      const auto& b = portedSeedsVec[i];
      if (a.x != b.x || a.y != b.y || a.z != b.z || std::abs(a.value - b.value) > 1e-12) {
        ADD_FAILURE() << fmt::format("Seed mismatch at i={}: legacy=({}, {}, {}, {}) ported=({}, {}, {}, {})",
                                     i,
                                     a.x,
                                     a.y,
                                     a.z,
                                     a.value,
                                     b.x,
                                     b.y,
                                     b.z,
                                     b.value);
        break;
      }
    }
  }

  LegacyRemoveNoisySeedResult legacyNoisy =
    removeNoisySeedLegacyC(legacySeeds0, legacyMask, /*seedMethod*/ portedCfg.seedMethod, /*screeningSeed*/ true);
  ASSERT_NE(legacyNoisy.seeds, nullptr);
  ASSERT_NE(legacyNoisy.mask, nullptr);
  legacyMask = legacyNoisy.mask;

  nim::Geo3dScalarField portedSeeds1 = nim::removeNoisySeedLegacyLike(std::move(portedSeeds0),
                                                                      portedMask,
                                                                      portedCfg.seedMethod,
                                                                      /*screeningSeed*/ true,
                                                                      nullptr);

  {
    SCOPED_TRACE("seed_extract_post_noise");
    const auto legacySeedsVec = toIntSeeds(legacyNoisy.seeds);
    const auto portedSeedsVec = toIntSeeds(portedSeeds1);
    ASSERT_EQ(legacySeedsVec.size(), portedSeedsVec.size());
    for (size_t i = 0; i < legacySeedsVec.size(); ++i) {
      const auto& a = legacySeedsVec[i];
      const auto& b = portedSeedsVec[i];
      if (a.x != b.x || a.y != b.y || a.z != b.z || std::abs(a.value - b.value) > 1e-12) {
        ADD_FAILURE() << fmt::format("Seed mismatch at i={}: legacy=({}, {}, {}, {}) ported=({}, {}, {}, {})",
                                     i,
                                     a.x,
                                     a.y,
                                     a.z,
                                     a.value,
                                     b.x,
                                     b.y,
                                     b.z,
                                     b.value);
        break;
      }
    }
  }

  Trace_Workspace* legacyTw = Locseg_Chain_Default_Trace_Workspace(nullptr, legacySignal);
  ASSERT_NE(legacyTw, nullptr);
  legacyTw->refit = portedCfg.refit ? _TRUE_ : _FALSE_;
  legacyTw->tune_end = portedCfg.tuneEnd ? _TRUE_ : _FALSE_;
  legacyTw->trace_mask_updating = _TRUE_;
  legacyTw->trace_mask = Make_Stack(GREY16, legacySignal->width, legacySignal->height, legacySignal->depth);
  ASSERT_NE(legacyTw->trace_mask, nullptr);
  Zero_Stack(legacyTw->trace_mask);

  LegacySeedSortResult legacySeedSort = sortSeedsLegacyGuiLikeWithZScale(legacyNoisy.seeds,
                                                                         legacySignal,
                                                                         legacyZScale,
                                                                         portedCfg.minSeedScore,
                                                                         legacyTw->trace_mask);
  ASSERT_NE(legacySeedSort.baseMask, nullptr);

  nim::TraceWorkspace portedTw;
  nim::locsegChainDefaultTraceWorkspaceLegacyLike(portedTw, portedSignal);
  portedTw.refit = portedCfg.refit;
  portedTw.tuneEnd = portedCfg.tuneEnd;
  portedTw.traceMaskUpdating = true;
  nim::traceWorkspaceInitTraceMaskLegacyLike(portedTw, portedSignal, /*clearing*/ false);
  nim::prepareTraceScoreThresholdLegacyLike(portedSignal, portedCfg, nim::TracingModeLegacyLike::Seed, portedTw);
  nim::SeedSortResultLegacyLike portedSorted =
    nim::sortSeedsLegacyLike(portedSeeds1, portedSignal, zToXYRatio, portedTw);

  {
    const VolumeDigest legacyBaseDigest = digestLegacyStackU8OrU16(legacySeedSort.baseMask);
    const VolumeDigest portedBaseDigest = digestZImgU8OrU16(portedSorted.baseMask);
    LOG(INFO) << fmt::format("Slice15 zToXYRatio=5 seed sort base mask: legacy hash={} nonzero={}, "
                             "ported hash={} nonzero={}",
                             legacyBaseDigest.hash,
                             legacyBaseDigest.nonzero,
                             portedBaseDigest.hash,
                             portedBaseDigest.nonzero);
  }

  {
    SCOPED_TRACE("seed_sort_base_mask");
    const size_t voxelNumber = portedSorted.baseMask.voxelNumber();
    ASSERT_EQ(static_cast<size_t>(legacySeedSort.baseMask->width) *
                static_cast<size_t>(legacySeedSort.baseMask->height) *
                static_cast<size_t>(legacySeedSort.baseMask->depth),
              voxelNumber);
    const auto* legacyBase = legacySeedSort.baseMask->array;
    const auto* portedBase = portedSorted.baseMask.timeData<uint8_t>(0);
    for (size_t i = 0; i < voxelNumber; ++i) {
      if (legacyBase[i] != portedBase[i]) {
        const size_t width = static_cast<size_t>(portedSignal.width());
        const size_t height = static_cast<size_t>(portedSignal.height());
        const size_t plane = width * height;
        const size_t z = i / plane;
        const size_t rem = i % plane;
        const size_t y = rem / width;
        const size_t x = rem % width;
        ADD_FAILURE() << fmt::format("Base mask mismatch at ({}, {}, {}): legacy={} ported={}",
                                     x,
                                     y,
                                     z,
                                     static_cast<int>(legacyBase[i]),
                                     static_cast<int>(portedBase[i]));
        break;
      }
    }
  }

  {
    SCOPED_TRACE("seed_sort_scores");
    ASSERT_EQ(legacySeedSort.scoreArray.size(), portedSorted.scoreArray.size());
    for (size_t i = 0; i < legacySeedSort.scoreArray.size(); ++i) {
      if (legacySeedSort.scoreArray[i] != portedSorted.scoreArray[i]) {
        const auto& legacyLocseg = legacySeedSort.locsegArray[i];
        const auto& portedLocseg = portedSorted.locsegArray[i];
        ADD_FAILURE() << fmt::format("Seed score mismatch at i={}: legacyScore={} portedScore={} "
                                     "legacyPos=({}, {}, {}) portedPos=({}, {}, {})",
                                     i,
                                     legacySeedSort.scoreArray[i],
                                     portedSorted.scoreArray[i],
                                     legacyLocseg.pos[0],
                                     legacyLocseg.pos[1],
                                     legacyLocseg.pos[2],
                                     portedLocseg.pos[0],
                                     portedLocseg.pos[1],
                                     portedLocseg.pos[2]);
        break;
      }
    }
  }

  CHECK(legacySeedSort.scoreArray.size() == portedSorted.scoreArray.size());

  legacyTw->min_score = portedCfg.minAutoScore;
  nim::prepareTraceScoreThresholdLegacyLike(portedSignal, portedCfg, nim::TracingModeLegacyLike::Auto, portedTw);

  int legacyNchain = 0;
  Locseg_Chain** legacyChainsRaw = nullptr;
  {
    ScopedStdoutSilencer silence;
    legacyChainsRaw = Trace_Locseg_S(legacySignal,
                                     legacyZScale,
                                     legacySeedSort.locsegArray.data(),
                                     legacySeedSort.scoreArray.data(),
                                     static_cast<int>(legacySeedSort.locsegArray.size()),
                                     legacyTw,
                                     &legacyNchain);
  }
  ASSERT_NE(legacyChainsRaw, nullptr);
  ASSERT_GE(legacyNchain, 0);

  std::vector<Locseg_Chain*> legacyChains;
  legacyChains.reserve(static_cast<size_t>(legacyNchain));
  for (int i = 0; i < legacyNchain; ++i) {
    legacyChains.push_back(legacyChainsRaw[i]);
  }
  free(legacyChainsRaw);

  std::vector<std::unique_ptr<nim::LocsegChain>> portedChains;
  {
    ScopedStdoutSilencer silence;
    portedChains = nim::traceAllSeedsLegacyLike(portedSignal,
                                                zToXYRatio,
                                                portedSorted.locsegArray,
                                                portedSorted.scoreArray,
                                                portedTw);
  }

  LOG(INFO) << fmt::format("Slice15 zToXYRatio=5 trace_all_seeds: legacy chains={} ported chains={}",
                           legacyNchain,
                           portedChains.size());

  ASSERT_EQ(static_cast<size_t>(legacyNchain), portedChains.size());

  {
    SCOPED_TRACE("trace_all_seeds_trace_mask_binary");
    ASSERT_TRUE(portedTw.traceMask != nullptr);
    ASSERT_TRUE(portedTw.traceMask->isType<uint8_t>());
    const size_t voxelNumber = portedTw.traceMask->voxelNumber();
    ASSERT_EQ(static_cast<size_t>(legacyTw->trace_mask->width) * static_cast<size_t>(legacyTw->trace_mask->height) *
                static_cast<size_t>(legacyTw->trace_mask->depth),
              voxelNumber);
    const auto* legacyTraceMask = reinterpret_cast<const uint16_t*>(legacyTw->trace_mask->array);
    const auto* portedTraceMask = portedTw.traceMask->timeData<uint8_t>(0);
    for (size_t i = 0; i < voxelNumber; ++i) {
      const uint8_t legacyValue = legacyTraceMask[i] > 0 ? uint8_t{1} : uint8_t{0};
      const uint8_t portedValue = portedTraceMask[i] > 0 ? uint8_t{1} : uint8_t{0};
      if (legacyValue != portedValue) {
        const size_t width = static_cast<size_t>(portedSignal.width());
        const size_t height = static_cast<size_t>(portedSignal.height());
        const size_t plane = width * height;
        const size_t z = i / plane;
        const size_t rem = i % plane;
        const size_t y = rem / width;
        const size_t x = rem % width;
        ADD_FAILURE() << fmt::format("Trace mask mismatch at ({}, {}, {}): legacy={} ported={}",
                                     x,
                                     y,
                                     z,
                                     static_cast<int>(legacyValue),
                                     static_cast<int>(portedValue));
        break;
      }
    }
  }

  for (size_t i = 0; i < portedChains.size(); ++i) {
    const double legacyGeolen = Locseg_Chain_Geolen(legacyChains[i]);
    const double portedGeolen = nim::locsegChainGeolenLegacyLike(*portedChains[i]);
    if (std::abs(legacyGeolen - portedGeolen) > 1e-6) {
      ADD_FAILURE() << fmt::format("Slice15 zToXYRatio=5 chain geolen mismatch at i={}: legacy={} ported={}",
                                   i,
                                   legacyGeolen,
                                   portedGeolen);
      break;
    }
  }

  std::optional<LegacySeedSortResult> legacyRecoverSort;
  std::vector<Locseg_Chain*> legacyRecoveredChains;
  nim::RecoverResultLegacyLike portedRecover;
  std::optional<nim::ZImg> portedRecoverLeftover;

  if (portedCfg.recover > 0) {
    Stack* legacyLeftover = Copy_Stack(legacyMask);
    ASSERT_NE(legacyLeftover, nullptr);

    Stack* legacyTraceMaskBinary = Make_Stack(GREY, legacyMask->width, legacyMask->height, legacyMask->depth);
    ASSERT_NE(legacyTraceMaskBinary, nullptr);

    const size_t legacyVoxelNumber = static_cast<size_t>(legacyMask->width) * legacyMask->height * legacyMask->depth;
    const auto* legacyTraceMaskLabels = reinterpret_cast<const uint16_t*>(legacyTw->trace_mask->array);
    auto* legacyTraceBin = legacyTraceMaskBinary->array;
    const auto* legacyBase = legacySeedSort.baseMask->array;
    for (size_t i = 0; i < legacyVoxelNumber; ++i) {
      legacyTraceBin[i] = (legacyTraceMaskLabels[i] > 0 || legacyBase[i] == 1) ? uint8_t{1} : uint8_t{0};
    }

    Stack* legacySubmask = Stack_Z_Dilate(legacyTraceMaskBinary, /*size*/ 5, legacySignal, nullptr);
    ASSERT_NE(legacySubmask, nullptr);
    Stack_Bsub(legacyLeftover, legacySubmask, legacyTraceMaskBinary);
    Kill_Stack(legacySubmask);
    legacySubmask = nullptr;

    Stack_Remove_Small_Object(legacyTraceMaskBinary, legacyLeftover, /*minSize*/ 27, /*connectivity*/ 26);
    C_Stack::translate(legacyLeftover, GREY, 1);
    Kill_Stack(legacyTraceMaskBinary);
    legacyTraceMaskBinary = nullptr;

    if (!Stack_Is_Dark(legacyLeftover)) {
      const double originalMinChainLength = legacyTw->min_chain_length;
      if (legacyTw->refit == _FALSE_) {
        legacyTw->min_chain_length = (NEUROSEG_DEFAULT_H - 1.0) * 2.0 - 1.0;
      } else {
        legacyTw->min_chain_length = (NEUROSEG_DEFAULT_H - 1.0) * 1.5 - 1.0;
      }

      {
        nim::ZImgInfo leftoverInfo(static_cast<size_t>(legacyLeftover->width),
                                   static_cast<size_t>(legacyLeftover->height),
                                   static_cast<size_t>(legacyLeftover->depth),
                                   1,
                                   1,
                                   1,
                                   nim::VoxelFormat::Unsigned);
        leftoverInfo.setVoxelFormat<uint8_t>();
        leftoverInfo.createDefaultDescriptions();
        portedRecoverLeftover.emplace(leftoverInfo);
        std::copy_n(legacyLeftover->array,
                    portedRecoverLeftover->voxelNumber(),
                    portedRecoverLeftover->timeData<uint8_t>(0));
      }

      Geo3d_Scalar_Field* legacyRecoverSeeds = extractSeedOriginalLegacyC(legacyLeftover);
      ASSERT_NE(legacyRecoverSeeds, nullptr);
      Kill_Stack(legacyLeftover);
      legacyLeftover = nullptr;

      legacyRecoverSort = sortSeedsLegacyGuiLikeWithZScale(legacyRecoverSeeds,
                                                           legacySignal,
                                                           legacyZScale,
                                                           portedCfg.minSeedScore,
                                                           legacyTw->trace_mask);
      const auto legacyRecoverSeedPoints = toIntSeeds(legacyRecoverSeeds);
      Kill_Geo3d_Scalar_Field(legacyRecoverSeeds);
      legacyRecoverSeeds = nullptr;

      {
        ASSERT_TRUE(portedRecoverLeftover.has_value());
        nim::prepareTraceScoreThresholdLegacyLike(portedSignal, portedCfg, nim::TracingModeLegacyLike::Seed, portedTw);
        nim::Geo3dScalarField portedRecoverSeeds0 = nim::extractSeedOriginalLegacyLike(*portedRecoverLeftover);
        {
          SCOPED_TRACE("recover_seed_extract_same_leftover");
          const auto portedRecoverSeedPoints = toIntSeeds(portedRecoverSeeds0);
          ASSERT_EQ(legacyRecoverSeedPoints.size(), portedRecoverSeedPoints.size());
          for (size_t seedIndex = 0; seedIndex < legacyRecoverSeedPoints.size(); ++seedIndex) {
            const auto& legacySeedPoint = legacyRecoverSeedPoints[seedIndex];
            const auto& portedSeedPoint = portedRecoverSeedPoints[seedIndex];
            if (legacySeedPoint.x != portedSeedPoint.x || legacySeedPoint.y != portedSeedPoint.y ||
                legacySeedPoint.z != portedSeedPoint.z ||
                std::abs(legacySeedPoint.value - portedSeedPoint.value) > 1e-12) {
              ADD_FAILURE() << fmt::format(
                "Recover seed extract mismatch at i={}: legacy=({}, {}, {}, {}) ported=({}, {}, {}, {})",
                seedIndex,
                legacySeedPoint.x,
                legacySeedPoint.y,
                legacySeedPoint.z,
                legacySeedPoint.value,
                portedSeedPoint.x,
                portedSeedPoint.y,
                portedSeedPoint.z,
                portedSeedPoint.value);
              break;
            }
          }
        }
        nim::SeedSortResultLegacyLike portedRecoverSorted =
          nim::sortSeedsLegacyLike(portedRecoverSeeds0, portedSignal, zToXYRatio, portedTw);

        SCOPED_TRACE("recover_seed_sort_base_mask_same_leftover");
        const size_t voxelNumber = portedRecoverSorted.baseMask.voxelNumber();
        ASSERT_EQ(static_cast<size_t>(legacyRecoverSort->baseMask->width) *
                    static_cast<size_t>(legacyRecoverSort->baseMask->height) *
                    static_cast<size_t>(legacyRecoverSort->baseMask->depth),
                  voxelNumber);
        const auto* legacyRecoverBaseMask = legacyRecoverSort->baseMask->array;
        const auto* portedRecoverBaseMask = portedRecoverSorted.baseMask.timeData<uint8_t>(0);
        std::optional<std::array<size_t, 3>> recoverBaseMaskMismatch;
        for (size_t i = 0; i < voxelNumber; ++i) {
          if (legacyRecoverBaseMask[i] != portedRecoverBaseMask[i]) {
            const size_t width = static_cast<size_t>(portedSignal.width());
            const size_t height = static_cast<size_t>(portedSignal.height());
            const size_t plane = width * height;
            const size_t z = i / plane;
            const size_t rem = i % plane;
            const size_t y = rem / width;
            const size_t x = rem % width;
            recoverBaseMaskMismatch = std::array<size_t, 3>{x, y, z};
            ADD_FAILURE() << fmt::format(
              "Recover seed sort mismatch on shared leftover at ({}, {}, {}): legacy={} ported={}",
              x,
              y,
              z,
              static_cast<int>(legacyRecoverBaseMask[i]),
              static_cast<int>(portedRecoverBaseMask[i]));
            break;
          }
        }

        ASSERT_EQ(legacyRecoverSort->scoreArray.size(), portedRecoverSorted.scoreArray.size());
        for (size_t i = 0; i < legacyRecoverSort->scoreArray.size(); ++i) {
          if (legacyRecoverSort->scoreArray[i] != portedRecoverSorted.scoreArray[i]) {
            const auto& legacyLocseg = legacyRecoverSort->locsegArray[i];
            const auto& portedLocseg = portedRecoverSorted.locsegArray[i];
            const bool legacyHit = legacyGuiHitMaskWithZScale(legacyLocseg, legacyTw->trace_mask, legacyZScale);
            const bool portedHit = nim::localNeurosegHitMaskLegacyLike(portedLocseg, *portedTw.traceMask, zToXYRatio);
            ADD_FAILURE() << fmt::format(
              "Recover seed score mismatch on shared leftover at i={}: legacyScore={} portedScore={} "
              "legacyHit={} portedHit={} legacyPos=({}, {}, {}) portedPos=({}, {}, {})",
              i,
              legacyRecoverSort->scoreArray[i],
              portedRecoverSorted.scoreArray[i],
              legacyHit,
              portedHit,
              legacyLocseg.pos[0],
              legacyLocseg.pos[1],
              legacyLocseg.pos[2],
              portedLocseg.pos[0],
              portedLocseg.pos[1],
              portedLocseg.pos[2]);

            if (i < legacyRecoverSeedPoints.size()) {
              const auto& legacySeedPoint = legacyRecoverSeedPoints[i];
              const int sx = legacySeedPoint.x;
              const int sy = legacySeedPoint.y;
              const int sz = legacySeedPoint.z;

              Local_Neuroseg legacyPreparedLocseg;
              Set_Neuroseg(&(legacyPreparedLocseg.seg),
                           legacySeedPoint.value < 3.0 ? legacySeedPoint.value + 0.5 : legacySeedPoint.value,
                           0.0,
                           NEUROSEG_DEFAULT_H,
                           0.0,
                           0.0,
                           0.0,
                           0.0,
                           1.0);
              double preparedPos[3] = {static_cast<double>(sx),
                                       static_cast<double>(sy),
                                       static_cast<double>(sz) / legacyZScale};
              Set_Neuroseg_Position(&legacyPreparedLocseg, preparedPos, NEUROSEG_CENTER);

              Locseg_Fit_Workspace* legacyPrepareFws = New_Locseg_Fit_Workspace();
              ASSERT_NE(legacyPrepareFws, nullptr);
              legacyPrepareFws->sws->fs.n = 2;
              legacyPrepareFws->sws->fs.options[0] = STACK_FIT_DOT;
              legacyPrepareFws->sws->fs.options[1] = STACK_FIT_CORRCOEF;
              legacyPrepareFws->pos_adjust = 1;
              Local_Neuroseg_Optimize_W(&legacyPreparedLocseg, legacySignal, legacyZScale, 0, legacyPrepareFws);
              const bool legacyPreparedHit =
                legacyGuiHitMaskWithZScale(legacyPreparedLocseg, legacyTw->trace_mask, legacyZScale);
              const double legacyPreparedScore = legacyPrepareFws->sws->fs.scores[1];
              Kill_Locseg_Fit_Workspace(legacyPrepareFws);

              nim::LocalNeuroseg portedPreparedLocseg;
              portedPreparedLocseg.seg.r1 =
                legacySeedPoint.value < 3.0 ? legacySeedPoint.value + 0.5 : legacySeedPoint.value;
              portedPreparedLocseg.seg.c = 0.0;
              portedPreparedLocseg.seg.h = nim::NeurosegDefaultHLegacyLike;
              portedPreparedLocseg.seg.theta = 0.0;
              portedPreparedLocseg.seg.psi = 0.0;
              portedPreparedLocseg.seg.curvature = 0.0;
              portedPreparedLocseg.seg.alpha = 0.0;
              portedPreparedLocseg.seg.scale = 1.0;
              nim::setNeurosegPositionLegacyLike(
                portedPreparedLocseg,
                {static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) * zToXYRatio},
                nim::NeuroposReferenceLegacyLike::Center);
              nim::LocsegFitWorkspace portedPrepareFws = portedTw.fitWorkspace;
              const double portedPreparedScore = nim::localNeurosegOptimizeWLegacyLike(portedPreparedLocseg,
                                                                                       portedSignal,
                                                                                       zToXYRatio,
                                                                                       0,
                                                                                       portedPrepareFws);
              const bool portedPreparedHit =
                nim::localNeurosegHitMaskLegacyLike(portedPreparedLocseg, *portedTw.traceMask, zToXYRatio);

              ADD_FAILURE() << fmt::format(
                "Recover prepared seed {}: voxel=({}, {}, {}) legacySeedValue={} legacyPreparedScore={} "
                "portedPreparedScore={} legacyPreparedHit={} portedPreparedHit={} "
                "legacyPreparedPos=({}, {}, {}) portedPreparedPos=({}, {}, {})",
                i,
                sx,
                sy,
                sz,
                legacySeedPoint.value,
                legacyPreparedScore,
                portedPreparedScore,
                legacyPreparedHit,
                portedPreparedHit,
                legacyPreparedLocseg.pos[0],
                legacyPreparedLocseg.pos[1],
                legacyPreparedLocseg.pos[2],
                portedPreparedLocseg.pos[0],
                portedPreparedLocseg.pos[1],
                portedPreparedLocseg.pos[2]);
            }
            if (recoverBaseMaskMismatch.has_value()) {
              const size_t mx = (*recoverBaseMaskMismatch)[0];
              const size_t my = (*recoverBaseMaskMismatch)[1];
              const size_t mz = (*recoverBaseMaskMismatch)[2];
              for (size_t candidateIndex = 0; candidateIndex < i; ++candidateIndex) {
                const int legacyLabelValue =
                  (legacyRecoverSort->scoreArray[candidateIndex] > portedCfg.minSeedScore) ? 2 : 1;
                const int portedLabelValue =
                  (portedRecoverSorted.scoreArray[candidateIndex] > portedCfg.minSeedScore) ? 2 : 1;

                Stack* legacyProbe = Make_Stack(GREY, legacySignal->width, legacySignal->height, legacySignal->depth);
                ASSERT_NE(legacyProbe, nullptr);
                Zero_Stack(legacyProbe);

                nim::ZImgInfo probeInfo(portedSignal.width(),
                                        portedSignal.height(),
                                        portedSignal.depth(),
                                        1,
                                        1,
                                        1,
                                        nim::VoxelFormat::Unsigned);
                probeInfo.setVoxelFormat<uint8_t>();
                probeInfo.createDefaultDescriptions();
                nim::ZImg portedProbe(probeInfo);
                portedProbe.fill(0);

                if (legacyRecoverSort->scoreArray[candidateIndex] > 0.0) {
                  Local_Neuroseg_Label_G(&legacyRecoverSort->locsegArray[candidateIndex],
                                         legacyProbe,
                                         -1,
                                         legacyLabelValue,
                                         legacyZScale);
                }
                if (portedRecoverSorted.scoreArray[candidateIndex] > 0.0) {
                  nim::localNeurosegLabelGLegacyLike(portedRecoverSorted.locsegArray[candidateIndex],
                                                     portedProbe,
                                                     -1,
                                                     portedLabelValue,
                                                     zToXYRatio);
                }

                const size_t probeIndex =
                  mx + my * static_cast<size_t>(portedSignal.width()) +
                  mz * static_cast<size_t>(portedSignal.width()) * static_cast<size_t>(portedSignal.height());
                const int legacyProbeValue = static_cast<int>(legacyProbe->array[probeIndex]);
                const int portedProbeValue = static_cast<int>(portedProbe.timeData<uint8_t>(0)[probeIndex]);
                if (legacyProbeValue != portedProbeValue) {
                  ADD_FAILURE() << fmt::format(
                    "Recover seed {} labels mismatch voxel ({}, {}, {}): legacy={} ported={} "
                    "legacyScore={} portedScore={}",
                    candidateIndex,
                    mx,
                    my,
                    mz,
                    legacyProbeValue,
                    portedProbeValue,
                    legacyRecoverSort->scoreArray[candidateIndex],
                    portedRecoverSorted.scoreArray[candidateIndex]);
                  Kill_Stack(legacyProbe);
                  break;
                }

                Kill_Stack(legacyProbe);
              }
            }
            break;
          }
        }

        {
          SCOPED_TRACE("recover_seed_sort_cumulative_label_prefix");
          const size_t prefixCount = std::min<size_t>(8, legacyRecoverSort->scoreArray.size());
          Stack* legacyPrefixMask = Make_Stack(GREY, legacySignal->width, legacySignal->height, legacySignal->depth);
          ASSERT_NE(legacyPrefixMask, nullptr);
          Zero_Stack(legacyPrefixMask);

          nim::ZImgInfo prefixInfo(portedSignal.width(),
                                   portedSignal.height(),
                                   portedSignal.depth(),
                                   1,
                                   1,
                                   1,
                                   nim::VoxelFormat::Unsigned);
          prefixInfo.setVoxelFormat<uint8_t>();
          prefixInfo.createDefaultDescriptions();
          nim::ZImg portedPrefixMask(prefixInfo);
          portedPrefixMask.fill(0);

          for (size_t prefixIndex = 0; prefixIndex < prefixCount; ++prefixIndex) {
            if (legacyRecoverSort->scoreArray[prefixIndex] > 0.0) {
              const int labelValue = (legacyRecoverSort->scoreArray[prefixIndex] > portedCfg.minSeedScore) ? 2 : 1;
              Local_Neuroseg_Label_G(&legacyRecoverSort->locsegArray[prefixIndex],
                                     legacyPrefixMask,
                                     -1,
                                     labelValue,
                                     legacyZScale);
            }
            if (portedRecoverSorted.scoreArray[prefixIndex] > 0.0) {
              const int labelValue = (portedRecoverSorted.scoreArray[prefixIndex] > portedCfg.minSeedScore) ? 2 : 1;
              nim::localNeurosegLabelGLegacyLike(portedRecoverSorted.locsegArray[prefixIndex],
                                                 portedPrefixMask,
                                                 -1,
                                                 labelValue,
                                                 zToXYRatio);
            }

            const VolumeDigest legacyPrefixDigest = digestLegacyStackU8OrU16(legacyPrefixMask);
            const VolumeDigest portedPrefixDigest = digestZImgU8OrU16(portedPrefixMask);
            if (legacyPrefixDigest.hash != portedPrefixDigest.hash) {
              ADD_FAILURE() << fmt::format(
                "Recover prefix label mismatch after seed {}: legacyHash={} legacyNonzero={} "
                "portedHash={} portedNonzero={} legacyScore={} portedScore={} "
                "legacyPos=({}, {}, {}) portedPos=({}, {}, {})",
                prefixIndex,
                legacyPrefixDigest.hash,
                legacyPrefixDigest.nonzero,
                portedPrefixDigest.hash,
                portedPrefixDigest.nonzero,
                legacyRecoverSort->scoreArray[prefixIndex],
                portedRecoverSorted.scoreArray[prefixIndex],
                legacyRecoverSort->locsegArray[prefixIndex].pos[0],
                legacyRecoverSort->locsegArray[prefixIndex].pos[1],
                legacyRecoverSort->locsegArray[prefixIndex].pos[2],
                portedRecoverSorted.locsegArray[prefixIndex].pos[0],
                portedRecoverSorted.locsegArray[prefixIndex].pos[1],
                portedRecoverSorted.locsegArray[prefixIndex].pos[2]);
              break;
            }
          }

          Kill_Stack(legacyPrefixMask);
        }
      }

      legacyTw->min_score = portedCfg.minAutoScore;

      int legacyRecoverNchain = 0;
      Locseg_Chain** legacyRecoverChainsRaw = nullptr;
      {
        ScopedStdoutSilencer silence;
        legacyRecoverChainsRaw = Trace_Locseg_S(legacySignal,
                                                legacyZScale,
                                                legacyRecoverSort->locsegArray.data(),
                                                legacyRecoverSort->scoreArray.data(),
                                                static_cast<int>(legacyRecoverSort->locsegArray.size()),
                                                legacyTw,
                                                &legacyRecoverNchain);
      }
      ASSERT_NE(legacyRecoverChainsRaw, nullptr);
      ASSERT_GE(legacyRecoverNchain, 0);

      legacyRecoveredChains.reserve(static_cast<size_t>(legacyRecoverNchain));
      for (int i = 0; i < legacyRecoverNchain; ++i) {
        legacyRecoveredChains.push_back(legacyRecoverChainsRaw[i]);
      }
      free(legacyRecoverChainsRaw);

      legacyTw->min_chain_length = originalMinChainLength;
    } else {
      Kill_Stack(legacyLeftover);
      legacyLeftover = nullptr;
    }

    std::optional<nim::ZImg> portedBaseMaskForRecover = portedSorted.baseMask;
    portedRecover = nim::recoverLegacyLike(portedSignal,
                                           portedCfg,
                                           zToXYRatio,
                                           portedMask,
                                           std::move(portedBaseMaskForRecover),
                                           portedTw);

    LOG(INFO) << fmt::format("Slice15 zToXYRatio=5 recover: legacy chains={} ported chains={}",
                             legacyRecoveredChains.size(),
                             portedRecover.chains.size());

    {
      SCOPED_TRACE("recover_seed_sort_base_mask");
      ASSERT_TRUE(legacyRecoverSort.has_value());
      ASSERT_TRUE(portedRecover.baseMask.has_value());
      const size_t voxelNumber = portedRecover.baseMask->voxelNumber();
      ASSERT_EQ(static_cast<size_t>(legacyRecoverSort->baseMask->width) *
                  static_cast<size_t>(legacyRecoverSort->baseMask->height) *
                  static_cast<size_t>(legacyRecoverSort->baseMask->depth),
                voxelNumber);
      const auto* legacyRecoverBaseMask = legacyRecoverSort->baseMask->array;
      const auto* portedRecoverBaseMask = portedRecover.baseMask->timeData<uint8_t>(0);
      for (size_t i = 0; i < voxelNumber; ++i) {
        if (legacyRecoverBaseMask[i] != portedRecoverBaseMask[i]) {
          const size_t width = static_cast<size_t>(portedSignal.width());
          const size_t height = static_cast<size_t>(portedSignal.height());
          const size_t plane = width * height;
          const size_t z = i / plane;
          const size_t rem = i % plane;
          const size_t y = rem / width;
          const size_t x = rem % width;
          ADD_FAILURE() << fmt::format("Recover base mask mismatch at ({}, {}, {}): legacy={} ported={}",
                                       x,
                                       y,
                                       z,
                                       static_cast<int>(legacyRecoverBaseMask[i]),
                                       static_cast<int>(portedRecoverBaseMask[i]));
          break;
        }
      }
    }

    ASSERT_EQ(legacyRecoveredChains.size(), portedRecover.chains.size());

    for (size_t i = 0; i < portedRecover.chains.size(); ++i) {
      const double legacyGeolen = Locseg_Chain_Geolen(legacyRecoveredChains[i]);
      const double portedGeolen = nim::locsegChainGeolenLegacyLike(*portedRecover.chains[i]);
      if (std::abs(legacyGeolen - portedGeolen) > 1e-6) {
        ADD_FAILURE() << fmt::format("Slice15 zToXYRatio=5 recovered chain geolen mismatch at i={}: legacy={} "
                                     "ported={}",
                                     i,
                                     legacyGeolen,
                                     portedGeolen);
        break;
      }
    }
  }

  legacyChains.insert(legacyChains.end(), legacyRecoveredChains.begin(), legacyRecoveredChains.end());
  legacyRecoveredChains.clear();
  for (auto& c : portedRecover.chains) {
    portedChains.push_back(std::move(c));
  }
  portedRecover.chains.clear();

  Connection_Test_Workspace* legacyCtw = New_Connection_Test_Workspace();
  ASSERT_NE(legacyCtw, nullptr);
  legacyCtw->sp_test = portedCfg.spTest ? _TRUE_ : _FALSE_;
  legacyCtw->crossover_test = portedCfg.crossoverTest ? _TRUE_ : _FALSE_;
  legacyCtw->dist_thre = portedCfg.maxEucDist;
  // Match `ZNeuronConstructor::reconstruct()` in the zrunneutucommand pipeline:
  // reconstruction runs on already-isotropized chains, so connection testing
  // stays in chain space with isotropic resolution.
  legacyCtw->resolution[0] = 1.0;
  legacyCtw->resolution[1] = 1.0;
  legacyCtw->resolution[2] = 1.0;
  if (legacyChains.size() > 500) {
    legacyCtw->sp_test = _FALSE_;
  }

  Neuron_Component* legacyChainArray = Make_Neuron_Component_Array(static_cast<int>(legacyChains.size()));
  ASSERT_NE(legacyChainArray, nullptr);
  for (size_t i = 0; i < legacyChains.size(); ++i) {
    Set_Neuron_Component(legacyChainArray + static_cast<int>(i), NEUROCOMP_TYPE_LOCSEG_CHAIN, legacyChains[i]);
  }

  Neuron_Structure* legacyNs = Locseg_Chain_Comp_Neurostruct(legacyChainArray,
                                                             static_cast<int>(legacyChains.size()),
                                                             legacySignal,
                                                             /*zScale*/ 1.0,
                                                             legacyCtw);
  ASSERT_NE(legacyNs, nullptr);
  Process_Neuron_Structure(legacyNs);
  Neuron_Structure* legacyNs2 = Neuron_Structure_Locseg_Chain_To_Circle_S(legacyNs, 1.0, 1.0);
  ASSERT_NE(legacyNs2, nullptr);
  Neuron_Structure_To_Tree(legacyNs2);

  Swc_Tree* legacyReconRaw = Neuron_Structure_To_Swc_Tree_Circle_Z(legacyNs2, zToXYRatio, nullptr);
  ASSERT_NE(legacyReconRaw, nullptr);

  auto legacyReconTree = std::make_unique<ZSwcTree>();
  legacyReconTree->setData(legacyReconRaw);
  legacyReconTree->resortId();

  Kill_Neuron_Structure(legacyNs);
  Kill_Neuron_Structure(legacyNs2);
  Kill_Connection_Test_Workspace(legacyCtw);
  legacyChains.clear();

  nim::ConnectionTestWorkspaceLegacyLike portedCtw;
  nim::defaultConnectionTestWorkspaceLegacyLike(portedCtw);
  portedCtw.spTest = portedCfg.spTest;
  portedCtw.crossoverTest = portedCfg.crossoverTest;
  portedCtw.distThre = portedCfg.maxEucDist;
  portedCtw.resolution = {1.0, 1.0, 1.0};
  if (portedChains.size() > 500) {
    portedCtw.spTest = false;
  }

  nim::NeuronStructureChainsLegacyLike portedNs =
    nim::locsegChainCompNeurostructLegacyLike(portedChains, &portedSignal, /*zToXYRatio*/ 1.0, portedCtw);
  nim::processNeuronStructureLegacyLike(portedNs);
  nim::NeuronStructureCirclesLegacyLike portedNs2 =
    nim::neuronStructureLocsegChainToCircleSLegacyLike(portedNs, /*xyScale*/ 1.0, /*zToXYRatio*/ 1.0);
  nim::neuronStructureToTreeLegacyLike(portedNs2);
  std::unique_ptr<nim::ZSwc> portedReconTree = nim::neuronStructureToSwcTreeCircleZLegacyLike(portedNs2, zToXYRatio);
  ASSERT_TRUE(portedReconTree != nullptr);
  ASSERT_FALSE(portedReconTree->empty());
  nim::resortId(*portedReconTree);

  bool allStagesMatch = true;
  auto compareStage = [&](std::string_view stageLabel, const ZSwcTree& legacyTree, const nim::ZSwc& portedTree) {
    const fs::path legacyPath = dir / fmt::format("legacy_{}.swc", stageLabel);
    const fs::path portedPath = dir / fmt::format("ported_{}.swc", stageLabel);

    std::unique_ptr<ZSwcTree> legacyCopy(legacyTree.clone());
    legacyCopy->save(legacyPath.string());

    nim::ZSwc portedCopy = portedTree;
    nim::writeSwcLegacyNeuTu(portedCopy, nim::filesystemPathToQString(portedPath), {});

    const std::string legacyText = readTextFile(legacyPath);
    const std::string portedText = readTextFile(portedPath);
    if (legacyText != portedText) {
      const size_t i = firstDiffIndex(legacyText, portedText);
      const auto [line, col] = lineColFromIndex(legacyText, i);
      const std::string legacyLine = lineAt(legacyText, i);
      const std::string portedLine = lineAt(portedText, i);
      ADD_FAILURE() << fmt::format("[{}] SWC mismatch at byte {} (line {}, col {}).\nLegacy: {}\nPorted: {}\n"
                                   "Keeping outputs for inspection under: {}",
                                   stageLabel,
                                   i,
                                   line,
                                   col,
                                   legacyLine,
                                   portedLine,
                                   dir.string());
      allStagesMatch = false;
    }
  };

  compareStage("reconstruct_ztoxy5", *legacyReconTree, *portedReconTree);

  if (allStagesMatch) {
    Swc_Tree_Remove_Zigzag(legacyReconTree->data());
    nim::swcTreeRemoveZigzagLegacyLike(*portedReconTree);
    compareStage("remove_zigzag_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Tune_Branch(legacyReconTree->data());
    nim::swcTreeTuneBranchLegacyLike(*portedReconTree);
    compareStage("tune_branch_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Remove_Spur(legacyReconTree->data());
    nim::swcTreeRemoveSpurLegacyLike(*portedReconTree);
    compareStage("remove_spur_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Merge_Close_Node(legacyReconTree->data(), 0.01);
    nim::swcTreeMergeCloseNodeLegacyLike(*portedReconTree, 0.01);
    compareStage("merge_close_node_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    Swc_Tree_Remove_Overshoot(legacyReconTree->data());
    nim::swcTreeRemoveOvershootLegacyLike(*portedReconTree);
    compareStage("remove_overshoot_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    ZSwcResampler legacyResampler;
    legacyResampler.optimalDownsample(legacyReconTree.get());

    nim::ZNeutubeSwcResampler portedResampler;
    portedResampler.optimalDownsample(*portedReconTree);
    compareStage("resample_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  if (allStagesMatch) {
    ZSwcPruner legacyPruner;
    legacyPruner.setMinLength(0);
    legacyPruner.removeOrphanBlob(legacyReconTree.get());

    nim::swcTreeRemoveOrphanBlobLegacyLike(*portedReconTree, /*minLength*/ 0.0, /*minOrphanCount*/ 10);
    compareStage("remove_orphan_blob_ztoxy5", *legacyReconTree, *portedReconTree);
  }

  Kill_Trace_Workspace(legacyTw);
  Kill_Geo3d_Scalar_Field(legacyNoisy.seeds);
  Kill_Stack(legacyMask);

  if (allStagesMatch) {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  Kill_Stack(legacySeedSort.baseMask);
  if (legacyRecoverSort && legacyRecoverSort->baseMask != nullptr) {
    Kill_Stack(legacyRecoverSort->baseMask);
  }
}

void expectDevOnlyAutoTraceMatchesLegacy(const fs::path& input)
{
  if (!fs::exists(input)) {
    GTEST_SKIP() << "Missing dev-only auto-trace input: " << input.string();
  }

  const fs::path dir = makeUniqueTempDir();
  const DevOnlyAutoTraceConfigPaths cfgPaths = writeDevOnlyAutoTraceConfigFiles(dir);
  const fs::path legacyOut = dir / "legacy_autotrace.swc";
  const fs::path v2Out = dir / "v2_autotrace.swc";
  const std::string datasetLabel = input.filename().string();

  const auto legacyStart = std::chrono::steady_clock::now();
  const int legacyRc = [&]() {
    ScopedStdoutSilencer silence;
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      input.string(),
      "-o",
      legacyOut.string(),
      "--config",
      cfgPaths.commandConfig.string(),
    });
    return nim::ZRunNeuTuCommand().run(argv.argc(), argv.argv());
  }();
  const auto legacyMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - legacyStart).count();

  const auto v2Start = std::chrono::steady_clock::now();
  const int v2Rc = [&]() {
    ScopedStdoutSilencer silence;
    ArgvBuilder argv({
      "Atlas",
      "--command",
      "--trace",
      input.string(),
      "-o",
      v2Out.string(),
      "--config",
      cfgPaths.commandConfig.string(),
    });
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), QString{});
  }();
  const auto v2Ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - v2Start).count();

  LOG(INFO) << fmt::format("Slice15 auto-trace runtime [{}]: legacy={} ms, v2={} ms (ratio={:.3f}x)",
                           datasetLabel,
                           legacyMs,
                           v2Ms,
                           legacyMs > 0 ? static_cast<double>(v2Ms) / static_cast<double>(legacyMs) : 0.0);

  EXPECT_EQ(legacyRc, v2Rc);
  EXPECT_EQ(legacyRc, 0);

  ASSERT_TRUE(fs::exists(legacyOut)) << legacyOut.string();
  ASSERT_TRUE(fs::exists(v2Out)) << v2Out.string();
  const std::string legacyText = readTextFile(legacyOut);
  const std::string v2Text = readTextFile(v2Out);
  if (legacyText != v2Text) {
    const size_t i = firstDiffIndex(legacyText, v2Text);
    const auto [line, col] = lineColFromIndex(legacyText, i);
    const std::string legacyLine = lineAt(legacyText, i);
    const std::string v2Line = lineAt(v2Text, i);
    ADD_FAILURE() << fmt::format("[{}] Auto-trace SWC mismatch at byte {} (line {}, col {}).\nLegacy: {}\nV2: {}\n"
                                 "Keeping outputs for inspection under: {}",
                                 datasetLabel,
                                 i,
                                 line,
                                 col,
                                 legacyLine,
                                 v2Line,
                                 dir.string());
  }

  if (legacyText == v2Text) {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

TEST(NeutubeCommand2Parity, AutoTrace_Slice15_LsmCh2Tif_MatchesLegacy_DevOnly)
{
  const fs::path input =
    fs::path(QDir::homePath().toStdString()) / "Dropbox/atlas_test/slice15/slice15_L34_Sum.lsm_ch2.tif";
  expectDevOnlyAutoTraceMatchesLegacy(input);
}

TEST(NeutubeCommand2Parity, AutoTrace_Slice15_L11Soma_LsmCh2Tif_MatchesLegacy_DevOnly)
{
  const fs::path input =
    fs::path(QDir::homePath().toStdString()) / "Dropbox/atlas_test/slice15/slice15_L11_Sum.lsm_ch2.tif";
  expectDevOnlyAutoTraceMatchesLegacy(input);
}

TEST(NeutubeCommand2Parity, CompareSwc_MatchesLegacy)
{
  const QDir compareDir(nim::getTestDataDir().filePath("benchmark/swc/compare"));
  if (!compareDir.exists()) {
    GTEST_SKIP() << "Missing SWC compare test data at: " << compareDir.absolutePath();
  }

  std::vector<std::string> legacyInputs;
  std::vector<QString> portedInputs;
  for (int i = 1; i <= 5; ++i) {
    const QString name = QString("compare%1.swc").arg(i);
    const QString swcPath = compareDir.filePath(name);
    if (!QFileInfo::exists(swcPath)) {
      GTEST_SKIP() << "Missing SWC compare file: " << swcPath;
    }
    legacyInputs.push_back(swcPath.toStdString());
    portedInputs.push_back(swcPath);
  }

  const std::string legacy = legacyCompareSwcPairs(legacyInputs, 1.0);
  const std::string ported = nim::formatCompareSwcPairs(nim::computeCompareSwc(portedInputs, 1.0));

  EXPECT_EQ(ported, legacy);
}

TEST(NeutubeCommand2Parity, CompareSwc_Scale2_MatchesLegacy)
{
  const QDir compareDir(nim::getTestDataDir().filePath("benchmark/swc/compare"));
  if (!compareDir.exists()) {
    GTEST_SKIP() << "Missing SWC compare test data at: " << compareDir.absolutePath();
  }

  std::vector<std::string> legacyInputs;
  std::vector<QString> portedInputs;
  for (int i = 1; i <= 5; ++i) {
    const QString name = QString("compare%1.swc").arg(i);
    const QString swcPath = compareDir.filePath(name);
    if (!QFileInfo::exists(swcPath)) {
      GTEST_SKIP() << "Missing SWC compare file: " << swcPath;
    }
    legacyInputs.push_back(swcPath.toStdString());
    portedInputs.push_back(swcPath);
  }

  constexpr double Scale = 2.0;
  const std::string legacy = legacyCompareSwcPairs(legacyInputs, Scale);
  const std::string ported = nim::formatCompareSwcPairs(nim::computeCompareSwc(portedInputs, Scale));

  EXPECT_EQ(ported, legacy);
}
