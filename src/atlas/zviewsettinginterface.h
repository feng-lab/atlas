#ifndef ZVIEWSETTINGINTERFACE_H
#define ZVIEWSETTINGINTERFACE_H

#include "zwidgetsgroup.h"

namespace nim {

class ZViewSettingInterface
{
public:
  virtual ZWidgetsGroup* viewSettingWidgetsGroupOf(size_t id) = 0;
};

} // namespace nim

#endif // ZVIEWSETTINGINTERFACE_H
