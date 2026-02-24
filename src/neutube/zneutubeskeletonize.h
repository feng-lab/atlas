#pragma once

#include <array>
#include <optional>
#include <string>

namespace nim::neutube {

int runSkeletonize(const std::string& inputPath,
                   const std::string& outputPath,
                   const std::string& skeletonizeConfigPath,
                   const std::optional<std::array<int, 3>>& downsampleIntervalOverride,
                   bool verbose);

} // namespace nim::neutube
