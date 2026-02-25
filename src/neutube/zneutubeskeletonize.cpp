#include "zneutubeskeletonize.h"

#include "zneutubeskeletonizer.h"
#include "zswcwriter.h"

#include "zjson.h"
#include "zlog.h"

#include "zimg.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nim {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool fileExists(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return fs::exists(fs::path(path), ec) && fs::is_regular_file(fs::path(path), ec);
}

void applySkeletonizeConfig(const json::object& cfg,
                            const std::optional<std::array<int, 3>>& overrideDownsampleInterval,
                            ZNeutubeSkeletonizer* skeletonizer)
{
  CHECK(skeletonizer != nullptr);

  if (auto it = cfg.find("minimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("finalMinimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setFinalLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("maximalDistance"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setDistanceThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("keepingSingleObject"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setKeepingSingleObject(it->value().as_bool());
  }
  if (auto it = cfg.find("rebase"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setRebase(it->value().as_bool());
  }
  if (auto it = cfg.find("fillingHole"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setFillingHole(it->value().as_bool());
  }
  if (auto it = cfg.find("minimalObjectSize"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setMinObjSize(static_cast<int>(it->value().as_int64()));
  }

  std::optional<std::array<int, 3>> cfgIntv;
  if (auto it = cfg.find("downsampleInterval"); it != cfg.end() && it->value().is_array()) {
    const auto& arr = it->value().as_array();
    if (arr.size() == 3 && arr[0].is_int64() && arr[1].is_int64() && arr[2].is_int64()) {
      cfgIntv = std::array<int, 3>{static_cast<int>(arr[0].as_int64()),
                                   static_cast<int>(arr[1].as_int64()),
                                   static_cast<int>(arr[2].as_int64())};
    } else {
      LOG(WARNING) << "Invalid skeletonize.downsampleInterval; expected array[3] of int";
    }
  }

  if (overrideDownsampleInterval) {
    skeletonizer->setDownsampleInterval((*overrideDownsampleInterval)[0],
                                        (*overrideDownsampleInterval)[1],
                                        (*overrideDownsampleInterval)[2]);
  } else if (cfgIntv) {
    skeletonizer->setDownsampleInterval((*cfgIntv)[0], (*cfgIntv)[1], (*cfgIntv)[2]);
  }
}

} // namespace

int runSkeletonize(const std::string& inputPath,
                   const std::string& outputPath,
                   const std::string& skeletonizeConfigPath,
                   const std::optional<std::array<int, 3>>& downsampleIntervalOverride,
                   bool verbose)
{
  if (inputPath.empty()) {
    LOG(ERROR) << "Skeletonize: missing input file.";
    return 1;
  }

  if (outputPath.empty()) {
    LOG(ERROR) << "Skeletonize: missing output file (-o).";
    return 1;
  }

  json::object skeletonizeCfg;
  if (!skeletonizeConfigPath.empty()) {
    try {
      skeletonizeCfg = loadJsonObject(QString::fromStdString(skeletonizeConfigPath));
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Failed to load skeletonize config '" << skeletonizeConfigPath << "': " << e.what();
      return 1;
    }
  }

  ZNeutubeSkeletonizer skeletonizer;
  applySkeletonizeConfig(skeletonizeCfg, downsampleIntervalOverride, &skeletonizer);

  if (verbose) {
    LOG(INFO) << "==========Skeletonize configuration (effective)========";
    LOG(INFO) << nim::jsonToFormattedString(skeletonizeCfg);
    skeletonizer.print();
    LOG(INFO) << "======================================================";
  }

  const auto start = std::chrono::steady_clock::now();

  if (!fileExists(inputPath)) {
    LOG(ERROR) << "Skeletonization failed: missing input file " << inputPath;
    return 1;
  }

  std::unique_ptr<ZSwc> tree;
  try {
    ZImg img(QString::fromStdString(inputPath));
    tree = skeletonizer.makeSkeleton(img);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Skeletonization failed: failed to read input image '" << inputPath << "': " << e.what();
    return 1;
  }

  if (tree && !tree->empty()) {
    writeSwcLegacyNeuTu(*tree, outputPath, {});
    LOG(INFO) << "SWC saved in " << outputPath;
  } else {
    LOG(INFO) << "No SWC generated.";
  }

  if (verbose) {
    const auto end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    LOG(INFO) << "Skeletonize elapsed: " << ms << " ms";
  }

  return 0;
}

} // namespace nim
