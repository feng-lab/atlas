#include "zneutubetrace.h"

#include "zneutubelegacy.h"

namespace nim::neutube {

int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool verbose)
{
  // Temporary scaffolding:
  // - Keeps behavior byte-identical while we port the neurolabi C tracer into
  //   clean C++ code under src/neutube/ (Goal 1).
  // - The long-term goal is for this function to become self-contained and
  //   stop calling into `neutube_legacy`.
  return neutube_legacy::runTrace(input, outputPath, position, level, diagnosis, traceConfigPath, jsonDirPath, verbose);
}

} // namespace nim::neutube
