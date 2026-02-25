#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <memory>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>

extern "C" {
#include "tz_darray.h"
#include "tz_geo3d_scalar_field.h"
#include "tz_local_neuroseg.h"
#include "tz_neuroseg.h"
#include "tz_perceptor.h"
#include "tz_stack_bwmorph.h"
#include "tz_stack_lib.h"
#include "tz_stack_neighborhood.h"
#include "tz_stack_objlabel.h"
#include "tz_stack_sampling.h"
#include "tz_trace_utils.h"
#include "tz_voxel_graphics.h"
#include "tz_workspace.h"
}

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
#include "zneutubestackfitscore.h"
#include "zneutubestackfitoptions.h"
#include "zneutubetraceseed.h"

#include "zimg.h"
#include "zimgneighborhooditerator.h"
#include "zimginfo.h"
#include "zimginit.h"
#include "zjson.h"
#include "zlog.h"
#include "zrunneutucommand.h"
#include "zrunneutucommand2.h"
#include "ztest.h"
#include "zswclayertrunkanalyzer.h"
#include "zswclayershollfeatureanalyzer.h"
#include "zswcnodebufferfeatureanalyzer.h"
#include "zswctree.h"
#include "zswctreematcher.h"

namespace fs = std::filesystem;

namespace {

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
  std::ifstream in(path, std::ios::binary);
  CHECK(in);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return s;
}

void writeTextFile(const fs::path& path, std::string_view text)
{
  std::ofstream out(path, std::ios::binary);
  CHECK(out);
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

  img.save(QString::fromStdString(path.string()));
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

  std::ostringstream stream;
  for (size_t i = 0; i < treeArray.size(); ++i) {
    for (size_t j = i + 1; j < treeArray.size(); ++j) {
      double score = compareSwcLikeLegacy(treeArray[i].get(), treeArray[j].get(), matcher);
      score /= std::max(selfScore[i], selfScore[j]);
      stream << i << "-" << j << ": " << score << std::endl;
    }
  }

  return stream.str();
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

  nim::TraceWorkspace portedTw;
  ZImgInfo info(static_cast<size_t>(W), static_cast<size_t>(H), static_cast<size_t>(D), 1, 1, 1, VoxelFormat::Unsigned);
  info.setVoxelFormat<uint16_t>();
  info.createDefaultDescriptions();

  portedTw.traceMask = std::make_unique<ZImg>(info);
  portedTw.traceMask->fill(0);
  *portedTw.traceMask->data<uint16_t>(3, 4, 2) = 42;

  const std::array<double, 3> portedPos = {3.0, 4.0, 2.0};
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

  EXPECT_DOUBLE_EQ(nim::darrayDotNLegacyLike(a.data(), b.data(), a.size()), darray_dot_n(a.data(), b.data(), a.size()));
  {
    const double legacy = darray_dot_nw(a.data(), b.data(), a.size());
    const double ported = nim::darrayDotNWLegacyLike(a.data(), b.data(), a.size());
    if (std::isnan(legacy)) {
      EXPECT_TRUE(std::isnan(ported));
    } else {
      EXPECT_DOUBLE_EQ(ported, legacy);
    }
  }
  EXPECT_DOUBLE_EQ(nim::darraySumNLegacyLike(a.data(), a.size()), darray_sum_n(a.data(), a.size()));
  EXPECT_DOUBLE_EQ(nim::darrayMeanNLegacyLike(a.data(), a.size()), darray_mean_n(a.data(), a.size()));
  EXPECT_DOUBLE_EQ(nim::darrayCorrcoefNLegacyLike(a.data(), b.data(), a.size()),
                   darray_corrcoef_n(a.data(), b.data(), a.size()));

  size_t legacyIdx = 0;
  size_t portedIdx = 0;
  const double legacyMax = darray_max(a.data(), a.size(), &legacyIdx);
  const double portedMax = nim::darrayMaxLegacyLike(a.data(), a.size(), &portedIdx);

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
  const double dotScore = nim::computeStackFitScoresLegacyLike(field.data(), signal.data(), field.size(), &fs);
  EXPECT_DOUBLE_EQ(dotScore, darray_dot_n(field.data(), signal.data(), field.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::Corrcoef);
  const double corrScore = nim::computeStackFitScoresLegacyLike(field.data(), signal.data(), field.size(), &fs);
  EXPECT_DOUBLE_EQ(corrScore, darray_corrcoef_n(field.data(), signal.data(), field.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::Edot);
  const double edotScore = nim::computeStackFitScoresLegacyLike(field.data(), signal.data(), field.size(), &fs);
  EXPECT_DOUBLE_EQ(edotScore,
                   darray_dot_n(field.data(), signal.data(), field.size()) +
                     darray_sum_n(signal.data(), signal.size()));

  fs.options[0] = static_cast<int>(nim::StackFitOption::CorrcoefSc);
  const double corrScScore = nim::computeStackFitScoresLegacyLike(field.data(), signal.data(), field.size(), &fs);
  const double legacyCorrSc =
    darray_corrcoef_n(field.data(), signal.data(), field.size()) * darray_max(signal.data(), signal.size(), nullptr);
  if (std::isnan(legacyCorrSc)) {
    EXPECT_TRUE(std::isnan(corrScScore));
  } else {
    EXPECT_DOUBLE_EQ(corrScScore, legacyCorrSc);
  }

  // nullptr fs behaves like legacy default: dot_n
  EXPECT_DOUBLE_EQ(nim::computeStackFitScoresLegacyLike(field.data(), signal.data(), field.size(), nullptr),
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
  const double portedScore = nim::computeStackFitScoresLegacyLike(filter.data(), signal.data(), FieldSize, &fsCpp);

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
  const double portedScore = nim::computeStackFitScoresLegacyLike(filter.data(), signal.data(), FieldSize, &fsCpp);

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

  const std::array<double, 2> zScales = {1.0, 2.0};
  for (double zScale : zScales) {
    auto expectSame = [&](const std::string& what, size_t i, double ported, double legacy) {
      if (std::isnan(legacy)) {
        EXPECT_TRUE(std::isnan(ported)) << what << " zScale=" << zScale << " i=" << i;
      } else {
        EXPECT_DOUBLE_EQ(ported, legacy) << what << " zScale=" << zScale << " i=" << i;
      }
    };

    // ------------------------------------------------------------
    // Sampling parity
    // ------------------------------------------------------------
    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling(&fieldC, stackC, zScale, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported = nim::geo3dScalarFieldStackSamplingLegacyLike(fieldCpp, stackImg, zScale);

      ASSERT_EQ(ported.size(), FieldSize);
      for (size_t i = 0; i < FieldSize; ++i) {
        expectSame("Sampling", i, ported[i], legacy[i]);
      }
      free(legacy);
    }

    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling_W(&fieldC, stackC, zScale, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported =
        nim::geo3dScalarFieldStackSamplingWeightedLegacyLike(fieldCpp, stackImg, zScale);

      ASSERT_EQ(ported.size(), FieldSize);
      for (size_t i = 0; i < FieldSize; ++i) {
        expectSame("Sampling_W", i, ported[i], legacy[i]);
      }
      free(legacy);
    }

    {
      double* legacy = Geo3d_Scalar_Field_Stack_Sampling_M(&fieldC, stackC, zScale, maskC, nullptr);
      CHECK(legacy != nullptr);
      const std::vector<double> ported =
        nim::geo3dScalarFieldStackSamplingMaskedLegacyLike(fieldCpp, stackImg, zScale, maskImg);

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

    const double legacyScore = Geo3d_Scalar_Field_Stack_Score(&fieldC, stackC, zScale, &fsLegacy);

    nim::StackFitScore fsPorted;
    fsPorted.n = fsLegacy.n;
    for (int j = 0; j < fsLegacy.n; ++j) {
      fsPorted.options[static_cast<size_t>(j)] = fsLegacy.options[j];
    }

    const double portedScore = nim::geo3dScalarFieldStackScoreLegacyLike(fieldCpp, stackImg, zScale, &fsPorted);
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

    const double legacyScoreM = Geo3d_Scalar_Field_Stack_Score_M(&fieldC, stackC, zScale, maskC, &fsLegacyM);

    nim::StackFitScore fsPortedM;
    fsPortedM.n = fsLegacyM.n;
    for (int j = 0; j < fsLegacyM.n; ++j) {
      fsPortedM.options[static_cast<size_t>(j)] = fsLegacyM.options[j];
    }

    const double portedScoreM =
      nim::geo3dScalarFieldStackScoreMaskedLegacyLike(fieldCpp, stackImg, zScale, maskImg, &fsPortedM);
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

TEST(NeutubeCommand2Parity, SkeletonizeAndTrace_TiffMatchesLegacy)
{
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    const int rc = nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

  const std::vector<QString> candidateRelativePaths = {
    "benchmark/fake_neuron.tif",
    "benchmark/fake_neuron2.tif",
    "benchmark/fake_neuron3.tif",
    "benchmark/fake_neuron4.tif",
    "benchmark/fake_spine.tif",
    "benchmark/faint_fiber.tif",
    "benchmark/sharp_turn.tif",
    "benchmark/fork2/fork2.tif",
    "benchmark/line.tif",
    "benchmark/gaussians.tif",
    // 2D edge cases (depth==1) exercise local-max / neighborhood guards.
    "benchmark/bline_2d_1.tif",
    "benchmark/bline_2d_2.tif",
    "benchmark/bline_2d.tif",
    "benchmark/bfork_2d.tif",
    "benchmark/fork_2d.tif",
    "benchmark/btrig_2d.tif",
    "benchmark/btrig2_2d.tif",
    "benchmark/crossover_2d.tif",
    "benchmark/crossover2_2d.tif",
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
    GTEST_SKIP() << "Missing auto-trace benchmark inputs under: " << nim::getTestDataDir().absolutePath().toStdString();
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
      return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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

TEST(NeutubeCommand2Parity, Skeletonize_FromTestDataBinary_MatchesLegacy)
{
  ScopedQtCoreApplication qtApp;
  std::ignore = nim::ZImgInit::instance("", "", "", false);

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
    GTEST_SKIP() << "Missing skeletonize benchmark inputs under: "
                 << nim::getTestDataDir().absolutePath().toStdString();
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
      return nim::ZRunNeuTuCommand2().run(argv.argc(), argv.argv(), std::string_view{});
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

TEST(NeutubeCommand2Parity, CompareSwc_MatchesLegacy)
{
  ScopedQtCoreApplication qtApp;

  const QDir compareDir(nim::getTestDataDir().filePath("benchmark/swc/compare"));
  if (!compareDir.exists()) {
    GTEST_SKIP() << "Missing SWC compare test data at: " << compareDir.absolutePath().toStdString();
  }

  std::vector<std::string> inputs;
  for (int i = 1; i <= 5; ++i) {
    const QString name = QString("compare%1.swc").arg(i);
    const QString swcPath = compareDir.filePath(name);
    if (!QFileInfo::exists(swcPath)) {
      GTEST_SKIP() << "Missing SWC compare file: " << swcPath.toStdString();
    }
    inputs.push_back(swcPath.toStdString());
  }

  const std::string legacy = legacyCompareSwcPairs(inputs, 1.0);
  const std::string ported = nim::formatCompareSwcPairs(nim::computeCompareSwc(inputs, 1.0));

  EXPECT_EQ(ported, legacy);
}

TEST(NeutubeCommand2Parity, CompareSwc_Scale2_MatchesLegacy)
{
  ScopedQtCoreApplication qtApp;

  const QDir compareDir(nim::getTestDataDir().filePath("benchmark/swc/compare"));
  if (!compareDir.exists()) {
    GTEST_SKIP() << "Missing SWC compare test data at: " << compareDir.absolutePath().toStdString();
  }

  std::vector<std::string> inputs;
  for (int i = 1; i <= 5; ++i) {
    const QString name = QString("compare%1.swc").arg(i);
    const QString swcPath = compareDir.filePath(name);
    if (!QFileInfo::exists(swcPath)) {
      GTEST_SKIP() << "Missing SWC compare file: " << swcPath.toStdString();
    }
    inputs.push_back(swcPath.toStdString());
  }

  constexpr double Scale = 2.0;
  const std::string legacy = legacyCompareSwcPairs(inputs, Scale);
  const std::string ported = nim::formatCompareSwcPairs(nim::computeCompareSwc(inputs, Scale));

  EXPECT_EQ(ported, legacy);
}
