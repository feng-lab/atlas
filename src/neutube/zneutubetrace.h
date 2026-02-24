#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim::neutube {

// Trace a neuron from either a seed position (interactive-like) or automatically
// (when no position is provided), matching NeuTu behavior.
//
// Implementation note:
// - This entrypoint is intentionally kept stable so we can switch the internal
//   implementation from legacy neurolabi to fully ported C++ incrementally.
int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool verbose);

} // namespace nim::neutube
