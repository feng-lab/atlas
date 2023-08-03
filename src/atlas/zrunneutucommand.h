#pragma once

#include <string>
#include <vector>

class ZSwcTree;
class ZSwcTreeMatcher;
class ZJsonObject;

namespace nim {

class ZRunNeuTuCommand
{
public:
  enum ECommand
  {
    OBJECT_MARKER,
    BOUNDARY_ORPHAN,
    FLYEM_NEURON_FEATURE,
    SKELETONIZE,
    SEPARATE_IMAGE,
    TRACE_NEURON,
    COMPARE_SWC,
    COMPUTE_SEED,
    GENERAL_COMMAND,
    UNKNOWN_COMMAND
  };

  int run(int argc, char* argv[]);

private:
  static ECommand getCommand(const char* cmd);

  int runSkeletonize(ZJsonObject& configJson);
  int runCompareSwc();
  int runTraceNeuron(const ZJsonObject& configJson);
  int runGeneral(const ZJsonObject& inputJson);

  static void loadConfig(const std::string& filePath, ZJsonObject& configJson);
  static void expandConfig(const std::string& configFilePath, const std::string& key, ZJsonObject& configJson);
  static std::string
  extractIncludePath(const std::string& configFilePath, const std::string& key, ZJsonObject& configJson);

  static double compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher);

private:
  void loadTraceConfig(const ZJsonObject& configJson);

  int skeletonizeFile(ZJsonObject& configJson);

  ZSwcTree* traceFile();

  ZJsonObject loadInputJson();

  int runTraceCommand(const std::vector<std::string>& input, const std::string& output, const ZJsonObject& config);
  void loadTraceConfigForTraceCommand(const ZJsonObject& config);
  [[nodiscard]] ZSwcTree* traceFile(const std::string& filePath, const ZJsonObject& inputConfig) const;

private:
  std::vector<std::string> m_input;
  std::string m_output;
  std::string m_configDir;
  std::string m_generalConfig;
  int m_intv[3] = {0, 0, 0};
  bool m_intvSpecified = false;
  std::vector<int> m_position;
  std::vector<int> m_size;
  int m_level = 0;
  double m_scale = 1.0;
  bool m_isVerbose = false;
  bool m_diagnosis = false;
};

} // namespace nim
