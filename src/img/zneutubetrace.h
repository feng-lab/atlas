#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim {

struct RunTraceOptions
{
  std::optional<std::array<int, 3>> position;
  int level = 0;
  bool diagnosis = false;
  std::string traceConfigPath;
  std::string jsonDirPath;
  bool verbose = false;

  bool useBlocked = false;
  std::string outputSessionDir;

  size_t selectedChannel = 0;
  size_t selectedTime = 0;
  std::array<size_t, 3> signalDownsampleRatio = {1, 1, 1};
  std::optional<double> zToXYRatioOverride;
};

// Trace a neuron from either a seed position (interactive-like) or automatically
// (when no position is provided), matching NeuTu behavior.
//
// Implementation note:
// - This entrypoint is intentionally kept stable so we can switch the internal
//   implementation from legacy neurolabi to fully ported C++ incrementally.
int runTrace(const std::vector<std::string>& input, const std::string& outputPath, const RunTraceOptions& options);

} // namespace nim
