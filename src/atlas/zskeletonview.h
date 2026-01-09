#pragma once

#include "zfilterview.h"
#include "zskeletondoc.h"
#include "zskeletonfilter.h"

namespace nim {

class ZSkeletonView : public ZFilterView<ZSkeletonDoc, ZSkeletonFilter>
{
  Q_OBJECT

public:
  ZSkeletonView(ZSkeletonDoc& doc, ZView& view);

private:
  void docSkeletonsAdded(const std::vector<size_t>& objs);

  void docSkeletonAdded(size_t id);

  void docSkeletonChanged(size_t id);
};

} // namespace nim
