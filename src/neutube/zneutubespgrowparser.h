#pragma once

#include "zneutubespgrow.h"
#include "zneutubevoxelarray.h"

#include "zimg.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace nim::neutube {

class ZNeutubeSpGrowParser
{
public:
  ZNeutubeSpGrowParser() = default;

  explicit ZNeutubeSpGrowParser(SpGrowWorkspace* workspace);

  [[nodiscard]] ZNeutubeVoxelArray extractLongestPath(double* length, bool masked);

  [[nodiscard]] std::vector<ZNeutubeVoxelArray> extractAllPath(double minLength, const ZImg& ballImg);

private:
  [[nodiscard]] ZNeutubeVoxelArray extractPath(int64_t index) const;

  [[nodiscard]] double pathLength(int64_t index, bool masked) const;

  [[nodiscard]] bool hasCheckedMask() const
  {
    return !m_checkedMask.isEmpty();
  }

private:
  SpGrowWorkspace* m_workspace = nullptr;

  std::vector<int64_t> m_fgArray;

  ZImg m_checkedMask;
  ZImg m_pathMask;
};

} // namespace nim::neutube
