#include "zrunneutucommand.h"

#include "zlog.h"
#include "zsysteminfo.h"

#include "tz_utilities.h"
#include "filesystem/utilities.h"
#include "zobject3dscan.h"
#include "zjsonparser.h"
#include "zjsonobject.h"
#include "zjsonobjectparser.h"
#include "zfiletype.h"
#include "zstackskeletonizer.h"
#include "zstackreader.h"
#include "zrandomgenerator.h"
#include "zneurontracer.h"
#include "zneurontracerconfig.h"
#include "zswctreematcher.h"
#include "zswclayershollfeatureanalyzer.h"
#include "zswclayertrunkanalyzer.h"
#include "zswcglobalfeatureanalyzer.h"
#include "zswcnodebufferfeatureanalyzer.h"
#include "zswcfactory.h"
#include "zstack.hxx"
#include "zstring.h"

#include <QFileInfoList>
#include <QDir>

DECLARE_int32(v);

namespace nim {

int ZRunNeuTuCommand::run(int argc, char* argv[])
{
  static const char* Spec[] = {"--command",
                               "[<input:string> ...]",
                               "[-o <string>]",
                               "[--config <string>]",
                               "[--intv <int> <int> <int>]",
                               "[--skeletonize]",
                               "[--general <string>]",
                               "[--compare_swc] [--scale <double>]",
                               "[--trace] [--level <int>]",
                               "[--compute_seed]",
                               "[--verbose]",
                               nullptr};

  Process_Arguments(argc, argv, const_cast<char**>(Spec), 1);

  if (Is_Arg_Matched(const_cast<char*>("--verbose"))) {
    m_isVerbose = true;
    FLAGS_v = 1;
  }

  for (int i = 0; i < argc; ++i) {
    VLOG(1) << argv[i];
  }

  m_configDir = ZSystemInfo::jsonDirPath().toStdString();
  // m_configDir = neutu::JoinPath(NeutubeConfig::getInstance().getPath(NeutubeConfig::EConfigItem::CONFIG_DIR),
  // "json");
  std::string configPath = neutu::JoinPath(m_configDir, "command_config.json");

  if (Is_Arg_Matched(const_cast<char*>("--config"))) {
    configPath = Get_String_Arg(const_cast<char*>("--config"));
  }

  auto configJson = loadConfig(configPath);
  // VLOG(1) << configJson.dumpString(2);

  ECommand command = UNKNOWN_COMMAND;
  if (!configJson.isEmpty()) {
    command = getCommand(ZJsonParser::stringValue(configJson["command"]).c_str());
    m_input.push_back(ZJsonParser::stringValue(configJson["input"]));
    m_output = ZJsonParser::stringValue(configJson["output"]);
    // VLOG(1) << ZJsonParser::stringValue(configJson["command"]);
    // VLOG(1) << ZJsonParser::stringValue(configJson["input"]);
    // VLOG(1) << ZJsonParser::stringValue(configJson["output"]);
  }

  if (Is_Arg_Matched(const_cast<char*>("--scale"))) {
    m_scale = Get_Double_Arg(const_cast<char*>("--scale"));
  }

  if (Is_Arg_Matched(const_cast<char*>("--level"))) {
    m_level = Get_Int_Arg(const_cast<char*>("--level"));
  }

  //  ZArgumentProcessor::processArguments(argc, argv, Spec);

  m_input.clear();
  int inputNumber = Get_Repeat_Count(const_cast<char*>("input"));
  //  if (ZArgumentProcessor::isArgMatched("input")) {
  //    int inputNumber = ZArgumentProcessor::getRepeatCount("input");
  m_input.resize(inputNumber);
  for (int i = 0; i < inputNumber; ++i) {
    m_input[i] = Get_String_Arg(const_cast<char*>("input"), i);
    //      m_input[i] = ZArgumentProcessor::getStringArg("input", i);
  }

  auto inputJson = loadInputJson();

  if (Is_Arg_Matched(const_cast<char*>("-o"))) {
    m_output = Get_String_Arg(const_cast<char*>("-o"));
  }

  if (Is_Arg_Matched(const_cast<char*>("--intv"))) {
    for (int i = 0; i < 3; ++i) {
      m_intv[i] = Get_Int_Arg(const_cast<char*>("--intv"), i + 1);
    }
    m_intvSpecified = true;
  }

  if (command == UNKNOWN_COMMAND) {
    if (Is_Arg_Matched(const_cast<char*>("--skeletonize"))) {
      command = SKELETONIZE;
      //    m_input.push_back(ZArgumentProcessor::getStringArg("input", 0));
    } else if (Is_Arg_Matched(const_cast<char*>("--trace"))) {
      //      m_input.push_back(ZArgumentProcessor::getStringArg("input", 0));
      m_output = Get_String_Arg(const_cast<char*>("-o"));
      command = TRACE_NEURON;
    } else if (Is_Arg_Matched(const_cast<char*>("--compare_swc"))) {
      command = COMPARE_SWC;
    } else if (Is_Arg_Matched(const_cast<char*>("--compute_seed"))) {
      command = COMPUTE_SEED;
    } else if (Is_Arg_Matched(const_cast<char*>("--general"))) {
      command = GENERAL_COMMAND;
      m_generalConfig = Get_String_Arg(const_cast<char*>("--general"));
      VLOG(1) << "Config: " << m_generalConfig;
    }
  }

  // registerModule();

  switch (command) {
    case SKELETONIZE:
      return runSkeletonize(configJson);
    case COMPARE_SWC:
      return runCompareSwc();
    case TRACE_NEURON:
      return runTraceNeuron(configJson);
    case GENERAL_COMMAND:
      return runGeneral(inputJson);
    default:
      LOG(INFO) << "Unknown command";
      return 1;
  }
}

ZRunNeuTuCommand::ECommand ZRunNeuTuCommand::getCommand(const char* cmd)
{
  if (eqstr(cmd, "sobj_marker")) {
    return OBJECT_MARKER;
  }

  if (eqstr(cmd, "boundary_orphan")) {
    return BOUNDARY_ORPHAN;
  }

  if (eqstr(cmd, "flyem_neuron_feature")) {
    return FLYEM_NEURON_FEATURE;
  }

  return UNKNOWN_COMMAND;
}

void ZRunNeuTuCommand::loadTraceConfig(const ZJsonObject& configJson)
{
  if (!ZNeuronTracerConfig::getInstance().loadJsonObject(configJson)) {
    ZNeuronTracerConfig::getInstance().load(m_configDir + "/trace_config.json");
  }
}

ZJsonObject ZRunNeuTuCommand::loadInputJson()
{
  if (m_input.empty()) {
    return {};
  }

  ZJsonObject obj;
  if (m_input[0] == "json") {
    std::string jsonInput = m_input[1];

    if (ZFileType::FileType(jsonInput) == ZFileType::EFileType::JSON) {
      obj.load(jsonInput);
    } else {
      if (!obj.decode(jsonInput, true)) {
        LOG(ERROR) << "Invalid input json: " << jsonInput;
      }
    }

    if (obj.hasKey("position")) {
      ZJsonArray posJson(obj.value("position"));
      if (posJson.size() == 3) {
        m_position.resize(3, 0);
        for (size_t i = 0; i < 3; ++i) {
          m_position[i] = ZJsonParser::integerValue(posJson.getData(), i);
        }
      } else {
        LOG(FATAL) << "ERROR: invalid position input!";
      }
    }

    if (obj.hasKey("size")) {
      ZJsonArray sizeJson(obj.value("size"));
      if (sizeJson.size() == 3) {
        m_size.resize(3);
        for (size_t i = 0; i < 3; ++i) {
          m_size[i] = ZJsonParser::integerValue(sizeJson.getData(), i);
        }
      } else {
        LOG(FATAL) << "ERROR: invalid size input!";
      }
    }

    if (obj.hasKey("swc")) {
      m_input[1] = ZJsonParser::stringValue(obj["swc"]);
    } else {
      m_input[1].clear();
    }

    m_input[0].clear();
    if (obj.hasKey("signal")) {
      m_input[0] = ZJsonParser::stringValue(obj["signal"]);
    }
  }

  return obj;
}

ZSwcTree* ZRunNeuTuCommand::traceFile()
{
  ZStack signal;
  signal.load(m_input[0]);

  ZNeuronTracer tracer;
  tracer.setIntensityField(&signal);
  tracer.setTraceLevel(m_level);

  ZSwcTree* tree = nullptr;

  if (m_position.size() == 3) {
    std::string swcPath;
    if (m_input.size() > 1) {
      swcPath = m_input[1];
    }
    if (ZFileType::FileType(swcPath) != ZFileType::EFileType::SWC) {
      ZSwcPath path = tracer.trace(m_position[0], m_position[1], m_position[2]);
      if (!path.empty()) {
        tree = new ZSwcTree;
        tree->setDataFromNodeRoot(path[0]);
      }
    } else {
      tree = new ZSwcTree;
      tree->load(swcPath);
      tracer.trace(m_position[0], m_position[1], m_position[2], tree);
    }
  } else {
    tree = tracer.trace(&signal);
  }
  return tree;
}

int ZRunNeuTuCommand::runTraceNeuron(const ZJsonObject& configJson)
{
  if (m_input.empty()) {
    LOG(INFO) << "No input specified. Abort.";
    return 1;
  }

  if (m_input[0].empty()) {
    LOG(INFO) << "No input data specified. Abort.";
    return 1;
  }

  if (m_output.empty()) {
    LOG(INFO) << "No output specified. Abort.";
    return 1;
  }

  int stat = 1;

  loadTraceConfig(configJson);

  ZSwcTree* tree = traceFile();

  if (tree != NULL) {
    tree->save(m_output);
    delete tree;

    stat = 0;
  }

  return stat;
}

int ZRunNeuTuCommand::runGeneral(const ZJsonObject& inputJson)
{
  if (!m_generalConfig.empty()) {
    ZJsonObject config;
    if (ZFileType::FileType(m_generalConfig) == ZFileType::EFileType::JSON) {
      config.load(m_generalConfig);
      config.setEntry("_source", m_generalConfig);
    } else {
      if (!config.decode(m_generalConfig, true)) {
        LOG(ERROR) << "Invalid config json: " << m_generalConfig;
      }
    }

    config.setEntry("_input", inputJson);

    std::string commandName = ZJsonParser::stringValue(config["command"]);
    LOG(INFO) << "Running command " << commandName << "...";
    if (commandName == "trace_neuron") {
      try {
        return runTraceCommand(m_input, m_output, config);
      }
      catch (std::exception& e) {
        LOG(ERROR) << "COMMAND FAILED: " << e.what();
        return 1;
      }
    } else {
      LOG(ERROR) << "Invalid command module: " << commandName;
      return 1;
    }
  }

  return 1;
}

double ZRunNeuTuCommand::compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher)
{
  double score = 0.0;

  if (tree1 != NULL && tree2 != NULL) {
    double sampleStep = 200.0;
    int matchingLevel = 2;

    ZSwcTree* tree1ForMatch = tree1->clone();
    tree1ForMatch->resample(sampleStep);

    ZSwcTree* tree2ForMatch = tree2->clone();
    tree2ForMatch->resample(sampleStep);

    //    double ratio1 =
    //        ZSwcGlobalFeatureAnalyzer::computeLateralVerticalRatio(*tree1);
    //    double ratio2 =
    //        ZSwcGlobalFeatureAnalyzer::computeLateralVerticalRatio(*tree2);

    //    double ratioDiff = max(ratio1, ratio2) / min(ratio1, ratio2);

    matcher.matchAllG(*tree1ForMatch, *tree2ForMatch, matchingLevel);

    score = matcher.matchingScore();

    //    if (m_scoreOption == SCORE_ORTREG) {
    //      score /= (1.0 + log(ratioDiff));
    //    }

    delete tree1ForMatch;
    delete tree2ForMatch;
  }

  return score;
}

int ZRunNeuTuCommand::runCompareSwc()
{
  if (m_input.empty()) {
    LOG(INFO) << "Please specify input.";
    return 0;
  }

  LOG(INFO) << "Computing pairwise similarity for ";
  std::vector<ZSwcTree*> treeArray(m_input.size(), NULL);
  for (size_t i = 0; i < m_input.size(); ++i) {
    LOG(INFO) << "  " << m_input[i];
    auto tree = new ZSwcTree;
    tree->load(m_input[i]);
    if (m_scale != 1.0) {
      tree->rescale(m_scale, m_scale, m_scale);
    }

    treeArray[i] = tree;
  }

  ZSwcTreeMatcher matcher;
  auto trunkAnalyzer = new ZSwcLayerTrunkAnalyzer;
  trunkAnalyzer->setStep(200.0);
  auto helperAnalyzer = new ZSwcLayerShollFeatureAnalyzer;
  helperAnalyzer->setLayerScale(4000.0);
  helperAnalyzer->setLayerMargin(100.0);

  auto analyzer = new ZSwcNodeBufferFeatureAnalyzer;
  analyzer->setHelper(helperAnalyzer);

  auto featureAnalyzer = dynamic_cast<ZSwcFeatureAnalyzer*>(analyzer);

  matcher.setTrunkAnalyzer(trunkAnalyzer);
  matcher.setFeatureAnalyzer(featureAnalyzer);

  std::ostringstream stream;

  std::vector<double> selfScore(m_input.size());
  for (size_t i = 0; i < m_input.size(); ++i) {
    selfScore[i] = compareSwc(treeArray[i], treeArray[i], matcher);
  }

  for (size_t i = 0; i < m_input.size(); ++i) {
    for (size_t j = i + 1; j < m_input.size(); ++j) {
      double score = compareSwc(treeArray[i], treeArray[j], matcher);
      score /= std::max(selfScore[i], selfScore[j]);
      stream << i << "-" << j << ": " << score << std::endl;
    }
  }

  LOG(INFO) << "Result:";
  for (size_t i = 0; i < m_input.size(); ++i) {
    LOG(INFO) << i << ": " << m_input[i];
  }
  LOG(INFO) << stream.str();

  return 1;
}

int ZRunNeuTuCommand::runSkeletonize(ZJsonObject& configJson)
{
  int stat;

  if (m_input.empty()) {
    LOG(INFO) << "Please specify input.";
    return 1;
  }

  if (m_isVerbose) {
    tic();
  }

  stat = skeletonizeFile(configJson);

  if (m_isVerbose) {
    ptoc();
  }

  return stat;
}

int ZRunNeuTuCommand::skeletonizeFile(ZJsonObject& configJson)
{
  if (!fexist(m_input[0].c_str())) {
    LOG(ERROR) << "Skeletonization Failed: The input file " << m_input[0] << " seems not exist.";
    return 1;
  }

  ZStackSkeletonizer skeletonizer;

  ZSwcTree* tree = NULL;

  if (configJson.hasKey("skeletonize")) {
    skeletonizer.configure(ZJsonObject(configJson["skeletonize"], ZJsonValue::SET_INCREASE_REF_COUNT));
  }

  if (m_intvSpecified) {
    skeletonizer.setDownsampleInterval(m_intv[0], m_intv[1], m_intv[2]);
  }

  if (m_isVerbose) {
    skeletonizer.print();
  }

  if (ZFileType::FileType(m_input[0]) == ZFileType::EFileType::TIFF) {
    if (m_output.empty()) {
      LOG(ERROR) << "Skeletonization Failed: The input is not a binary image.";
      return 1;
    }
    ZStack stack;
    stack.load(m_input[0]);

    if (!stack.isBinary()) {
      LOG(INFO) << "The image is not binary. Binarizing...";
      stack.binarize();
    }
    tree = skeletonizer.makeSkeleton(stack);
  } else if (ZFileType::FileType(m_input[0]) == ZFileType::EFileType::OBJECT_SCAN) {
    ZObject3dScan obj;
    obj.load(m_input[0]);
    if (m_isVerbose) {
      LOG(INFO) << obj.getVoxelNumber() << " foreground voxels.";
    }
    tree = skeletonizer.makeSkeleton(obj);
  } else {
    LOG(ERROR) << "Skeletonization Failed: Unrecognized output: " << m_input[0];
  }

  if (tree != NULL) {
    if (!tree->isEmpty()) {
      if (!m_output.empty()) {
        tree->save(m_output);
        LOG(INFO) << "SWC saved in " << m_output;
      }
    } else {
      LOG(INFO) << "No SWC generated.";
    }
    delete tree;
  } else {
    LOG(INFO) << "No SWC generated.";
  }

  return 0;
}

ZJsonObject ZRunNeuTuCommand::loadConfig(const std::string& filePath)
{
  ZJsonObject configJson;
  configJson.load(filePath);

  expandConfig(filePath, "skeletonize", configJson);
  expandConfig(filePath, "trace", configJson);

  VLOG(1) << "==========Command configuration========";
  VLOG(1) << configJson.dumpString(2);
  VLOG(1) << "=======================================";

  return configJson;
}

std::string
ZRunNeuTuCommand::extractIncludePath(const std::string& configFilePath, const std::string& key, ZJsonObject& configJson)
{
  QDir configDir = QDir(ZString::dirPath(configFilePath).c_str());

  QString filePath;

  ZJsonObject subJson(configJson.value(key.c_str()));

  if (subJson.hasKey("include")) {
    QFileInfo fileInfo(ZJsonParser::stringValue(subJson["include"]).c_str());
    if (fileInfo.isRelative()) {
      filePath = configDir.absoluteFilePath(fileInfo.filePath());
    } else {
      filePath = fileInfo.absoluteFilePath();
    }
  }

  return filePath.toStdString();
}

void ZRunNeuTuCommand::expandConfig(const std::string& configFilePath,
                                    const std::string& objKey,
                                    ZJsonObject& configJson)
{
  if (configJson.hasKey(objKey.c_str())) {
    std::string includeFilePath = extractIncludePath(configFilePath, objKey, configJson);

    if (!includeFilePath.empty()) {
      if (fexist(includeFilePath.c_str())) {
        ZJsonObject subJson(configJson.value(objKey.c_str()));
        ZJsonObject includeJson;
        includeJson.load(includeFilePath);

        const char* key;
        json_t* value;
        ZJsonObject_foreach(includeJson, key, value)
        {
          if (!subJson.hasKey(key)) {
            ZJsonValue obj = ZJsonValue(value, ZJsonValue::SET_INCREASE_REF_COUNT);
            subJson.setEntry(key, obj);
          }
        }
        subJson.removeKey("include");
      } else {
        LOG(INFO) << "Missing include file: " << includeFilePath;
      }
    }
  }
}

int ZRunNeuTuCommand::runTraceCommand(const std::vector<std::string>& input,
                                      const std::string& output,
                                      const ZJsonObject& config)
{
  ZJsonObject inputJson(config.value("_input"));

  ZJsonObjectParser parser(inputJson);
  std::string additionalInput = parser.getValue("signal", "");

  std::vector<std::string> updatedInput = input;
  if (!additionalInput.empty() && updatedInput.empty()) {
    updatedInput.push_back(additionalInput);
  }

  if (updatedInput.empty() || output.empty()) {
    return 1;
  }

  loadTraceConfigForTraceCommand(config);

  if (ZJsonObjectParser::GetValue(config, "action", "") == "inspect") {
    ZNeuronTracerConfig::getInstance().print();
    return 0;
  }

  ZSwcTree* tree = traceFile(updatedInput[0], inputJson);

  if (tree) {
    LOG(INFO) << "Saving " + output + "...";
    tree->save(output);
  } else {
    LOG(WARNING) << "WARNING: No result generated.";
  }

  return 0;
}

void ZRunNeuTuCommand::loadTraceConfigForTraceCommand(const ZJsonObject& config)
{
  ZJsonObject actualConfig = config;

  if (config.hasKey("path")) {
    std::string path = ZJsonObjectParser::GetValue(config, "path", "");
    if (path.empty() || path == "default") {
      path = ZSystemInfo::jsonDir().absoluteFilePath("trace_config.json").toStdString();
    }
    actualConfig.load(path);
  }

  if (!ZNeuronTracerConfig::getInstance().loadJsonObject(actualConfig)) {
    LOG(WARNING) << "Configuration Failed: Failed to load the config.";
  }

  m_diagnosis = ZJsonObjectParser::GetValue(config, "diagnosis", false);
  //  ZNeuronTracerConfig::getInstance().setCrossoverTest(false);
}

ZSwcTree* ZRunNeuTuCommand::traceFile(const std::string& filePath, const ZJsonObject& inputConfig) const
{
  //  ZStack signal;
  //  signal.load(filePath);

  ZStack* signal = ZStackReader::Read(filePath);

  ZSwcTree* tree = nullptr;

  if (signal) {
    ZNeuronTracer tracer;
    tracer.setIntensityField(signal);
    tracer.setTraceLevel(m_level);
    tracer.setDiagnosis(m_diagnosis);

    ZJsonObjectParser parser(inputConfig);
    std::string maskPath = parser.getValue("mask", "");
    ZStack* mask = nullptr;
    if (!maskPath.empty()) {
      LOG(INFO) << "Using a predefined mask: " << maskPath;
      if (!neutu::FileExists(maskPath)) {
        LOG(ERROR) << "Missing file: Cannot find the mask file " + maskPath + ".";
        return nullptr;
      }
      if (ZFileType::FileType(maskPath) != ZFileType::EFileType::TIFF) {
        LOG(ERROR) << "File error: Failed to recognize the mask file " + maskPath + " as a TIFF";
        return nullptr;
      }

      mask = ZStackReader::Read(maskPath);
      if (mask == nullptr) {
        LOG(WARNING) << "File error: Failed to read mask file " + maskPath + ".";
      } else {
        int threshold = parser.getValue("maskThreshold", 0);
        if (threshold < 0) {
          //        Stack *newMask = tracer.makeMask(mask);
          tracer._makeMask = [&](Stack* /*stack*/) {
            return tracer.makeMask(mask->c_stack());
          };
        } else {
          mask->binarize(threshold);
          tracer._makeMask = [=](Stack* /*stack*/) {
            return C_Stack::clone(mask->c_stack());
          };
        }
      }
    }

    tree = tracer.trace(signal);

    delete mask;
    delete signal;

    return tree;
  }

  return tree;
}

} // namespace nim
