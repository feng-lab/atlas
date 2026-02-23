#pragma once

#include "zjson.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim::neutube_legacy {

int runSkeletonize(const std::string& inputPath,
                   const std::string& outputPath,
                   const std::string& skeletonizeConfigPath,
                   const std::optional<std::array<int, 3>>& downsampleIntervalOverride,
                   bool verbose);

int runCompareSwc(const std::vector<std::string>& inputPaths, double scale);

int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool verbose);

int runGeneral(const std::string& generalConfigTextOrPath,
               const json::object& generalCfg,
               const json::object& inputJson,
               const std::vector<std::string>& positionalInput,
               const std::string& outputPath,
               int level,
               bool diagnosis,
               const std::string& traceIncludePath,
               const std::string& jsonDirPath,
               bool verbose);

} // namespace nim::neutube_legacy
