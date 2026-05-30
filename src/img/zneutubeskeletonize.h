#pragma once

#include <QString>

#include <array>
#include <optional>

namespace nim {

int runSkeletonize(const QString& inputPath,
                   const QString& outputPath,
                   const QString& skeletonizeConfigPath,
                   const std::optional<std::array<int, 3>>& downsampleIntervalOverride,
                   bool verbose);

} // namespace nim
