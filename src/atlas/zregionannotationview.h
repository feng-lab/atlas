#ifndef ZREGIONANNOTATIONVIEW_H
#define ZREGIONANNOTATIONVIEW_H

#include <map>
#include "zfilterview.h"
#include "zregionAnnotationdoc.h"
#include "zregionAnnotationfilter.h"

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

#endif // ZREGIONANNOTATIONVIEW_H
