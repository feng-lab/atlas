#pragma once

#include "zwidgetsgroup.h"

namespace nim {

class ZViewSettingInterface
{
public:
  virtual std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) = 0;
};

} // namespace nim

