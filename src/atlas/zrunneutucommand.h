#pragma once

#include "zjsonobject.h"
#include <string>
#include <vector>

class ZSwcTree;
class ZSwcTreeMatcher;
class ZStack;

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

  int runSkeletonize();
  int runCompareSwc();
  int runTraceNeuron();
  int runGeneral();

  void loadConfig(const std::string& filePath);
  void expandConfig(const std::string& configFilePath, const std::string& key);
  std::string extractIncludePath(const std::string& configFilePath, const std::string& key);

  static double compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher);

private:
  void loadTraceConfig();

  int skeletonizeFile();

  ZSwcTree* traceFile();

  ZJsonObject loadInputJson();

  int runTraceCommand(const std::vector<std::string>& input, const std::string& output, const ZJsonObject& config);
  void loadTraceConfig(const ZJsonObject& config);
  [[nodiscard]] ZSwcTree* traceFile(const std::string& filePath, const ZJsonObject& inputConfig) const;

private:
  std::vector<std::string> m_input;
  std::string m_output;
  ZJsonObject m_configJson;
  std::string m_configDir;
  std::string m_generalConfig;
  ZJsonObject m_inputJson;
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
