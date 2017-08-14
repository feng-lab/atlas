#pragma once

#include "zimginfo.h"
#include <H5Cpp.h>

namespace nim {

class ZImgInfoIO
{
public:
  static ZImgInfo load(const H5::Group& grp);
  static void save(H5::Group& grp, const ZImgInfo& info);
};

} // namespace nim



