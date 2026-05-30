#include "zneutubeskeletonize.h"

#include "zneutubeskeletonizer.h"
#include "zswcwriter.h"

#include "zjson.h"
#include "zlog.h"

#include "zimg.h"
#include "zioutils.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace nim {

namespace {

void applySkeletonizeConfig(const json::object& cfg,
                            const std::optional<std::array<int, 3>>& overrideDownsampleInterval,
                            ZNeutubeSkeletonizer* skeletonizer)
{
  CHECK(skeletonizer != nullptr);

  if (auto it = cfg.find("minimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("lengthThreshold"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("finalMinimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setFinalLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("finalLengthThreshold"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setFinalLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("maximalDistance"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setDistanceThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("distanceThreshold"); it != cfg.end() && it->value().is_number()) {
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
  if (auto it = cfg.find("level"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevel(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("grayOp"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevelOp(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("levelOp"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevelOp(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("interpolating"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setInterpolating(it->value().as_bool());
  }
  if (auto it = cfg.find("removingBorder"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setRemovingBorder(it->value().as_bool());
  }
  if (auto it = cfg.find("connectingBranch"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setConnectingBranch(it->value().as_bool());
  }
  if (auto it = cfg.find("usingOriginalSignal"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->useOriginalSignal(it->value().as_bool());
  }
  if (auto it = cfg.find("resampleSwc"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setResampleSwc(it->value().as_bool());
  }
  if (auto it = cfg.find("autoGrayThreshold"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setAutoGrayThreshold(it->value().as_bool());
  }
  if (auto it = cfg.find("resolution"); it != cfg.end() && it->value().is_array()) {
    const auto& arr = it->value().as_array();
    if (arr.size() == 2 && arr[0].is_number() && arr[1].is_number()) {
      skeletonizer->setResolution(arr[0].to_number<double>(), arr[1].to_number<double>());
    } else if (arr.size() == 3 && arr[0].is_number() && arr[2].is_number()) {
      skeletonizer->setResolution(arr[0].to_number<double>(), arr[2].to_number<double>());
    } else {
      LOG(WARNING) << "Invalid skeletonize.resolution; expected array[2] or array[3] of numbers";
    }
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

int runSkeletonize(const QString& inputPath,
                   const QString& outputPath,
                   const QString& skeletonizeConfigPath,
                   const std::optional<std::array<int, 3>>& downsampleIntervalOverride,
                   bool verbose)
{
  if (inputPath.isEmpty()) {
    LOG(ERROR) << "Skeletonize: missing input file.";
    return 1;
  }

  if (outputPath.isEmpty()) {
    LOG(ERROR) << "Skeletonize: missing output file (-o).";
    return 1;
  }

  json::object skeletonizeCfg;
  if (!skeletonizeConfigPath.isEmpty()) {
    try {
      skeletonizeCfg = loadJsonObject(skeletonizeConfigPath);
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
    ZImg img(inputPath);
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
