#pragma once

#include "zfilterview.h"
#include "zregionAnnotationdoc.h"
#include "zregionAnnotationfilter.h"
#include <map>

namespace nim {

class ZRegionAnnotationView : public ZFilterView<ZRegionAnnotationDoc, ZRegionAnnotationFilter>
{
Q_OBJECT
public:
  ZRegionAnnotationView(ZRegionAnnotationDoc& doc, ZView& view);

private:
  void docRegionAnnotationsAdded(const QList<size_t>& objs);

  void docRegionAnnotationAdded(size_t id);
};

} // namespace nim

