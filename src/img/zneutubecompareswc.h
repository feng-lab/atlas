#pragma once

#include <QString>

#include <cstddef>
#include <string>
#include <vector>

namespace nim {

struct CompareSwcPairScore
{
  size_t i = 0;
  size_t j = 0;
  double score = 0.0;
};

struct CompareSwcResult
{
  std::vector<QString> inputs;
  std::vector<CompareSwcPairScore> pairs;
};

[[nodiscard]] CompareSwcResult computeCompareSwc(const std::vector<QString>& inputPaths, double scale);

[[nodiscard]] std::string formatCompareSwcPairs(const CompareSwcResult& result);

int runCompareSwc(const std::vector<QString>& inputPaths, double scale);

} // namespace nim
