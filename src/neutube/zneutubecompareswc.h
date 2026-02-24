#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nim::neutube {

struct CompareSwcPairScore
{
  size_t i = 0;
  size_t j = 0;
  double score = 0.0;
};

struct CompareSwcResult
{
  std::vector<std::string> inputs;
  std::vector<CompareSwcPairScore> pairs;
};

[[nodiscard]] CompareSwcResult computeCompareSwc(const std::vector<std::string>& inputPaths, double scale);

[[nodiscard]] std::string formatCompareSwcPairs(const CompareSwcResult& result);

int runCompareSwc(const std::vector<std::string>& inputPaths, double scale);

} // namespace nim::neutube
