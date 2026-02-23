#include "zneutubelegacy.h"

#include <QDir>
#include <QFileInfo>

#include "zimgstackinterface.h"

#include "zlog.h"

#include "c_stack.h"
#include "zfiletype.h"
#include "zneurontracer.h"
#include "zneurontracerconfig.h"
#include "zobject3dscan.h"
#include "zstack.hxx"
#include "zstackskeletonizer.h"
#include "zswclayertrunkanalyzer.h"
#include "zswclayershollfeatureanalyzer.h"
#include "zswcnodebufferfeatureanalyzer.h"
#include "zswctree.h"
#include "zswctreematcher.h"

#include <chrono>
#include <algorithm>
#include <memory>
#include <sstream>

namespace nim::neutube_legacy {

namespace {

[[nodiscard]] bool fileExists(const QString& path)
{
  const QFileInfo fi(path);
  return fi.exists() && fi.isFile();
}

void applySkeletonizeConfig(const json::object& cfg,
                            const std::optional<std::array<int, 3>>& overrideDownsampleInterval,
                            ZStackSkeletonizer* skeletonizer)
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

[[nodiscard]] double compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher)
{
  double score = 0.0;

  if (tree1 != nullptr && tree2 != nullptr) {
    const double sampleStep = 200.0;
    const int matchingLevel = 2;

    std::unique_ptr<ZSwcTree> tree1ForMatch(tree1->clone());
    tree1ForMatch->resample(sampleStep);

    std::unique_ptr<ZSwcTree> tree2ForMatch(tree2->clone());
    tree2ForMatch->resample(sampleStep);

    matcher.matchAllG(*tree1ForMatch, *tree2ForMatch, matchingLevel);
    score = matcher.matchingScore();
  }

  return score;
}

[[nodiscard]] std::unique_ptr<ZSwcTree> traceNeuronFromSeed(const std::string& filePath,
                                                            const std::optional<std::array<int, 3>>& position,
                                                            int level,
                                                            bool diagnosis,
                                                            const std::string& swcPath)
{
  QString readError;
  std::unique_ptr<ZStack> signal(readZStack(filePath, nullptr, &readError));
  if (!signal) {
    LOG(ERROR) << "Failed to read input image: " << filePath << " (" << readError.toStdString() << ")";
    return nullptr;
  }

  ZNeuronTracer tracer;
  tracer.setIntensityField(signal.get());
  tracer.setTraceLevel(level);
  tracer.setDiagnosis(diagnosis);

  if (position) {
    const int x = (*position)[0];
    const int y = (*position)[1];
    const int z = (*position)[2];

    if (ZFileType::FileType(swcPath) != ZFileType::EFileType::SWC) {
      ZSwcPath path = tracer.trace(x, y, z);
      if (path.empty()) {
        return nullptr;
      }
      auto tree = std::make_unique<ZSwcTree>();
      tree->setDataFromNodeRoot(path[0]);
      return tree;
    }

    auto tree = std::make_unique<ZSwcTree>();
    tree->load(swcPath);
    tracer.trace(x, y, z, tree.get());
    return tree;
  }

  return std::unique_ptr<ZSwcTree>(tracer.trace(signal.get()));
}

[[nodiscard]] std::unique_ptr<ZSwcTree>
traceNeuronAuto(const std::string& filePath, const json::object& inputConfig, int level, bool diagnosis)
{
  QString readError;
  std::unique_ptr<ZStack> signal(readZStack(filePath, nullptr, &readError));
  if (!signal) {
    LOG(ERROR) << "Failed to read input image: " << filePath << " (" << readError.toStdString() << ")";
    return nullptr;
  }

  ZNeuronTracer tracer;
  tracer.setIntensityField(signal.get());
  tracer.setTraceLevel(level);
  tracer.setDiagnosis(diagnosis);

  std::unique_ptr<ZStack> mask;
  if (auto it = inputConfig.find("mask"); it != inputConfig.end() && it->value().is_string()) {
    const std::string maskPath = std::string(it->value().as_string().c_str());
    if (!maskPath.empty()) {
      const QString qMaskPath = QString::fromStdString(maskPath);
      LOG(INFO) << "Using a predefined mask: " << maskPath;
      if (!fileExists(qMaskPath)) {
        LOG(ERROR) << "Missing file: Cannot find the mask file " << maskPath;
        return nullptr;
      }
      if (ZFileType::FileType(maskPath) != ZFileType::EFileType::TIFF) {
        LOG(ERROR) << "File error: Failed to recognize the mask file " << maskPath << " as a TIFF";
        return nullptr;
      }

      QString maskError;
      mask.reset(readZStack(maskPath, nullptr, &maskError));
      if (!mask) {
        LOG(WARNING) << "File error: Failed to read mask file " << maskPath << " (" << maskError.toStdString() << ")";
      } else {
        int threshold = 0;
        if (auto thrIt = inputConfig.find("maskThreshold"); thrIt != inputConfig.end() && thrIt->value().is_int64()) {
          threshold = static_cast<int>(thrIt->value().as_int64());
        }

        if (threshold < 0) {
          tracer._makeMask = [&tracer, maskPtr = mask.get()](Stack* /*stack*/) {
            return tracer.makeMask(maskPtr->c_stack());
          };
        } else {
          mask->binarize(threshold);
          tracer._makeMask = [maskPtr = mask.get()](Stack* /*stack*/) {
            return C_Stack::clone(maskPtr->c_stack());
          };
        }
      }
    }
  }

  return std::unique_ptr<ZSwcTree>(tracer.trace(signal.get()));
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

  ZStackSkeletonizer skeletonizer;
  applySkeletonizeConfig(skeletonizeCfg, downsampleIntervalOverride, &skeletonizer);

  if (verbose) {
    LOG(INFO) << "==========Skeletonize configuration (effective)========";
    LOG(INFO) << nim::jsonToFormattedString(skeletonizeCfg);
    skeletonizer.print();
    LOG(INFO) << "======================================================";
  }

  const auto start = std::chrono::steady_clock::now();

  if (!fileExists(QString::fromStdString(inputPath))) {
    LOG(ERROR) << "Skeletonization failed: missing input file " << inputPath;
    return 1;
  }

  std::unique_ptr<ZSwcTree> tree;
  const auto fileType = ZFileType::FileType(inputPath);
  if (fileType == ZFileType::EFileType::TIFF) {
    if (outputPath.empty()) {
      LOG(ERROR) << "Skeletonization Failed: The input is not a binary image.";
      return 1;
    }
    QString stackError;
    std::unique_ptr<ZStack> stack(readZStack(inputPath, nullptr, &stackError));
    if (!stack) {
      LOG(ERROR) << "Skeletonization failed: failed to read image '" << inputPath << "': " << stackError.toStdString();
      return 1;
    }

    if (!stack->isBinary()) {
      LOG(INFO) << "The image is not binary. Binarizing...";
      stack->binarize();
    }
    tree.reset(skeletonizer.makeSkeleton(*stack));
  } else if (fileType == ZFileType::EFileType::OBJECT_SCAN) {
    ZObject3dScan obj;
    obj.load(inputPath);
    if (verbose) {
      LOG(INFO) << obj.getVoxelNumber() << " foreground voxels.";
    }
    tree.reset(skeletonizer.makeSkeleton(obj));
  } else {
    LOG(ERROR) << "Skeletonization Failed: Unrecognized output: " << inputPath;
  }

  if (tree && !tree->isEmpty()) {
    if (!outputPath.empty()) {
      tree->save(outputPath);
      LOG(INFO) << "SWC saved in " << outputPath;
    }
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

int runCompareSwc(const std::vector<std::string>& inputPaths, double scale)
{
  if (inputPaths.empty()) {
    LOG(ERROR) << "Compare SWC: please specify input SWC files.";
    return 1;
  }

  LOG(INFO) << "Computing pairwise similarity for:";
  std::vector<std::unique_ptr<ZSwcTree>> trees;
  trees.reserve(inputPaths.size());
  for (const auto& p : inputPaths) {
    LOG(INFO) << "  " << p;
    auto tree = std::make_unique<ZSwcTree>();
    tree->load(p);
    if (scale != 1.0) {
      tree->rescale(scale, scale, scale);
    }
    trees.push_back(std::move(tree));
  }

  ZSwcTreeMatcher matcher;
  auto trunkAnalyzer = std::make_unique<ZSwcLayerTrunkAnalyzer>();
  trunkAnalyzer->setStep(200.0);
  auto helperAnalyzer = std::make_unique<ZSwcLayerShollFeatureAnalyzer>();
  helperAnalyzer->setLayerScale(4000.0);
  helperAnalyzer->setLayerMargin(100.0);

  auto analyzer = std::make_unique<ZSwcNodeBufferFeatureAnalyzer>();
  analyzer->setHelper(helperAnalyzer.get());

  auto* featureAnalyzer = dynamic_cast<ZSwcFeatureAnalyzer*>(analyzer.get());
  matcher.setTrunkAnalyzer(trunkAnalyzer.get());
  matcher.setFeatureAnalyzer(featureAnalyzer);

  std::vector<double> selfScore(trees.size(), 0.0);
  for (size_t i = 0; i < trees.size(); ++i) {
    selfScore[i] = compareSwc(trees[i].get(), trees[i].get(), matcher);
  }

  std::ostringstream stream;
  for (size_t i = 0; i < trees.size(); ++i) {
    for (size_t j = i + 1; j < trees.size(); ++j) {
      double score = compareSwc(trees[i].get(), trees[j].get(), matcher);
      score /= std::max(selfScore[i], selfScore[j]);
      stream << i << "-" << j << ": " << score << std::endl;
    }
  }

  LOG(INFO) << "Result:";
  for (size_t i = 0; i < inputPaths.size(); ++i) {
    LOG(INFO) << i << ": " << inputPaths[i];
  }
  LOG(INFO) << stream.str();

  return 1;
}

int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool /*verbose*/)
{
  if (input.empty()) {
    LOG(INFO) << "No input specified. Abort.";
    return 1;
  }

  if (input[0].empty()) {
    LOG(INFO) << "No input data specified. Abort.";
    return 1;
  }

  if (outputPath.empty()) {
    LOG(INFO) << "No output specified. Abort.";
    return 1;
  }

  if (!traceConfigPath.empty()) {
    if (!ZNeuronTracerConfig::getInstance().load(traceConfigPath)) {
      LOG(WARNING) << "Tracing configuration failed: failed to load " << traceConfigPath;
    }
  } else if (!jsonDirPath.empty()) {
    const std::string defaultTraceConfig =
      QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
    if (!ZNeuronTracerConfig::getInstance().load(defaultTraceConfig)) {
      LOG(WARNING) << "Tracing configuration failed: failed to load " << defaultTraceConfig;
    }
  } else {
    LOG(WARNING) << "Tracing configuration skipped: no trace config path and no json dir available.";
  }

  const std::string swcPath = (input.size() > 1) ? input[1] : std::string{};
  auto tree = traceNeuronFromSeed(input[0], position, level, diagnosis, swcPath);
  if (!tree) {
    LOG(WARNING) << "WARNING: No result generated.";
    return 1;
  }

  tree->save(outputPath);
  return 0;
}

int runGeneral(const std::string& generalConfigTextOrPath,
               const json::object& generalCfgIn,
               const json::object& inputJson,
               const std::vector<std::string>& positionalInput,
               const std::string& outputPath,
               int level,
               bool diagnosis,
               const std::string& traceIncludePath,
               const std::string& jsonDirPath,
               bool /*verbose*/)
{
  if (generalConfigTextOrPath.empty()) {
    LOG(ERROR) << "General: missing --general config (JSON string or JSON file path).";
    return 1;
  }

  json::object generalCfg = generalCfgIn;

  // Attach parsed input JSON for parity with legacy `runGeneral`.
  generalCfg["_input"] = inputJson;
  generalCfg["_source"] = generalConfigTextOrPath;

  const auto commandIt = generalCfg.find("command");
  const std::string commandName = (commandIt != generalCfg.end() && commandIt->value().is_string())
                                    ? std::string(commandIt->value().as_string())
                                    : std::string{};

  LOG(INFO) << "Running command " << commandName << "...";
  if (commandName != "trace_neuron") {
    LOG(ERROR) << "Invalid command module: " << commandName;
    return 1;
  }

  // Determine effective input.
  std::vector<std::string> updatedInput = positionalInput;
  if (updatedInput.empty()) {
    auto inputIt = generalCfg.find("_input");
    if (inputIt != generalCfg.end() && inputIt->value().is_object()) {
      const auto& in = inputIt->value().as_object();
      if (auto sigIt = in.find("signal"); sigIt != in.end() && sigIt->value().is_string()) {
        updatedInput.emplace_back(std::string(sigIt->value().as_string().c_str()));
      }
    }
  }

  if (updatedInput.empty() || outputPath.empty()) {
    LOG(ERROR) << "trace_neuron: missing input signal and/or output (-o).";
    return 1;
  }

  // Load trace config path for the general command (may override the command_config include).
  std::string configPath;
  if (auto it = generalCfg.find("path"); it != generalCfg.end() && it->value().is_string()) {
    configPath = std::string(it->value().as_string().c_str());
    if (configPath.empty() || configPath == "default") {
      if (!jsonDirPath.empty()) {
        configPath = QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
      }
    }
  } else if (!traceIncludePath.empty()) {
    configPath = traceIncludePath;
  } else {
    if (!jsonDirPath.empty()) {
      configPath = QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
    }
  }

  if (configPath.empty()) {
    LOG(WARNING) << "Configuration skipped: no trace config path resolved.";
  } else if (!ZNeuronTracerConfig::getInstance().load(configPath)) {
    LOG(WARNING) << "Configuration failed: failed to load " << configPath;
  }

  if (auto it = generalCfg.find("diagnosis"); it != generalCfg.end() && it->value().is_bool()) {
    diagnosis = it->value().as_bool();
  }

  if (auto it = generalCfg.find("action"); it != generalCfg.end() && it->value().is_string()) {
    const std::string action = std::string(it->value().as_string().c_str());
    if (action == "inspect") {
      ZNeuronTracerConfig::getInstance().print();
      return 0;
    }
  }

  const json::object in = generalCfg.at("_input").is_object() ? generalCfg.at("_input").as_object() : json::object{};
  auto tree = traceNeuronAuto(updatedInput[0], in, level, diagnosis);

  if (tree) {
    LOG(INFO) << "Saving " << outputPath << "...";
    tree->save(outputPath);
  } else {
    LOG(WARNING) << "WARNING: No result generated.";
  }

  return 0;
}

} // namespace nim::neutube_legacy
