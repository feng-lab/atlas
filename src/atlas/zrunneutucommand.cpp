#include "zrunneutucommand.h"

#include <ostream>
#include <set>

#include <QFileInfoList>
#include <QDir>
#include <QApplication>
#include <QUrl>
#include <QUrlQuery>

#include "zlog.h"
#include "zsysteminfo.h"

#include "tz_utilities.h"
#include "filesystem/utilities.h"
#include "zobject3dscan.h"
#include "zjsonparser.h"
#include "zjsonobject.h"
#include "zfiletype.h"
#include "zstackskeletonizer.h"
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

#include "command/zcommandmodule.h"
#include "command/zneurontracecommand.h"

namespace nim {

ZRunNeuTuCommand::ZRunNeuTuCommand()
{
  init();
}

void ZRunNeuTuCommand::init()
{
  m_ravelerHeight = 2599;
  m_zStart = 1499;

  m_isVerbose = false;
  for (int i = 0; i < 3; ++i) {
    m_intv[i] = 0;
    m_blockOffset[i] = 0;
  }
  m_level = 0;
  m_scale = 1.0;
  m_fullOverlapScreen = false;
  m_forceUpdate = false;
  m_namedOnly = false;
  m_intvSpecified = false;
}

void ZRunNeuTuCommand::registerModule()
{
  registerModule<ZNeuronTraceCommand>("trace_neuron");
}

template<typename T>
void ZRunNeuTuCommand::registerModule(const std::string& name)
{
  registerModule(name, new T);
}

void ZRunNeuTuCommand::registerModule(const std::string& name, ZCommandModule* module)
{
  if (!name.empty() && module != NULL) {
    if (m_commandMap.count(name) > 0) {
      LOG(WARNING) << "Cannot overwrite a registered module: " << name;
    } else {
      module->setForceUpdate(m_forceUpdate);
      m_commandMap[name] = module;
    }
  }
}

ZCommandModule* ZRunNeuTuCommand::getModule(const std::string& name)
{
  ZCommandModule* module = NULL;
  if (m_commandMap.count(name) > 0) {
    module = m_commandMap[name];
  }

  return module;
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

void ZRunNeuTuCommand::loadTraceConfig()
{
  if (!ZNeuronTracerConfig::getInstance().loadJsonObject(m_configJson)) {
    ZNeuronTracerConfig::getInstance().load(m_configDir + "/trace_config.json");
  }
}

ZJsonObject ZRunNeuTuCommand::loadInputJson()
{
  if (m_input.empty()) {
    return ZJsonObject();
  }

  ZJsonObject obj;
  if (m_input[0] == "json") {
    std::string jsonInput = m_input[1];

    if (ZFileType::FileType(jsonInput) == ZFileType::EFileType::JSON) {
      obj.load(jsonInput);
    } else {
      if (!obj.decode(jsonInput, true)) {
        std::cerr << "Invalid input json: " << jsonInput << std::endl;
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
        std::cerr << "ERROR: invalid position input!" << std::endl;
        exit(1);
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
        std::cerr << "ERROR: invalid size input!" << std::endl;
        exit(1);
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

int ZRunNeuTuCommand::runTraceNeuron()
{
  if (m_input.empty()) {
    std::cout << "No input specified. Abort." << std::endl;
    return 1;
  }

  if (m_input[0].empty()) {
    std::cout << "No input data specified. Abort." << std::endl;
    return 1;
  }

  if (m_output.empty()) {
    std::cout << "No output specified. Abort." << std::endl;
    return 1;
  }

  int stat = 1;

  loadTraceConfig();

  ZSwcTree* tree = traceFile();

  if (tree != NULL) {
    tree->save(m_output);
    delete tree;

    stat = 0;
  }

  return stat;
}

int ZRunNeuTuCommand::runGeneral()
{
  if (!m_generalConfig.empty()) {
#ifdef _DEBUG_
    std::cout << "Config: " << m_generalConfig << std::endl;
#endif

    ZJsonObject config;
    if (ZFileType::FileType(m_generalConfig) == ZFileType::EFileType::JSON) {
      config.load(m_generalConfig);
      config.setEntry("_source", m_generalConfig);
    } else {
      if (!config.decode(m_generalConfig, true)) {
        std::cerr << "Invalid config json: " << m_generalConfig << std::endl;
      }
    }

    config.setEntry("_input", m_inputJson);

    std::string commandName = ZJsonParser::stringValue(config["command"]);
    std::cout << "Running command " << commandName << "..." << std::endl;
    ZCommandModule* module = getModule(commandName);
    if (module != NULL) {
      try {
        return module->run(m_input, m_output, config);
      }
      catch (std::exception& e) {
        std::cerr << "COMMAND FAILED: " << e.what() << std::endl;
        return 1;
      }
    } else {
      std::cerr << "Invalid command module: " << commandName << std::endl;

      return 1;
    }
  }

  return 1;
}

double ZRunNeuTuCommand::compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher) const
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
    std::cout << "Please specify input." << std::endl;
    return 0;
  }

  std::cout << "Computing pairwise similarity for " << std::endl;
  std::vector<ZSwcTree*> treeArray(m_input.size(), NULL);
  for (size_t i = 0; i < m_input.size(); ++i) {
    std::cout << "  " << m_input[i] << std::endl;
    ZSwcTree* tree = new ZSwcTree;
    tree->load(m_input[i]);
    if (m_scale != 1.0) {
      tree->rescale(m_scale, m_scale, m_scale);
    }

    treeArray[i] = tree;
  }

  ZSwcTreeMatcher matcher;
  ZSwcLayerTrunkAnalyzer* trunkAnalyzer = new ZSwcLayerTrunkAnalyzer;
  trunkAnalyzer->setStep(200.0);
  ZSwcLayerShollFeatureAnalyzer* helperAnalyzer = new ZSwcLayerShollFeatureAnalyzer;
  helperAnalyzer->setLayerScale(4000.0);
  helperAnalyzer->setLayerMargin(100.0);

  ZSwcNodeBufferFeatureAnalyzer* analyzer = new ZSwcNodeBufferFeatureAnalyzer;
  analyzer->setHelper(helperAnalyzer);

  ZSwcFeatureAnalyzer* featureAnalyzer = dynamic_cast<ZSwcFeatureAnalyzer*>(analyzer);

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

  std::cout << "Result:" << std::endl;
  for (size_t i = 0; i < m_input.size(); ++i) {
    std::cout << i << ": " << m_input[i] << std::endl;
  }
  std::cout << stream.str();

  return 1;
}

int ZRunNeuTuCommand::runSkeletonize()
{
  int stat = 0;

  if (m_input.empty()) {
    std::cout << "Please specify input." << std::endl;
    return 1;
  }

  if (m_isVerbose) {
    tic();
  }

  stat = skeletonizeFile();

  if (m_isVerbose) {
    ptoc();
  }

  return stat;
}

int ZRunNeuTuCommand::skeletonizeFile()
{
  if (!fexist(m_input[0].c_str())) {
    LOG(ERROR) << "Skeletonization Failed: The input file " << m_input[0] << " seems not exist.";
    return 1;
  }

  ZStackSkeletonizer skeletonizer;

  ZSwcTree* tree = NULL;

  if (m_configJson.hasKey("skeletonize")) {
    skeletonizer.configure(ZJsonObject(m_configJson["skeletonize"], ZJsonValue::SET_INCREASE_REF_COUNT));
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
      std::cout << "The image is not binary. Binarizing..." << std::endl;
      stack.binarize();
    }
    tree = skeletonizer.makeSkeleton(stack);
  } else if (ZFileType::FileType(m_input[0]) == ZFileType::EFileType::OBJECT_SCAN) {
    ZObject3dScan obj;
    obj.load(m_input[0]);
    if (m_isVerbose) {
      std::cout << obj.getVoxelNumber() << " foreground voxels." << std::endl;
    }
    tree = skeletonizer.makeSkeleton(obj);
  } else {
    LOG(ERROR) << "Skeletonization Failed: Unrecognized output: " << m_input[0];
  }

  if (tree != NULL) {
    if (!tree->isEmpty()) {
      if (!m_output.empty()) {
        tree->save(m_output);
        std::cout << "SWC saved in " << m_output << std::endl;
      }
    } else {
      std::cout << "No SWC generated." << std::endl;
    }
    delete tree;
  } else {
    std::cout << "No SWC generated." << std::endl;
  }

  return 0;
}

int ZRunNeuTuCommand::run(int argc, char* argv[])
{
  static const char* Spec[] = {"--command",
                               "[--unit_test]",
                               "[<input:string> ...]",
                               "[-o <string>]",
                               "[--config <string>]",
                               "[--intv <int> <int> <int>]",
                               "[--skeletonize] [--force] [--bodyid <string>] [--named_only]",
                               "[--general <string>]",
                               "[--compare_swc] [--scale <double>]",
                               "[--trace] [--level <int>]",
                               "[--separate <string>]",
                               "[--foutput <string>]",
                               "[--compute_seed]",
                               //    "[--position <int> <int> <int>]",
                               //    "[--size <int> <int> <int>]",
                               "[--dvid <string>]",
                               "[--test]",
                               "[--verbose]",
                               0};

#ifdef _DEBUG_2
  for (int i = 0; i < argc; ++i) {
    std::cout << argv[i] << std::endl;
  }
#endif

  Process_Arguments(argc, argv, const_cast<char**>(Spec), 1);

  m_configDir = ZSystemInfo::resourcesDir().absoluteFilePath("json").toStdString();
  // m_configDir = neutu::JoinPath(NeutubeConfig::getInstance().getPath(NeutubeConfig::EConfigItem::CONFIG_DIR),
  // "json");
  std::string configPath = neutu::JoinPath(m_configDir, "command_config.json");

  if (Is_Arg_Matched(const_cast<char*>("--config"))) {
    configPath = Get_String_Arg(const_cast<char*>("--config"));
  }

  loadConfig(configPath);

  ECommand command = UNKNOWN_COMMAND;
  if (!m_configJson.isEmpty()) {
    command = getCommand(ZJsonParser::stringValue(m_configJson["command"]).c_str());
    m_input.push_back(ZJsonParser::stringValue(m_configJson["input"]));
    m_output = ZJsonParser::stringValue(m_configJson["output"]);
    if (m_configJson.hasKey("synapse")) {
      m_synapseFile = ZJsonParser::stringValue(m_configJson["synapse"]);
    }

    if (m_configJson.hasKey("raveler_height")) {
      m_ravelerHeight = ZJsonParser::integerValue(m_configJson["raveler_height"]);
    }

    if (m_configJson.hasKey("z_start")) {
      m_zStart = ZJsonParser::integerValue(m_configJson["z_start"]);
    }

    if (m_configJson.hasKey("block_offset")) {
      for (int i = 0; i < 3; ++i) {
        m_blockOffset[i] = ZJsonParser::numberValue(m_configJson["block_offset"], i);
      }
    }

    m_blockFile = ZJsonParser::stringValue(m_configJson["block_file"]);
    m_referenceBlockFile = ZJsonParser::stringValue(m_configJson["block_reference"]);
  }

  m_forceUpdate = false;
  if (Is_Arg_Matched(const_cast<char*>("--force"))) {
    m_forceUpdate = true;
  }

  m_namedOnly = false;
  if (Is_Arg_Matched(const_cast<char*>("--named_only"))) {
    m_namedOnly = true;
  }

  if (Is_Arg_Matched(const_cast<char*>("--foutput"))) {
    m_outputFlag = Get_String_Arg(const_cast<char*>("--foutput"));
  }

  m_scale = 1.0;
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

  m_inputJson = loadInputJson();

  if (Is_Arg_Matched(const_cast<char*>("-o"))) {
    m_output = Get_String_Arg(const_cast<char*>("-o"));
  }

  if (Is_Arg_Matched(const_cast<char*>("--verbose"))) {
    m_isVerbose = true;
  }

  if (Is_Arg_Matched(const_cast<char*>("--intv"))) {
    for (int i = 0; i < 3; ++i) {
      m_intv[i] = Get_Int_Arg(const_cast<char*>("--intv"), i + 1);
    }
    m_intvSpecified = true;
  }

  //  if (Is_Arg_Matched(const_cast<char*>("--position"))) {
  //    m_position.resize(3, 0);
  //    for (int i = 0; i < 3; ++i) {
  //      m_position[i] = Get_Int_Arg(const_cast<char*>("--position"), i + 1);
  //    }
  //  }

  //  if (Is_Arg_Matched(const_cast<char*>("--size"))) {
  //    for (int i = 0; i < 3; ++i) {
  //      m_size[i] = Get_Int_Arg(const_cast<char*>("--size"), i + 1);
  //    }
  //  }

  //  if (Is_Arg_Matched(const_cast<char*>("--bodyid"))) {
  //    ZString bodyIdStr(Get_String_Arg(const_cast<char*>("--bodyid")));
  //
  //    if (bodyIdStr.endsWith("txt", ZString::CASE_INSENSITIVE)) {
  //      importBodies(bodyIdStr);
  //    } else {
  //      m_bodyIdArray = bodyIdStr.toUint64Array();
  //      if (m_bodyIdArray.empty()) {
  //        _error("No valid body ID specified. Abort!");
  //        return 1;
  //      }
  //    }
  //  }

  if (command == UNKNOWN_COMMAND) {
    if (Is_Arg_Matched(const_cast<char*>("--skeletonize"))) {
      command = SKELETONIZE;
      //    m_input.push_back(ZArgumentProcessor::getStringArg("input", 0));
    } else if (Is_Arg_Matched(const_cast<char*>("--separate"))) {
      command = SEPARATE_IMAGE;
      //      m_input.push_back(ZArgumentProcessor::getStringArg("input", 0));
      m_input.push_back(Get_String_Arg(const_cast<char*>("--separate")));
      m_output = Get_String_Arg(const_cast<char*>("-o"));
    } else if (Is_Arg_Matched(const_cast<char*>("--trace"))) {
      //      m_input.push_back(ZArgumentProcessor::getStringArg("input", 0));
      m_output = Get_String_Arg(const_cast<char*>("-o"));
      command = TRACE_NEURON;
    } else if (Is_Arg_Matched(const_cast<char*>("--test"))) {
      command = TEST_SELF;
    } else if (Is_Arg_Matched(const_cast<char*>("--compare_swc"))) {
      command = COMPARE_SWC;
    } else if (Is_Arg_Matched(const_cast<char*>("--compute_seed"))) {
      command = COMPUTE_SEED;
    } else if (Is_Arg_Matched(const_cast<char*>("--general"))) {
      command = GENERAL_COMMAND;
      m_generalConfig = Get_String_Arg(const_cast<char*>("--general"));
#ifdef _DEBUG_2
      std::cout << "Config:" << std::endl;
      std::cout << m_generalConfig << std::endl;
#endif
    }
  }

  registerModule();

  switch (command) {
    case SKELETONIZE:
      return runSkeletonize();
    case COMPARE_SWC:
      return runCompareSwc();
    case TRACE_NEURON:
      return runTraceNeuron();
    case GENERAL_COMMAND:
      return runGeneral();
      break;
    default:
      LOG(INFO) << "Unknown command";
      return 1;
  }

  return 0;
}

void ZRunNeuTuCommand::loadConfig(const std::string& filePath)
{
  //  ZJsonObject m_configJson;
  m_configJson.load(filePath);

  expandConfig(filePath, "skeletonize");
  expandConfig(filePath, "trace");

#ifdef _DEBUG_
  std::cout << "==========Command configuration========" << std::endl;
  std::cout << m_configJson.dumpString(2) << std::endl;
  std::cout << "=======================================" << std::endl;
#endif
}

std::string ZRunNeuTuCommand::extractIncludePath(const std::string& configFilePath, const std::string& key)
{
  QDir configDir = QDir(ZString::dirPath(configFilePath).c_str());

  QString filePath;

  ZJsonObject subJson(m_configJson.value(key.c_str()));

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

void ZRunNeuTuCommand::expandConfig(const std::string& configFilePath, const std::string& objKey)
{
  if (m_configJson.hasKey(objKey.c_str())) {
    std::string includeFilePath = extractIncludePath(configFilePath, objKey);

    if (!includeFilePath.empty()) {
      if (fexist(includeFilePath.c_str())) {
        ZJsonObject subJson(m_configJson.value(objKey.c_str()));
        ZJsonObject includeJson;
        includeJson.load(includeFilePath.c_str());

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
        std::cout << "Missing include file: " << includeFilePath << std::endl;
      }
    }
  }
}

} // namespace nim
