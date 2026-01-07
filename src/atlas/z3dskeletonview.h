#pragma once

#include "z3dfilterview.h"
#include "z3dskeletonfilter.h"
#include "zskeletondoc.h"

namespace nim {

class Z3DSkeletonView : public Z3DFilterView<ZSkeletonDoc, Z3DSkeletonFilter>
{
public:
  Z3DSkeletonView(ZSkeletonDoc& doc, Z3DRenderingEngine& engine);

private:
  void docSkeletonsAdded(const std::vector<size_t>& objs);

  void docSkeletonAdded(size_t id);
};

} // namespace nim

