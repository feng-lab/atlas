#pragma once

#include <string>
#include <vector>
#include <set>

#include "zjsonobject.h"

class ZSwcTree;
class ZSwcTreeMatcher;
class ZStack;

namespace nim {

class ZRunNeuTuCommand
{
public:
  ZRunNeuTuCommand();

  enum ECommand
  {
    OBJECT_MARKER,
    BOUNDARY_ORPHAN,
    OBJECT_OVERLAP,
    SYNAPSE_OBJECT,
    CLASS_LIST,
    FLYEM_NEURON_FEATURE,
    SKELETONIZE,
    SEPARATE_IMAGE,
    TRACE_NEURON,
    TEST_SELF,
    COMPARE_SWC,
    COMPUTE_SEED,
    GENERAL_COMMAND,
    UNKNOWN_COMMAND
  };

  int run(int argc, char* argv[]);

private:
  void init();

  static ECommand getCommand(const char *cmd);

  int runSkeletonize();
  int runCompareSwc();
  int runTraceNeuron();

  void loadConfig(const std::string& filePath);
  void expandConfig(const std::string& configFilePath, const std::string& key);
  std::string extractIncludePath(const std::string& configFilePath, const std::string& key);

  double compareSwc(ZSwcTree* tree1, ZSwcTree* tree2, ZSwcTreeMatcher& matcher) const;

private:
  void loadTraceConfig();

  int skeletonizeFile();

  ZSwcTree* traceFile();

  ZJsonObject loadInputJson();

private:
  std::vector<std::string> m_input;
  std::string m_output;
  std::string m_blockFile;
  std::string m_referenceBlockFile;
  std::string m_synapseFile;
  ZJsonObject m_configJson;
  std::string m_configDir;
  std::string m_outputFlag;
  std::string m_generalConfig;
  ZJsonObject m_inputJson;
  //  std::string m_initialSwcPath;
  int m_ravelerHeight;
  int m_zStart;
  int m_intv[3];
  bool m_intvSpecified;
  int m_blockOffset[3];
  std::vector<int> m_position;
  std::vector<int> m_size;
  int m_level;
  double m_scale;
  bool m_fullOverlapScreen;
  bool m_isVerbose;
  bool m_forceUpdate;
  bool m_namedOnly;
  std::vector<uint64_t> m_bodyIdArray;
};

} // namespace nim
