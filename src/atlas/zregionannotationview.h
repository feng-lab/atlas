#pragma once

#include "zfilterview.h"
#include "zregionannotationdoc.h"
#include "zregionannotationfilter.h"

namespace nim {

class ZRegionAnnotationView : public ZFilterView<ZRegionAnnotationDoc, ZRegionAnnotationFilter>
{
  Q_OBJECT

public:
  ZRegionAnnotationView(ZRegionAnnotationDoc& doc, ZView& view);

private:
  void docRegionAnnotationsAdded(const std::vector<size_t>& objs);

  void docRegionAnnotationAdded(size_t id);
};

} // namespace nim
