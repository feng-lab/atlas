#pragma once

#include <QString>
#include <cstdint>

namespace nim {

struct ZGpuMemoryCapacityInfo
{
  uint64_t capacityBytes = 0;
  bool hasUnifiedMemory = false;
  QString source;
};

ZGpuMemoryCapacityInfo detectSystemGpuMemoryCapacity();

} // namespace nim
