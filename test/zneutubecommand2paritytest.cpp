#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "zneutubeneighborhood.h"
#include "zneutubeobjlabel.h"

#include "zimg.h"
#include "zimgneighborhooditerator.h"
#include "zimginfo.h"
#include "zimginit.h"
#include "zjson.h"
#include "zlog.h"
#include "zrunneutucommand.h"
#include "zrunneutucommand2.h"

extern "C" {
#include "tz_stack_neighborhood.h"
#include "tz_stack_objlabel.h"
}

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
                                                           const nim::neutube::LabelLargeObjectsParams& params)
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

} // namespace

TEST(NeutubeLegacyNeighborhood, OffsetsMatchLegacyTables)
{
  const int connectivities[] = {4, 8, 6, 10, 18, 26};
  for (const int conn : connectivities) {
    const nim::ZNeighborhood& nb = nim::neutube::neighborhoodLegacyOrder(conn);
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

  ZImgNeighborhoodConstIterator<uint8_t> it(neutube::neighborhoodLegacyOrder(6), img, region);
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

  neutube::LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = 5;
  params.connectivity = 26;

  const auto ported = neutube::labelLargeObjectsLegacy(img, params);
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

  neutube::LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = 1;
  params.connectivity = 26;

  const auto ported = neutube::labelLargeObjectsLegacy(img, params);
  EXPECT_EQ(ported.numLargeObjects, static_cast<size_t>(300));
  EXPECT_TRUE(ported.labels.isType<uint16_t>()) << ported.labels.info();

  const auto legacy = labelLargeObjectsLegacyC(img, params);
  EXPECT_EQ(legacy.numLargeObjects, 300);
  EXPECT_EQ(legacy.kind, GREY16);

  const auto a = toU32Labels(ported.labels);
  ASSERT_EQ(a.size(), legacy.labels.size());
  EXPECT_EQ(a, legacy.labels);
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

  // V2 runner: Atlas --command2 --skeletonize <input> -o <out> --config <command_config.json>
  {
    ArgvBuilder argv({
      "Atlas",
      "--command2",
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
      "--command2",
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
